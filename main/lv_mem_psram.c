// LVGL heap in PSRAM (CONFIG_LV_USE_CUSTOM_MALLOC).
//
// With the C library allocator every LVGL object landed in internal RAM: ESP-IDF serves
// allocations below CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (16 KB) from internal RAM, and
// 16 tiles took ~26 KB of the ~46 KB left (2026-11-22). LVGL's draw buffers are allocated
// by the board port with heap_caps_malloc and stay internal.
#include "esp_heap_caps.h"
#include "lvgl.h"

#define LV_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    (void)mem;
    (void)bytes;
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    (void)pool;
}

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, LV_CAPS);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);   // PSRAM full: fall back
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *n = heap_caps_realloc(p, new_size, LV_CAPS);
    return n ? n : heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    (void)mon_p;
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}
