// openHAB client, see wp_oh.h.
//
// Tasks (stacks in PSRAM where there is one, none of them touches flash):
//   oh      main loop: version check, screen loading, subscription, periodic reload
//   oh_sse  GET /rest/events/states: connection id, then state updates
//   oh_reg  GET /rest/events?topics=items added/updated/removed: triggers a reload
//   oh_cmd  item commands and rule runs from a queue
#include "wp_oh.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_mgr.h"

static const char *TAG = "oh";

#define RELOAD_PERIOD_US     (10LL * 60 * 1000 * 1000)   // REQ-SCR-5
#define RELOAD_DEBOUNCE_MS   2000      // a registry change often comes as a burst
#define STREAM_TIMEOUT_MS    15000     // openHAB sends ALIVE every 10 s
#define REQ_TIMEOUT_MS       8000
#if CONFIG_SPIRAM
#define RESP_MAX             (256 * 1024)
#define STATES_MSG_MAX       (64 * 1024)   // the snapshot after subscribing holds every item
#define REGISTRY_MSG_MAX     (16 * 1024)
#else
// No PSRAM: sized for small setups (a screen's item list was ~2 KB on 2026-11-22).
#define RESP_MAX             (16 * 1024)
#define STATES_MSG_MAX       (8 * 1024)
#define REGISTRY_MSG_MAX     (4 * 1024)
#endif
#define CMD_QUEUE_LEN        16
#define URL_LEN              (WP_URL_MAX + 320)

#define EV_RELOAD   BIT0    // registry changed, or wp_oh_reload()
#define EV_STREAM   BIT1    // the state stream has a new connection id

static const int s_backoff_ms[] = {1000, 2000, 5000, 10000};   // REQ-OH-5

typedef struct {
    bool rule;
    char name[WP_ITEM_NAME_MAX + 1];
    char value[WP_STATE_MAX + 1];
} cmd_t;

static SemaphoreHandle_t s_lock;          // s_model, s_st, s_stream_id, flags
static EventGroupHandle_t s_ev;
static QueueHandle_t s_cmdq;
static wp_oh_model_t s_empty;
static wp_oh_model_t *s_model = &s_empty;
static wp_oh_status_t s_st;
static char s_stream_id[64];
static bool s_model_ok;
static int64_t s_loaded_at;
static wp_oh_listener_t s_listener;
static void *s_listener_ctx;

static void notify(wp_oh_event_t ev, const char *item)
{
    wp_oh_listener_t cb = s_listener;
    if (cb) cb(ev, item, s_listener_ctx);
}

static bool online(void)
{
    if (!wp_config_get()->oh_url[0]) return false;
    wifi_mgr_status_t w;
    wifi_mgr_get_status(&w);
    return w.state == WIFI_MGR_CONNECTED;
}

// ---- status ----

// Call with s_lock held. Returns true when 'connected' changed.
static bool update_connected_locked(void)
{
    bool c = s_model_ok && s_st.stream_up;
    bool changed = c != s_st.connected;
    s_st.connected = c;
    if (c) s_st.last_error[0] = '\0';
    return changed;
}

static void set_flag(bool *flag, bool v)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *flag = v;
    bool changed = update_connected_locked();
    bool c = s_st.connected;
    xSemaphoreGive(s_lock);
    if (changed) {
        ESP_LOGI(TAG, "openHAB %s", c ? "connected" : "offline");
        notify(WP_OH_EV_CONNECTION, NULL);
    }
}

// Errors are logged once per distinct message: the warning log is persisted to flash.
static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_error(const char *fmt, ...)
{
    char msg[sizeof(s_st.last_error)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool fresh = strcmp(msg, s_st.last_error) != 0;
    strlcpy(s_st.last_error, msg, sizeof(s_st.last_error));
    xSemaphoreGive(s_lock);
    if (fresh) ESP_LOGW(TAG, "%s", msg);
}

// ---- HTTP ----

typedef struct {
    char *buf;
    int len, cap;
    bool overflow;
} body_t;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    body_t *b = e->user_data;
    if (b && e->event_id == HTTP_EVENT_ON_DATA) {
        if (b->len + e->data_len < b->cap) {
            memcpy(b->buf + b->len, e->data, e->data_len);
            b->len += e->data_len;
            b->buf[b->len] = '\0';
        } else {
            b->overflow = true;
        }
    }
    return ESP_OK;
}

static void add_auth(esp_http_client_handle_t c)
{
    const char *tok = wp_config_get()->oh_token;
    if (tok[0]) {
        char auth[WP_TOKEN_MAX + 8];
        snprintf(auth, sizeof(auth), "Bearer %s", tok);
        esp_http_client_set_header(c, "Authorization", auth);
    }
}

// Percent-encodes everything except [A-Za-z0-9_.~-] (rule UIDs may contain ':' etc.).
static void url_encode(char *out, size_t cap, const char *in)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = '\0';
}

// One request. Returns the HTTP status, or -1 when there was no response (err set).
static int http_request(esp_http_client_method_t method, const char *path, const char *ctype,
                        const char *body, body_t *out, esp_err_t *err)
{
    char url[URL_LEN];
    snprintf(url, sizeof(url), "%s%s", wp_config_get()->oh_url, path);
    if (out) {
        out->len = 0;
        out->overflow = false;
        out->buf[0] = '\0';
    }
    esp_http_client_config_t hc = {
        .url = url,
        .method = method,
        .timeout_ms = REQ_TIMEOUT_MS,
        .event_handler = on_http,
        .user_data = out,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) {
        *err = ESP_ERR_INVALID_ARG;
        return -1;
    }
    add_auth(c);
    esp_http_client_set_header(c, "Accept", "application/json");
    if (body) {
        esp_http_client_set_header(c, "Content-Type", ctype);
        esp_http_client_set_post_field(c, body, strlen(body));
    }
    *err = esp_http_client_perform(c);
    int status = *err == ESP_OK ? esp_http_client_get_status_code(c) : -1;
    esp_http_client_cleanup(c);
    if (out && out->overflow && status == 200) {
        *err = ESP_ERR_NO_MEM;
        return -1;
    }
    return status;
}

static const char *http_problem(int status, esp_err_t err, char *buf, size_t len)
{
    if (status < 0) {
        snprintf(buf, len, err == ESP_ERR_NO_MEM ? "response too large" : "no connection (%s)",
                 esp_err_to_name(err));
    } else if (status == 401 || status == 403) {
        snprintf(buf, len, "token rejected (HTTP %d)", status);
    } else {
        snprintf(buf, len, "HTTP %d", status);
    }
    return buf;
}

// ---- version check ----

static bool check_version(body_t *b)
{
    esp_err_t err;
    int status = http_request(HTTP_METHOD_GET, "/rest/", NULL, NULL, b, &err);
    char p[48];
    if (status != 200) {
        set_error("/rest/: %s", http_problem(status, err, p, sizeof(p)));
        return false;
    }
    cJSON *j = cJSON_Parse(b->buf);
    const cJSON *rv = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "runtimeInfo"), "version");
    bool ok = cJSON_IsString(rv);
    if (ok) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool fresh = strcmp(s_st.version, rv->valuestring) != 0;
        strlcpy(s_st.version, rv->valuestring, sizeof(s_st.version));
        xSemaphoreGive(s_lock);
        if (fresh) ESP_LOGI(TAG, "openHAB %s at %s", rv->valuestring, wp_config_get()->oh_url);
    } else {
        set_error("not an openHAB REST API");
    }
    cJSON_Delete(j);
    return ok;
}

// ---- screens ----

static void free_model(wp_oh_model_t *m)
{
    if (!m || m == &s_empty) return;
    for (int i = 0; i < m->screen_count; i++) heap_caps_free(m->screens[i].tiles);
    heap_caps_free(m->status.tiles);
    heap_caps_free(m);
}

// Grows a screen's tile block to room for `want` tiles (capped at WP_MAX_TILES). Blocks are
// sized from the response instead of always WP_MAX_TILES, so a reload (old and new model
// in memory at once) only needs small blocks: matters without PSRAM.
static bool reserve_tiles(wp_oh_screen_t *s, int *cap, int want)
{
    if (want > WP_MAX_TILES) want = WP_MAX_TILES;
    if (want < 1) want = 1;
    if (want <= *cap) return true;
    wp_tile_t *t = heap_caps_realloc(s->tiles, want * sizeof(wp_tile_t), WP_CAPS_BIG);
    if (!t) return false;
    memset(t + *cap, 0, (want - *cap) * sizeof(wp_tile_t));
    s->tiles = t;
    *cap = want;
    return true;
}

static bool load_screen(wp_oh_screen_t *s, const char *tag, const char *title, bool with_rules, body_t *b)
{
    strlcpy(s->tag, tag, sizeof(s->tag));
    strlcpy(s->title, title, sizeof(s->title));
    s->tiles = NULL;
    int cap = 0;
    char path[200], p[48];
    esp_err_t err;
    snprintf(path, sizeof(path),
             "/rest/items?tags=%s&metadata=wallpanel&recursive=false"
             "&fields=name,type,groupType,label,state,transformedState,tags,metadata,stateDescription", tag);
    int status = http_request(HTTP_METHOD_GET, path, NULL, NULL, b, &err);
    if (status != 200) {
        set_error("items of %s: %s", tag, http_problem(status, err, p, sizeof(p)));
        return false;
    }
    int avail = wp_model_count_array(b->buf);
    if (avail < 0) {
        set_error("items of %s: invalid JSON", tag);
        return false;
    }
    if (!reserve_tiles(s, &cap, avail)) {
        set_error("out of memory loading screen %s", tag);
        return false;
    }
    bool trunc = false, trunc2 = false;
    int n = wp_model_parse_items(b->buf, s->tiles, cap, &trunc);
    if (n < 0) {
        set_error("items of %s: invalid JSON", tag);
        return false;
    }
    if (with_rules) {
        snprintf(path, sizeof(path), "/rest/rules?tags=%s&summary=true", tag);
        status = http_request(HTTP_METHOD_GET, path, NULL, NULL, b, &err);
        if (status != 200) {
            set_error("rules of %s: %s", tag, http_problem(status, err, p, sizeof(p)));
            return false;
        }
        avail = wp_model_count_array(b->buf);
        if (avail < 0) {
            set_error("rules of %s: invalid JSON", tag);
            return false;
        }
        if (!reserve_tiles(s, &cap, n + avail)) {
            set_error("out of memory loading screen %s", tag);
            return false;
        }
        int r = wp_model_parse_rules(b->buf, s->tiles + n, cap - n, &trunc2);
        if (r < 0) {
            set_error("rules of %s: invalid JSON", tag);
            return false;
        }
        n += r;
    }
    s->count = n;
    s->truncated = trunc || trunc2;
    if (s->truncated) {
        ESP_LOGW(TAG, "screen %s has more than %d tiles; the rest are not shown", tag, WP_MAX_TILES);
    }
    wp_model_sort(s->tiles, n);
    // Keep only the tiles that were loaded (some items may not have become tiles).
    wp_tile_t *fit = heap_caps_realloc(s->tiles, (n ? n : 1) * sizeof(wp_tile_t), WP_CAPS_BIG);
    if (fit) s->tiles = fit;
    s->lights = 0;
    for (int i = 0; i < n; i++) s->lights += s->tiles[i].light;
    return true;
}

// Keeps openHAB's formatted displayState across a reload while the state is unchanged
// (the item list only has raw states).
static void carry_display(wp_oh_model_t *nm, const wp_oh_model_t *old)
{
    wp_oh_screen_t *ns[WP_MAX_SCREENS + 1];
    int nn = 0;
    for (int i = 0; i < nm->screen_count; i++) ns[nn++] = &nm->screens[i];
    ns[nn++] = &nm->status;
    const wp_oh_screen_t *os[WP_MAX_SCREENS + 1];
    int on = 0;
    for (int i = 0; i < old->screen_count; i++) os[on++] = &old->screens[i];
    os[on++] = &old->status;

    for (int a = 0; a < nn; a++) {
        for (int i = 0; i < ns[a]->count; i++) {
            wp_tile_t *t = &ns[a]->tiles[i];
            if (t->is_rule) continue;
            for (int b = 0; b < on; b++) {
                if (!os[b]->tiles) continue;
                for (int j = 0; j < os[b]->count; j++) {
                    const wp_tile_t *o = &os[b]->tiles[j];
                    if (!strcmp(o->name, t->name) && !strcmp(o->state, t->state)) {
                        strlcpy(t->display, o->display, sizeof(t->display));
                        goto next;
                    }
                }
            }
        next:;
        }
    }
}

static bool load_model(body_t *b)
{
    const wp_cfg_t *cfg = wp_config_get();
    wp_screen_t specs[WP_MAX_SCREENS];
    int n = wp_config_parse_screens(cfg->screens, specs, WP_MAX_SCREENS);
    if (n <= 0) {
        set_error("screen list is empty or malformed");
        return false;
    }
    wp_oh_model_t *nm = heap_caps_calloc(1, sizeof(*nm), WP_CAPS_BIG);
    if (!nm) {
        set_error("out of memory loading screens");
        return false;
    }
    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        nm->screen_count = i + 1;
        ok = load_screen(&nm->screens[i], specs[i].tag, specs[i].title, true, b);
    }
    if (ok && cfg->status_tag[0]) {
        ok = load_screen(&nm->status, cfg->status_tag, "Status", false, b);
    }
    if (!ok) {
        free_model(nm);
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    wp_oh_model_t *old = s_model;
    carry_display(nm, old);
    nm->version = old->version + 1;
    s_model = nm;
    s_model_ok = true;
    s_loaded_at = esp_timer_get_time();
    s_st.reloads++;
    bool changed = update_connected_locked();
    xSemaphoreGive(s_lock);
    free_model(old);

    int tiles = nm->status.count;
    for (int i = 0; i < nm->screen_count; i++) tiles += nm->screens[i].count;
    ESP_LOGI(TAG, "loaded %d screens, %d tiles (%d status)", nm->screen_count, tiles, nm->status.count);
    notify(WP_OH_EV_MODEL, NULL);
    if (changed) notify(WP_OH_EV_CONNECTION, NULL);
    return true;
}

// POST the item names of the current model to the state stream (REQ-OH-3 step 2).
static void subscribe(void)
{
    char id[sizeof(s_stream_id)];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(id, s_stream_id, sizeof(id));
    cJSON *arr = cJSON_CreateArray();
    const wp_oh_model_t *m = s_model;
    for (int i = 0; i <= m->screen_count; i++) {
        const wp_oh_screen_t *s = i < m->screen_count ? &m->screens[i] : &m->status;
        for (int j = 0; j < s->count; j++) {
            if (!s->tiles[j].is_rule) cJSON_AddItemToArray(arr, cJSON_CreateString(s->tiles[j].name));
        }
    }
    xSemaphoreGive(s_lock);
    if (!id[0]) {
        cJSON_Delete(arr);
        return;
    }
    int count = cJSON_GetArraySize(arr);
    char *body = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!body) return;
    char path[96], p[48];
    snprintf(path, sizeof(path), "/rest/events/states/%s", id);
    esp_err_t err;
    int status = http_request(HTTP_METHOD_POST, path, "application/json", body, NULL, &err);
    cJSON_free(body);
    if (status == 200) {
        ESP_LOGI(TAG, "subscribed to %d items", count);
    } else {
        set_error("subscribe: %s", http_problem(status, err, p, sizeof(p)));
    }
}

static void oh_task(void *arg)
{
    body_t b = {.buf = heap_caps_malloc(RESP_MAX, WP_CAPS_BIG), .cap = RESP_MAX};
    if (!b.buf) {
        ESP_LOGE(TAG, "no memory for responses");
        vTaskDelete(NULL);
        return;
    }
    int fails = 0;
    for (;;) {
        if (!online()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!check_version(&b) || !load_model(&b)) {
            int i = fails < 3 ? fails : 3;
            fails++;
            vTaskDelay(pdMS_TO_TICKS(s_backoff_ms[i]));
            continue;
        }
        fails = 0;
        // subscribe() reads the current stream id, so a pending EV_STREAM is covered by it.
        xEventGroupClearBits(s_ev, EV_RELOAD | EV_STREAM);
        subscribe();
        int64_t next_reload = esp_timer_get_time() + RELOAD_PERIOD_US;
        bool ok = true;
        while (ok && online()) {
            EventBits_t bits = xEventGroupWaitBits(s_ev, EV_RELOAD | EV_STREAM, pdTRUE, pdFALSE,
                                                   pdMS_TO_TICKS(1000));
            bool reload = (bits & EV_RELOAD) || esp_timer_get_time() >= next_reload;
            if (bits & EV_STREAM) {
                // A new stream after a drop means openHAB may have changed meanwhile
                // (REQ-SCR-5 "on reconnect"); right after a load, just subscribe.
                if (esp_timer_get_time() - s_loaded_at > 5LL * 1000 * 1000) reload = true;
                else subscribe();
            }
            if (reload) {
                if (bits & EV_RELOAD) {
                    vTaskDelay(pdMS_TO_TICKS(RELOAD_DEBOUNCE_MS));
                    xEventGroupClearBits(s_ev, EV_RELOAD);
                }
                ok = load_model(&b);
                if (ok) subscribe();
                next_reload = esp_timer_get_time() + RELOAD_PERIOD_US;
            }
        }
        // Lost WiFi or a reload failed: start over with the version check.
        set_flag(&s_model_ok, false);
    }
}

// ---- SSE streams ----

// Opens path as an event stream and feeds it to p until it drops. Returns true when the
// connection was established (HTTP 200).
static bool run_stream(const char *path, wp_sse_t *p, bool *up, const char *name)
{
    char url[URL_LEN];
    snprintf(url, sizeof(url), "%s%s", wp_config_get()->oh_url, path);
    esp_http_client_config_t hc = {
        .url = url,
        .timeout_ms = STREAM_TIMEOUT_MS,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) return false;
    add_auth(c);
    esp_http_client_set_header(c, "Accept", "text/event-stream");
    bool established = false;
    char pr[48];
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        set_error("%s stream: %s", name, http_problem(-1, err, pr, sizeof(pr)));
    } else {
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        if (status != 200) {
            set_error("%s stream: %s", name, http_problem(status, ESP_OK, pr, sizeof(pr)));
        } else {
            established = true;
            wp_sse_reset(p);
            if (up != &s_st.stream_up) set_flag(up, true);   // the state stream is "up" on its ready event
            // One byte per read: esp_http_client_read() only returns once it has filled the
            // whole buffer (or the read times out), so a larger read would sit on a short
            // event until enough later traffic arrived. SSE volume here is small.
            char ch;
            for (;;) {
                int r = esp_http_client_read(c, &ch, 1);
                if (r > 0) {
                    wp_sse_feed(p, &ch, 1);
                    continue;
                }
                if (r == -ESP_ERR_HTTP_EAGAIN) {
                    ESP_LOGW(TAG, "%s stream: nothing for %d s, reconnecting", name, STREAM_TIMEOUT_MS / 1000);
                } else {
                    ESP_LOGI(TAG, "%s stream closed", name);
                }
                break;
            }
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    set_flag(up, false);
    return established;
}

static void apply_state(const char *item, const char *state, const char *display, void *ctx)
{
    bool hit = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    wp_oh_model_t *m = s_model;
    for (int i = 0; i <= m->screen_count; i++) {
        wp_oh_screen_t *s = i < m->screen_count ? &m->screens[i] : &m->status;
        for (int j = 0; j < s->count; j++) {
            wp_tile_t *t = &s->tiles[j];
            if (t->is_rule || strcmp(t->name, item) != 0) continue;
            strlcpy(t->state, state, sizeof(t->state));
            strlcpy(t->display, display ? display : state, sizeof(t->display));
            hit = true;
        }
    }
    if (hit) s_st.updates++;
    xSemaphoreGive(s_lock);
    if (hit) notify(WP_OH_EV_STATE, item);
}

// openHAB names ordinary events "message" on /rest/events and sends no name on
// /rest/events/states; accept both.
static bool plain_event(const char *event)
{
    return !event[0] || !strcmp(event, "message");
}

static void on_states_msg(const char *event, const char *data, void *ctx)
{
    if (!strcmp(event, "ready")) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s_stream_id, data, sizeof(s_stream_id));
        xSemaphoreGive(s_lock);
        set_flag(&s_st.stream_up, true);
        xEventGroupSetBits(s_ev, EV_STREAM);
    } else if (plain_event(event) && data[0] == '{') {
        if (wp_model_parse_state_event(data, apply_state, NULL) < 0) {
            ESP_LOGW(TAG, "unreadable state event (%u bytes)", (unsigned)strlen(data));
        }
    }
    // "alive" only keeps the read timeout from expiring
}

static void on_registry_msg(const char *event, const char *data, void *ctx)
{
    // Item and rule added/updated/removed (metadata changes publish no event in openHAB 5.2;
    // they arrive with the 10-minute reload).
    if (plain_event(event) && (strstr(data, "\"type\":\"Item") || strstr(data, "\"type\":\"Rule"))) {
        xEventGroupSetBits(s_ev, EV_RELOAD);
    }
}

static void stream_task(void *arg)
{
    bool states = arg != NULL;
    const char *name = states ? "state" : "registry";
    const char *path = states ? "/rest/events/states"
                              : "/rest/events?topics=openhab/items/*/added,openhab/items/*/updated,openhab/items/*/removed,"
                                "openhab/rules/*/added,openhab/rules/*/updated,openhab/rules/*/removed";
    bool *up = states ? &s_st.stream_up : &s_st.registry_up;
    wp_sse_t p;
    if (!wp_sse_init(&p, states ? STATES_MSG_MAX : REGISTRY_MSG_MAX, states ? on_states_msg : on_registry_msg, NULL)) {
        ESP_LOGE(TAG, "no memory for the %s stream", name);
        vTaskDelete(NULL);
        return;
    }
    int fails = 0;
    for (;;) {
        if (!online()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        unsigned dropped = p.dropped;
        if (run_stream(path, &p, up, name)) fails = 0;
        if (states) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_stream_id[0] = '\0';
            xSemaphoreGive(s_lock);
        } else if (p.dropped != dropped) {
            xEventGroupSetBits(s_ev, EV_RELOAD);   // a registry event too big to parse is still a change
        }
        int i = fails < 3 ? fails : 3;
        fails++;
        vTaskDelay(pdMS_TO_TICKS(s_backoff_ms[i]));
    }
}

// ---- commands ----

static void cmd_task(void *arg)
{
    cmd_t c;
    for (;;) {
        if (xQueueReceive(s_cmdq, &c, portMAX_DELAY) != pdTRUE) continue;
        char enc[WP_ITEM_NAME_MAX * 3 + 1], path[WP_ITEM_NAME_MAX * 3 + 40], p[48];
        url_encode(enc, sizeof(enc), c.name);
        esp_err_t err;
        int status;
        if (c.rule) {
            snprintf(path, sizeof(path), "/rest/rules/%s/runnow", enc);
            status = http_request(HTTP_METHOD_POST, path, "application/json", "{}", NULL, &err);
        } else {
            snprintf(path, sizeof(path), "/rest/items/%s", enc);
            status = http_request(HTTP_METHOD_POST, path, "text/plain", c.value, NULL, &err);
        }
        bool ok = status >= 200 && status < 300;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (ok) {
            s_st.commands_ok++;
        } else {
            s_st.commands_failed++;
            snprintf(s_st.last_command_error, sizeof(s_st.last_command_error), "%.24s: %s", c.name,
                     http_problem(status, err, p, sizeof(p)));
        }
        xSemaphoreGive(s_lock);
        if (ok) {
            ESP_LOGI(TAG, "%s %s%s%s", c.rule ? "ran rule" : "sent", c.name, c.rule ? "" : " ", c.rule ? "" : c.value);
        } else {
            ESP_LOGW(TAG, "%s %s failed: %s", c.rule ? "rule" : "command to", c.name, http_problem(status, err, p, sizeof(p)));
        }
    }
}

static esp_err_t enqueue(bool rule, const char *name, const char *value)
{
    if (!s_cmdq) return ESP_ERR_INVALID_STATE;
    if (!name || !name[0] || strlen(name) > WP_ITEM_NAME_MAX) return ESP_ERR_INVALID_ARG;
    if (!rule && (!value || !value[0] || strlen(value) > WP_STATE_MAX)) return ESP_ERR_INVALID_ARG;
    cmd_t c = {.rule = rule};
    strlcpy(c.name, name, sizeof(c.name));
    strlcpy(c.value, value ? value : "", sizeof(c.value));
    return xQueueSend(s_cmdq, &c, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t wp_oh_command(const char *item, const char *command)
{
    return enqueue(false, item, command);
}

esp_err_t wp_oh_run_rule(const char *uid)
{
    return enqueue(true, uid, NULL);
}

// ---- public ----

void wp_oh_reload(void)
{
    if (s_ev) xEventGroupSetBits(s_ev, EV_RELOAD);
}

void wp_oh_set_listener(wp_oh_listener_t cb, void *ctx)
{
    s_listener_ctx = ctx;
    s_listener = cb;
}

const wp_oh_model_t *wp_oh_model_lock(void)
{
    if (!s_lock) return &s_empty;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    return s_model;
}

void wp_oh_model_unlock(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}

void wp_oh_get_status(wp_oh_status_t *out)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    if (s_lock) xSemaphoreGive(s_lock);
    if (!wp_config_get()->oh_url[0]) out->state = "not configured";
    else if (out->connected) out->state = "connected";
    else if (out->last_error[0]) out->state = "error";
    else out->state = "connecting";
}

esp_err_t wp_oh_start(void)
{
    s_st.state = "not configured";
    // Every dropped connection (e.g. a WiFi roam) made the HTTP client log 3-6 errors,
    // and the warning log is saved to flash. This client reports problems itself, once.
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_NONE);
    esp_log_level_set("transport_base", ESP_LOG_NONE);
    esp_log_level_set("esp-tls", ESP_LOG_NONE);
    s_lock = xSemaphoreCreateMutex();
    s_ev = xEventGroupCreate();
    s_cmdq = xQueueCreateWithCaps(CMD_QUEUE_LEN, sizeof(cmd_t), WP_CAPS_BIG);
    if (!s_lock || !s_ev || !s_cmdq) return ESP_ERR_NO_MEM;
    // PSRAM stacks: internal RAM is scarce, and these tasks never write flash.
    bool ok = xTaskCreateWithCaps(oh_task, "oh", 6144, NULL, 4, NULL, WP_CAPS_BIG) == pdPASS &&
              xTaskCreateWithCaps(stream_task, "oh_sse", 6144, (void *)1, 5, NULL, WP_CAPS_BIG) == pdPASS &&
              xTaskCreateWithCaps(stream_task, "oh_reg", 6144, NULL, 3, NULL, WP_CAPS_BIG) == pdPASS &&
              xTaskCreateWithCaps(cmd_task, "oh_cmd", 5120, NULL, 5, NULL, WP_CAPS_BIG) == pdPASS;
    return ok ? ESP_OK : ESP_FAIL;
}
