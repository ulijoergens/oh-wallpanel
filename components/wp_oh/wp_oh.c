// openHAB client, see wp_oh.h.
#include "wp_oh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_mgr.h"
#include "wp_config.h"

static const char *TAG = "oh";

#define POLL_OK_MS    60000
#define POLL_ERR_MS   10000
#define RESP_MAX      4096

static SemaphoreHandle_t s_lock;
static wp_oh_status_t s_st = {.state = "not configured"};

static void set_state(const char *state, const char *version, const char *err)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = strcmp(s_st.state, state) != 0;
    s_st.state = state;
    s_st.connected = strcmp(state, "connected") == 0;
    if (version) strlcpy(s_st.version, version, sizeof(s_st.version));
    strlcpy(s_st.last_error, err ? err : "", sizeof(s_st.last_error));
    xSemaphoreGive(s_lock);
    if (changed) {
        if (err) ESP_LOGW(TAG, "openHAB %s: %s", state, err);
        else ESP_LOGI(TAG, "openHAB %s%s%s", state, version ? " " : "", version ? version : "");
    }
}

typedef struct {
    char *buf;
    int len;
} resp_t;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    resp_t *r = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && r->len + e->data_len < RESP_MAX) {
        memcpy(r->buf + r->len, e->data, e->data_len);
        r->len += e->data_len;
        r->buf[r->len] = '\0';
    }
    return ESP_OK;
}

// One connection check. Returns true when openHAB answered with its version.
static bool check(const wp_cfg_t *cfg)
{
    char url[WP_URL_MAX + 8];
    snprintf(url, sizeof(url), "%s/rest/", cfg->oh_url);
    resp_t r = {.buf = malloc(RESP_MAX), .len = 0};
    if (!r.buf) return false;
    r.buf[0] = '\0';
    esp_http_client_config_t hc = {
        .url = url,
        .timeout_ms = 5000,
        .event_handler = on_http,
        .user_data = &r,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) {
        free(r.buf);
        set_state("error", NULL, "invalid URL");
        return false;
    }
    char auth[WP_TOKEN_MAX + 8];
    if (cfg->oh_token[0]) {
        snprintf(auth, sizeof(auth), "Bearer %s", cfg->oh_token);
        esp_http_client_set_header(c, "Authorization", auth);
    }
    esp_http_client_set_header(c, "Accept", "application/json");
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    bool ok = false;
    char msg[64];
    if (err != ESP_OK) {
        snprintf(msg, sizeof(msg), "no connection (%s)", esp_err_to_name(err));
        set_state("error", NULL, msg);
    } else if (status == 401 || status == 403) {
        set_state("error", NULL, "token rejected (HTTP 401/403)");
    } else if (status != 200) {
        snprintf(msg, sizeof(msg), "HTTP %d from /rest/", status);
        set_state("error", NULL, msg);
    } else {
        cJSON *j = cJSON_Parse(r.buf);
        cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "version");
        cJSON *rt = cJSON_GetObjectItemCaseSensitive(j, "runtimeInfo");
        cJSON *rv = cJSON_GetObjectItemCaseSensitive(rt, "version");
        if (cJSON_IsString(rv) || cJSON_IsString(v)) {
            set_state("connected", cJSON_IsString(rv) ? rv->valuestring : v->valuestring, NULL);
            ok = true;
        } else {
            set_state("error", NULL, "not an openHAB REST API");
        }
        cJSON_Delete(j);
    }
    free(r.buf);
    return ok;
}

static void oh_task(void *arg)
{
    const wp_cfg_t *cfg = wp_config_get();
    for (;;) {
        if (!wifi_mgr_is_up() || !cfg->oh_url[0]) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        wifi_mgr_status_t w;
        wifi_mgr_get_status(&w);
        if (w.state != WIFI_MGR_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        bool ok = check(cfg);
        vTaskDelay(pdMS_TO_TICKS(ok ? POLL_OK_MS : POLL_ERR_MS));
    }
}

esp_err_t wp_oh_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (wp_config_get()->oh_url[0]) {
        s_st.state = "connecting";
    }
    return xTaskCreate(oh_task, "oh", 6144, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_FAIL;
}

void wp_oh_get_status(wp_oh_status_t *out)
{
    if (!s_lock) {
        *out = s_st;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_lock);
}
