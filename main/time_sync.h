#pragma once
#include <stdbool.h>
// SNTP client with the configured POSIX time zone (FSD REQ-STAT-1).
void time_sync_start(void);
bool time_is_set(void);
