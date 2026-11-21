/* oh-wallpanel, milestone 1: display/touch/backlight bring-up test screen. */
#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp/board.h"
#include "bsp/lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "wp";
static lv_obj_t *s_coord, *s_cross_h, *s_cross_v, *s_dot;
static bool s_bl_on = true;

static void screen_touch_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_label_set_text_fmt(s_coord, "touch x=%d y=%d", (int)p.x, (int)p.y);
    lv_obj_set_pos(s_cross_h, 0, p.y);
    lv_obj_set_pos(s_cross_v, p.x, 0);
    lv_obj_set_pos(s_dot, p.x - 10, p.y - 10);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) ESP_LOGI(TAG, "touch %d,%d", (int)p.x, (int)p.y);
}

static void bl_timer_cb(lv_timer_t *t)
{
    waveshare_rgb_lcd_bl_on();
    s_bl_on = true;
    lv_timer_delete(t);
}

static void bl_btn_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "backlight off for 3 s");
    waveshare_rgb_lcd_bl_off();
    s_bl_on = false;
    lv_timer_create(bl_timer_cb, 3000, NULL);
}

static lv_obj_t *line(lv_obj_t *par, int w, int h, lv_color_t c)
{
    lv_obj_t *o = lv_obj_create(par);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_clickable(o, false);
    return o;
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
    lv_obj_set_scrollable(scr, false);

    /* Colour bars: red, green, blue, white, plus a grey ramp, to check RGB order and bit depth. */
    static const uint32_t bars[] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF, 0xFFFF00, 0x00FFFF, 0xFF00FF, 0x000000};
    static const char *names[] = {"R", "G", "B", "W", "Y", "C", "M", "K"};
    for (int i = 0; i < 8; i++) {
        lv_obj_t *b = line(scr, 100, 120, lv_color_hex(bars[i]));
        lv_obj_set_pos(b, i * 100, 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, names[i]);
        lv_obj_set_style_text_color(l, lv_color_hex(i == 3 || i == 4 || i == 5 ? 0x000000 : 0xFFFFFF), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
        lv_obj_center(l);
    }
    for (int i = 0; i < 32; i++) {
        uint8_t v = i * 255 / 31;
        lv_obj_set_pos(line(scr, 25, 30, lv_color_make(v, v, v)), i * 25, 120);
    }

    /* Corner markers: tap them to check touch scaling at the edges. */
    lv_obj_set_pos(line(scr, 8, 8, lv_color_hex(0xFF8000)), 0, 472);
    lv_obj_set_pos(line(scr, 8, 8, lv_color_hex(0xFF8000)), 792, 472);

    s_coord = lv_label_create(scr);
    lv_obj_set_style_text_color(s_coord, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_coord, &lv_font_montserrat_28, 0);
    lv_label_set_text(s_coord, "oh-wallpanel bring-up: touch anywhere");
    lv_obj_set_pos(s_coord, 20, 170);

    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 260, 70);
    lv_obj_set_pos(btn, 520, 380);
    lv_obj_add_event_cb(btn, bl_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Backlight off 3 s");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_20, 0);
    lv_obj_center(bl);

    lv_obj_t *sl = lv_slider_create(scr);
    lv_obj_set_size(sl, 420, 24);
    lv_obj_set_pos(sl, 40, 410);

    s_cross_h = line(scr, 800, 1, lv_color_hex(0x00FF80));
    s_cross_v = line(scr, 1, 480, lv_color_hex(0x00FF80));
    s_dot = line(scr, 20, 20, lv_color_hex(0x00FF80));
    lv_obj_set_style_radius(s_dot, 10, 0);
    lv_obj_set_pos(s_cross_h, 0, -10);
    lv_obj_set_pos(s_cross_v, -10, 0);
    lv_obj_set_pos(s_dot, -30, -30);

    lv_obj_set_clickable(scr, true);
    lv_obj_add_event_cb(scr, screen_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr, screen_touch_cb, LV_EVENT_PRESSING, NULL);
}

void app_main(void)
{
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_touch_handle_t touch = NULL;
    ESP_ERROR_CHECK(waveshare_esp32_s3_rgb_lcd_init(&panel, &touch));
    if (!touch) ESP_LOGE(TAG, "touch controller not found");
    ESP_ERROR_CHECK(lvgl_port_init(panel, touch));
    if (waveshare_rgb_lcd_bl_on() != ESP_OK) ESP_LOGW(TAG, "backlight on failed");
    if (lvgl_port_lock(-1)) {
        build_ui();
        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "bring-up UI running; free internal %u, PSRAM %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
