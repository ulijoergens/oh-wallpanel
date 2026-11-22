#pragma once
#include <stdbool.h>
#include "esp_err.h"
// LCD user interface: status bar, setup/info screen (REQ-NET-2) and the tile screens (ui_tiles.c).
esp_err_t ui_start(void);     // display and touch; the backlight stays off
void ui_backlight_on(void);
void ui_dump_framebuffer(void);   // console 'fbdump' (developer aid, see tools/fbdump.py)
void ui_show_tab(int tab, int scroll_y);   // console 'tab' (developer aid)
void ui_tap(int x, int y);   // console 'tap' (developer aid)
void ui_touchlog(bool on);   // console 'touchlog' (developer aid)
void ui_touchtest(bool on);  // console 'touchtest' (developer aid)
void ui_dump_gt911(void);     // console 'gt911' (developer aid)
