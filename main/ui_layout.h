// Screen geometry and fonts, derived from the board's resolution (bsp/board.h).
#pragma once

#include "bsp/board.h"
#include "lvgl.h"

#define SCR_W LCD_H_RES
#define SCR_H LCD_V_RES

#if LCD_H_RES >= 800
// 800x480: 4 x 2 tiles of 190 x 176
#define BAR_H        40
#define TAB_H        56
#define GAP          8
#define TILE_COLS    4
#define TILE_H       176
#define TILE_PAD     14
#define BTN_H        44
#define ROLLER_BTN_W 48
#define SETPT_BTN_W  72
#define MARGIN       40
#define QR_PX        280
#define STATUS_GAP   28
#define ICON_GAP     36      // status bar: distance between the WiFi and openHAB icons
#define BAR_SIDE     96      // status bar: room for the clock (left) and the icons (right)
#define FONT_TEXT    (&lv_font_montserrat_20)
#define FONT_BIG     (&lv_font_montserrat_28)
#else
// 480x320: 3 x 2 tiles of 152 x 117
#define BAR_H        32
#define TAB_H        40
#define GAP          6
#define TILE_COLS    3
#define TILE_H       ((SCR_H - BAR_H - TAB_H - 3 * GAP) / 2)
#define TILE_PAD     10
#define BTN_H        34
#define ROLLER_BTN_W 38
#define SETPT_BTN_W  54
#define MARGIN       16
#define QR_PX        180
#define STATUS_GAP   14
#define ICON_GAP     28
#define BAR_SIDE     72
#define FONT_TEXT    (&lv_font_montserrat_16)
#define FONT_BIG     (&lv_font_montserrat_20)
#endif

#define TILE_W       ((SCR_W - (TILE_COLS + 1) * GAP) / TILE_COLS)
