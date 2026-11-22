// Round dimmer screen (boards with BOARD_ROUND, e.g. the ESP32-2424S012), implementing the
// ui_tiles.h interface in place of the tile grid.
//
// It shows one dimmable item at a time: every Dimmer and Color item (and Dimmer group) on
// the configured screens, in screen order. A ring around the edge sets the brightness
// (sent when the finger lifts), tapping the middle toggles ON/OFF, and swiping left/right
// goes to the next/previous dimmer. Feedback as on the tiles (REQ-TILE-5): the ring turns
// yellow while a command waits for openHAB's new state and flashes red when none comes.
#include "ui_tiles.h"
#include "ui_layout.h"
#if BOARD_ROUND

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "wp_config.h"
#include "wp_oh.h"

static const char *TAG = "dimmer";

#define MAX_DIMMERS  16
#define PENDING_US   (3LL * 1000 * 1000)   // REQ-TILE-5
#define FLASH_US     (1200LL * 1000)

#define RING_SIZE    228
#define RING_W       16
#define KNOB_SIZE    128   // the round toggle button in the middle
#define NAME_Y       200   // centre of the name, in the ring's opening at the bottom
#define DOTS_Y       222

#define COL_BG       0x101418
#define COL_TILE     0x1c232b
#define COL_TEXT     0xe8eef4
#define COL_DARK     0x101418
#define COL_MUTED    0x8a96a3
#define COL_LIGHT_ON 0xe0a33a
#define COL_WARN     0xf2b33d
#define COL_ERR      0xe5534b

typedef struct {
    int screen, index;                   // position in the model
    char name[WP_ITEM_NAME_MAX + 1];
    char label[WP_LABEL_MAX + 1];
    char state[WP_STATE_MAX + 1];
} dimmer_t;

static lv_obj_t *s_parent, *s_ring, *s_btn, *s_value, *s_icon, *s_name, *s_dots, *s_none, *s_overlay;
static dimmer_t s_dim[MAX_DIMMERS];
static int s_count, s_cur;
static uint32_t s_built_version;
static volatile bool s_dirty_model = true, s_dirty_state = true, s_dirty_conn = true;
static bool s_went_home, s_swiped;

// feedback for the current dimmer
static bool s_pending, s_has_target;
static int64_t s_pending_until, s_flash_until;
static char s_pending_from[WP_STATE_MAX + 1];
static int s_target;

// what is on screen (to avoid redundant LVGL updates)
static int s_shown_cur = -1, s_shown_value = -2, s_shown_ring = -1, s_shown_look = -1;

// ---- helpers ----

static bool bad_state(const char *s)
{
    return !s[0] || !strcmp(s, "NULL") || !strcmp(s, "UNDEF");
}

// 0..100 for Dimmer and Color (third HSB value); -1 if unknown.
static int percent_of(const char *s)
{
    if (bad_state(s)) return -1;
    if (!strcmp(s, "ON")) return 100;
    if (!strcmp(s, "OFF")) return 0;
    const char *last = strrchr(s, ',');
    float v = strtof(last ? last + 1 : s, NULL);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return (int)lroundf(v);
}

static void reset_feedback(void)
{
    s_pending = s_has_target = false;
    s_flash_until = 0;
}

static void select_dimmer(int i)
{
    if (s_count == 0) return;
    s_cur = ((i % s_count) + s_count) % s_count;
    reset_feedback();
}

// ---- commands ----

static void send(const char *cmd)
{
    dimmer_t *d = &s_dim[s_cur];
    esp_err_t err = wp_oh_command(d->name, cmd);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "command %s %s not queued: %s", d->name, cmd, esp_err_to_name(err));
        s_pending = s_has_target = false;
        s_flash_until = esp_timer_get_time() + FLASH_US;
        return;
    }
    s_pending = true;
    s_pending_until = esp_timer_get_time() + PENDING_US;
    strlcpy(s_pending_from, d->state, sizeof(s_pending_from));
}

static void on_btn(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_swiped = false;
    } else if (code == LV_EVENT_CLICKED && !s_swiped && s_count) {
        s_has_target = false;
        send(percent_of(s_dim[s_cur].state) > 0 ? "OFF" : "ON");
    }
}

static void on_ring(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (!s_count) return;
    if (code == LV_EVENT_VALUE_CHANGED) {
        s_shown_value = -2;   // show the value under the finger (render_current)
    } else if (code == LV_EVENT_RELEASED) {
        int v = lv_arc_get_value(s_ring);
        char cmd[8];
        snprintf(cmd, sizeof(cmd), "%d", v);
        s_has_target = true;
        s_target = v;
        send(cmd);
    }
}

static void on_gesture(lv_event_t *e)
{
    if (s_count < 2) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
        s_swiped = true;   // no toggle when the swipe started on the button
        lv_indev_wait_release(lv_indev_active());
        select_dimmer(s_cur + (dir == LV_DIR_LEFT ? 1 : -1));
    }
}

// ---- building ----

static lv_obj_t *mk_label(lv_obj_t *par, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(par);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static void build_widgets(lv_obj_t *p)
{
    lv_obj_add_event_cb(p, on_gesture, LV_EVENT_GESTURE, NULL);

    s_ring = lv_arc_create(p);
    lv_obj_set_size(s_ring, RING_SIZE, RING_SIZE);
    lv_obj_center(s_ring);
    lv_arc_set_rotation(s_ring, 135);
    lv_arc_set_bg_angles(s_ring, 0, 270);
    lv_arc_set_range(s_ring, 0, 100);
    lv_obj_set_style_arc_width(s_ring, RING_W, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, RING_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(COL_TILE), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ring, 2, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s_ring, lv_color_hex(COL_TEXT), LV_PART_KNOB);
    lv_obj_set_adv_hittest(s_ring, true);        // only the ring itself is touchable
    lv_obj_set_gesture_bubble(s_ring, false);   // dragging it is not a swipe
    lv_obj_add_event_cb(s_ring, on_ring, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_ring, on_ring, LV_EVENT_RELEASED, NULL);

    s_btn = lv_button_create(p);
    lv_obj_set_size(s_btn, KNOB_SIZE, KNOB_SIZE);
    lv_obj_center(s_btn);
    lv_obj_set_style_radius(s_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(s_btn, 0, 0);
    lv_obj_add_event_cb(s_btn, on_btn, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_btn, on_btn, LV_EVENT_CLICKED, NULL);
    s_value = mk_label(s_btn, &lv_font_montserrat_40, COL_TEXT);
    lv_obj_align(s_value, LV_ALIGN_CENTER, 0, -8);
    s_icon = mk_label(s_btn, FONT_TEXT, COL_TEXT);
    lv_label_set_text(s_icon, LV_SYMBOL_POWER);
    lv_obj_align(s_icon, LV_ALIGN_CENTER, 0, 32);

    s_name = mk_label(p, FONT_TEXT, COL_TEXT);
    lv_obj_set_width(s_name, 124);
    lv_obj_set_style_text_align(s_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_name, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_align(s_name, LV_ALIGN_TOP_MID, 0, NAME_Y - 9);

    s_dots = mk_label(p, &lv_font_montserrat_14, COL_MUTED);
    lv_obj_align(s_dots, LV_ALIGN_TOP_MID, 0, DOTS_Y - 8);

    s_none = mk_label(p, FONT_TEXT, COL_MUTED);
    lv_obj_set_width(s_none, 170);
    lv_obj_set_style_text_align(s_none, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_none, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(s_none, "No dimmers on this panel's screens");
    lv_obj_center(s_none);
}

static void rebuild(void)
{
    char keep[WP_ITEM_NAME_MAX + 1] = "";
    if (s_count) strlcpy(keep, s_dim[s_cur].name, sizeof(keep));

    const wp_oh_model_t *m = wp_oh_model_lock();
    s_count = 0;
    int total = 0;
    for (int si = 0; si < m->screen_count; si++) {
        const wp_oh_screen_t *s = &m->screens[si];
        for (int i = 0; i < s->count; i++) {
            const wp_tile_t *t = &s->tiles[i];
            if (t->read_only || (t->kind != WP_TILE_DIMMER && t->kind != WP_TILE_COLOR)) continue;
            total++;
            if (s_count == MAX_DIMMERS) continue;
            dimmer_t *d = &s_dim[s_count++];
            d->screen = si;
            d->index = i;
            strlcpy(d->name, t->name, sizeof(d->name));
            strlcpy(d->label, t->label[0] ? t->label : t->name, sizeof(d->label));
            strlcpy(d->state, t->state, sizeof(d->state));
        }
    }
    s_built_version = m->version;
    wp_oh_model_unlock();
    if (total > MAX_DIMMERS) ESP_LOGW(TAG, "%d dimmers, showing the first %d", total, MAX_DIMMERS);

    int cur = 0;
    for (int i = 0; i < s_count && keep[0]; i++) {
        if (!strcmp(s_dim[i].name, keep)) cur = i;
    }
    s_cur = cur;
    if (!keep[0] || strcmp(keep, s_count ? s_dim[cur].name : "")) reset_feedback();

    bool any = s_count > 0;
    lv_obj_set_hidden(s_ring, !any);
    lv_obj_set_hidden(s_btn, !any);
    lv_obj_set_hidden(s_name, !any);
    lv_obj_set_hidden(s_dots, s_count < 2);
    lv_obj_set_hidden(s_none, any);
    s_shown_cur = s_shown_value = s_shown_ring = s_shown_look = -2;
    s_dirty_state = true;
    ESP_LOGI(TAG, "%d dimmers", s_count);
}

// ---- syncing ----

static void render_current(int64_t now)
{
    if (!s_count) return;
    dimmer_t *d = &s_dim[s_cur];

    if (s_pending && strcmp(d->state, s_pending_from) != 0) {
        s_pending = s_has_target = false;   // openHAB reported a new state: confirmed
    }
    if (s_pending && now > s_pending_until) {
        s_pending = s_has_target = false;   // REQ-TILE-5: back to the real state, flash an error
        s_flash_until = now + FLASH_US;
        ESP_LOGW(TAG, "no new state for %s within 3 s", d->name);
    }

    if (s_cur != s_shown_cur) {
        s_shown_cur = s_cur;
        lv_label_set_text(s_name, d->label);
        char dots[3 * MAX_DIMMERS + 1];
        int o = 0;
        for (int i = 0; i < s_count && o < (int)sizeof(dots) - 4; i++) {
            o += snprintf(dots + o, sizeof(dots) - o, "%s", i == s_cur ? LV_SYMBOL_BULLET : ".");
        }
        lv_label_set_text(s_dots, dots);
        s_shown_value = s_shown_ring = s_shown_look = -2;
    }

    bool held = lv_obj_has_state(s_ring, LV_STATE_PRESSED);
    int p = percent_of(d->state);
    int value = held ? lv_arc_get_value(s_ring) : s_has_target ? s_target : p;
    if (value != s_shown_value) {
        s_shown_value = value;
        if (value < 0) lv_label_set_text(s_value, "-");
        else lv_label_set_text_fmt(s_value, "%d%%", value);
    }
    if (!held && !s_pending) {
        int ring = p < 0 ? 0 : p;
        if (ring != s_shown_ring) {
            s_shown_ring = ring;
            lv_arc_set_value(s_ring, ring);
        }
    }

    // look: bit0 on, bit1 pending, bit2 error flash
    bool on = (held || s_has_target ? value : p) > 0;
    int look = (on ? 1 : 0) | (s_pending ? 2 : 0) | (now < s_flash_until ? 4 : 0);
    if (look != s_shown_look) {
        s_shown_look = look;
        uint32_t ring = (look & 4) ? COL_ERR : (look & 2) ? COL_WARN : on ? COL_LIGHT_ON : COL_MUTED;
        lv_obj_set_style_arc_color(s_ring, lv_color_hex(ring), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_btn, lv_color_hex(on ? COL_LIGHT_ON : COL_TILE), 0);
        uint32_t fg = on ? COL_DARK : COL_TEXT;
        lv_obj_set_style_text_color(s_value, lv_color_hex(fg), 0);
        lv_obj_set_style_text_color(s_icon, lv_color_hex(on ? COL_DARK : COL_MUTED), 0);
    }
}

static void sync_states(void)
{
    const wp_oh_model_t *m = wp_oh_model_lock();
    if (m->version != s_built_version) {
        s_dirty_model = true;
        wp_oh_model_unlock();
        return;
    }
    for (int i = 0; i < s_count; i++) {
        const wp_tile_t *t = &m->screens[s_dim[i].screen].tiles[s_dim[i].index];
        strlcpy(s_dim[i].state, t->state, sizeof(s_dim[i].state));
    }
    wp_oh_model_unlock();
}

static void on_oh_event(wp_oh_event_t ev, const char *item, void *ctx)
{
    // openHAB client task: no LVGL here, only flags for tiles_tick()
    if (ev == WP_OH_EV_MODEL) s_dirty_model = true;
    else if (ev == WP_OH_EV_STATE) s_dirty_state = true;
    else s_dirty_conn = true;
}

// ---- public (ui_tiles.h) ----

void tiles_init(lv_obj_t *parent, lv_obj_t *status_box)
{
    (void)status_box;   // no status bar on the round screen
    s_parent = parent;
    build_widgets(parent);

    // REQ-OH-5: greyed out and not touchable while openHAB is offline.
    s_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_70, 0);
    lv_obj_set_clickable(s_overlay, true);   // swallows touches
    lv_obj_t *l = mk_label(s_overlay, FONT_TEXT, COL_TEXT);
    lv_label_set_text(l, "openHAB offline");
    lv_obj_center(l);
    lv_obj_set_hidden(s_overlay, true);
    wp_oh_set_listener(on_oh_event, NULL);
}

bool tiles_ready(void)
{
    return s_built_version != 0;
}

void tiles_tick(void)
{
    if (s_dirty_model) {
        const wp_oh_model_t *m = wp_oh_model_lock();
        uint32_t v = m->version;
        wp_oh_model_unlock();
        s_dirty_model = false;
        if (v != 0 && v != s_built_version) rebuild();
    }
    if (!s_built_version) return;
    if (s_dirty_conn) {
        s_dirty_conn = false;
        wp_oh_status_t st;
        wp_oh_get_status(&st);
        lv_obj_set_hidden(s_overlay, st.connected);
    }
    if (s_dirty_state) {
        s_dirty_state = false;
        sync_states();
    }
    render_current(esp_timer_get_time());

    // REQ-SCR-7: back to the first dimmer after home_s seconds without a touch.
    uint32_t idle_ms = lv_display_get_inactive_time(NULL);
    uint32_t home_ms = (uint32_t)wp_config_get()->home_timeout_s * 1000;
    if (home_ms && idle_ms > home_ms) {
        if (!s_went_home && s_cur != 0) select_dimmer(0);
        s_went_home = true;
    } else {
        s_went_home = false;
    }
}

void tiles_show(int tab, int scroll_y)
{
    (void)scroll_y;
    select_dimmer(tab);   // console 'tab <n>': show dimmer n
}

#endif // BOARD_ROUND
