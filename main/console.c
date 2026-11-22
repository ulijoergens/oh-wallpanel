// Serial console for provisioning and bench testing (USB-Serial/JTAG, /dev/ttyACM0).
//
//   status                      device, WiFi and log summary
//   show                        current settings (passwords hidden)
//   set <key> <value...>        change one setting and save it; most need a reboot
//   log                         warnings and errors, newest first
//   net | ping <ip> [count]     network diagnostics
//   reboot | factory-reset
#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_console.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "ping/ping_sock.h"
#include "wp_config.h"
#include "wp_log.h"
#include "wp_oh.h"
#include "ui.h"
#include "wifi_mgr.h"

static int cmd_status(int argc, char **argv)
{
    const wp_cfg_t *c = wp_config_get();
    wifi_mgr_status_t w;
    wifi_mgr_get_status(&w);
    printf("panel    %s, firmware %s, uptime %llds, free internal %u, PSRAM %u\n", c->name,
           esp_app_get_description()->version, esp_timer_get_time() / 1000000,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("wifi     %s, network \"%s\"", wifi_mgr_state_name(w.state), w.ssid);
    if (w.state == WIFI_MGR_CONNECTED) {
        printf(", IP %s, RSSI %d dBm", w.ip, w.rssi);
    } else if (w.lost_for_s) {
        printf(", lost for %lus, attempts %lu", (unsigned long)w.lost_for_s, (unsigned long)w.attempts);
    }
    if (w.next_retry_s) {
        printf(", next attempt in %lus", (unsigned long)w.next_retry_s);
    }
    printf("\n");
    if (w.ap_active) {
        printf("ap       \"%s\" at %s\n", w.ap_ssid, w.ap_ip);
    }
    wp_oh_status_t o;
    wp_oh_get_status(&o);
    printf("openhab  %s %s%s%s%s\n", o.state, c->oh_url, o.version[0] ? ", version " : "", o.version,
           o.last_error[0] ? "" : "");
    if (o.last_error[0]) {
        printf("         %s\n", o.last_error);
    }
    if (c->oh_url[0]) {
        printf("         streams: states %s, registry %s; %lu loads, %lu updates, commands %lu ok / %lu failed\n",
               o.stream_up ? "up" : "down", o.registry_up ? "up" : "down", (unsigned long)o.reloads,
               (unsigned long)o.updates, (unsigned long)o.commands_ok, (unsigned long)o.commands_failed);
        if (o.last_command_error[0]) printf("         last command error: %s\n", o.last_command_error);
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S %Z", &tm);
    printf("time     %s\n", now > 1700000000 ? ts : "not synchronised");
    printf("log      %lu warnings/errors since boot\n", (unsigned long)wp_log_count());
    return 0;
}

static int cmd_show(int argc, char **argv)
{
    const wp_cfg_t *c = wp_config_get();
    printf("name        %s\nssid        %s\npass        %s\n", c->name, c->wifi_ssid, c->wifi_pass[0] ? "(set)" : "(empty)");
    printf("oh_url      %s\noh_token    %s\nscreens     %s\nstatus_tag  %s\ntz          %s\n", c->oh_url,
           c->oh_token[0] ? "(set)" : "(empty)", c->screens, c->status_tag, c->tz);
    printf("home_s      %u\ndark_s      %u\nnight       %s %02u:%02u-%02u:%02u\n", c->home_timeout_s, c->dark_timeout_s,
           c->night_mode ? "on" : "off", c->night_start_min / 60, c->night_start_min % 60, c->night_end_min / 60,
           c->night_end_min % 60);
    return 0;
}

static int set_str(char *dst, size_t len, const char *v)
{
    if (strlen(v) >= len) {
        printf("value too long (max %u characters)\n", (unsigned)(len - 1));
        return 1;
    }
    strcpy(dst, v);
    return 0;
}

static int cmd_set(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: set <key> [value...]   keys: name ssid pass oh_url oh_token screens status_tag tz "
               "home_s dark_s night (on|off) night_start night_end (HH:MM)\n");
        return 1;
    }
    // Join the remaining arguments so values may contain spaces (e.g. SSIDs).
    char val[WP_SCREENS_MAX + 1] = "";
    for (int i = 2; i < argc; i++) {
        if (strlen(val) + strlen(argv[i]) + 2 > sizeof(val)) {
            printf("value too long\n");
            return 1;
        }
        if (i > 2) {
            strcat(val, " ");
        }
        strcat(val, argv[i]);
    }
    wp_cfg_t c = *wp_config_get();
    const char *k = argv[1];
    int err = 0;
    unsigned hh, mm;
    if (!strcmp(k, "name")) err = set_str(c.name, sizeof(c.name), val);
    else if (!strcmp(k, "ssid")) err = set_str(c.wifi_ssid, sizeof(c.wifi_ssid), val);
    else if (!strcmp(k, "pass")) err = set_str(c.wifi_pass, sizeof(c.wifi_pass), val);
    else if (!strcmp(k, "oh_url")) err = set_str(c.oh_url, sizeof(c.oh_url), val);
    else if (!strcmp(k, "oh_token")) err = set_str(c.oh_token, sizeof(c.oh_token), val);
    else if (!strcmp(k, "screens")) err = set_str(c.screens, sizeof(c.screens), val);
    else if (!strcmp(k, "status_tag")) err = set_str(c.status_tag, sizeof(c.status_tag), val);
    else if (!strcmp(k, "tz")) err = set_str(c.tz, sizeof(c.tz), val);
    else if (!strcmp(k, "home_s")) c.home_timeout_s = (uint16_t)atoi(val);
    else if (!strcmp(k, "dark_s")) c.dark_timeout_s = (uint16_t)atoi(val);
    else if (!strcmp(k, "night")) c.night_mode = !strcmp(val, "on") || !strcmp(val, "1");
    else if (!strcmp(k, "night_start") && sscanf(val, "%u:%u", &hh, &mm) == 2 && hh < 24 && mm < 60) c.night_start_min = hh * 60 + mm;
    else if (!strcmp(k, "night_end") && sscanf(val, "%u:%u", &hh, &mm) == 2 && hh < 24 && mm < 60) c.night_end_min = hh * 60 + mm;
    else {
        printf("unknown key or bad value \"%s\"\n", k);
        return 1;
    }
    if (err) {
        return 1;
    }
    const char *why = wp_config_validate(&c);
    if (why) {
        printf("not saved: %s\n", why);
        return 1;
    }
    if (wp_config_save(&c) != ESP_OK) {
        printf("saving failed\n");
        return 1;
    }
    printf("saved; type \"reboot\" to apply\n");
    return 0;
}

static int cmd_log(int argc, char **argv)
{
    static wp_log_entry_t e[WP_LOG_ENTRIES];
    int n = wp_log_get(e, WP_LOG_ENTRIES);
    for (int i = 0; i < n; i++) {
        printf("%c %8.1fs  %s\n", e[i].level, e[i].uptime_ms / 1000.0, e[i].text);
    }
    if (n == 0) {
        printf("no warnings or errors\n");
    }
    return 0;
}

static int cmd_net(int argc, char **argv)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK) {
        printf("ip       " IPSTR "\nnetmask  " IPSTR "\ngateway  " IPSTR "\n", IP2STR(&ip.ip),
               IP2STR(&ip.netmask), IP2STR(&ip.gw));
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
            printf("dns      " IPSTR "\n", IP2STR(&dns.ip.u_addr.ip4));
        }
    }
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        printf("mac      " MACSTR "\n", MAC2STR(mac));
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        printf("ap       \"%s\" bssid " MACSTR ", channel %d, RSSI %d dBm\n", (const char *)ap.ssid,
               MAC2STR(ap.bssid), ap.primary, ap.rssi);
    } else {
        printf("ap       not connected\n");
    }
    return 0;
}

static SemaphoreHandle_t s_ping_done;

static void ping_ok(esp_ping_handle_t h, void *arg)
{
    uint32_t seq, ms;
    ip_addr_t from;
    esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms));
    esp_ping_get_profile(h, ESP_PING_PROF_IPADDR, &from, sizeof(from));
    printf("reply from %s: seq=%lu time=%lu ms\n", ipaddr_ntoa(&from), (unsigned long)seq, (unsigned long)ms);
}

static void ping_timeout(esp_ping_handle_t h, void *arg)
{
    uint32_t seq;
    esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    printf("timeout: seq=%lu\n", (unsigned long)seq);
}

static void ping_end(esp_ping_handle_t h, void *arg)
{
    uint32_t sent, got;
    esp_ping_get_profile(h, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
    esp_ping_get_profile(h, ESP_PING_PROF_REPLY, &got, sizeof(got));
    printf("%lu sent, %lu received\n", (unsigned long)sent, (unsigned long)got);
    xSemaphoreGive(s_ping_done);
}

static int cmd_ping(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: ping <ipv4-address> [count]\n");
        return 1;
    }
    esp_ping_config_t pc = ESP_PING_DEFAULT_CONFIG();
    if (!ipaddr_aton(argv[1], &pc.target_addr)) {
        printf("not an IP address: %s\n", argv[1]);
        return 1;
    }
    pc.count = argc > 2 ? (uint32_t)atoi(argv[2]) : 4;
    if (pc.count == 0 || pc.count > 20) {
        pc.count = 4;
    }
    if (!s_ping_done) {
        s_ping_done = xSemaphoreCreateBinary();
    }
    esp_ping_callbacks_t cbs = {.on_ping_success = ping_ok, .on_ping_timeout = ping_timeout,
                                .on_ping_end = ping_end};
    esp_ping_handle_t h;
    if (esp_ping_new_session(&pc, &cbs, &h) != ESP_OK) {
        printf("ping failed to start\n");
        return 1;
    }
    esp_ping_start(h);
    xSemaphoreTake(s_ping_done, pdMS_TO_TICKS(pc.count * 2000 + 2000));
    esp_ping_delete_session(h);
    return 0;
}

// Deliberate crashes to verify the crash report (REQ-LOG-2).
static int cmd_crash_test(int argc, char **argv)
{
    if (argc < 2 || (strcmp(argv[1], "abort") && strcmp(argv[1], "intwdt"))) {
        printf("usage: crash-test abort|intwdt   (restarts the device and stores a crash report)\n");
        return 1;
    }
    printf("crashing now (%s)\n", argv[1]);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!strcmp(argv[1], "abort")) {
        abort();
    }
    portDISABLE_INTERRUPTS();   // hold interrupts off until the interrupt watchdog fires
    for (;;) {
    }
    return 0;
}

static int cmd_wifi_scan(int argc, char **argv)
{
    static wifi_mgr_net_t nets[30];
    int n = wifi_mgr_scan(nets, 30);
    if (n < 0) {
        printf("scan not possible right now (WiFi is connecting)\n");
        return 1;
    }
    for (int i = 0; i < n; i++) {
        printf("%4d dBm  ch %2u  %s  %s\n", nets[i].rssi, nets[i].channel, nets[i].secured ? "secured" : "open   ", nets[i].ssid);
    }
    printf("%d network(s)\n", n);
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    printf("rebooting\n");
    fflush(stdout);
    esp_restart();
    return 0;
}

static int cmd_factory_reset(int argc, char **argv)
{
    wp_config_factory_reset();
    return cmd_reboot(argc, argv);
}

// Line reader on the USB-Serial/JTAG port. IDF's esp_console REPL is not used: its
// terminal probe (linenoiseProbe) crashed or hung the panel at start-up with this console
// (NULL load in IDF 6.1, 2026-11-22). No line editing; backspace works.
static void console_task(void *arg)
{
    char line[256];
    size_t len = 0;
    printf("\nwp> ");
    fflush(stdout);
    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (len == 0) {
                if (c == '\r') {
                    printf("\nwp> ");
                    fflush(stdout);
                }
                continue;
            }
            line[len] = '\0';
            len = 0;
            printf("\n");
            int ret;
            esp_err_t err = esp_console_run(line, &ret);
            if (err == ESP_ERR_NOT_FOUND) {
                printf("unknown command; type \"help\"\n");
            } else if (err == ESP_OK && ret != 0) {
                printf("command returned %d\n", ret);
            }
            printf("wp> ");
            fflush(stdout);
        } else if (c == 0x08 || c == 0x7f) {
            if (len > 0) {
                len--;
                printf("\b \b");
                fflush(stdout);
            }
        } else if (c >= 0x20 && len + 1 < sizeof(line)) {
            line[len++] = (char)c;
            putchar(c);
            fflush(stdout);
        }
    }
}

// ---- openHAB (milestone 3: data layer, inspected and driven from the console) ----

static void print_screen(const wp_oh_screen_t *s, bool status)
{
    printf("%s \"%s\" (tag %s): %d tiles%s%s\n", status ? "status" : "screen", s->title, s->tag, s->count,
           s->truncated ? ", TRUNCATED" : "", !status && s->lights >= 2 ? ", All off" : "");
    for (int i = 0; i < s->count; i++) {
        const wp_tile_t *t = &s->tiles[i];
        printf("  %-8s %-20.20s %-24s %s%s%s\n", wp_tile_kind_name(t->kind), t->label,
               t->display[0] ? t->display : t->state, t->name, t->light ? " [light]" : "",
               t->alert ? " [alert]" : "");
    }
}

static int cmd_screens(int argc, char **argv)
{
    // Copy one screen at a time so the model lock is not held while printing.
    wp_oh_screen_t copy;
    copy.tiles = heap_caps_malloc(WP_MAX_TILES * sizeof(wp_tile_t), MALLOC_CAP_SPIRAM);
    if (!copy.tiles) {
        printf("out of memory\n");
        return 1;
    }
    const wp_oh_model_t *m = wp_oh_model_lock();
    int n = m->screen_count;
    uint32_t ver = m->version;
    wp_oh_model_unlock();
    if (!ver) {
        printf("no screens loaded yet\n");
    }
    for (int i = 0; ver && i <= n; i++) {
        m = wp_oh_model_lock();
        bool same = m->version == ver;
        if (same) {
            const wp_oh_screen_t *s = i < n ? &m->screens[i] : &m->status;
            wp_tile_t *keep = copy.tiles;
            copy = *s;
            copy.tiles = keep;
            memcpy(copy.tiles, s->tiles, s->count * sizeof(wp_tile_t));
        }
        wp_oh_model_unlock();
        if (!same) {
            printf("(screens reloaded meanwhile, run it again)\n");
            break;
        }
        print_screen(&copy, i == n);
    }
    heap_caps_free(copy.tiles);
    return 0;
}

static int cmd_send(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: send <item> <command>   e.g. send Kitchen_Light ON\n");
        return 1;
    }
    char val[WP_STATE_MAX + 1] = "";
    for (int i = 2; i < argc; i++) {   // values may contain spaces: "21.5 °C"
        if (i > 2) strlcat(val, " ", sizeof(val));
        strlcat(val, argv[i], sizeof(val));
    }
    esp_err_t err = wp_oh_command(argv[1], val);
    printf(err == ESP_OK ? "queued\n" : "not queued: %s\n", esp_err_to_name(err));
    return err != ESP_OK;
}

static int cmd_run(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: run <rule-uid>\n");
        return 1;
    }
    esp_err_t err = wp_oh_run_rule(argv[1]);
    printf(err == ESP_OK ? "queued\n" : "not queued: %s\n", esp_err_to_name(err));
    return err != ESP_OK;
}

static int cmd_fbdump(int argc, char **argv)
{
    ui_dump_framebuffer();
    return 0;
}

static int cmd_tab(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: tab <n> [scroll_y]   (0 = home screen)\n");
        return 1;
    }
    ui_show_tab(atoi(argv[1]), argc > 2 ? atoi(argv[2]) : 0);
    return 0;
}

static int cmd_tap(int argc, char **argv)
{
    if (argc != 3) {
        printf("usage: tap <x> <y>\n");
        return 1;
    }
    ui_tap(atoi(argv[1]), atoi(argv[2]));
    return 0;
}

static int cmd_touchlog(int argc, char **argv)
{
    bool on = argc < 2 || strcmp(argv[1], "off") != 0;
    ui_touchlog(on);
    printf("touch log %s\n", on ? "on" : "off");
    return 0;
}

static int cmd_touchtest(int argc, char **argv)
{
    bool on = argc < 2 || strcmp(argv[1], "off") != 0;
    ui_touchtest(on);
    printf("touch test %s\n", on ? "on" : "off");
    return 0;
}

static int cmd_gt911(int argc, char **argv)
{
    ui_dump_gt911();
    return 0;
}

static int cmd_reload(int argc, char **argv)
{
    wp_oh_reload();
    printf("reloading screens\n");
    return 0;
}

void console_start(void)
{
    usb_serial_jtag_driver_config_t uc = {.rx_buffer_size = 512, .tx_buffer_size = 1024};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&uc));
    usb_serial_jtag_vfs_use_driver();   // stdout now goes through the driver (no busy-waiting)
    esp_console_config_t cc = ESP_CONSOLE_CONFIG_DEFAULT();
    cc.max_cmdline_length = 256;
    ESP_ERROR_CHECK(esp_console_init(&cc));

    const esp_console_cmd_t cmds[] = {
        {.command = "status", .help = "Device, WiFi and log summary", .func = cmd_status},
        {.command = "show", .help = "Show settings (passwords hidden)", .func = cmd_show},
        {.command = "set", .help = "set <key> <value>: change and save one setting", .func = cmd_set},
        {.command = "log", .help = "Warnings and errors, newest first", .func = cmd_log},
        {.command = "wifi-scan", .help = "List WiFi networks in range", .func = cmd_wifi_scan},
        {.command = "net", .help = "IP address, gateway, DNS and access point details", .func = cmd_net},
        {.command = "ping", .help = "ping <ip> [count]: check reachability from the device", .func = cmd_ping},
        {.command = "screens", .help = "Screens and tiles loaded from openHAB, with live states", .func = cmd_screens},
        {.command = "send", .help = "send <item> <command>: send an openHAB item command", .func = cmd_send},
        {.command = "run", .help = "run <rule-uid>: run an openHAB rule (scene)", .func = cmd_run},
        {.command = "fbdump", .help = "Dump the screen for tools/fbdump.py (developer aid)", .func = cmd_fbdump},
        {.command = "tab", .help = "tab <n> [scroll_y]: show a screen (developer aid)", .func = cmd_tab},
        {.command = "tap", .help = "tap <x> <y>: click what is shown there (developer aid)", .func = cmd_tap},
        {.command = "touchlog", .help = "touchlog [on|off]: log what each touch hits (developer aid)", .func = cmd_touchlog},
        {.command = "touchtest", .help = "touchtest [on|off]: full-screen touch test (developer aid)", .func = cmd_touchtest},
        {.command = "gt911", .help = "Dump the touch controller configuration (developer aid)", .func = cmd_gt911},
        {.command = "reload", .help = "Reload the screens from openHAB now", .func = cmd_reload},
        {.command = "reboot", .help = "Restart the device", .func = cmd_reboot},
        {.command = "crash-test", .help = "crash-test abort|intwdt: crash on purpose to test crash reports", .func = cmd_crash_test},
        {.command = "factory-reset", .help = "Erase all settings and restart", .func = cmd_factory_reset},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    esp_console_register_help_command();
    xTaskCreate(console_task, "console", 6144, NULL, 2, NULL);
}
