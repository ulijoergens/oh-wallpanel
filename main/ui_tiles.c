// Tile screens, see ui_tiles.h.
//
// The openHAB client calls our listener from its own tasks; the listener only sets flags.
// tiles_tick() (LVGL task) then rebuilds or syncs from the model under its lock.
#include "ui_tiles.h"
#include "ui_layout.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "wp_config.h"
#include "wp_oh.h"

static const char *TAG = "tiles";

#define MAX_UI_TILES ((WP_MAX_SCREENS) * (WP_MAX_TILES + 1))   // +1: "All off"
#define MAX_STATUS   4                   // REQ-STAT-1
#define PENDING_US   (3LL * 1000 * 1000) // REQ-TILE-5
#define FLASH_US     (1200LL * 1000)

#define COL_BG       0x101418
#define COL_TILE     0x1c232b
#define COL_TILE_PR  0x2a333d
#define COL_TEXT     0xe8eef4
#define COL_DARK     0x101418
#define COL_MUTED    0x8a96a3
#define COL_LIGHT_ON 0xe0a33a            // lights that are on
#define COL_ON       0x2f7fd8            // other switches that are on
#define COL_SCENE    0x24303c
#define COL_ACCENT   0x5aa9ff
#define COL_WARN     0xf2b33d
#define COL_ERR      0xe5534b
#define COL_OK       0x3ccf6e

typedef enum { U_TILE, U_ALL_OFF } utype_t;

typedef struct {
    utype_t type;
    int screen, index;                   // position in the model
    wp_tile_kind_t kind;
    bool is_rule, light;
    char name[WP_ITEM_NAME_MAX + 1];
    float min, max, step;
    lv_obj_t *obj, *icon, *label, *value, *slider;

    char state[WP_STATE_MAX + 1];        // latest from the model
    char display[WP_STATE_MAX + 1];

    // what is on screen now (to avoid redundant LVGL updates)
    char shown_text[WP_STATE_MAX + 1];
    int shown_look;                      // -1 = never drawn
    int shown_slider;

    // REQ-TILE-5
    bool pending;
    int64_t pending_until;
    char pending_from[WP_STATE_MAX + 1];
    bool has_target;                     // setpoint/slider value sent, shown until confirmed
    float target;
    int64_t flash_until;                 // error (red) or scene feedback
    bool flash_error;
} tile_ui_t;

typedef struct {
    int index;
    lv_obj_t *label;
    char shown[WP_LABEL_MAX + WP_STATE_MAX + 4];
    int shown_alert;
} status_ui_t;

// Tile geometry of one screen: the normal size, or smaller in the compact layout.
typedef struct {
    int w, h, pad;
    const lv_font_t *text, *big;
    int btn_h, roller_w, setpt_w;
} geom_t;

static lv_obj_t *s_parent, *s_status_box, *s_tv, *s_overlay;
static tile_ui_t *s_tiles;               // PSRAM
static int s_ntiles;
static status_ui_t s_status[MAX_STATUS];
static int s_nstatus;
static uint32_t s_built_version;
static bool s_built_compact;
static volatile bool s_dirty_model = true, s_dirty_state = true, s_dirty_conn = true;
static bool s_went_home;

// ---- helpers ----

static bool bad_state(const char *s)
{
    return !s[0] || !strcmp(s, "NULL") || !strcmp(s, "UNDEF");
}

// 0..100 for Dimmer, Color (third HSB value) and Rollershutter; -1 if unknown.
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

static bool is_on(const tile_ui_t *u)
{
    switch (u->kind) {
    case WP_TILE_SWITCH:
        return !strcmp(u->state, "ON");
    case WP_TILE_DIMMER:
    case WP_TILE_COLOR:
        return percent_of(u->state) > 0;
    default:
        return false;
    }
}

static int decimals_for(float step)
{
    if (!(step > 0)) return 1;
    for (int d = 0; d < 3; d++) {
        float scaled = step * powf(10, d);
        if (fabsf(scaled - roundf(scaled)) < 0.001f) return d;
    }
    return 2;
}

// "16 °C" -> 16, unit "°C". Returns false if there is no number.
static bool split_quantity(const char *s, float *v, char *unit, size_t ulen)
{
    if (bad_state(s)) return false;
    char *end;
    *v = strtof(s, &end);
    if (end == s) return false;
    while (*end == ' ') end++;
    strlcpy(unit, end, ulen);
    return true;
}

// ---- commands ----

static void start_pending(tile_ui_t *u)
{
    u->pending = true;
    u->pending_until = esp_timer_get_time() + PENDING_US;
    strlcpy(u->pending_from, u->state, sizeof(u->pending_from));
}

static void flash(tile_ui_t *u, bool error)
{
    u->flash_until = esp_timer_get_time() + FLASH_US;
    u->flash_error = error;
}

static void send(tile_ui_t *u, const char *cmd, bool wait_for_state)
{
    esp_err_t err = wp_oh_command(u->name, cmd);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "command %s %s not queued: %s", u->name, cmd, esp_err_to_name(err));
        flash(u, true);
        return;
    }
    if (wait_for_state) start_pending(u);
    else flash(u, false);
}

static void on_tile_clicked(lv_event_t *e)
{
    tile_ui_t *u = lv_event_get_user_data(e);
    if (u->type == U_ALL_OFF) {
        // REQ-TILE-4: OFF to every switchable light on this screen.
        bool ok = true;
        for (int i = 0; i < s_ntiles; i++) {
            tile_ui_t *t = &s_tiles[i];
            if (t->type == U_TILE && t->screen == u->screen && t->light) {
                if (wp_oh_command(t->name, "OFF") != ESP_OK) ok = false;
                else if (is_on(t)) start_pending(t);
            }
        }
        flash(u, !ok);
        return;
    }
    switch (u->kind) {
    case WP_TILE_SWITCH:
    case WP_TILE_DIMMER:
    case WP_TILE_COLOR:
        u->has_target = false;
        send(u, is_on(u) ? "OFF" : "ON", true);
        break;
    case WP_TILE_SCENE:
        if (u->is_rule) {
            esp_err_t err = wp_oh_run_rule(u->name);
            flash(u, err != ESP_OK);
        } else {
            send(u, "ON", false);
        }
        break;
    default:
        break;
    }
}

static void on_slider_released(lv_event_t *e)
{
    tile_ui_t *u = lv_event_get_user_data(e);
    int v = lv_slider_get_value(u->slider);
    char cmd[8];
    snprintf(cmd, sizeof(cmd), "%d", v);
    u->has_target = true;
    u->target = v;
    send(u, cmd, true);
}

static void on_roller_btn(lv_event_t *e)
{
    tile_ui_t *u = lv_event_get_user_data(e);
    const char *cmd = lv_obj_get_user_data(lv_event_get_target_obj(e));
    send(u, cmd, false);   // a moving shutter may not report a new position at once
}

static void on_setpoint_btn(lv_event_t *e)
{
    tile_ui_t *u = lv_event_get_user_data(e);
    int dir = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    float cur;
    char unit[16] = "";
    if (u->has_target) {
        cur = u->target;   // quick repeated taps build on the value already sent
        float dummy;
        split_quantity(u->state, &dummy, unit, sizeof(unit));
    } else if (!split_quantity(u->state, &cur, unit, sizeof(unit))) {
        flash(u, true);
        return;
    }
    float next = roundf((cur + dir * u->step) / u->step) * u->step;
    if (!isnan(u->min) && next < u->min) next = u->min;
    if (!isnan(u->max) && next > u->max) next = u->max;
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "%.*f%s%s", decimals_for(u->step), next, unit[0] ? " " : "", unit);
    u->has_target = true;
    u->target = next;
    send(u, cmd, true);
}

// ---- building ----

static lv_obj_t *mk_label(lv_obj_t *par, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(par);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static lv_obj_t *mk_button(lv_obj_t *par, const char *text, int w, int h, const lv_font_t *font)
{
    lv_obj_t *b = lv_button_create(par);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_TILE_PR), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = mk_label(b, font, COL_TEXT);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

static const char *icon_for(const wp_tile_t *t, bool all_off)
{
    static const struct { const char *name, *sym; } map[] = {
        {"light", LV_SYMBOL_CHARGE}, {"power", LV_SYMBOL_POWER}, {"home", LV_SYMBOL_HOME},
        {"bell", LV_SYMBOL_BELL}, {"alarm", LV_SYMBOL_WARNING}, {"warning", LV_SYMBOL_WARNING},
        {"play", LV_SYMBOL_PLAY}, {"settings", LV_SYMBOL_SETTINGS}, {"water", LV_SYMBOL_TINT},
        {"wifi", LV_SYMBOL_WIFI}, {"eye", LV_SYMBOL_EYE_OPEN}, {"location", LV_SYMBOL_GPS},
        {"battery", LV_SYMBOL_BATTERY_FULL}, {"camera", LV_SYMBOL_VIDEO}, {"audio", LV_SYMBOL_AUDIO},
        {"refresh", LV_SYMBOL_REFRESH}, {"up", LV_SYMBOL_UP}, {"down", LV_SYMBOL_DOWN},
    };
    if (all_off) return LV_SYMBOL_POWER;
    if (t->icon[0]) {
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
            if (!strcmp(t->icon, map[i].name)) return map[i].sym;
        }
    }
    switch (t->kind) {
    case WP_TILE_SWITCH: return t->light ? LV_SYMBOL_CHARGE : LV_SYMBOL_POWER;
    case WP_TILE_DIMMER:
    case WP_TILE_COLOR: return LV_SYMBOL_CHARGE;
    case WP_TILE_ROLLER: return LV_SYMBOL_BARS;
    case WP_TILE_SETPOINT: return LV_SYMBOL_SETTINGS;
    case WP_TILE_SCENE: return LV_SYMBOL_PLAY;
    default: return "";
    }
}

static lv_obj_t *mk_tile(lv_obj_t *page, tile_ui_t *u, bool clickable, const geom_t *g)
{
    lv_obj_t *o = lv_obj_create(page);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, g->w, g->h);
    lv_obj_set_style_radius(o, 14, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(COL_TILE), 0);
    lv_obj_set_style_border_color(o, lv_color_hex(COL_WARN), 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, g->pad, 0);
    lv_obj_set_scrollable(o, false);
    if (clickable) {
        lv_obj_set_clickable(o, true);
        lv_obj_set_style_bg_color(o, lv_color_hex(COL_TILE_PR), LV_STATE_PRESSED);
        lv_obj_add_event_cb(o, on_tile_clicked, LV_EVENT_CLICKED, u);
    } else {
        lv_obj_set_clickable(o, false);
    }
    lv_obj_set_scroll_on_focus(o, true);
    u->obj = o;
    return o;
}

static void build_tile(lv_obj_t *page, tile_ui_t *u, const wp_tile_t *t, const geom_t *g)
{
    u->type = U_TILE;
    u->kind = t->kind;
    u->is_rule = t->is_rule;
    u->light = t->light;
    u->min = t->min;
    u->max = t->max;
    u->step = t->step > 0 ? t->step : 0.5f;
    strlcpy(u->name, t->name, sizeof(u->name));
    strlcpy(u->state, t->state, sizeof(u->state));
    strlcpy(u->display, t->display, sizeof(u->display));

    bool clickable = t->kind == WP_TILE_SWITCH || t->kind == WP_TILE_DIMMER || t->kind == WP_TILE_COLOR ||
                     t->kind == WP_TILE_SCENE;
    lv_obj_t *o = mk_tile(page, u, clickable, g);
    const char *sym = icon_for(t, false);

    u->label = mk_label(o, g->text, COL_TEXT);
    lv_label_set_text(u->label, t->label);
    lv_obj_set_width(u->label, g->w - 2 * g->pad);
    lv_label_set_long_mode(u->label, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_max_lines(u->label, 2);   // longer labels end in "..."

    if (sym[0] && t->kind != WP_TILE_SETPOINT) {
        u->icon = mk_label(o, g->big, COL_MUTED);
        lv_label_set_text(u->icon, sym);
        lv_obj_align(u->icon, LV_ALIGN_TOP_LEFT, 0, 0);
    }

    switch (t->kind) {
    case WP_TILE_SWITCH:
        u->value = mk_label(o, g->text, COL_MUTED);
        lv_obj_align(u->value, LV_ALIGN_TOP_RIGHT, 0, 4);
        lv_obj_align(u->label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        break;
    case WP_TILE_DIMMER:
    case WP_TILE_COLOR:
        u->value = mk_label(o, g->text, COL_MUTED);
        lv_obj_align(u->value, LV_ALIGN_TOP_RIGHT, 0, 4);
        lv_obj_align(u->label, LV_ALIGN_LEFT_MID, 0, -4);
        u->slider = lv_slider_create(o);
        lv_slider_set_range(u->slider, 0, 100);
        lv_obj_set_size(u->slider, g->w - 2 * g->pad - 12, 14);
        lv_obj_align(u->slider, LV_ALIGN_BOTTOM_MID, 0, -8);
        // readable on both the dark (off) and the amber (on) tile
        lv_obj_set_style_bg_color(u->slider, lv_color_hex(COL_DARK), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(u->slider, LV_OPA_40, LV_PART_MAIN);
        lv_obj_set_style_bg_color(u->slider, lv_color_hex(COL_TEXT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(u->slider, lv_color_hex(COL_TEXT), LV_PART_KNOB);
        lv_obj_set_style_shadow_width(u->slider, 6, LV_PART_KNOB);
        lv_obj_set_style_shadow_color(u->slider, lv_color_hex(COL_DARK), LV_PART_KNOB);
        lv_obj_set_ext_click_area(u->slider, 16);
        lv_obj_add_event_cb(u->slider, on_slider_released, LV_EVENT_RELEASED, u);
        break;
    case WP_TILE_ROLLER: {
        u->value = mk_label(o, g->text, COL_MUTED);
        lv_obj_align(u->value, LV_ALIGN_TOP_RIGHT, 0, 4);
        lv_obj_align(u->label, LV_ALIGN_LEFT_MID, 0, -12);
        static const char *cmds[] = {"UP", "STOP", "DOWN"};
        static const char *syms[] = {LV_SYMBOL_UP, LV_SYMBOL_STOP, LV_SYMBOL_DOWN};
        for (int i = 0; i < 3; i++) {
            lv_obj_t *b = mk_button(o, syms[i], g->roller_w, g->btn_h, g->text);
            lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, i * (g->roller_w + 6), 0);
            lv_obj_set_user_data(b, (void *)cmds[i]);
            lv_obj_add_event_cb(b, on_roller_btn, LV_EVENT_CLICKED, u);
        }
        break;
    }
    case WP_TILE_SETPOINT: {
        lv_obj_align(u->label, LV_ALIGN_TOP_LEFT, 0, 0);
        u->value = mk_label(o, g->big, COL_TEXT);
        lv_obj_align(u->value, LV_ALIGN_CENTER, 0, 8);
        for (int i = 0; i < 2; i++) {
            lv_obj_t *b = mk_button(o, i ? LV_SYMBOL_PLUS : LV_SYMBOL_MINUS, g->setpt_w, g->btn_h, g->text);
            lv_obj_align(b, i ? LV_ALIGN_BOTTOM_RIGHT : LV_ALIGN_BOTTOM_LEFT, 0, 0);
            lv_obj_set_user_data(b, (void *)(intptr_t)(i ? 1 : -1));
            lv_obj_add_event_cb(b, on_setpoint_btn, LV_EVENT_CLICKED, u);
        }
        break;
    }
    case WP_TILE_SCENE:
        lv_obj_set_style_bg_color(o, lv_color_hex(COL_SCENE), 0);
        lv_obj_set_style_text_color(u->icon, lv_color_hex(COL_ACCENT), 0);
        lv_obj_align(u->label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        break;
    default:   // value
        lv_obj_set_style_text_color(u->label, lv_color_hex(COL_MUTED), 0);
        lv_obj_align(u->label, LV_ALIGN_TOP_LEFT, 0, 0);
        if (u->icon) lv_obj_set_hidden(u->icon, true);
        u->value = mk_label(o, g->big, COL_TEXT);
        lv_obj_set_width(u->value, g->w - 2 * g->pad);
        lv_label_set_long_mode(u->value, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_align(u->value, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        break;
    }
}

static void build_all_off(lv_obj_t *page, tile_ui_t *u, int screen, const geom_t *g)
{
    memset(u, 0, sizeof(*u));
    u->type = U_ALL_OFF;
    u->screen = screen;
    u->index = -1;
    u->shown_look = -1;
    lv_obj_t *o = mk_tile(page, u, true, g);
    lv_obj_set_style_bg_color(o, lv_color_hex(COL_SCENE), 0);
    u->icon = mk_label(o, g->big, COL_ACCENT);
    lv_label_set_text(u->icon, LV_SYMBOL_POWER);
    lv_obj_align(u->icon, LV_ALIGN_TOP_LEFT, 0, 0);
    u->label = mk_label(o, g->text, COL_TEXT);
    lv_label_set_text(u->label, "All lights off");
    lv_obj_align(u->label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static geom_t normal_geom(void)
{
    return (geom_t){TILE_W, TILE_H, TILE_PAD, FONT_TEXT, FONT_BIG, BTN_H, ROLLER_BTN_W, SETPT_BTN_W};
}

// REQ-SCR-8: the grid that shows n tiles in the page area (w x h) with the largest
// tiles. Screens that already fit at the normal size keep it; tiles never grow.
static geom_t compact_geom(int n, int w, int h)
{
    geom_t g = normal_geom();
    int rows_normal = (h - GAP) / (TILE_H + GAP);
    if (n <= TILE_COLS * rows_normal) return g;
    int best_w = 0, best_h = 0, best = 0;
    for (int cols = 1; cols <= n; cols++) {
        int rows = (n + cols - 1) / cols;
        int tw = (w - (cols + 1) * GAP) / cols, th = (h - (rows + 1) * GAP) / rows;
        if (tw > TILE_W) tw = TILE_W;
        if (th > TILE_H) th = TILE_H;
        int score = tw < th ? tw : th;
        if (score > best) {
            best = score;
            best_w = tw;
            best_h = th;
        }
    }
    // Too many tiles to fit usably: keep a minimum size and let the page scroll.
    g.w = best_w < 72 ? 72 : best_w;
    g.h = best_h < 56 ? 56 : best_h;
    int m = g.w < g.h ? g.w : g.h;
    g.pad = m >= 130 ? TILE_PAD : m >= 90 ? 8 : 6;
    g.text = (m >= 140 && FONT_TEXT == &lv_font_montserrat_20) ? &lv_font_montserrat_20 :
             m >= 90 ? &lv_font_montserrat_16 : &lv_font_montserrat_14;
    g.big = (g.h >= 130 && FONT_BIG == &lv_font_montserrat_28) ? &lv_font_montserrat_28 :
            g.h >= 80 ? &lv_font_montserrat_20 : &lv_font_montserrat_16;
    int inner_w = g.w - 2 * g.pad, inner_h = g.h - 2 * g.pad;
    g.btn_h = inner_h / 3 < BTN_H ? inner_h / 3 : BTN_H;
    if (g.btn_h < 24) g.btn_h = 24;
    g.roller_w = (inner_w - 12) / 3 < ROLLER_BTN_W ? (inner_w - 12) / 3 : ROLLER_BTN_W;
    g.setpt_w = (inner_w - 8) / 2 < SETPT_BTN_W ? (inner_w - 8) / 2 : SETPT_BTN_W;
    return g;
}

static void style_tab_bar(void)
{
    lv_obj_t *bar = lv_tabview_get_tab_bar(s_tv);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1c232b), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    uint32_t n = lv_obj_get_child_count(bar);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *b = lv_obj_get_child(bar, i);
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(b, lv_color_hex(COL_MUTED), 0);
        lv_obj_set_style_text_color(b, lv_color_hex(COL_ACCENT), LV_STATE_CHECKED);
        lv_obj_set_style_text_font(b, FONT_TEXT, 0);
        lv_obj_set_style_border_side(b, LV_BORDER_SIDE_TOP, LV_STATE_CHECKED);
        lv_obj_set_style_border_width(b, 3, LV_STATE_CHECKED);
        lv_obj_set_style_border_color(b, lv_color_hex(COL_ACCENT), LV_STATE_CHECKED);
    }
    if (n <= 1) lv_obj_set_hidden(bar, true);   // one screen: no tabs needed
}

static void rebuild(void)
{
    char active_tag[WP_TAG_MAX + 1] = "";
    const wp_oh_model_t *m = wp_oh_model_lock();
    if (s_tv && s_built_version) {
        uint32_t a = lv_tabview_get_tab_active(s_tv);
        // remember the screen by tag, so a reload does not jump back to Home
        for (int i = 0; i < s_ntiles; i++) {
            if (s_tiles[i].screen == (int)a && a < (uint32_t)m->screen_count) {
                strlcpy(active_tag, m->screens[a].tag, sizeof(active_tag));
                break;
            }
        }
    }
    if (s_tv) lv_obj_delete(s_tv);
    lv_obj_clean(s_status_box);
    s_ntiles = 0;
    s_nstatus = 0;

    s_tv = lv_tabview_create(s_parent);
    lv_obj_set_size(s_tv, lv_pct(100), lv_pct(100));
    lv_tabview_set_tab_bar_position(s_tv, LV_DIR_BOTTOM);
    lv_tabview_set_tab_bar_size(s_tv, TAB_H);
    lv_obj_set_style_bg_color(s_tv, lv_color_hex(COL_BG), 0);
    lv_obj_move_background(s_tv);

    int active = 0;
    bool compact = wp_config_get()->compact;
    // page area: below the status bar, above the tab bar (hidden with a single screen)
    int area_h = SCR_H - BAR_H - (m->screen_count > 1 ? TAB_H : 0);
    for (int si = 0; si < m->screen_count; si++) {
        const wp_oh_screen_t *s = &m->screens[si];
        int n = s->count + (s->lights >= 2 ? 1 : 0);
        geom_t g = compact ? compact_geom(n, SCR_W, area_h) : normal_geom();
        if (!strcmp(s->tag, active_tag)) active = si;
        lv_obj_t *page = lv_tabview_add_tab(s_tv, s->title);
        lv_obj_set_style_pad_all(page, GAP, 0);
        lv_obj_set_style_pad_row(page, GAP, 0);
        lv_obj_set_style_pad_column(page, GAP, 0);
        lv_obj_set_flex_flow(page, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_scroll_dir(page, LV_DIR_VER);
        if (s->lights >= 2 && s_ntiles < MAX_UI_TILES) {
            build_all_off(page, &s_tiles[s_ntiles++], si, &g);
        }
        for (int ti = 0; ti < s->count && s_ntiles < MAX_UI_TILES; ti++) {
            tile_ui_t *u = &s_tiles[s_ntiles++];
            memset(u, 0, sizeof(*u));
            u->screen = si;
            u->index = ti;
            u->shown_look = -1;
            u->shown_slider = -2;
            build_tile(page, u, &s->tiles[ti], &g);
        }
        if (s->count == 0) {
            lv_obj_t *l = mk_label(page, FONT_TEXT, COL_MUTED);
            lv_label_set_text_fmt(l, "No items or rules have the tag \"%s\" yet.", s->tag);
        }
    }
    for (int i = 0; i < m->status.count && s_nstatus < MAX_STATUS; i++) {
        status_ui_t *st = &s_status[s_nstatus++];
        st->index = i;
        st->shown[0] = '\0';
        st->shown_alert = -1;
        st->label = mk_label(s_status_box, FONT_TEXT, COL_TEXT);
    }
    s_built_version = m->version;
    s_built_compact = compact;
    wp_oh_model_unlock();

    style_tab_bar();
    lv_tabview_set_active(s_tv, active, LV_ANIM_OFF);
    if (s_overlay) lv_obj_move_foreground(s_overlay);
    s_dirty_state = true;
    ESP_LOGI(TAG, "built %d screens, %d tiles, %d status items", m->screen_count, s_ntiles, s_nstatus);
}

// ---- syncing ----

static void render_tile(tile_ui_t *u, int64_t now)
{
    if (u->type == U_ALL_OFF) {
        int look = now < u->flash_until ? (u->flash_error ? 2 : 1) : 0;
        if (look != u->shown_look) {
            u->shown_look = look;
            lv_obj_set_style_border_width(u->obj, look ? 3 : 0, 0);
            lv_obj_set_style_border_color(u->obj, lv_color_hex(look == 2 ? COL_ERR : COL_OK), 0);
        }
        return;
    }
    if (u->pending && strcmp(u->state, u->pending_from) != 0) {
        u->pending = false;   // openHAB reported a new state: confirmed
        u->has_target = false;
    }
    if (u->pending && now > u->pending_until) {
        u->pending = false;   // REQ-TILE-5: back to the real state, flash an error
        u->has_target = false;
        flash(u, true);
        ESP_LOGW(TAG, "no new state for %s within 3 s", u->name);
    }
    if (!u->pending && u->kind != WP_TILE_SCENE && u->kind != WP_TILE_ROLLER) u->has_target = false;

    // text
    char text[WP_STATE_MAX + 1];
    switch (u->kind) {
    case WP_TILE_SWITCH:
        snprintf(text, sizeof(text), "%s", bad_state(u->state) ? "-" : u->state);
        break;
    case WP_TILE_DIMMER:
    case WP_TILE_COLOR:
    case WP_TILE_ROLLER: {
        int p = u->has_target ? (int)u->target : percent_of(u->state);
        if (p < 0) snprintf(text, sizeof(text), "-");
        else snprintf(text, sizeof(text), "%d %%", p);
        break;
    }
    case WP_TILE_SETPOINT: {
        float v;
        char unit[16] = "";
        if (split_quantity(u->state, &v, unit, sizeof(unit)) || u->has_target) {
            if (u->has_target) v = u->target;
            snprintf(text, sizeof(text), "%.*f%s%s", decimals_for(u->step), v, unit[0] ? " " : "", unit);
        } else {
            snprintf(text, sizeof(text), "-");
        }
        break;
    }
    case WP_TILE_SCENE:
        text[0] = '\0';
        break;
    default:
        snprintf(text, sizeof(text), "%s", bad_state(u->state) ? "-" : (u->display[0] ? u->display : u->state));
        break;
    }
    if (u->value && strcmp(text, u->shown_text) != 0) {
        strlcpy(u->shown_text, text, sizeof(u->shown_text));
        lv_label_set_text(u->value, text);
    }

    // slider follows the state unless the user holds it or a value is pending
    if (u->slider && !lv_obj_has_state(u->slider, LV_STATE_PRESSED) && !u->pending) {
        int p = percent_of(u->state);
        if (p < 0) p = 0;
        if (p != u->shown_slider) {
            u->shown_slider = p;
            lv_slider_set_value(u->slider, p, LV_ANIM_OFF);
        }
    }

    // look: bit0 on, bit1 pending, bit2 error flash, bit3 ok flash
    bool on = is_on(u);
    bool fl = now < u->flash_until;
    int look = (on ? 1 : 0) | (u->pending ? 2 : 0) | (fl && u->flash_error ? 4 : 0) | (fl && !u->flash_error ? 8 : 0);
    if (look != u->shown_look) {
        u->shown_look = look;
        if (u->kind == WP_TILE_SWITCH || u->kind == WP_TILE_DIMMER || u->kind == WP_TILE_COLOR) {
            uint32_t bg = on ? (u->light || u->kind != WP_TILE_SWITCH ? COL_LIGHT_ON : COL_ON) : COL_TILE;
            uint32_t fg = on && bg == COL_LIGHT_ON ? COL_DARK : COL_TEXT;
            lv_obj_set_style_bg_color(u->obj, lv_color_hex(bg), 0);
            lv_obj_set_style_text_color(u->label, lv_color_hex(fg), 0);
            if (u->value) lv_obj_set_style_text_color(u->value, lv_color_hex(on ? fg : COL_MUTED), 0);
            if (u->icon) lv_obj_set_style_text_color(u->icon, lv_color_hex(on ? fg : COL_MUTED), 0);
        }
        int bw = (look & (2 | 4 | 8)) ? 3 : 0;
        uint32_t bc = (look & 4) ? COL_ERR : (look & 8) ? COL_OK : COL_WARN;
        lv_obj_set_style_border_width(u->obj, bw, 0);
        lv_obj_set_style_border_color(u->obj, lv_color_hex(bc), 0);
    }
}

static void render_status(status_ui_t *st, const wp_tile_t *t)
{
    char text[sizeof(st->shown)];
    const char *v = bad_state(t->state) ? "-" : (t->display[0] ? t->display : t->state);
    snprintf(text, sizeof(text), "%s %s", t->label, v);
    if (strcmp(text, st->shown) != 0) {
        strlcpy(st->shown, text, sizeof(st->shown));
        lv_label_set_text(st->label, text);
    }
    // REQ-STAT-3
    int alert = t->alert && (!strcmp(t->state, "ON") || !strcmp(t->state, "OPEN"));
    if (alert != st->shown_alert) {
        st->shown_alert = alert;
        lv_obj_set_style_text_color(st->label, lv_color_hex(alert ? COL_ERR : COL_TEXT), 0);
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
    for (int i = 0; i < s_ntiles; i++) {
        tile_ui_t *u = &s_tiles[i];
        if (u->type != U_TILE) continue;
        const wp_tile_t *t = &m->screens[u->screen].tiles[u->index];
        strlcpy(u->state, t->state, sizeof(u->state));
        strlcpy(u->display, t->display, sizeof(u->display));
    }
    for (int i = 0; i < s_nstatus; i++) {
        render_status(&s_status[i], &m->status.tiles[s_status[i].index]);
    }
    wp_oh_model_unlock();
}

static void update_overlay(void)
{
    wp_oh_status_t st;
    wp_oh_get_status(&st);
    lv_obj_set_hidden(s_overlay, st.connected);
}

static void on_oh_event(wp_oh_event_t ev, const char *item, void *ctx)
{
    // openHAB client task: no LVGL here, only flags for tiles_tick()
    if (ev == WP_OH_EV_MODEL) s_dirty_model = true;
    else if (ev == WP_OH_EV_STATE) s_dirty_state = true;
    else s_dirty_conn = true;
}

// ---- public ----

void tiles_init(lv_obj_t *parent, lv_obj_t *status_box)
{
    s_parent = parent;
    s_status_box = status_box;
    s_tiles = heap_caps_calloc(MAX_UI_TILES, sizeof(tile_ui_t), MALLOC_CAP_SPIRAM);
    if (!s_tiles) {
        ESP_LOGE(TAG, "no memory for tiles");
        return;
    }
    // REQ-OH-5: greyed out and not touchable while openHAB is offline.
    s_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_60, 0);
    lv_obj_set_clickable(s_overlay, true);   // swallows touches
    lv_obj_t *l = mk_label(s_overlay, FONT_BIG, COL_TEXT);
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
    if (!s_tiles) return;
    if (s_dirty_model) {
        const wp_oh_model_t *m = wp_oh_model_lock();
        uint32_t v = m->version;
        wp_oh_model_unlock();
        s_dirty_model = false;
        if (v != 0 && v != s_built_version) rebuild();
    }
    if (s_built_version && wp_config_get()->compact != s_built_compact) rebuild();   // console 'set compact'

    if (!s_built_version) return;
    if (s_dirty_conn) {
        s_dirty_conn = false;
        update_overlay();
    }
    if (s_dirty_state) {
        s_dirty_state = false;
        sync_states();
    }
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < s_ntiles; i++) render_tile(&s_tiles[i], now);

    // REQ-SCR-7: back to the home screen after home_s seconds without a touch.
    uint32_t idle_ms = lv_display_get_inactive_time(NULL);
    uint32_t home_ms = (uint32_t)wp_config_get()->home_timeout_s * 1000;
    if (home_ms && idle_ms > home_ms) {
        if (!s_went_home && lv_tabview_get_tab_active(s_tv) != 0) {
            lv_tabview_set_active(s_tv, 0, LV_ANIM_OFF);
        }
        s_went_home = true;
    } else {
        s_went_home = false;
    }
}

void tiles_show(int tab, int scroll_y)
{
    if (!s_tv) return;
    lv_tabview_set_active(s_tv, tab, LV_ANIM_OFF);
    lv_obj_t *page = lv_obj_get_child(lv_tabview_get_content(s_tv), tab);
    if (page) lv_obj_scroll_to_y(page, scroll_y, LV_ANIM_OFF);
}
