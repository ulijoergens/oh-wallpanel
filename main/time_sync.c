// SNTP time sync, see time_sync.h.
#include "time_sync.h"

#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "wp_config.h"

static const char *TAG = "time";

static void synced(struct timeval *tv)
{
    time_t now = tv->tv_sec;
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm);
    ESP_LOGI(TAG, "time synchronised: %s", buf);
}

void time_sync_start(void)
{
    setenv("TZ", wp_config_get()->tz, 1);
    tzset();
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = synced;
    cfg.server_from_dhcp = true;          // prefer the router's NTP server if DHCP offers one
    cfg.renew_servers_after_new_IP = true;
    cfg.index_of_first_server = 1;        // keep pool.ntp.org as a fallback in slot 0
    esp_netif_sntp_init(&cfg);
}

bool time_is_set(void)
{
    return time(NULL) > 1700000000;   // after Nov 2023: SNTP has run
}
