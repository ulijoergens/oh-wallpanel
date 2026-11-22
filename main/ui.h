#pragma once
#include "esp_err.h"
// LCD user interface. Milestone 2: status bar plus a setup/info screen (REQ-NET-2).
esp_err_t ui_start(void);     // display and touch; the backlight stays off
void ui_backlight_on(void);
