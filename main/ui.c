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
#include "ui_tiles.h"
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
static lv_obj_t *s_setup, *s_info, *s_tiles_panel, *s_status_box;
static lv_obj_t *s_qr, *s_setup_ap, *s_setup_hint;
static lv_obj_t *s_info_name, *s_info_body;
static char s_qr_text[96];
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
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

    // Status bar (REQ-STAT-1): clock, up to four status items, WiFi and openHAB icons.
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 800, BAR_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_BAR), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    s_clock = label(bar, &lv_font_montserrat_20, COL_TEXT);
    lv_obj_align(s_clock, LV_ALIGN_LEFT_MID, 16, 0);
    lv_label_set_text(s_clock, "--:--");
    s_oh = label(bar, &lv_font_montserrat_20, COL_MUTED);
    lv_label_set_text(s_oh, LV_SYMBOL_HOME);
    lv_obj_align(s_oh, LV_ALIGN_RIGHT_MID, -16, 0);
    s_wifi = label(bar, &lv_font_montserrat_20, COL_MUTED);
    lv_label_set_text(s_wifi, LV_SYMBOL_WIFI);
    lv_obj_align(s_wifi, LV_ALIGN_RIGHT_MID, -52, 0);
    s_status_box = lv_obj_create(bar);
    lv_obj_remove_style_all(s_status_box);
    lv_obj_set_size(s_status_box, 800 - 96 - 96, BAR_H);
    lv_obj_set_pos(s_status_box, 96, 0);
    lv_obj_set_flex_flow(s_status_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_status_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_status_box, 28, 0);
    lv_obj_set_scrollable(s_status_box, false);

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

    // Tile screens (milestone 4), shown once openHAB screens are loaded.
    s_tiles_panel = panel(scr);
    lv_obj_set_hidden(s_tiles_panel, true);
    tiles_init(s_tiles_panel, s_status_box);

    // Info screen: on WiFi but no screens loaded yet (openHAB not set up or unreachable).
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
    lv_obj_set_style_text_color(s_wifi, lv_color_hex(on_wifi ? COL_OK : w.ap_active ? COL_WARN : COL_ERR), 0);
    lv_obj_set_style_text_color(s_oh, lv_color_hex(o.connected ? COL_OK : !cfg->oh_url[0] ? COL_MUTED : COL_ERR), 0);

    bool setup = w.ap_active && !on_wifi;
    bool tiles = !setup && tiles_ready();
    lv_obj_set_hidden(s_setup, !setup);
    lv_obj_set_hidden(s_tiles_panel, !tiles);
    lv_obj_set_hidden(s_info, setup || tiles);
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
                                  "WiFi:  %s  (%s, %d dBm)\nopenHAB:  %s\n\nSettings:  http://%s/",
                                  w.ssid, w.ip, w.rssi, ohline, w.ip);
        } else {
            lv_label_set_text_fmt(s_info_body, "Connecting to WiFi \"%s\" ...", w.ssid);
        }
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
    esp_err_t err = waveshare_esp32_s3_rgb_lcd_init(&panel, &touch);
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

// Developer aid: prints the frame buffer as base64 of run-length pairs (u16 count, u16
// RGB565 pixel, little endian) between FBDUMP-BEGIN/END lines. tools/fbdump.py turns it
// into a PNG. The UI is mostly flat colour, so a screen is typically 20-60 KB.
void ui_dump_framebuffer(void)
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    void *fbv = NULL;
    if (!s_panel || esp_lcd_rgb_panel_get_frame_buffer(s_panel, 1, &fbv) != ESP_OK || !fbv) {
        printf("no frame buffer\n");
        return;
    }
    const uint16_t *fb = fbv;
    const int n = LCD_H_RES * LCD_V_RES;
    esp_log_level_t lvl = esp_log_level_get("*");
    esp_log_level_set("*", ESP_LOG_NONE);   // log lines inside the dump would corrupt it
    printf("FBDUMP-BEGIN %d %d\n", LCD_H_RES, LCD_V_RES);
    uint8_t chunk[56];   // 14 whole pairs per line: a lost line cannot misalign the rest
    int used = 0;
    char line[80];
    int i = 0;
    while (i < n) {
        uint16_t px = fb[i];
        int run = 1;
        while (i + run < n && run < 65535 && fb[i + run] == px) run++;
        i += run;
        uint8_t pair[4] = {run & 0xFF, run >> 8, px & 0xFF, px >> 8};
        for (int k = 0; k < 4; k++) {
            chunk[used++] = pair[k];
            if (used == sizeof(chunk) || (i >= n && k == 3)) {
                int o = 0;
                for (int j = 0; j < used; j += 3) {
                    uint32_t v = chunk[j] << 16 | (j + 1 < used ? chunk[j + 1] << 8 : 0) | (j + 2 < used ? chunk[j + 2] : 0);
                    line[o++] = b64[(v >> 18) & 63];
                    line[o++] = b64[(v >> 12) & 63];
                    line[o++] = j + 1 < used ? b64[(v >> 6) & 63] : '=';
                    line[o++] = j + 2 < used ? b64[v & 63] : '=';
                }
                line[o] = '\0';
                printf("%s\n", line);
                used = 0;
            }
        }
    }
    printf("FBDUMP-END\n");
    esp_log_level_set("*", lvl);
}

void ui_show_tab(int tab, int scroll_y)
{
    if (lvgl_port_lock(1000)) {
        tiles_show(tab, scroll_y);
        lvgl_port_unlock();
    }
}

// Developer aid: sends a click to the clickable object at (x, y), as a finger tap would.
// Sliders need dragging and are not covered.
void ui_tap(int x, int y)
{
    if (!lvgl_port_lock(1000)) return;
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
    if (!lvgl_port_lock(1000)) return;
    if (on && !s_tt_layer) {
        s_tt_layer = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_tt_layer);
        lv_obj_set_size(s_tt_layer, 800, 480);
        lv_obj_set_style_bg_color(s_tt_layer, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_tt_layer, LV_OPA_COVER, 0);
        lv_obj_set_clickable(s_tt_layer, true);   // swallow touches: nothing underneath reacts
        for (int y = 0; y < 480; y += 80) {
            for (int x = 0; x < 800; x += 100) {
                lv_obj_t *l = lv_label_create(s_tt_layer);
                lv_obj_set_style_text_color(l, lv_color_hex(0x606870), 0);
                lv_label_set_text_fmt(l, "+ %d,%d", x + 50, y + 40);
                lv_obj_set_pos(l, x + 44, y + 30);
            }
        }
        s_tt_label = lv_label_create(s_tt_layer);
        lv_obj_set_style_text_font(s_tt_label, &lv_font_montserrat_28, 0);
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
    if (!lvgl_port_lock(1000)) return;
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
    if (!s_touch) {
        printf("no touch controller\n");
        return;
    }
    uint8_t cfg[0x8101 - 0x8047], info[11];
    if (!lvgl_port_lock(1000)) return;
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
