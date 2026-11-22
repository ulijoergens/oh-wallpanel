// Unit tests for panel settings defaults, validation and screen-list parsing.
#include <string.h>
#include "unity.h"
#include "wp_config.h"

static const uint8_t MAC[6] = {0x80, 0x65, 0x99, 0xa3, 0x48, 0xe0};

static wp_cfg_t good(void)
{
    wp_cfg_t c;
    wp_config_defaults(&c, MAC);
    return c;
}

TEST_CASE("defaults follow the FSD and are valid", "[config]")
{
    wp_cfg_t c = good();
    TEST_ASSERT_EQUAL_STRING("WallPanel-48E0", c.name);
    TEST_ASSERT_EQUAL_STRING("wp_home=Home", c.screens);
    TEST_ASSERT_EQUAL_STRING("wp_status", c.status_tag);
    TEST_ASSERT_EQUAL(60, c.home_timeout_s);
    TEST_ASSERT_EQUAL(120, c.dark_timeout_s);
    TEST_ASSERT_TRUE(c.night_mode);
    TEST_ASSERT_EQUAL(1380, c.night_start_min);
    TEST_ASSERT_EQUAL(360, c.night_end_min);
    TEST_ASSERT_NULL(wp_config_validate(&c));
}

TEST_CASE("screen list parsing", "[config]")
{
    wp_screen_t s[WP_MAX_SCREENS];
    TEST_ASSERT_EQUAL(3, wp_config_parse_screens("wp_home=Home, wp_kitchen = Kitchen ,wp_out", s, WP_MAX_SCREENS));
    TEST_ASSERT_EQUAL_STRING("wp_home", s[0].tag);
    TEST_ASSERT_EQUAL_STRING("Home", s[0].title);
    TEST_ASSERT_EQUAL_STRING("wp_kitchen", s[1].tag);
    TEST_ASSERT_EQUAL_STRING("Kitchen", s[1].title);
    TEST_ASSERT_EQUAL_STRING("wp_out", s[2].tag);
    TEST_ASSERT_EQUAL_STRING("wp_out", s[2].title);
    TEST_ASSERT_EQUAL(1, wp_config_parse_screens("a=Living room,", s, WP_MAX_SCREENS));
    TEST_ASSERT_EQUAL_STRING("Living room", s[0].title);
    TEST_ASSERT_EQUAL(1, wp_config_parse_screens("a=", s, WP_MAX_SCREENS));
    TEST_ASSERT_EQUAL_STRING("a", s[0].title);
    TEST_ASSERT_EQUAL(0, wp_config_parse_screens("", s, WP_MAX_SCREENS));
    TEST_ASSERT_EQUAL(-1, wp_config_parse_screens("a,,b", s, WP_MAX_SCREENS));
    TEST_ASSERT_EQUAL(-1, wp_config_parse_screens("=Home", s, WP_MAX_SCREENS));
    TEST_ASSERT_EQUAL(-1, wp_config_parse_screens("bad tag", s, WP_MAX_SCREENS));
    TEST_ASSERT_EQUAL(-1, wp_config_parse_screens("a,b,a", s, WP_MAX_SCREENS));
    TEST_ASSERT_EQUAL(-1, wp_config_parse_screens("a,b,c", s, 2));
}

TEST_CASE("validation rejects bad fields", "[config]")
{
    wp_cfg_t c = good();
    c.wifi_pass[0] = 'x'; c.wifi_pass[1] = 0;
    TEST_ASSERT_NOT_NULL(wp_config_validate(&c));
    c = good();
    strcpy(c.oh_url, "192.168.1.139:8080");
    TEST_ASSERT_NOT_NULL(wp_config_validate(&c));
    strcpy(c.oh_url, "http://192.168.1.139:8080/");
    TEST_ASSERT_NOT_NULL(wp_config_validate(&c));
    strcpy(c.oh_url, "http://192.168.1.139:8080");
    TEST_ASSERT_NULL(wp_config_validate(&c));
    c = good();
    strcpy(c.screens, "");
    TEST_ASSERT_NOT_NULL(wp_config_validate(&c));
    c = good();
    strcpy(c.status_tag, "a b");
    TEST_ASSERT_NOT_NULL(wp_config_validate(&c));
    c = good();
    c.night_end_min = 1440;
    TEST_ASSERT_NOT_NULL(wp_config_validate(&c));
    c = good();
    strcpy(c.oh_token, "oh.panel.abc def");
    TEST_ASSERT_NOT_NULL(wp_config_validate(&c));
}
