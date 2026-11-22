// Unit tests for wp_model (item/rule parsing, tile types, sorting, SSE parsing).
// JSON shapes are taken from openHAB 5.2.4 responses.
#include <math.h>
#include <string.h>
#include "unity.h"
#include "wp_model.h"

static const char *ITEMS =
    "["
    "{\"groupType\":\"Switch\",\"state\":\"ON\",\"type\":\"Group\",\"name\":\"gLights\",\"label\":\"All lights\","
    " \"tags\":[\"wp_home\",\"landingpage\"],"
    " \"metadata\":{\"wallpanel\":{\"value\":\"\",\"config\":{\"label\":\"All lights\",\"order\":1.0}}}},"
    "{\"state\":\"16 \\u00b0C\",\"type\":\"Number:Temperature\",\"name\":\"Bedroom_Setpoint\",\"label\":\"Occupied Heating Setpoint\","
    " \"tags\":[\"Temperature\",\"wp_home\",\"Setpoint\"],"
    " \"stateDescription\":{\"minimum\":5,\"maximum\":30,\"step\":1,\"pattern\":\"%.1f %unit%\",\"readOnly\":false},"
    " \"metadata\":{\"wallpanel\":{\"value\":\"\",\"config\":{\"step\":0.5,\"label\":\"Bedroom heating\",\"min\":12.0,\"max\":\"24\",\"order\":\"6\"}}}},"
    "{\"state\":\"38.6,52.9,0\",\"type\":\"Color\",\"name\":\"Salon_Strip\",\"label\":\"Color\",\"tags\":[\"Control\",\"Light\",\"wp_home\"]},"
    "{\"state\":\"OPEN\",\"type\":\"Contact\",\"name\":\"Door\",\"label\":\"Front door\",\"tags\":[\"wp_home\"]},"
    "{\"state\":\"7.36 \\u00b0C\",\"type\":\"Number:Temperature\",\"name\":\"Outside_Temp\",\"label\":\"Outdoors\","
    " \"tags\":[\"Setpoint\"],\"stateDescription\":{\"readOnly\":true}},"
    "{\"state\":\"8\",\"type\":\"Group\",\"groupType\":\"Switch\",\"name\":\"gCount\",\"label\":\"Lights on [%d]\",\"tags\":[]},"
    "{\"state\":\"OFF\",\"type\":\"Switch\",\"name\":\"Movie\",\"label\":\"Movie\",\"tags\":[\"Light\"],"
    " \"metadata\":{\"wallpanel\":{\"value\":\"scene\",\"config\":{\"alert\":\"true\",\"icon\":\"film\"}}}},"
    "{\"state\":\"0\",\"type\":\"Rollershutter\",\"name\":\"Pergola\",\"label\":\"Control\",\"tags\":[\"Point\"]},"
    "{\"state\":\"NULL\",\"type\":\"Dimmer\",\"name\":\"Courtyard\",\"tags\":[\"Lighting\"]},"
    "{\"state\":\"100\",\"type\":\"Group\",\"groupType\":\"Dimmer\",\"name\":\"gPergola\",\"label\":\"Pergola lights\",\"tags\":[\"Light\"]},"
    "{\"name\":\"no_type\"}"
    "]";

static const wp_tile_t *find(const wp_tile_t *t, int n, const char *name)
{
    for (int i = 0; i < n; i++) {
        if (!strcmp(t[i].name, name)) return &t[i];
    }
    TEST_FAIL_MESSAGE(name);
    return NULL;
}

TEST_CASE("items: tile types follow FSD 3.1", "[wp_model]")
{
    static wp_tile_t t[16];
    bool trunc;
    int n = wp_model_parse_items(ITEMS, t, 16, &trunc);
    TEST_ASSERT_EQUAL(10, n);  // the entry without a type is skipped
    TEST_ASSERT_FALSE(trunc);
    TEST_ASSERT_EQUAL(WP_TILE_SWITCH, find(t, n, "gLights")->kind);
    TEST_ASSERT_EQUAL(WP_TILE_SETPOINT, find(t, n, "Bedroom_Setpoint")->kind);
    TEST_ASSERT_EQUAL(WP_TILE_COLOR, find(t, n, "Salon_Strip")->kind);
    TEST_ASSERT_EQUAL(WP_TILE_VALUE, find(t, n, "Door")->kind);
    TEST_ASSERT_EQUAL(WP_TILE_VALUE, find(t, n, "Outside_Temp")->kind);   // read-only beats Setpoint
    TEST_ASSERT_EQUAL(WP_TILE_SWITCH, find(t, n, "gCount")->kind);
    TEST_ASSERT_EQUAL(WP_TILE_SCENE, find(t, n, "Movie")->kind);          // metadata override
    TEST_ASSERT_EQUAL(WP_TILE_ROLLER, find(t, n, "Pergola")->kind);
    TEST_ASSERT_EQUAL(WP_TILE_DIMMER, find(t, n, "Courtyard")->kind);
    TEST_ASSERT_EQUAL(WP_TILE_DIMMER, find(t, n, "gPergola")->kind);     // Dimmer group: one dimmer for all
}

TEST_CASE("items: labels, limits, flags", "[wp_model]")
{
    static wp_tile_t t[16];
    int n = wp_model_parse_items(ITEMS, t, 16, NULL);
    const wp_tile_t *sp = find(t, n, "Bedroom_Setpoint");
    TEST_ASSERT_EQUAL_STRING("Bedroom heating", sp->label);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, sp->step);     // metadata beats stateDescription
    TEST_ASSERT_EQUAL_FLOAT(12.0f, sp->min);
    TEST_ASSERT_EQUAL_FLOAT(24.0f, sp->max);     // string in metadata
    TEST_ASSERT_EQUAL_FLOAT(6.0f, sp->order);    // string in metadata
    TEST_ASSERT_EQUAL_STRING("16 \xc2\xb0" "C", sp->state);

    TEST_ASSERT_EQUAL_STRING("Lights on", find(t, n, "gCount")->label);   // [%d] dropped
    TEST_ASSERT_EQUAL_STRING("Courtyard", find(t, n, "Courtyard")->label); // no label: name
    TEST_ASSERT_EQUAL_STRING("NULL", find(t, n, "Courtyard")->state);

    TEST_ASSERT_TRUE(find(t, n, "Salon_Strip")->light);
    TEST_ASSERT_TRUE(find(t, n, "Courtyard")->light);     // tag Lighting
    TEST_ASSERT_FALSE(find(t, n, "gLights")->light);      // untagged group: not a light
    TEST_ASSERT_TRUE(find(t, n, "gPergola")->light);      // group tagged Light counts
    TEST_ASSERT_FALSE(find(t, n, "Movie")->light);        // a scene is not a switchable light
    TEST_ASSERT_TRUE(find(t, n, "Movie")->alert);
    TEST_ASSERT_EQUAL_STRING("film", find(t, n, "Movie")->icon);
    TEST_ASSERT_TRUE(find(t, n, "Door")->read_only);
    TEST_ASSERT_TRUE(isnan(find(t, n, "Salon_Strip")->order));
}

TEST_CASE("items: truncation and bad JSON", "[wp_model]")
{
    static wp_tile_t t[3];
    bool trunc = false;
    TEST_ASSERT_EQUAL(3, wp_model_parse_items(ITEMS, t, 3, &trunc));
    TEST_ASSERT_TRUE(trunc);
    TEST_ASSERT_EQUAL(-1, wp_model_parse_items("{\"not\":\"an array\"}", t, 3, &trunc));
    TEST_ASSERT_EQUAL(-1, wp_model_parse_items("[{", t, 3, &trunc));
    TEST_ASSERT_EQUAL(0, wp_model_parse_items("[]", t, 3, &trunc));
}

TEST_CASE("items: long UTF-8 labels are cut on a character boundary", "[wp_model]")
{
    // 47 x 'a' then "é" (2 bytes): the cut at 48 bytes would split "é", so it is dropped.
    static const char *j =
        "[{\"type\":\"Switch\",\"name\":\"L\",\"label\":"
        "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\u00e9bbbb\"}]";
    static wp_tile_t t[1];
    TEST_ASSERT_EQUAL(1, wp_model_parse_items(j, t, 1, NULL));
    TEST_ASSERT_EQUAL(47, strlen(t[0].label));   // 47 'a', the 2-byte "é" did not fit
}

TEST_CASE("rules become scene tiles", "[wp_model]")
{
    static const char *j =
        "[{\"status\":{\"status\":\"IDLE\"},\"uid\":\"VMClow\",\"name\":\"VMC low\",\"tags\":[\"wp_home\"]},"
        " {\"uid\":\"noname\"}, {\"name\":\"no uid\"}]";
    static wp_tile_t t[4];
    int n = wp_model_parse_rules(j, t, 4, NULL);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL(WP_TILE_SCENE, t[0].kind);
    TEST_ASSERT_TRUE(t[0].is_rule);
    TEST_ASSERT_EQUAL_STRING("VMClow", t[0].name);
    TEST_ASSERT_EQUAL_STRING("VMC low", t[0].label);
    TEST_ASSERT_EQUAL_STRING("noname", t[1].label);
}

TEST_CASE("sort: order first, then label, unordered last", "[wp_model]")
{
    static wp_tile_t t[4];
    memset(t, 0, sizeof(t));
    const char *labels[] = {"beta", "Alpha", "zulu", "mike"};
    float orders[] = {NAN, NAN, 2, 1};
    for (int i = 0; i < 4; i++) {
        strcpy(t[i].label, labels[i]);
        strcpy(t[i].name, labels[i]);
        t[i].order = orders[i];
    }
    wp_model_sort(t, 4);
    TEST_ASSERT_EQUAL_STRING("mike", t[0].label);
    TEST_ASSERT_EQUAL_STRING("zulu", t[1].label);
    TEST_ASSERT_EQUAL_STRING("Alpha", t[2].label);   // case-insensitive
    TEST_ASSERT_EQUAL_STRING("beta", t[3].label);
}

typedef struct {
    int n;
    char item[4][32], state[4][32], display[4][32];
} states_t;

static void on_state(const char *item, const char *state, const char *display, void *ctx)
{
    states_t *s = ctx;
    if (s->n >= 4) return;
    strcpy(s->item[s->n], item);
    strcpy(s->state[s->n], state);
    strcpy(s->display[s->n], display ? display : "(none)");
    s->n++;
}

TEST_CASE("state events", "[wp_model]")
{
    states_t s = {0};
    int n = wp_model_parse_state_event(
        "{\"Out\":{\"state\":\"7.36 \\u00b0C\",\"displayState\":\"7.4 \\u00b0C\",\"numericState\":7.36,\"type\":\"Quantity\"},"
        "\"Grid\":{\"state\":\"1928\",\"numericState\":1928.0,\"type\":\"Decimal\"},"
        "\"Bad\":{\"type\":\"Decimal\"}}",
        on_state, &s);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_STRING("Out", s.item[0]);
    TEST_ASSERT_EQUAL_STRING("7.4 \xc2\xb0" "C", s.display[0]);
    TEST_ASSERT_EQUAL_STRING("1928", s.state[1]);
    TEST_ASSERT_EQUAL_STRING("(none)", s.display[1]);
    TEST_ASSERT_EQUAL(-1, wp_model_parse_state_event("[1]", on_state, &s));
}

typedef struct {
    int n;
    char event[4][32];
    char data[4][96];
} msgs_t;

static void on_msg(const char *event, const char *data, void *ctx)
{
    msgs_t *m = ctx;
    if (m->n >= 4) return;
    strlcpy(m->event[m->n], event, sizeof(m->event[0]));
    strlcpy(m->data[m->n], data, sizeof(m->data[0]));
    m->n++;
}

TEST_CASE("SSE: messages split across reads", "[wp_model]")
{
    static const char *stream =
        "event: ready\r\nid: 0\r\ndata: 7073-02dc\r\n\r\n"
        ": a comment\n"
        "data: {\"a\":1}\n\n"
        "event: message\ndata: line1\ndata: line2\n\n"
        "event: alive\ndata: {\"type\":\"ALIVE\",\"interval\":10}\n\n";
    msgs_t m = {0};
    wp_sse_t p;
    TEST_ASSERT_TRUE(wp_sse_init(&p, 256, on_msg, &m));
    for (size_t i = 0; stream[i]; i += 3) {      // feed in 3-byte pieces
        size_t len = strlen(stream + i) < 3 ? strlen(stream + i) : 3;
        wp_sse_feed(&p, stream + i, len);
    }
    TEST_ASSERT_EQUAL(4, m.n);
    TEST_ASSERT_EQUAL_STRING("ready", m.event[0]);
    TEST_ASSERT_EQUAL_STRING("7073-02dc", m.data[0]);
    TEST_ASSERT_EQUAL_STRING("", m.event[1]);
    TEST_ASSERT_EQUAL_STRING("{\"a\":1}", m.data[1]);
    TEST_ASSERT_EQUAL_STRING("message", m.event[2]);
    TEST_ASSERT_EQUAL_STRING("line1\nline2", m.data[2]);
    TEST_ASSERT_EQUAL_STRING("alive", m.event[3]);
    wp_sse_free(&p);
}

TEST_CASE("SSE: too-long messages are dropped, later ones still arrive", "[wp_model]")
{
    msgs_t m = {0};
    wp_sse_t p;
    TEST_ASSERT_TRUE(wp_sse_init(&p, 16, on_msg, &m));
    char big[200] = "data: ";
    memset(big + 6, 'x', 150);
    strcpy(big + 156, "\n\n");
    wp_sse_feed(&p, big, strlen(big));
    wp_sse_feed(&p, "data: ok\n\n", 10);
    TEST_ASSERT_EQUAL(1, m.n);
    TEST_ASSERT_EQUAL_STRING("ok", m.data[0]);
    TEST_ASSERT_EQUAL(1, p.dropped);

    // reset discards a half-received message
    wp_sse_feed(&p, "data: partial", 13);
    wp_sse_reset(&p);
    wp_sse_feed(&p, "data: after\n\n", 13);
    TEST_ASSERT_EQUAL(2, m.n);
    TEST_ASSERT_EQUAL_STRING("after", m.data[1]);
    wp_sse_free(&p);
}
