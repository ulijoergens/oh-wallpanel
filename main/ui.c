// LCD user interface, see ui.h. All LVGL calls run in the LVGL task (timer callbacks)
// or under lvgl_port_lock().
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "bsp/board.h"
#include "bsp/lvgl_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "src/libs/qrcode/qrcodegen.h"
#include "time_sync.h"
#include "wifi_mgr.h"
#include "wp_config.h"
#include "wp_oh.h"

static const char *TAG = "ui";

#define BAR_H      40
#define COL_BG     0x101418
#define COL_BAR    0x1c232b
#define COL_TEXT   0xe8eef4
#define COL_MUTED  0x8a96a3
#define COL_OK     0x3ccf6e
#define COL_WARN   0xf2b33d
#define COL_ERR    0xe5534b

static lv_obj_t *s_clock, *s_wifi, *s_oh;
static lv_obj_t *s_setup, *s_info;
static lv_obj_t *s_qr, *s_setup_ap, *s_setup_hint;
static lv_obj_t *s_info_name, *s_info_body;
static char s_qr_text[96];
static uint16_t *s_qr_buf;   // RGB565 canvas buffer in PSRAM

#define QR_PX 280

// Draws text as a QR code into an RGB565 canvas, using the qrcodegen library bundled with
// LVGL. LVGL 9.6's lv_qrcode widget is not used: rendering its 1-bit (I1) canvas hung the
// panel on every boot (silent watchdog reset, verified 2026-11-22: 10 of 10 boots).
static void qr_draw(const char *text)
{
    static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(6)], tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
    for (int i = 0; i < QR_PX * QR_PX; i++) s_qr_buf[i] = 0xFFFF;
    if (qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM, 1, 6, qrcodegen_Mask_AUTO, true)) {
        int n = qrcodegen_getSize(qr);
        int scale = QR_PX / (n + 4);            // two modules of quiet zone on each side
        int off = (QR_PX - n * scale) / 2;
        for (int y = 0; y < n; y++) {
            for (int x = 0; x < n; x++) {
                if (!qrcodegen_getModule(qr, x, y)) continue;
                for (int dy = 0; dy < scale; dy++) {
                    uint16_t *row = s_qr_buf + (off + y * scale + dy) * QR_PX + off + x * scale;
                    for (int dx = 0; dx < scale; dx++) row[dx] = 0x0000;
                }
            }
        }
    } else {
        ESP_LOGE(TAG, "QR encoding failed for \"%s\"", text);
    }
    lv_obj_invalidate(s_qr);
}

static lv_obj_t *label(lv_obj_t *par, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(par);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static lv_obj_t *panel(lv_obj_t *scr)
{
    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, 800, 480 - BAR_H);
    lv_obj_set_pos(p, 0, BAR_H);
    lv_obj_set_scrollable(p, false);
    return p;
}

static void build(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_scrollable(scr, false);

    // Status bar (REQ-STAT-1): clock left, connection state right.
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 800, BAR_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_BAR), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    s_clock = label(bar, &lv_font_montserrat_20, COL_TEXT);
    lv_obj_align(s_clock, LV_ALIGN_LEFT_MID, 16, 0);
    lv_label_set_text(s_clock, "--:--");
    s_oh = label(bar, &lv_font_montserrat_20, COL_MUTED);
    lv_obj_align(s_oh, LV_ALIGN_RIGHT_MID, -16, 0);
    s_wifi = label(bar, &lv_font_montserrat_20, COL_MUTED);
    lv_obj_align_to(s_wifi, s_oh, LV_ALIGN_OUT_LEFT_MID, -24, 0);

    // Setup screen (REQ-NET-2): AP name and a QR code that joins it.
    s_setup = panel(scr);
    lv_obj_t *t = label(s_setup, &lv_font_montserrat_28, COL_TEXT);
    lv_label_set_text(t, "Set up this panel");
    lv_obj_set_pos(t, 40, 40);
    lv_obj_t *steps = label(s_setup, &lv_font_montserrat_20, COL_TEXT);
    lv_label_set_text(steps, "1.  Scan the QR code with your phone,\n     or join this WiFi network:");
    lv_obj_set_pos(steps, 40, 100);
    s_setup_ap = label(s_setup, &lv_font_montserrat_28, COL_WARN);
    lv_obj_set_pos(s_setup_ap, 80, 165);
    s_setup_hint = label(s_setup, &lv_font_montserrat_20, COL_TEXT);
    lv_obj_set_pos(s_setup_hint, 40, 220);
    lv_obj_set_width(s_setup_hint, 440);
    lv_label_set_long_mode(s_setup_hint, LV_LABEL_LONG_MODE_WRAP);
    s_qr_buf = heap_caps_malloc(QR_PX * QR_PX * 2, MALLOC_CAP_SPIRAM);
    s_qr = lv_canvas_create(s_setup);
    if (s_qr_buf) {
        for (int i = 0; i < QR_PX * QR_PX; i++) s_qr_buf[i] = 0xFFFF;
        lv_canvas_set_buffer(s_qr, s_qr_buf, QR_PX, QR_PX, LV_COLOR_FORMAT_RGB565);
    }
    lv_obj_set_pos(s_qr, 490, 50);

    // Info screen: shown when on WiFi, until the tile screens exist (milestone 4).
    s_info = panel(scr);
    s_info_name = label(s_info, &lv_font_montserrat_28, COL_TEXT);
    lv_obj_set_pos(s_info_name, 40, 40);
    s_info_body = label(s_info, &lv_font_montserrat_20, COL_TEXT);
    lv_obj_set_pos(s_info_body, 40, 100);
    lv_obj_set_width(s_info_body, 720);
    lv_label_set_long_mode(s_info_body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_hidden(s_info, true);
}

static void refresh(lv_timer_t *t)
{
    const wp_cfg_t *cfg = wp_config_get();
    wifi_mgr_status_t w;
    wifi_mgr_get_status(&w);
    wp_oh_status_t o;
    wp_oh_get_status(&o);

    if (time_is_set()) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        lv_label_set_text_fmt(s_clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
    }

    bool on_wifi = w.state == WIFI_MGR_CONNECTED;
    lv_label_set_text_fmt(s_wifi, LV_SYMBOL_WIFI " %s", on_wifi ? "" : w.ap_active ? "Setup" : "...");
    lv_obj_set_style_text_color(s_wifi, lv_color_hex(on_wifi ? COL_OK : w.ap_active ? COL_WARN : COL_ERR), 0);
    const char *oh = !cfg->oh_url[0] ? "openHAB not set up" : o.connected ? "openHAB" : "openHAB offline";
    lv_label_set_text(s_oh, oh);
    lv_obj_set_style_text_color(s_oh, lv_color_hex(o.connected ? COL_OK : !cfg->oh_url[0] ? COL_MUTED : COL_ERR), 0);
    lv_obj_align_to(s_wifi, s_oh, LV_ALIGN_OUT_LEFT_MID, -24, 0);

    bool setup = w.ap_active && !on_wifi;
    lv_obj_set_hidden(s_setup, !setup);
    lv_obj_set_hidden(s_info, setup);
    if (setup) {
        char qr[96];
        snprintf(qr, sizeof(qr), "WIFI:T:nopass;S:%s;;", w.ap_ssid);
        if (strcmp(qr, s_qr_text) != 0) {
            strcpy(s_qr_text, qr);
            if (s_qr_buf) qr_draw(s_qr_text);
        }
        lv_label_set_text(s_setup_ap, w.ap_ssid);
        lv_label_set_text_fmt(s_setup_hint,
                              "2.  The settings page opens by itself.\n     If not, open http://%s\n\n"
                              "3.  Choose your WiFi, enter the password\n     and save.%s%s%s",
                              w.ap_ip, cfg->wifi_ssid[0] ? "\n\nRetrying \"" : "", cfg->wifi_ssid,
                              cfg->wifi_ssid[0] ? "\" in the background." : "");
    } else {
        lv_label_set_text(s_info_name, cfg->name);
        char ohline[240];
        if (!cfg->oh_url[0]) {
            snprintf(ohline, sizeof(ohline), "not configured yet");
        } else if (o.connected) {
            snprintf(ohline, sizeof(ohline), "connected to %s (openHAB %s)", cfg->oh_url, o.version);
        } else {
            snprintf(ohline, sizeof(ohline), "%s: %s", cfg->oh_url, o.last_error[0] ? o.last_error : o.state);
        }
        if (on_wifi) {
            lv_label_set_text_fmt(s_info_body,
                                  "WiFi:  %s  (%s, %d dBm)\nopenHAB:  %s\n\nSettings:  http://%s/\n\n"
                                  "The openHAB screens follow in the next milestones.",
                                  w.ssid, w.ip, w.rssi, ohline, w.ip);
        } else {
            lv_label_set_text_fmt(s_info_body, "Connecting to WiFi \"%s\" ...", w.ssid);
        }
    }
}

esp_err_t ui_start(void)
{
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_touch_handle_t touch = NULL;
    esp_err_t err = waveshare_esp32_s3_rgb_lcd_init(&panel, &touch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(err));
        return err;
    }
    if (!touch) {
        ESP_LOGE(TAG, "touch controller not found");
    }
    err = lvgl_port_init(panel, touch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LVGL init failed: %s", esp_err_to_name(err));
        return err;
    }
    if (lvgl_port_lock(-1)) {
        build();
        lv_timer_t *t = lv_timer_create(refresh, 1000, NULL);
        lv_timer_ready(t);
        lvgl_port_unlock();
    }
    return ESP_OK;
}

// The CH422G (backlight) shares the I2C bus with the GT911, which the LVGL task polls.
// Concurrent transfers from two tasks hung the board (interrupt watchdog, 2026-11-22), so
// every backlight change happens under the LVGL lock, i.e. never during a touch read.
void ui_backlight_on(void)
{
    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "backlight on: LVGL busy");
        return;
    }
    esp_err_t err = waveshare_rgb_lcd_bl_on();
    lvgl_port_unlock();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "backlight on failed: %s", esp_err_to_name(err));
    }
}
