// openHAB client (docs/FSD.md §3, §4).
//
// While WiFi is up and a URL is configured it:
// - checks GET /rest/ (version, URL and token);
// - loads the screens: items and rules with each screen tag, plus the status items
//   (REQ-SCR-2/3, REQ-STAT-2), at start, on reconnect, every 10 minutes and when the item
//   registry changes (REQ-SCR-5);
// - keeps states live through the SSE state subscription (REQ-OH-3);
// - sends item commands and runs rules from a queue (REQ-OH-4).
// Streams reconnect with backoff 1, 2, 5, 10 s (REQ-OH-5).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "wp_config.h"
#include "wp_model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *state;        // "not configured", "connecting", "connected", "error"
    char version[16];         // openHAB runtime version once connected
    char last_error[64];
    bool connected;           // screens loaded and the state stream is up
    bool stream_up;           // SSE state subscription
    bool registry_up;         // SSE registry-change stream
    uint32_t reloads;         // successful screen loads since boot
    uint32_t updates;         // item state updates received
    uint32_t commands_ok, commands_failed;
    char last_command_error[64];
} wp_oh_status_t;

typedef struct {
    char tag[WP_TAG_MAX + 1];
    char title[WP_NAME_MAX + 1];
    int count;
    int lights;               // REQ-TILE-4: "All off" is shown when >= 2
    bool truncated;           // more than WP_MAX_TILES tiles had the tag
    wp_tile_t *tiles;
} wp_oh_screen_t;

typedef struct {
    int screen_count;
    wp_oh_screen_t screens[WP_MAX_SCREENS];
    wp_oh_screen_t status;    // items with the status tag (REQ-STAT-2)
    uint32_t version;         // changes on every reload
} wp_oh_model_t;

typedef enum {
    WP_OH_EV_MODEL,           // screens reloaded: rebuild everything
    WP_OH_EV_STATE,           // one item changed state (item = its name)
    WP_OH_EV_CONNECTION,      // connected / disconnected
} wp_oh_event_t;

// Called from the client's tasks, without the model lock held. Keep it short.
typedef void (*wp_oh_listener_t)(wp_oh_event_t ev, const char *item, void *ctx);

esp_err_t wp_oh_start(void);
void wp_oh_get_status(wp_oh_status_t *out);
void wp_oh_set_listener(wp_oh_listener_t cb, void *ctx);

// The current model, never NULL. Hold the lock briefly and do not call wp_oh_* inside it.
const wp_oh_model_t *wp_oh_model_lock(void);
void wp_oh_model_unlock(void);

// Queued; the result shows up in the status counters and the log. ESP_ERR_NO_MEM when
// the queue is full, ESP_ERR_INVALID_ARG for empty or too-long arguments.
esp_err_t wp_oh_command(const char *item, const char *command);
esp_err_t wp_oh_run_rule(const char *uid);

void wp_oh_reload(void);      // reload the screens now

#ifdef __cplusplus
}
#endif
