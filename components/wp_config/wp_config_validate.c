// wp_config: defaults, validation and screen-list parsing (pure functions, unit-tested).
#include "wp_config.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

void wp_config_defaults(wp_cfg_t *cfg, const uint8_t mac[6])
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->name, sizeof(cfg->name), "WallPanel-%02X%02X", mac[4], mac[5]);
    strcpy(cfg->screens, "wp_home=Home");
    strcpy(cfg->status_tag, "wp_status");
    strcpy(cfg->tz, "CET-1CEST,M3.5.0,M10.5.0/3");   // Europe/Paris
    cfg->home_timeout_s = 60;
    cfg->dark_timeout_s = 120;
    cfg->night_mode = true;
    cfg->night_start_min = 23 * 60;
    cfg->night_end_min = 6 * 60;
    cfg->bl_ramp_ms = 200;   // the backlight switching dipped the supply into brownout (2026-11-22)
}

bool wp_tag_valid(const char *tag)
{
    size_t n = strlen(tag);
    if (n < 1 || n > WP_TAG_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = tag[i];
        if (!isalnum((unsigned char)c) && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

static const char *trim(const char *s, const char *e, size_t *len)
{
    while (s < e && isspace((unsigned char)*s)) s++;
    while (e > s && isspace((unsigned char)e[-1])) e--;
    *len = (size_t)(e - s);
    return s;
}

int wp_config_parse_screens(const char *spec, wp_screen_t *out, int max)
{
    int n = 0;
    const char *p = spec;
    while (*p) {
        const char *end = strchr(p, ',');
        if (!end) end = p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));
        size_t tl, il;
        const char *tag = trim(p, eq ? eq : end, &tl);
        const char *title = eq ? trim(eq + 1, end, &il) : tag;
        if (!eq) il = tl;
        if (tl == 0 && !eq && *end == '\0' && n > 0) break;   // trailing comma
        if (tl == 0 || tl > WP_TAG_MAX || il > WP_NAME_MAX || n >= max) return -1;
        wp_screen_t s = {0};
        memcpy(s.tag, tag, tl);
        memcpy(s.title, il ? title : tag, il ? il : tl);
        if (!wp_tag_valid(s.tag)) return -1;
        for (int i = 0; i < n; i++) {
            if (strcmp(out[i].tag, s.tag) == 0) return -1;   // duplicate tag
        }
        out[n++] = s;
        p = *end ? end + 1 : end;
    }
    return n;
}

static bool printable(const char *s)
{
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20 || *s == 0x7f) return false;
    }
    return true;
}

const char *wp_config_validate(const wp_cfg_t *c)
{
    size_t n = strlen(c->name);
    if (n < 1 || n > WP_NAME_MAX || !printable(c->name)) return "Panel name: 1 to 32 printable characters";
    if (strlen(c->wifi_ssid) > WP_SSID_MAX) return "WiFi SSID: at most 32 characters";
    n = strlen(c->wifi_pass);
    if (n != 0 && (n < 8 || n > WP_PASS_MAX)) return "WiFi password: empty or 8 to 64 characters";
    n = strlen(c->oh_url);
    if (n) {
        if (strncmp(c->oh_url, "http://", 7) != 0 && strncmp(c->oh_url, "https://", 8) != 0)
            return "openHAB URL: must start with http:// or https://";
        if (c->oh_url[n - 1] == '/') return "openHAB URL: no trailing slash";
        if (!printable(c->oh_url) || strchr(c->oh_url, ' ')) return "openHAB URL: no spaces or control characters";
    }
    if (!printable(c->oh_token) || strchr(c->oh_token, ' ')) return "openHAB token: no spaces or control characters";
    wp_screen_t s[WP_MAX_SCREENS];
    if (wp_config_parse_screens(c->screens, s, WP_MAX_SCREENS) < 1)
        return "Screens: 1 to 8 entries 'tag' or 'tag=Title', tags of A-Z a-z 0-9 _ -, no duplicates";
    if (!wp_tag_valid(c->status_tag)) return "Status tag: 1 to 32 characters A-Z a-z 0-9 _ -";
    n = strlen(c->tz);
    if (n < 3 || !printable(c->tz)) return "Time zone: POSIX TZ string, e.g. CET-1CEST,M3.5.0,M10.5.0/3";
    if (c->home_timeout_s > 3600) return "Return to home: 0 to 3600 s";
    if (c->dark_timeout_s > 3600) return "Screen off: 0 to 3600 s";
    if (c->night_start_min >= 1440 || c->night_end_min >= 1440) return "Night mode: times 00:00 to 23:59";
    if (c->bl_ramp_ms > 1000) return "Backlight soft start: 0 to 1000 ms";
    return NULL;
}
