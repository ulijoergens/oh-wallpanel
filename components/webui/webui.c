// Web UI server, see webui.h.
#include "webui.h"
#include "webui_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "esp_heap_caps.h"
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "wp_captive.h"
#include "wp_config.h"
#include "wp_log.h"
#include "wp_oh.h"
#include "wifi_mgr.h"

static const char *TAG = "web";

#define MAX_BODY 1024

// ---------------------------------------------------------------- static files

#define ASSET(name)                                                        \
    extern const char name##_start[] asm("_binary_" #name "_start");       \
    extern const char name##_end[] asm("_binary_" #name "_end")

ASSET(index_html);
ASSET(app_js);
ASSET(style_css);

typedef struct {
    const char *start;
    const char *end;
    const char *type;
} asset_t;

static esp_err_t send_asset(httpd_req_t *req)
{
    if (wp_captive_redirect(req)) {   // AP mode, request for a foreign host name
        return ESP_OK;
    }
    const asset_t *a = req->user_ctx;
    httpd_resp_set_type(req, a->type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    // EMBED_TXTFILES appends a NUL terminator; don't send it.
    return httpd_resp_send(req, a->start, a->end - a->start - 1);
}

static const asset_t s_index = {index_html_start, index_html_end, "text/html; charset=utf-8"};
static const asset_t s_app = {app_js_start, app_js_end, "application/javascript"};
static const asset_t s_css = {style_css_start, style_css_end, "text/css"};

// ---------------------------------------------------------------- helpers

esp_err_t webui_send_json(httpd_req_t *req, cJSON *root)
{
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, txt);
    free(txt);
    return err;
}

esp_err_t webui_send_error(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", msg);
    httpd_resp_set_status(req, status);
    return webui_send_json(req, o);
}

static void reboot_cb(void *arg)
{
    esp_restart();
}

static void reboot_soon(void)
{
    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t a = {.callback = reboot_cb, .name = "reboot"};
        esp_timer_create(&a, &t);
    }
    esp_timer_start_once(t, 1500 * 1000);   // let the HTTP response go out first
}

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_EXT: return "reset pin";
    default: return "other";
    }
}

// ---------------------------------------------------------------- API handlers

static esp_err_t api_status(httpd_req_t *req)
{
    const wp_cfg_t *c = wp_config_get();
    const esp_app_desc_t *app = esp_app_get_description();
    wifi_mgr_status_t w;
    wifi_mgr_get_status(&w);

    cJSON *root = cJSON_CreateObject();
    cJSON *d = cJSON_AddObjectToObject(root, "device");
    cJSON_AddStringToObject(d, "name", c->name);
    cJSON_AddStringToObject(d, "firmware", app->version);
    cJSON_AddStringToObject(d, "idf", app->idf_ver);
    cJSON_AddStringToObject(d, "partition", esp_ota_get_running_partition()->label);
    cJSON_AddNumberToObject(d, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(d, "heap_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(d, "heap_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(d, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddStringToObject(d, "reset_reason", reset_reason());
    time_t now = time(NULL);
    if (now > 1700000000) {
        cJSON_AddNumberToObject(d, "time", (double)now);
    }

    cJSON *wj = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wj, "state", wifi_mgr_state_name(w.state));
    cJSON_AddStringToObject(wj, "ssid", w.ssid);
    cJSON_AddStringToObject(wj, "ip", w.ip);
    if (w.state == WIFI_MGR_CONNECTED) {
        cJSON_AddNumberToObject(wj, "rssi", w.rssi);
    }
    cJSON_AddBoolToObject(wj, "ap_active", w.ap_active);
    cJSON_AddStringToObject(wj, "ap_ssid", w.ap_ssid);
    cJSON_AddStringToObject(wj, "ap_ip", w.ap_ip);
    cJSON_AddBoolToObject(wj, "captive_portal", wp_captive_active());
    cJSON_AddNumberToObject(wj, "lost_for_s", w.lost_for_s);
    cJSON_AddNumberToObject(wj, "next_retry_s", w.next_retry_s);
    cJSON_AddNumberToObject(wj, "attempts", w.attempts);

    wp_oh_status_t os;
    wp_oh_get_status(&os);
    cJSON *oh = cJSON_AddObjectToObject(root, "openhab");
    cJSON_AddStringToObject(oh, "state", os.state);
    cJSON_AddStringToObject(oh, "url", c->oh_url);
    cJSON_AddStringToObject(oh, "version", os.version);
    cJSON_AddStringToObject(oh, "last_error", os.last_error);

    cJSON *l = cJSON_AddObjectToObject(root, "log");
    cJSON_AddNumberToObject(l, "count", wp_log_count());
    return webui_send_json(req, root);
}

static void hhmm(char *out, size_t len, uint16_t min)
{
    snprintf(out, len, "%02u:%02u", min / 60, min % 60);
}

static esp_err_t api_settings_get(httpd_req_t *req)
{
    const wp_cfg_t *c = wp_config_get();
    char t[8];
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", c->name);
    cJSON_AddStringToObject(o, "wifi_ssid", c->wifi_ssid);
    cJSON_AddBoolToObject(o, "wifi_pass_set", c->wifi_pass[0] != '\0');
    cJSON_AddStringToObject(o, "oh_url", c->oh_url);
    cJSON_AddBoolToObject(o, "oh_token_set", c->oh_token[0] != '\0');   // REQ-OH-1: never shown again
    cJSON_AddStringToObject(o, "screens", c->screens);
    cJSON_AddStringToObject(o, "status_tag", c->status_tag);
    cJSON_AddStringToObject(o, "tz", c->tz);
    cJSON_AddNumberToObject(o, "home_timeout_s", c->home_timeout_s);
    cJSON_AddNumberToObject(o, "dark_timeout_s", c->dark_timeout_s);
    cJSON_AddBoolToObject(o, "night_mode", c->night_mode);
    hhmm(t, sizeof(t), c->night_start_min);
    cJSON_AddStringToObject(o, "night_start", t);
    hhmm(t, sizeof(t), c->night_end_min);
    cJSON_AddStringToObject(o, "night_end", t);
    return webui_send_json(req, o);
}

// Copies a string field if present. Returns an error message or NULL.
static const char *take_str(cJSON *o, const char *key, char *dst, size_t len, const char *label)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!v) {
        return NULL;
    }
    if (!cJSON_IsString(v)) {
        return label;
    }
    if (strlen(v->valuestring) >= len) {
        return label;
    }
    strcpy(dst, v->valuestring);
    return NULL;
}

static const char *take_int(cJSON *o, const char *key, int min, int max, int *dst, const char *label)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!v) {
        return NULL;
    }
    if (!cJSON_IsNumber(v) || v->valuedouble < min || v->valuedouble > max ||
        v->valuedouble != (int)v->valuedouble) {
        return label;
    }
    *dst = (int)v->valuedouble;
    return NULL;
}

cJSON *webui_read_json(httpd_req_t *req, const char **why)
{
    *why = NULL;
    if (req->content_len <= 0 || req->content_len > MAX_BODY) {
        *why = "Request body missing or too large";
        return NULL;
    }
    char *body = malloc(req->content_len + 1);
    if (!body) {
        *why = "Out of memory";
        return NULL;
    }
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            free(body);
            *why = "Request body incomplete";
            return NULL;
        }
        got += r;
    }
    body[got] = '\0';
    cJSON *in = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsObject(in)) {
        cJSON_Delete(in);
        *why = "Invalid JSON";
        return NULL;
    }
    return in;
}

// "HH:MM" -> minutes after midnight
static const char *take_time(cJSON *o, const char *key, uint16_t *dst, const char *label)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!v) {
        return NULL;
    }
    unsigned h, m;
    char extra;
    if (!cJSON_IsString(v) || sscanf(v->valuestring, "%u:%u%c", &h, &m, &extra) != 2 || h > 23 || m > 59) {
        return label;
    }
    *dst = (uint16_t)(h * 60 + m);
    return NULL;
}

static esp_err_t api_settings_post(httpd_req_t *req)
{
    const char *why;
    cJSON *in = webui_read_json(req, &why);
    if (!in) {
        return webui_send_error(req, "400 Bad Request", why);
    }

    wp_cfg_t c = *wp_config_get();
    const char *bad = NULL;
    int home = c.home_timeout_s, dark = c.dark_timeout_s;
    char wifi_pass[WP_PASS_MAX + 1] = "", token[WP_TOKEN_MAX + 1] = "";

    if (!bad) bad = take_str(in, "name", c.name, sizeof(c.name), "Panel name: 1 to 32 printable characters");
    if (!bad) bad = take_str(in, "wifi_ssid", c.wifi_ssid, sizeof(c.wifi_ssid), "WiFi SSID: at most 32 characters");
    if (!bad) bad = take_str(in, "wifi_pass", wifi_pass, sizeof(wifi_pass), "WiFi password: at most 64 characters");
    if (!bad) bad = take_str(in, "oh_url", c.oh_url, sizeof(c.oh_url), "openHAB URL: at most 96 characters");
    if (!bad) bad = take_str(in, "oh_token", token, sizeof(token), "openHAB token: at most 160 characters");
    if (!bad) bad = take_str(in, "screens", c.screens, sizeof(c.screens), "Screens: at most 255 characters");
    if (!bad) bad = take_str(in, "status_tag", c.status_tag, sizeof(c.status_tag), "Status tag: at most 32 characters");
    if (!bad) bad = take_str(in, "tz", c.tz, sizeof(c.tz), "Time zone: at most 48 characters");
    if (!bad) bad = take_int(in, "home_timeout_s", 0, 3600, &home, "Return to home: 0 to 3600 s");
    if (!bad) bad = take_int(in, "dark_timeout_s", 0, 3600, &dark, "Screen off: 0 to 3600 s");
    if (!bad) bad = take_time(in, "night_start", &c.night_start_min, "Night start: HH:MM");
    if (!bad) bad = take_time(in, "night_end", &c.night_end_min, "Night end: HH:MM");
    cJSON *nm = cJSON_GetObjectItemCaseSensitive(in, "night_mode");
    if (!bad && nm) {
        if (!cJSON_IsBool(nm)) {
            bad = "Night mode: true or false";
        } else {
            c.night_mode = cJSON_IsTrue(nm);
        }
    }
    // Secrets are write-only: a non-empty value replaces the stored one; "*_clear" empties it.
    if (wifi_pass[0]) {
        strcpy(c.wifi_pass, wifi_pass);
    } else if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(in, "wifi_pass_clear"))) {
        c.wifi_pass[0] = '\0';
    }
    if (token[0]) {
        strcpy(c.oh_token, token);
    } else if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(in, "oh_token_clear"))) {
        c.oh_token[0] = '\0';
    }
    cJSON_Delete(in);
    if (bad) {
        return webui_send_error(req, "400 Bad Request", bad);
    }
    c.home_timeout_s = (uint16_t)home;
    c.dark_timeout_s = (uint16_t)dark;

    why = wp_config_validate(&c);
    if (why) {
        return webui_send_error(req, "400 Bad Request", why);
    }
    if (wp_config_save(&c) != ESP_OK) {
        return webui_send_error(req, "500 Internal Server Error", "Saving failed");
    }
    ESP_LOGI(TAG, "settings changed from the web UI, rebooting");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "reboot", true);
    esp_err_t err = webui_send_json(req, o);
    reboot_soon();
    return err;
}

static esp_err_t api_log(httpd_req_t *req)
{
    wp_log_entry_t *e = malloc(sizeof(wp_log_entry_t) * WP_LOG_ENTRIES);
    if (!e) {
        return webui_send_error(req, "500 Internal Server Error", "Out of memory");
    }
    int n = wp_log_get(e, WP_LOG_ENTRIES);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    cJSON_AddNumberToObject(root, "count", wp_log_count());
    cJSON *arr = cJSON_AddArrayToObject(root, "entries");
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddNumberToObject(it, "uptime_ms", (double)e[i].uptime_ms);
        if (e[i].epoch_s) {
            cJSON_AddNumberToObject(it, "time", (double)e[i].epoch_s);
        }
        char lvl[2] = {e[i].level, 0};
        cJSON_AddStringToObject(it, "level", lvl);
        cJSON_AddStringToObject(it, "text", e[i].text);
        cJSON_AddItemToArray(arr, it);
    }
    free(e);

    // Stored entries from earlier boots (REQ-LOG-1)
    cJSON_AddNumberToObject(root, "boot", wp_log_boot());
    wp_log_persisted_t *p = malloc(sizeof(wp_log_persisted_t) * WP_LOG_PERSIST);
    cJSON *prev = cJSON_AddArrayToObject(root, "previous");
    if (p) {
        int m = wp_log_get_previous(p, WP_LOG_PERSIST);
        for (int i = 0; i < m; i++) {
            cJSON *it = cJSON_CreateObject();
            cJSON_AddNumberToObject(it, "boot", p[i].boot);
            cJSON_AddNumberToObject(it, "uptime_s", p[i].uptime_s);
            if (p[i].epoch_s) {
                cJSON_AddNumberToObject(it, "time", p[i].epoch_s);
            }
            char lvl[2] = {p[i].level, 0};
            cJSON_AddStringToObject(it, "level", lvl);
            cJSON_AddStringToObject(it, "text", p[i].text);
            cJSON_AddItemToArray(prev, it);
        }
        free(p);
    }
    return webui_send_json(req, root);
}

static esp_err_t api_log_delete(httpd_req_t *req)
{
    wp_log_clear_persisted();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return webui_send_json(req, o);
}

static esp_err_t api_wifi_scan(httpd_req_t *req)
{
    wifi_mgr_net_t *nets = malloc(sizeof(wifi_mgr_net_t) * 30);
    if (!nets) {
        return webui_send_error(req, "500 Internal Server Error", "Out of memory");
    }
    int n = wifi_mgr_scan(nets, 30);
    if (n < 0) {
        free(nets);
        return webui_send_error(req, "503 Service Unavailable", "Scanning is not possible right now (WiFi is connecting); try again in a few seconds");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "networks");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", nets[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", nets[i].rssi);
        cJSON_AddNumberToObject(o, "channel", nets[i].channel);
        cJSON_AddBoolToObject(o, "secured", nets[i].secured);
        cJSON_AddItemToArray(arr, o);
    }
    free(nets);
    return webui_send_json(req, root);
}

static esp_err_t api_reboot(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    esp_err_t err = webui_send_json(req, o);
    reboot_soon();
    return err;
}

// ---------------------------------------------------------------- server

static int s_handlers;

void webui_register(httpd_handle_t srv, const httpd_uri_t *uri)
{
    esp_err_t err = httpd_register_uri_handler(srv, uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot register %s %s: %s", http_method_str(uri->method), uri->uri, esp_err_to_name(err));
    } else {
        s_handlers++;
    }
}

// Unknown paths: in AP mode, connectivity checks (/generate_204, /hotspot-detect.html, ...)
// are sent to the settings page; otherwise a normal 404.
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    if (wp_captive_redirect(req)) {
        return ESP_OK;
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
}

esp_err_t webui_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 32;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 15;   // firmware uploads over WiFi can stall briefly

    httpd_handle_t srv = NULL;
    esp_err_t err = httpd_start(&srv, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = send_asset, .user_ctx = (void *)&s_index},
        {.uri = "/index.html", .method = HTTP_GET, .handler = send_asset, .user_ctx = (void *)&s_index},
        {.uri = "/app.js", .method = HTTP_GET, .handler = send_asset, .user_ctx = (void *)&s_app},
        {.uri = "/style.css", .method = HTTP_GET, .handler = send_asset, .user_ctx = (void *)&s_css},
        {.uri = "/api/status", .method = HTTP_GET, .handler = api_status},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = api_settings_get},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = api_settings_post},
        {.uri = "/api/log", .method = HTTP_GET, .handler = api_log},
        {.uri = "/api/log", .method = HTTP_DELETE, .handler = api_log_delete},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = api_reboot},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = api_wifi_scan},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        webui_register(srv, &uris[i]);
    }
    webui_register_crash(srv);
    webui_register_ota(srv);
    httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, not_found);
    ESP_LOGI(TAG, "web UI running on port %d (%d of %d URI handlers used)", cfg.server_port, s_handlers,
             cfg.max_uri_handlers);
    return ESP_OK;
}
