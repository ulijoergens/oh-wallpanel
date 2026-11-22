// Panel data model, see wp_model.h.
#include "wp_model.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cJSON.h"

const char *wp_tile_kind_name(wp_tile_kind_t k)
{
    switch (k) {
    case WP_TILE_SWITCH: return "switch";
    case WP_TILE_DIMMER: return "dimmer";
    case WP_TILE_COLOR: return "color";
    case WP_TILE_ROLLER: return "roller";
    case WP_TILE_SETPOINT: return "setpoint";
    case WP_TILE_VALUE: return "value";
    case WP_TILE_SCENE: return "scene";
    }
    return "?";
}

// strlcpy that never cuts a UTF-8 sequence in half (LVGL would show a broken glyph).
static void copy_utf8(char *dst, size_t cap, const char *src)
{
    if (!cap) return;
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;   // src[n] is a continuation byte: back up to the lead byte, drop it too
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static const char *json_str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

// A metadata config value may be a number or a string ("3", "0.5"): openHAB's UI stores
// whatever the user typed.
static float json_num(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsNumber(v)) return (float)v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring[0]) {
        char *end;
        float f = strtof(v->valuestring, &end);
        if (*end == '\0') return f;
    }
    return NAN;
}

static bool json_true(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsTrue(v) || (cJSON_IsString(v) && strcasecmp(v->valuestring, "true") == 0);
}

static bool has_tag(const cJSON *tags, const char *tag)
{
    const cJSON *t;
    cJSON_ArrayForEach(t, tags) {
        if (cJSON_IsString(t) && strcmp(t->valuestring, tag) == 0) return true;
    }
    return false;
}

// "Number:Temperature" -> "Number"
static void base_type(char *out, size_t cap, const char *type)
{
    copy_utf8(out, cap, type);
    char *colon = strchr(out, ':');
    if (colon) *colon = '\0';
}

// Old-style labels carry a format: "Lights on [%d]". Drop the bracket part.
static void strip_label_format(char *label)
{
    char *br = strrchr(label, '[');
    if (br && strchr(br, ']')) {
        while (br > label && br[-1] == ' ') br--;
        *br = '\0';
    }
}

static bool kind_from_override(const char *v, wp_tile_kind_t *out)
{
    static const struct { const char *name; wp_tile_kind_t kind; } map[] = {
        {"switch", WP_TILE_SWITCH}, {"dimmer", WP_TILE_DIMMER}, {"color", WP_TILE_COLOR},
        {"rollershutter", WP_TILE_ROLLER}, {"roller", WP_TILE_ROLLER},
        {"setpoint", WP_TILE_SETPOINT}, {"value", WP_TILE_VALUE}, {"scene", WP_TILE_SCENE},
    };
    if (!v) return false;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcasecmp(v, map[i].name) == 0) {
            *out = map[i].kind;
            return true;
        }
    }
    return false;
}

// FSD §3.1 table.
static wp_tile_kind_t default_kind(const char *base, const char *group_base, const cJSON *tags)
{
    if (strcmp(base, "Switch") == 0) return WP_TILE_SWITCH;
    if (strcmp(base, "Dimmer") == 0) return WP_TILE_DIMMER;
    if (strcmp(base, "Color") == 0) return WP_TILE_COLOR;
    if (strcmp(base, "Rollershutter") == 0) return WP_TILE_ROLLER;
    if (strcmp(base, "Number") == 0 && has_tag(tags, "Setpoint")) return WP_TILE_SETPOINT;
    if (strcmp(base, "Group") == 0) {
        // A Dimmer/Color group gets a dimmer: one tile driving several lights (e.g. the
        // pergola's three lights). Commands to a group go to all its members.
        if (!strcmp(group_base, "Dimmer") || !strcmp(group_base, "Color")) return WP_TILE_DIMMER;
        if (!strcmp(group_base, "Switch")) return WP_TILE_SWITCH;   // toggle acting on the whole group
        if (!strcmp(group_base, "Rollershutter")) return WP_TILE_ROLLER;
    }
    return WP_TILE_VALUE;
}

static void parse_item(const cJSON *it, wp_tile_t *t)
{
    memset(t, 0, sizeof(*t));
    t->order = t->min = t->max = t->step = NAN;

    const cJSON *tags = cJSON_GetObjectItemCaseSensitive(it, "tags");
    const cJSON *md = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(it, "metadata"), "wallpanel");
    const cJSON *cfg = cJSON_GetObjectItemCaseSensitive(md, "config");
    const cJSON *sd = cJSON_GetObjectItemCaseSensitive(it, "stateDescription");

    copy_utf8(t->name, sizeof(t->name), json_str(it, "name"));
    copy_utf8(t->type, sizeof(t->type), json_str(it, "type"));
    copy_utf8(t->group_type, sizeof(t->group_type), json_str(it, "groupType"));

    const char *label = json_str(cfg, "label");
    if (!label || !label[0]) {
        label = json_str(it, "label");
    }
    copy_utf8(t->label, sizeof(t->label), label && label[0] ? label : t->name);
    strip_label_format(t->label);
    if (!t->label[0]) copy_utf8(t->label, sizeof(t->label), t->name);
    copy_utf8(t->icon, sizeof(t->icon), json_str(cfg, "icon"));

    const char *st = json_str(it, "state");
    copy_utf8(t->state, sizeof(t->state), st ? st : "NULL");
    const char *ts = json_str(it, "transformedState");
    copy_utf8(t->display, sizeof(t->display), ts ? ts : t->state);

    char base[WP_TYPE_MAX + 1], gbase[WP_TYPE_MAX + 1];
    base_type(base, sizeof(base), t->type);
    base_type(gbase, sizeof(gbase), t->group_type);

    t->read_only = json_true(sd, "readOnly") || !strcmp(base, "Contact");
    if (!kind_from_override(json_str(md, "value"), &t->kind)) {
        t->kind = default_kind(base, gbase, tags);
    }
    if (t->read_only && t->kind != WP_TILE_VALUE) {
        t->kind = WP_TILE_VALUE;   // REQ-TILE-2: never a control for a read-only item
    }

    bool lightable = t->kind == WP_TILE_SWITCH || t->kind == WP_TILE_DIMMER || t->kind == WP_TILE_COLOR;
    // Groups count only when tagged Light themselves: an untagged whole-house group such
    // as gLights must not be switched off by a room's "All off".
    t->light = lightable && (has_tag(tags, "Light") || has_tag(tags, "Lighting"));
    t->alert = json_true(cfg, "alert");

    t->order = json_num(cfg, "order");
    t->min = json_num(sd, "minimum");
    t->max = json_num(sd, "maximum");
    t->step = json_num(sd, "step");
    float v;
    if (!isnan(v = json_num(cfg, "min"))) t->min = v;
    if (!isnan(v = json_num(cfg, "max"))) t->max = v;
    if (!isnan(v = json_num(cfg, "step"))) t->step = v;
    if (t->kind == WP_TILE_SETPOINT && !(t->step > 0)) {
        t->step = 0.5f;   // FSD §3.1 default
    }
}

int wp_model_parse_items(const char *json, wp_tile_t *out, int max, bool *truncated)
{
    if (truncated) *truncated = false;
    cJSON *arr = cJSON_Parse(json);
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return -1;
    }
    int n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (!json_str(it, "name") || !json_str(it, "type")) continue;
        if (n >= max) {
            if (truncated) *truncated = true;
            break;
        }
        parse_item(it, &out[n++]);
    }
    cJSON_Delete(arr);
    return n;
}

int wp_model_parse_rules(const char *json, wp_tile_t *out, int max, bool *truncated)
{
    if (truncated) *truncated = false;
    cJSON *arr = cJSON_Parse(json);
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return -1;
    }
    int n = 0;
    const cJSON *r;
    cJSON_ArrayForEach(r, arr) {
        const char *uid = json_str(r, "uid");
        if (!uid) continue;
        if (n >= max) {
            if (truncated) *truncated = true;
            break;
        }
        wp_tile_t *t = &out[n++];
        memset(t, 0, sizeof(*t));
        t->order = t->min = t->max = t->step = NAN;
        copy_utf8(t->name, sizeof(t->name), uid);
        const char *name = json_str(r, "name");
        copy_utf8(t->label, sizeof(t->label), name && name[0] ? name : uid);
        strcpy(t->type, "Rule");
        t->kind = WP_TILE_SCENE;
        t->is_rule = true;
        strcpy(t->state, "-");
        strcpy(t->display, "");
    }
    cJSON_Delete(arr);
    return n;
}

static int cmp_tiles(const void *a, const void *b)
{
    const wp_tile_t *x = a, *y = b;
    bool xo = !isnan(x->order), yo = !isnan(y->order);
    if (xo != yo) return xo ? -1 : 1;
    if (xo && x->order != y->order) return x->order < y->order ? -1 : 1;
    int c = strcasecmp(x->label, y->label);
    return c ? c : strcmp(x->name, y->name);
}

void wp_model_sort(wp_tile_t *tiles, int n)
{
    if (n > 1) qsort(tiles, n, sizeof(*tiles), cmp_tiles);
}

int wp_model_parse_state_event(const char *json, wp_state_cb_t cb, void *ctx)
{
    cJSON *o = cJSON_Parse(json);
    if (!cJSON_IsObject(o)) {
        cJSON_Delete(o);
        return -1;
    }
    int n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, o) {
        const char *st = json_str(it, "state");
        if (!it->string || !st) continue;
        cb(it->string, st, json_str(it, "displayState"), ctx);
        n++;
    }
    cJSON_Delete(o);
    return n;
}

// ---- text/event-stream ----

bool wp_sse_init(wp_sse_t *p, size_t max_message, wp_sse_cb_t cb, void *ctx)
{
    memset(p, 0, sizeof(*p));
    p->line_cap = max_message + 64;   // "data: " prefix and some slack
    p->data_cap = max_message + 1;
    p->line = malloc(p->line_cap);
    p->data = malloc(p->data_cap);
    p->cb = cb;
    p->ctx = ctx;
    if (!p->line || !p->data) {
        wp_sse_free(p);
        return false;
    }
    p->data[0] = '\0';
    return true;
}

void wp_sse_reset(wp_sse_t *p)
{
    p->line_len = p->data_len = 0;
    if (p->data) p->data[0] = '\0';
    p->event[0] = '\0';
    p->overflow = false;
}

void wp_sse_free(wp_sse_t *p)
{
    free(p->line);
    free(p->data);
    p->line = p->data = NULL;
}

static void end_of_message(wp_sse_t *p)
{
    if (p->overflow) {
        p->dropped++;
    } else if (p->data_len > 0 || p->event[0]) {
        p->data[p->data_len] = '\0';
        p->cb(p->event, p->data, p->ctx);
    }
    p->data_len = 0;
    p->data[0] = '\0';
    p->event[0] = '\0';
    p->overflow = false;
}

static void handle_line(wp_sse_t *p, char *line, size_t len)
{
    if (len == 0) {
        end_of_message(p);
        return;
    }
    if (line[0] == ':' || p->overflow) return;   // comment, or skipping a too-long message
    char *val = memchr(line, ':', len);
    size_t flen = val ? (size_t)(val - line) : len;
    const char *v = "";
    size_t vlen = 0;
    if (val) {
        v = val + 1;
        vlen = len - flen - 1;
        if (vlen && *v == ' ') {
            v++;
            vlen--;
        }
    }
    if (flen == 4 && memcmp(line, "data", 4) == 0) {
        size_t need = p->data_len + (p->data_len ? 1 : 0) + vlen;
        if (need >= p->data_cap) {
            p->overflow = true;
            return;
        }
        if (p->data_len) p->data[p->data_len++] = '\n';
        memcpy(p->data + p->data_len, v, vlen);
        p->data_len += vlen;
    } else if (flen == 5 && memcmp(line, "event", 5) == 0) {
        size_t n = vlen < sizeof(p->event) - 1 ? vlen : sizeof(p->event) - 1;
        memcpy(p->event, v, n);
        p->event[n] = '\0';
    }
    // "id" and "retry" are not needed
}

void wp_sse_feed(wp_sse_t *p, const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '\n') {
            size_t n = p->line_len;
            if (n && p->line[n - 1] == '\r') n--;
            if (p->line_len >= p->line_cap) {
                p->overflow = true;   // the line itself did not fit
                n = 0;
                p->line_len = 0;
                continue;             // a too-long line is not the blank end-of-message line
            }
            p->line[n] = '\0';
            handle_line(p, p->line, n);
            p->line_len = 0;
        } else if (p->line_len < p->line_cap) {
            p->line[p->line_len++] = c;
        } else {
            p->line_len = p->line_cap;   // keep counting as "full" until the newline
        }
    }
}
