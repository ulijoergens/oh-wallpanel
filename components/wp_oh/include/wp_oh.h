// openHAB client (docs/FSD.md §4). Milestone 2: connection check only.
//
// While WiFi is up and a URL is configured, GET <url>/rest/ (with the API token) is
// polled; it reports the openHAB version and proves URL and token work.
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *state;        // "not configured", "connecting", "connected", "error"
    char version[16];         // openHAB runtime version once connected
    char last_error[64];
    bool connected;
} wp_oh_status_t;

esp_err_t wp_oh_start(void);
void wp_oh_get_status(wp_oh_status_t *out);

#ifdef __cplusplus
}
#endif
