// Tile screens built from the openHAB model (docs/FSD.md §3, milestone 4).
// All functions run in the LVGL task (timer callbacks) or under lvgl_port_lock().
#pragma once

#include <stdbool.h>
#include "lvgl.h"

// Creates the tab view inside parent (the area below the status bar) and the status
// items inside status_box (a flex row in the status bar).
void tiles_init(lv_obj_t *parent, lv_obj_t *status_box);

// Called every 100 ms: rebuilds after a model reload, syncs states, handles pending
// timeouts, the offline overlay and the return to the home screen (REQ-SCR-7).
void tiles_tick(void);

bool tiles_ready(void);           // a model has been loaded and the tiles exist

void tiles_show(int tab, int scroll_y);   // developer aid (console 'tab')
