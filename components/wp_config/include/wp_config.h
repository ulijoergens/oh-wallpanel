// Panel settings (docs/FSD.md REQ-NET-1, REQ-OH-1, REQ-SCR-1, REQ-STAT-2, REQ-DISP-1/2).
//
// Stored in NVS (namespace "wp") as one versioned blob. wp_config_validate() and the
// parsing helpers are pure functions so they can be unit-tested.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WP_NAME_MAX     32
#define WP_SSID_MAX     32
#define WP_PASS_MAX     64
#define WP_URL_MAX      96
#define WP_TOKEN_MAX    160
#define WP_SCREENS_MAX  255
#define WP_TAG_MAX      32
#define WP_TZ_MAX       48
#define WP_MAX_SCREENS  8

typedef struct {
    char name[WP_NAME_MAX + 1];          // panel name, also the hostname
    char wifi_ssid[WP_SSID_MAX + 1];     // empty: start in AP mode
    char wifi_pass[WP_PASS_MAX + 1];     // empty (open network) or 8..64 chars
    char oh_url[WP_URL_MAX + 1];         // http://host:port, no trailing slash; empty: not configured
    char oh_token[WP_TOKEN_MAX + 1];     // openHAB API token; write-only in the UI
    char screens[WP_SCREENS_MAX + 1];    // "tag[=Title], ..." the first is the home screen
    char status_tag[WP_TAG_MAX + 1];     // REQ-STAT-2, default wp_status
    char tz[WP_TZ_MAX + 1];              // POSIX TZ string, default Europe/Paris
    uint16_t home_timeout_s;             // REQ-SCR-7, 0 = off
    uint16_t dark_timeout_s;             // REQ-DISP-1, 0 = off
    bool night_mode;                     // REQ-DISP-2
    uint16_t night_start_min;            // minutes after midnight, default 23:00
    uint16_t night_end_min;              // default 06:00
    // New fields go at the end: an older, shorter stored record still loads (the rest
    // keeps its defaults).
    uint16_t bl_ramp_ms;                 // backlight soft start/stop, 0 = switch hard
    bool compact;                        // REQ-SCR-8: shrink tiles so each screen fits without scrolling
} wp_cfg_t;

typedef struct {
    char tag[WP_TAG_MAX + 1];
    char title[WP_NAME_MAX + 1];         // defaults to the tag
} wp_screen_t;

void wp_config_defaults(wp_cfg_t *cfg, const uint8_t mac[6]);

// NULL if valid, otherwise a short English error message naming the field.
const char *wp_config_validate(const wp_cfg_t *cfg);

// Parses cfg->screens. Returns the number of screens (0..max), or -1 if malformed.
int wp_config_parse_screens(const char *spec, wp_screen_t *out, int max);

bool wp_tag_valid(const char *tag);      // 1..32 chars of [A-Za-z0-9_-]

esp_err_t wp_config_init(void);          // after nvs_flash_init()
const wp_cfg_t *wp_config_get(void);
esp_err_t wp_config_save(const wp_cfg_t *cfg);   // ESP_ERR_INVALID_ARG if invalid
esp_err_t wp_config_factory_reset(void);         // takes effect after a reboot

#ifdef __cplusplus
}
#endif
