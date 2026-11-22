// Panel data model (docs/FSD.md §3): tiles built from openHAB items and rules.
//
// Pure functions without I/O, so they can be unit-tested on their own: parsing the
// /rest/items and /rest/rules responses, choosing the tile type, sorting, and parsing
// SSE streams and their state events.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WP_ITEM_NAME_MAX  96     // item name or rule UID
#define WP_LABEL_MAX      48
#define WP_TYPE_MAX       32
#define WP_ICON_MAX       16
#define WP_STATE_MAX      64
#define WP_MAX_TILES      48     // per screen; more are dropped (logged as truncated)

typedef enum {
    WP_TILE_SWITCH,      // toggle ON/OFF (also Switch/Dimmer groups)
    WP_TILE_DIMMER,      // toggle + 0..100 slider
    WP_TILE_COLOR,       // as dimmer, brightness = 3rd HSB value
    WP_TILE_ROLLER,      // UP / STOP / DOWN + position
    WP_TILE_SETPOINT,    // - value +
    WP_TILE_VALUE,       // read-only value
    WP_TILE_SCENE,       // rule: run it; item with wallpanel="scene": send ON
} wp_tile_kind_t;

typedef struct {
    char name[WP_ITEM_NAME_MAX + 1];     // item name, or rule UID when is_rule
    char label[WP_LABEL_MAX + 1];
    char type[WP_TYPE_MAX + 1];          // "Dimmer", "Number:Temperature", "Group", ...; "Rule"
    char group_type[WP_TYPE_MAX + 1];    // base type of a Group item
    char icon[WP_ICON_MAX + 1];          // wallpanel icon override, "" = default
    wp_tile_kind_t kind;
    bool is_rule;
    bool read_only;                      // REQ-TILE-2
    bool light;                          // counts for "All off" (REQ-TILE-4)
    bool alert;                          // status item shown red while ON/OPEN (REQ-STAT-3)
    float order;                         // REQ-SCR-4, NAN = none (sorted last)
    float min, max, step;                // setpoint limits, NAN = none
    char state[WP_STATE_MAX + 1];        // raw state: "ON", "38.6,52.9,0", "7.36 °C", "NULL"
    char display[WP_STATE_MAX + 1];      // openHAB displayState, else the raw state
} wp_tile_t;

const char *wp_tile_kind_name(wp_tile_kind_t k);

// Parses a GET /rest/items?...&metadata=wallpanel response (a JSON array) into tiles.
// Returns the number of tiles written (at most max), or -1 if the JSON is not an array.
// *truncated is set when more items than max were present.
int wp_model_parse_items(const char *json, wp_tile_t *out, int max, bool *truncated);

// Parses a GET /rest/rules?tags=..&summary=true response into scene tiles.
int wp_model_parse_rules(const char *json, wp_tile_t *out, int max, bool *truncated);

// REQ-SCR-4: by wallpanel order (tiles without one last), then by label.
void wp_model_sort(wp_tile_t *tiles, int n);

// Parses one SSE state event, {"Item":{"state":"..","displayState":".."}, ...}, calling
// cb once per item. displayState is NULL when openHAB sent none.
// Returns the number of items, or -1 if the JSON is not an object.
typedef void (*wp_state_cb_t)(const char *item, const char *state, const char *display, void *ctx);
int wp_model_parse_state_event(const char *json, wp_state_cb_t cb, void *ctx);

// Incremental text/event-stream parser. Feed it raw bytes; it calls cb for every complete
// message with its event name ("" if none) and data (data lines joined by '\n').
// Messages whose data exceeds the buffer are dropped and counted in 'dropped'.
typedef void (*wp_sse_cb_t)(const char *event, const char *data, void *ctx);
typedef struct {
    char *line;           // current line
    size_t line_len, line_cap;
    char *data;           // data of the current message
    size_t data_len, data_cap;
    char event[32];
    bool overflow;        // current message too long, skip it
    unsigned dropped;
    wp_sse_cb_t cb;
    void *ctx;
} wp_sse_t;

// Buffers are allocated with malloc (PSRAM when large). Returns false when out of memory.
bool wp_sse_init(wp_sse_t *p, size_t max_message, wp_sse_cb_t cb, void *ctx);
void wp_sse_reset(wp_sse_t *p);          // drop partial state, e.g. after a reconnect
void wp_sse_feed(wp_sse_t *p, const char *buf, size_t len);
void wp_sse_free(wp_sse_t *p);

#ifdef __cplusplus
}
#endif
