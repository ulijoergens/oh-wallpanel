// Panel hardware abstraction. One board is selected at build time (Kconfig WP_BOARD, see
// boards/*.defaults); everything above this header is board independent.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_lcd_types.h"
#include "esp_lcd_touch.h"

#if CONFIG_WP_BOARD_WT32_SC01_PLUS
// Wireless-Tag WT32-SC01 Plus: ST7796 on an 8-bit i80 bus (the panel has its own frame
// memory), FT6336 touch, backlight on a GPIO. Used in landscape.
#define BOARD_NAME          "WT32-SC01 Plus"
#define LCD_H_RES           480
#define LCD_V_RES           320
#define BOARD_LCD_I80       1
#define BOARD_HAS_FRAMEBUFFER 0
#elif CONFIG_WP_BOARD_ESP32_2424S012
// ESP32-2424S012: ESP32-C3 without PSRAM, round 1.28" GC9A01 on SPI (no read-back),
// CST816D touch, backlight on a GPIO. The UI shows one dimmer at a time (ui_dimmer.c).
#define BOARD_NAME          "ESP32-2424S012"
#define LCD_H_RES           240
#define LCD_V_RES           240
#define BOARD_LCD_SPI       1
#define BOARD_ROUND         1
#define BOARD_HAS_FRAMEBUFFER 0
#else
// Waveshare ESP32-S3-Touch-LCD-4.3: 16-bit RGB panel streamed from a PSRAM frame buffer,
// GT911 touch, backlight and resets on a CH422G I/O expander.
#define BOARD_NAME          "Waveshare ESP32-S3-Touch-LCD-4.3"
#define LCD_H_RES           800
#define LCD_V_RES           480
#define BOARD_LCD_RGB       1
#define BOARD_HAS_FRAMEBUFFER 1
#define RGB_BOUNCE_BUFFER_SIZE (LCD_H_RES * CONFIG_LCD_RGB_BOUNCE_BUFFER_HEIGHT)
#endif

// The panel IO reports when a transfer is done and only then gets LVGL's buffer back
// (i80, SPI); RGB panels copy into a frame buffer and sync on VSYNC instead.
#define BOARD_FLUSH_ASYNC   (BOARD_LCD_I80 || BOARD_LCD_SPI)

// Brings up the display and the touch controller. *ret_touch is NULL when the touch
// controller did not answer (the display still works).
esp_err_t board_display_init(esp_lcd_panel_handle_t *ret_panel, esp_lcd_touch_handle_t *ret_touch);

// Switches the backlight, ramping the brightness over ramp_ms (0 = at once). May block for
// ramp_ms. Call with the LVGL lock held: on the Waveshare board the I/O expander shares
// the I2C bus with the touch controller, which the LVGL task polls.
esp_err_t board_backlight(bool on, uint32_t ramp_ms);

// The frame buffer the panel shows (boards with BOARD_HAS_FRAMEBUFFER only).
esp_err_t board_framebuffer(esp_lcd_panel_handle_t panel, void **fb);
