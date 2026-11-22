// oh-wallpanel: openHAB wall panel. See docs/FSD.md for the requirements.
#include <stdbool.h>
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "console.h"
#include "time_sync.h"
#include "ui.h"
#include "webui.h"
#include "wifi_mgr.h"
#include "wp_config.h"
#include "wp_crash.h"
#include "wp_log.h"
#include "wp_oh.h"

static const char *TAG = "main";

#define OTA_CONFIRM_TIMEOUT_S 60   // REQ-OTA-1

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was full or from a newer format, erasing it");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

// A freshly OTA-updated image runs in "pending verify" state. Confirm it once the panel is
// reachable (WiFi or AP up and web server running); otherwise roll back.
static void confirm_ota_image(bool web_ok)
{
    esp_ota_img_states_t st;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    ESP_LOGI(TAG, "new firmware: waiting up to %ds for WiFi before confirming it", OTA_CONFIRM_TIMEOUT_S);
    for (int i = 0; i < OTA_CONFIRM_TIMEOUT_S * 10 && !wifi_mgr_is_up(); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (web_ok && wifi_mgr_is_up()) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed");
    } else {
        ESP_LOGE(TAG, "new firmware failed its self-check, rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

void app_main(void)
{
    wp_log_init();
    // Console first: it switches stdout to the USB-Serial/JTAG driver, which raced with
    // other tasks logging and hung the panel when done later (2026-11-22).
    console_start();
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "oh-wallpanel %s starting (slot %s)", app->version, esp_ota_get_running_partition()->label);

    init_nvs();
    wp_log_persist_start();
    wp_crash_init();
    ESP_ERROR_CHECK(wp_config_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Display first: its DMA/draw buffers need internal RAM before WiFi takes its share.
    if (ui_start() == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));   // first frame drawn, then light it
        ui_backlight_on();
    } else {
        ESP_LOGE(TAG, "display failed to start; the web UI still works");
    }
    ESP_ERROR_CHECK(wifi_mgr_start());
    bool web_ok = webui_start() == ESP_OK;
    time_sync_start();
    if (wp_oh_start() != ESP_OK) {
        ESP_LOGE(TAG, "openHAB client failed to start");
    }
    confirm_ota_image(web_ok);
}
