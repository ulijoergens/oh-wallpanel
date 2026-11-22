// LCD user interface, see ui.h. All LVGL calls run in the LVGL task (timer callbacks)
// or under lvgl_port_lock().
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "bsp/board.h"
#include "bsp/lvgl_port.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "lvgl.h"
#include "src/libs/qrcode/qrcodegen.h"
#include "time_sync.h"
#include "ui_layout.h"
#include "ui_tiles.h"
#include "wifi_mgr.h"
#include "wp_config.h"
#include "wp_oh.h"

static const char *TAG = "ui";

#define COL_BG     0x101418
#define COL_BAR    0x1c232b
#define COL_TEXT   0xe8eef4
#define COL_MUTED  0x8a96a3
#define COL_OK     0x3ccf6e
#define COL_WARN   0xf2b33d
#define COL_ERR    0xe5534b

static lv_obj_t *s_clock, *s_wifi, *s_oh;
static lv_obj_t *s_setup, *s_info, *s_tiles_panel, *s_status_box;
static lv_obj_t *s_qr, *s_setup_ap, *s_setup_hint;
static lv_obj_t *s_info_name, *s_info_body;
static char s_qr_text[96];
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static volatile bool s_ui_started;   // the console runs before the display exists

// LVGL lock for calls from other tasks (console, main). Before ui_start() the lock does
// not exist yet; lvgl_port_lock() would assert.
static bool ui_lock(void)
{
    if (!s_ui_started) {
        printf("UI not started yet\n");
        return false;
    }
    return lvgl_port_lock(1000);
}
static lv_obj_t *s_wake;    // catches the waking touch while the screen is dark
static void on_wake_touch(lv_event_t *e);
static uint16_t *s_qr_buf;   // RGB565 canvas buffer, only while the setup screen shows

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

// The QR canvas needs QR_PX^2 * 2 bytes (29 KB on the round board, which has no PSRAM),
// so it exists only while the setup screen is shown.
static void qr_show(bool show)
{
    if (show && !s_qr_buf) {
        s_qr_buf = heap_caps_malloc(QR_PX * QR_PX * 2, WP_CAPS_BIG);
        if (!s_qr_buf) return;
        for (int i = 0; i < QR_PX * QR_PX; i++) s_qr_buf[i] = 0xFFFF;
        s_qr = lv_canvas_create(s_setup);
        lv_canvas_set_buffer(s_qr, s_qr_buf, QR_PX, QR_PX, LV_COLOR_FORMAT_RGB565);
#if BOARD_ROUND
        lv_obj_align(s_qr, LV_ALIGN_CENTER, 0, -4);
#else
        lv_obj_set_pos(s_qr, SCR_W - QR_PX - MARGIN + 10, MARGIN + 10);
#endif
        s_qr_text[0] = '\0';   // draw it again
    } else if (!show && s_qr_buf) {
        lv_obj_delete(s_qr);
        s_qr = NULL;
        heap_caps_free(s_qr_buf);
        s_qr_buf = NULL;
    }
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
    lv_obj_set_size(p, SCR_W, SCR_H - BAR_H);
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

    // Status bar (REQ-STAT-1): clock, up to four status items, WiFi and openHAB icons.
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, SCR_W, BAR_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_BAR), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    s_clock = label(bar, FONT_TEXT, COL_TEXT);
    lv_obj_align(s_clock, LV_ALIGN_LEFT_MID, 16, 0);
    lv_label_set_text(s_clock, "--:--");
    s_oh = label(bar, FONT_TEXT, COL_MUTED);
    lv_label_set_text(s_oh, LV_SYMBOL_HOME);
    lv_obj_align(s_oh, LV_ALIGN_RIGHT_MID, -16, 0);
    s_wifi = label(bar, FONT_TEXT, COL_MUTED);
    lv_label_set_text(s_wifi, LV_SYMBOL_WIFI);
    lv_obj_align(s_wifi, LV_ALIGN_RIGHT_MID, -16 - ICON_GAP, 0);
    s_status_box = lv_obj_create(bar);
    lv_obj_remove_style_all(s_status_box);
    lv_obj_set_size(s_status_box, SCR_W - 2 * BAR_SIDE, BAR_H);
    lv_obj_set_pos(s_status_box, BAR_SIDE, 0);
    lv_obj_set_flex_flow(s_status_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_status_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_status_box, STATUS_GAP, 0);
    lv_obj_set_scrollable(s_status_box, false);
    lv_obj_set_hidden(bar, BAR_H == 0);   // round screen: no status bar

    // Setup screen (REQ-NET-2): AP name and a QR code that joins it.
    s_setup = panel(scr);
#if BOARD_ROUND
    // Round: title, QR code and AP name stacked in the middle; the steps do not fit.
    lv_obj_t *t = label(s_setup, FONT_TEXT, COL_TEXT);
    lv_label_set_text(t, "Scan to set up");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, MARGIN);
    s_setup_ap = label(s_setup, FONT_TEXT, COL_WARN);
    lv_label_set_text(s_setup_ap, " ");
    lv_obj_align(s_setup_ap, LV_ALIGN_BOTTOM_MID, 0, -MARGIN - 6);
    s_setup_hint = label(s_setup, FONT_TEXT, COL_TEXT);
    lv_obj_set_hidden(s_setup_hint, true);
    // QR canvas: created by qr_show() only while the setup screen is up
#else
    lv_obj_t *t = label(s_setup, FONT_BIG, COL_TEXT);
    lv_label_set_text(t, "Set up this panel");
    lv_obj_set_pos(t, MARGIN, MARGIN);
    const int text_w = SCR_W - QR_PX - 3 * MARGIN;   // left of the QR code
    lv_obj_t *steps = label(s_setup, FONT_TEXT, COL_TEXT);
    lv_label_set_text(steps, "1.  Scan the QR code with your phone, or join this WiFi network:");
    lv_obj_set_width(steps, text_w);
    lv_label_set_long_mode(steps, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align_to(steps, t, LV_ALIGN_OUT_BOTTOM_LEFT, 0, MARGIN / 2 + 8);
    s_setup_ap = label(s_setup, FONT_BIG, COL_WARN);
    lv_label_set_text(s_setup_ap, " ");
    lv_obj_align_to(s_setup_ap, steps, LV_ALIGN_OUT_BOTTOM_LEFT, MARGIN, MARGIN / 2);
    s_setup_hint = label(s_setup, FONT_TEXT, COL_TEXT);
    lv_obj_set_width(s_setup_hint, text_w);
    lv_label_set_long_mode(s_setup_hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align_to(s_setup_hint, s_setup_ap, LV_ALIGN_OUT_BOTTOM_LEFT, -MARGIN, MARGIN / 2);
    // QR canvas: created by qr_show() only while the setup screen is up
#endif

    // Tile screens (milestone 4), shown once openHAB screens are loaded.
    s_tiles_panel = panel(scr);
    lv_obj_set_hidden(s_tiles_panel, true);
    tiles_init(s_tiles_panel, s_status_box);

    // Info screen: on WiFi but no screens loaded yet (openHAB not set up or unreachable).
    s_info = panel(scr);
    s_info_name = label(s_info, FONT_BIG, COL_TEXT);
    s_info_body = label(s_info, FONT_TEXT, COL_TEXT);
    lv_label_set_long_mode(s_info_body, LV_LABEL_LONG_MODE_WRAP);
#if BOARD_ROUND
    lv_obj_align(s_info_name, LV_ALIGN_TOP_MID, 0, 2 * MARGIN);
    lv_obj_set_width(s_info_body, SCR_W - 4 * MARGIN);
    lv_obj_set_style_text_align(s_info_body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_info_body, LV_ALIGN_CENTER, 0, 16);
#else
    lv_obj_set_pos(s_info_name, MARGIN, MARGIN);
    lv_obj_set_pos(s_info_body, MARGIN, MARGIN + (BAR_H * 3) / 2);
    lv_obj_set_width(s_info_body, SCR_W - 2 * MARGIN);
#endif
    lv_obj_set_hidden(s_info, true);

    s_wake = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_wake);
    lv_obj_set_size(s_wake, SCR_W, SCR_H);
    lv_obj_set_clickable(s_wake, true);
    lv_obj_add_event_cb(s_wake, on_wake_touch, LV_EVENT_ALL, NULL);
    lv_obj_set_hidden(s_wake, true);
}

// ---- backlight (REQ-DISP-1/2) ----

#define NIGHT_DEFAULT_TIMEOUT_S 30   // at night the screen never stays on

static bool s_bl_ready;     // main turned the backlight on once (first frame drawn)
static bool s_bl_on;
static bool s_was_night;

// Called in the LVGL task or under the (recursive) LVGL lock: on the Waveshare board the
// CH422G shares the I2C bus with the touch controller, which the LVGL task polls.
static void set_backlight(bool on)
{
    if (on == s_bl_on) return;
    esp_err_t err = board_backlight(on, wp_config_get()->bl_ramp_ms);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "backlight %s failed: %s", on ? "on" : "off", esp_err_to_name(err));
        return;
    }
    s_bl_on = on;
    lv_obj_set_hidden(s_wake, on);
    // Above everything else on the top layer (e.g. the touch test), or the waking tap
    // lands there and the screen stays dark (2026-11-22).
    if (!on) lv_obj_move_foreground(s_wake);
    ESP_LOGI(TAG, "screen %s", on ? "on" : "dark");
}

// The waking touch only turns the light on. The catcher stays until the finger lifts,
// so the press never reaches a tile underneath (REQ-DISP-1).
static void on_wake_touch(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        set_backlight(true);
        lv_obj_set_hidden(s_wake, false);   // keep catching until released
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_obj_set_hidden(s_wake, true);
    }
}

static bool in_night(const wp_cfg_t *cfg)
{
    if (!cfg->night_mode || !time_is_set() || cfg->night_start_min == cfg->night_end_min) return false;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int m = tm.tm_hour * 60 + tm.tm_min;
    int a = cfg->night_start_min, b = cfg->night_end_min;
    return a < b ? (m >= a && m < b) : (m >= a || m < b);   // the window may span midnight
}

static void screen_policy(bool setup)
{
    if (!s_bl_ready) return;
    const wp_cfg_t *cfg = wp_config_get();
    if (setup) {          // the setup screen with its QR code must stay readable
        set_backlight(true);
        return;
    }
    bool night = in_night(cfg);
    bool dark = night && !s_was_night;   // REQ-DISP-2: the night starts dark
    s_was_night = night;
    uint32_t timeout_s = cfg->dark_timeout_s;
    if (night && timeout_s == 0) timeout_s = NIGHT_DEFAULT_TIMEOUT_S;
    if (timeout_s && lv_display_get_inactive_time(NULL) > timeout_s * 1000) dark = true;
    if (dark) set_backlight(false);
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
    lv_obj_set_style_text_color(s_wifi, lv_color_hex(on_wifi ? COL_OK : w.ap_active ? COL_WARN : COL_ERR), 0);
    lv_obj_set_style_text_color(s_oh, lv_color_hex(o.connected ? COL_OK : !cfg->oh_url[0] ? COL_MUTED : COL_ERR), 0);

    bool setup = w.ap_active && !on_wifi;
    bool tiles = !setup && tiles_ready();
    screen_policy(setup);
    lv_obj_set_hidden(s_setup, !setup);
    lv_obj_set_hidden(s_tiles_panel, !tiles);
    lv_obj_set_hidden(s_info, setup || tiles);
    qr_show(setup);
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
#if BOARD_ROUND
        // Round: short lines, no URL of the openHAB server.
        if (!cfg->oh_url[0]) snprintf(ohline, sizeof(ohline), "openHAB not set up");
        else if (o.connected) snprintf(ohline, sizeof(ohline), "openHAB connected");
        else snprintf(ohline, sizeof(ohline), "openHAB: %s", o.last_error[0] ? o.last_error : o.state);
        if (on_wifi) {
            lv_label_set_text_fmt(s_info_body, "%s\n\nhttp://%s/", ohline, w.ip);
        } else {
            lv_label_set_text_fmt(s_info_body, "Connecting to\n\"%s\" ...", w.ssid);
        }
#else
        if (on_wifi) {
            lv_label_set_text_fmt(s_info_body,
                                  "WiFi:  %s  (%s, %d dBm)\nopenHAB:  %s\n\nSettings:  http://%s/",
                                  w.ssid, w.ip, w.rssi, ohline, w.ip);
        } else {
            lv_label_set_text_fmt(s_info_body, "Connecting to WiFi \"%s\" ...", w.ssid);
        }
#endif
    }
}

static void tiles_timer(lv_timer_t *t)
{
    tiles_tick();
}

esp_err_t ui_start(void)
{
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_touch_handle_t touch = NULL;
    esp_err_t err = board_display_init(&panel, &touch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(err));
        return err;
    }
    if (!touch) {
        ESP_LOGE(TAG, "touch controller not found");
    }
    s_panel = panel;
    s_touch = touch;
    err = lvgl_port_init(panel, touch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LVGL init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_ui_started = true;
    if (lvgl_port_lock(-1)) {
        build();
        lv_timer_t *t = lv_timer_create(refresh, 1000, NULL);
        lv_timer_ready(t);
        lv_timer_create(tiles_timer, 100, NULL);
        lvgl_port_unlock();
    }
    return ESP_OK;
}

// The CH422G (backlight) shares the I2C bus with the GT911, which the LVGL task polls.
// Concurrent transfers from two tasks hung the board (interrupt watchdog, 2026-11-22), so
// every backlight change happens under the LVGL lock, i.e. never during a touch read.
void ui_backlight_on(void)
{
    if (!ui_lock()) {
        ESP_LOGW(TAG, "backlight on: LVGL busy");
        return;
    }
    set_backlight(true);
    s_bl_ready = true;
    lvgl_port_unlock();
}

// ---- console 'fbdump' (developer aid, see tools/fbdump.py) ----
//
// Output: base64 of run-length pairs (u16 count, u16 RGB565 pixel, little endian) between
// FBDUMP-BEGIN/END lines. The UI is mostly flat colour, so a screen is typically 20-60 KB.

static struct {
    uint8_t chunk[56];   // 14 whole pairs per line: a lost line cannot misalign the rest
    int used;
    uint16_t px;
    int run;
} s_enc;

static void enc_flush_line(void)
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char line[80];
    int o = 0, used = s_enc.used;
    const uint8_t *c = s_enc.chunk;
    for (int j = 0; j < used; j += 3) {
        uint32_t v = c[j] << 16 | (j + 1 < used ? c[j + 1] << 8 : 0) | (j + 2 < used ? c[j + 2] : 0);
        line[o++] = b64[(v >> 18) & 63];
        line[o++] = b64[(v >> 12) & 63];
        line[o++] = j + 1 < used ? b64[(v >> 6) & 63] : '=';
        line[o++] = j + 2 < used ? b64[v & 63] : '=';
    }
    line[o] = '\0';
    printf("%s\n", line);
    s_enc.used = 0;
}

static void enc_pair(void)
{
    uint8_t pair[4] = {s_enc.run & 0xFF, s_enc.run >> 8, s_enc.px & 0xFF, s_enc.px >> 8};
    for (int k = 0; k < 4; k++) {
        s_enc.chunk[s_enc.used++] = pair[k];
        if (s_enc.used == sizeof(s_enc.chunk)) enc_flush_line();
    }
}

static void enc_begin(void)
{
    s_enc.used = 0;
    s_enc.run = 0;
    printf("FBDUMP-BEGIN %d %d\n", LCD_H_RES, LCD_V_RES);
}

static void enc_pixels(const uint16_t *px, int n)
{
    for (int i = 0; i < n; i++) {
        if (s_enc.run && px[i] == s_enc.px && s_enc.run < 65535) {
            s_enc.run++;
            continue;
        }
        if (s_enc.run) enc_pair();
        s_enc.px = px[i];
        s_enc.run = 1;
    }
}

static void enc_end(void)
{
    if (s_enc.run) enc_pair();
    if (s_enc.used) enc_flush_line();
    printf("FBDUMP-END\n");
}

#if BOARD_LCD_SPI
// No read-back and no RAM for a full snapshot: redraw the whole screen and encode the
// stripes on their way to the panel (top layer included, e.g. the touch test).
static SemaphoreHandle_t s_tap_done;
static int s_tap_row;

static void dump_tap(const lv_area_t *a, const uint16_t *px)
{
    if (a->x1 != 0 || a->x2 != LCD_H_RES - 1 || a->y1 != s_tap_row) return;   // not the next stripe
    enc_pixels(px, LCD_H_RES * (a->y2 - a->y1 + 1));
    s_tap_row = a->y2 + 1;
    if (s_tap_row >= LCD_V_RES) {
        lvgl_port_set_flush_tap(NULL);
        xSemaphoreGive(s_tap_done);
    }
}
#elif !BOARD_HAS_FRAMEBUFFER
static lv_draw_buf_t *s_snap;
static SemaphoreHandle_t s_snap_done;

static void snapshot_cb(void *arg)
{
    s_snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    xSemaphoreGive(s_snap_done);
}
#endif

void ui_dump_framebuffer(void)
{
#if BOARD_HAS_FRAMEBUFFER
    void *fbv = NULL;
    if (!s_panel || board_framebuffer(s_panel, &fbv) != ESP_OK || !fbv) {
        printf("no frame buffer\n");
        return;
    }
    esp_log_level_t lvl = esp_log_level_get("*");
    esp_log_level_set("*", ESP_LOG_NONE);   // log lines inside the dump would corrupt it
    enc_begin();
    enc_pixels(fbv, LCD_H_RES * LCD_V_RES);
    enc_end();
    esp_log_level_set("*", lvl);
#elif BOARD_LCD_SPI
    if (!ui_lock()) return;
    if (!s_tap_done) s_tap_done = xSemaphoreCreateBinary();
    xSemaphoreTake(s_tap_done, 0);
    esp_log_level_t lvl = esp_log_level_get("*");
    esp_log_level_set("*", ESP_LOG_NONE);
    enc_begin();
    s_tap_row = 0;
    lvgl_port_set_flush_tap(dump_tap);
    lv_obj_invalidate(lv_screen_active());
    lvgl_port_unlock();
    bool ok = xSemaphoreTake(s_tap_done, pdMS_TO_TICKS(5000)) == pdTRUE;
    lvgl_port_set_flush_tap(NULL);
    if (ok) enc_end();
    esp_log_level_set("*", lvl);
    if (!ok) printf("\nFBDUMP-FAILED (screen not redrawn in time)\n");
#else
    // The panel keeps its frame in its own memory: render the active screen instead
    // (without the top layer, i.e. without the touch test).
    // Rendering needs the LVGL task's stack (the console task overflowed, 2026-11-22).
    if (!ui_lock()) return;
    if (!s_snap_done) s_snap_done = xSemaphoreCreateBinary();
    xSemaphoreTake(s_snap_done, 0);
    s_snap = NULL;
    lv_async_call(snapshot_cb, NULL);
    lvgl_port_unlock();
    lv_draw_buf_t *snap = xSemaphoreTake(s_snap_done, pdMS_TO_TICKS(5000)) == pdTRUE ? s_snap : NULL;
    if (!snap || snap->header.stride != LCD_H_RES * 2) {
        printf("snapshot failed\n");
        if (snap) lv_draw_buf_destroy(snap);
        return;
    }
    esp_log_level_t lvl = esp_log_level_get("*");
    esp_log_level_set("*", ESP_LOG_NONE);
    enc_begin();
    enc_pixels((const uint16_t *)snap->data, LCD_H_RES * LCD_V_RES);
    enc_end();
    esp_log_level_set("*", lvl);
    lv_draw_buf_destroy(snap);
#endif
}

void ui_show_tab(int tab, int scroll_y)
{
    if (ui_lock()) {
        tiles_show(tab, scroll_y);
        lvgl_port_unlock();
    }
}

// Developer aid: sends a click to the clickable object at (x, y), as a finger tap would.
// Sliders need dragging and are not covered.
void ui_tap(int x, int y)
{
    if (!ui_lock()) return;
    lv_point_t p = {x, y};
    lv_obj_t *o = lv_indev_search_obj(lv_screen_active(), &p);
    while (o && !lv_obj_is_clickable(o)) o = lv_obj_get_parent(o);
    if (o) {
        lv_obj_send_event(o, LV_EVENT_CLICKED, NULL);
        printf("tapped %s at %d,%d\n", lv_obj_check_type(o, &lv_button_class) ? "button" : "object", x, y);
    } else {
        printf("nothing clickable at %d,%d\n", x, y);
    }
    lvgl_port_unlock();
}

// Developer aid: logs what each touch hits (console 'touchlog on|off').
static bool s_touchlog;

static const char *obj_desc(lv_obj_t *o, char *buf, size_t len)
{
    if (!o) return "none";
    const lv_obj_class_t *c = lv_obj_get_class(o);
    const char *cls = c == &lv_button_class ? "button" : c == &lv_slider_class ? "slider" :
                      c == &lv_label_class ? "label" : c == &lv_tabview_class ? "tabview" : "obj";
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    snprintf(buf, len, "%s@%p [%d,%d..%d,%d]%s%s", cls, (void *)o, (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2,
             lv_obj_is_clickable(o) ? " clickable" : "", lv_obj_is_hidden(o) ? " hidden" : "");
    return buf;
}

static lv_obj_t *s_tt_layer, *s_tt_dot, *s_tt_label;

static void touch_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (s_tt_layer && (code == LV_EVENT_PRESSING || code == LV_EVENT_PRESSED)) {
        lv_point_t tp;
        lv_indev_get_point(lv_indev_active(), &tp);
        lv_obj_set_pos(s_tt_dot, tp.x - 15, tp.y - 15);
        lv_label_set_text_fmt(s_tt_label, "touch at %d, %d", (int)tp.x, (int)tp.y);
    }
    if (!s_touchlog) return;
    const char *name = code == LV_EVENT_PRESSED ? "PRESSED" : code == LV_EVENT_RELEASED ? "RELEASED" :
                       code == LV_EVENT_CLICKED ? "CLICKED" : code == LV_EVENT_SCROLL_BEGIN ? "SCROLL_BEGIN" :
                       code == LV_EVENT_PRESS_LOST ? "PRESS_LOST" : NULL;
    if (!name) return;
    lv_indev_t *in = lv_indev_active();
    lv_point_t p = {0, 0};
    if (in) lv_indev_get_point(in, &p);
    char a[96], b[96], c[96];
    lv_obj_t *under = lv_indev_search_obj(lv_screen_active(), &p);
    ESP_LOGI(TAG, "touch %s at %d,%d active=%s under=%s scroll=%s", name, (int)p.x, (int)p.y,
             obj_desc(in ? lv_indev_get_active_obj() : NULL, a, sizeof(a)), obj_desc(under, b, sizeof(b)),
             obj_desc(in ? lv_indev_get_scroll_obj(in) : NULL, c, sizeof(c)));
}

// Developer aid: full-screen touch test (console 'touchtest on|off'). A red dot follows the
// reported touch point; a grid shows where it should be.
void ui_touchtest(bool on)
{
    void ui_touchlog(bool on);
    ui_touchlog(true);   // installs the indev event hook
    if (!ui_lock()) return;
    if (on && !s_tt_layer) {
        s_tt_layer = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_tt_layer);
        lv_obj_set_size(s_tt_layer, SCR_W, SCR_H);
        lv_obj_set_style_bg_color(s_tt_layer, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_tt_layer, LV_OPA_COVER, 0);
        lv_obj_set_clickable(s_tt_layer, true);   // swallow touches: nothing underneath reacts
        const int dx = SCR_W / 8, dy = SCR_H / 6;
        for (int y = 0; y < SCR_H; y += dy) {
            for (int x = 0; x < SCR_W; x += dx) {
                lv_obj_t *l = lv_label_create(s_tt_layer);
                lv_obj_set_style_text_color(l, lv_color_hex(0x606870), 0);
                lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
                lv_label_set_text_fmt(l, "+ %d,%d", x + dx / 2, y + dy / 2);
                lv_obj_set_pos(l, x + dx / 2 - 6, y + dy / 2 - 10);
            }
        }
        s_tt_label = lv_label_create(s_tt_layer);
        lv_obj_set_style_text_font(s_tt_label, FONT_BIG, 0);
        lv_obj_set_style_text_color(s_tt_label, lv_color_hex(0xffffff), 0);
        lv_label_set_text(s_tt_label, "Touch test: drag a finger around");
        lv_obj_align(s_tt_label, LV_ALIGN_CENTER, 0, 0);
        s_tt_dot = lv_obj_create(s_tt_layer);
        lv_obj_remove_style_all(s_tt_dot);
        lv_obj_set_size(s_tt_dot, 30, 30);
        lv_obj_set_style_radius(s_tt_dot, 15, 0);
        lv_obj_set_style_bg_color(s_tt_dot, lv_color_hex(0xff3030), 0);
        lv_obj_set_style_bg_opa(s_tt_dot, LV_OPA_COVER, 0);
        lv_obj_set_pos(s_tt_dot, -40, -40);
    } else if (!on && s_tt_layer) {
        lv_obj_delete(s_tt_layer);
        s_tt_layer = s_tt_dot = s_tt_label = NULL;
    }
    lvgl_port_unlock();
}

void ui_touchlog(bool on)
{
    if (!ui_lock()) return;
    static bool installed;
    if (!installed) {
        for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in)) {
            lv_indev_add_event_cb(in, touch_event, LV_EVENT_ALL, NULL);
        }
        installed = true;
    }
    s_touchlog = on;
    lvgl_port_unlock();
}

// Developer aid: dumps the GT911 configuration (console 'gt911'). Reads 16 bytes at a time
// (REQ-HW-3) under the LVGL lock, so it never overlaps the touch polling on the I2C bus.
static esp_err_t gt911_read(uint16_t reg, uint8_t *buf, int len)
{
    for (int off = 0; off < len; off += 16) {
        int n = len - off < 16 ? len - off : 16;
        esp_err_t err = esp_lcd_panel_io_rx_param(s_touch->io, reg + off, buf + off, n);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

void ui_dump_gt911(void)
{
#if !CONFIG_WP_BOARD_WAVESHARE_43
    printf("this board has no GT911\n");
    return;
#endif
    if (!s_touch) {
        printf("no touch controller\n");
        return;
    }
    uint8_t cfg[0x8101 - 0x8047], info[11];
    if (!ui_lock()) return;
    esp_err_t e1 = gt911_read(0x8047, cfg, sizeof(cfg));
    esp_err_t e2 = gt911_read(0x8140, info, sizeof(info));
    lvgl_port_unlock();
    if (e1 != ESP_OK || e2 != ESP_OK) {
        printf("read failed: %s / %s\n", esp_err_to_name(e1), esp_err_to_name(e2));
        return;
    }
    printf("product id \"%c%c%c%c\", firmware 0x%02X%02X, resolution %u x %u, vendor 0x%02X\n",
           info[0], info[1], info[2], info[3], info[5], info[4], info[6] | info[7] << 8,
           info[8] | info[9] << 8, info[10]);
    uint8_t sum = 0;
    for (int i = 0; i < 0x80FF - 0x8047; i++) sum += cfg[i];
    printf("config version %u, x max %u, y max %u, touch points %u, switch1 0x%02X, switch2 0x%02X\n",
           cfg[0], cfg[1] | cfg[2] << 8, cfg[3] | cfg[4] << 8, cfg[5], cfg[6], cfg[7]);
    printf("touch level %u, leave level %u, checksum 0x%02X (expected 0x%02X), fresh %u\n",
           cfg[0x8053 - 0x8047], cfg[0x8054 - 0x8047], cfg[0x80FF - 0x8047], (uint8_t)(~sum + 1),
           cfg[0x8100 - 0x8047]);
    printf("raw 0x8047..0x8100:");
    for (int i = 0; i < (int)sizeof(cfg); i++) {
        if (i % 16 == 0) printf("\n  %04X:", 0x8047 + i);
        printf(" %02X", cfg[i]);
    }
    printf("\n");
}

// Developer aid: switches the backlight like a timeout/wake would (console 'screen').
void ui_screen(bool on)
{
    if (!ui_lock()) return;
    if (on) lv_display_trigger_activity(NULL);
    set_backlight(on);
    lvgl_port_unlock();
}
