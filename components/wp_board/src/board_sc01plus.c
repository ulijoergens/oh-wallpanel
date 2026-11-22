// Wireless-Tag WT32-SC01 Plus: ESP32-S3 (16 MB flash, 2 MB quad PSRAM), 3.5" 320x480 ST7796
// on an 8-bit i80 bus, FT6336 capacitive touch, backlight on GPIO45. The panel is used in
// landscape (480x320). Pinout: WT32-SC01-Plus datasheet V1.3 / RIOT board docs.
#include "sdkconfig.h"
#if CONFIG_WP_BOARD_WT32_SC01_PLUS

#include "bsp/board.h"
#include "bsp/lvgl_port.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

#define PIN_LCD_DC      GPIO_NUM_0     // also the BOOT strap; only driven after boot
#define PIN_LCD_WR      GPIO_NUM_47
#define PIN_LCD_RST     GPIO_NUM_4     // shared with the touch controller's reset
#define PIN_LCD_BL      GPIO_NUM_45
static const int s_data_pins[8] = {9, 46, 3, 8, 18, 17, 16, 15};

#define PIN_TP_SDA      GPIO_NUM_6
#define PIN_TP_SCL      GPIO_NUM_5
#define PIN_TP_INT      GPIO_NUM_7
#define TP_I2C_HZ       400000

// Native panel orientation is portrait (320x480). Landscape with the USB-C port on the
// right-hand side when viewed from the front.
#define LCD_SWAP_XY     true
#define LCD_MIRROR_X    true
#define LCD_MIRROR_Y    true
#define TP_SWAP_XY      true
#define TP_MIRROR_X     false
#define TP_MIRROR_Y     true

#define BL_MODE         LEDC_LOW_SPEED_MODE
#define BL_TIMER        LEDC_TIMER_0
#define BL_CHANNEL      LEDC_CHANNEL_0
#define BL_RES          LEDC_TIMER_10_BIT
#define BL_MAX          ((1 << 10) - 1)

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    return lvgl_port_notify_flush_done();
}

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = BL_MODE,
        .duty_resolution = BL_RES,
        .timer_num = BL_TIMER,
        .freq_hz = 20000,   // above the audible range
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&t);
    if (err != ESP_OK) return err;
    ledc_channel_config_t c = {
        .gpio_num = PIN_LCD_BL,
        .speed_mode = BL_MODE,
        .channel = BL_CHANNEL,
        .timer_sel = BL_TIMER,
        .duty = 0,   // dark until the UI has drawn its first frame
    };
    err = ledc_channel_config(&c);
    if (err != ESP_OK) return err;
    return ledc_fade_func_install(0);
}

esp_err_t board_backlight(bool on, uint32_t ramp_ms)
{
    uint32_t duty = on ? BL_MAX : 0;
    if (ramp_ms == 0) {
        esp_err_t err = ledc_set_duty(BL_MODE, BL_CHANNEL, duty);
        return err == ESP_OK ? ledc_update_duty(BL_MODE, BL_CHANNEL) : err;
    }
    esp_err_t err = ledc_set_fade_with_time(BL_MODE, BL_CHANNEL, duty, ramp_ms);
    return err == ESP_OK ? ledc_fade_start(BL_MODE, BL_CHANNEL, LEDC_FADE_WAIT_DONE) : err;
}

esp_err_t board_framebuffer(esp_lcd_panel_handle_t panel, void **fb)
{
    return ESP_ERR_NOT_SUPPORTED;   // the frame memory is inside the ST7796
}

static esp_err_t touch_init(esp_lcd_touch_handle_t *ret_touch)
{
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_TP_SDA,
        .scl_io_num = PIN_TP_SCL,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) return err;

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    io_cfg.scl_speed_hz = TP_I2C_HZ;
    err = esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io);
    if (err != ESP_OK) return err;

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_V_RES,   // native (portrait) coordinates
        .y_max = LCD_H_RES,
        .rst_gpio_num = GPIO_NUM_NC,   // reset together with the LCD
        .int_gpio_num = GPIO_NUM_NC,   // polled
        .flags = {
            .swap_xy = TP_SWAP_XY,
            .mirror_x = TP_MIRROR_X,
            .mirror_y = TP_MIRROR_Y,
        },
    };
    return esp_lcd_touch_new_i2c_ft5x06(io, &tp_cfg, ret_touch);
}

esp_err_t board_display_init(esp_lcd_panel_handle_t *ret_panel, esp_lcd_touch_handle_t *ret_touch)
{
    esp_err_t err = backlight_init();
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "ST7796 on i80 bus, %d MHz", CONFIG_LCD_I80_PCLK_MHZ);
    esp_lcd_i80_bus_handle_t bus = NULL;
    esp_lcd_i80_bus_config_t bus_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num = PIN_LCD_DC,
        .wr_gpio_num = PIN_LCD_WR,
        .bus_width = 8,
        .max_transfer_bytes = LCD_H_RES * LVGL_PORT_BUFFER_HEIGHT * 2,
        .dma_burst_size = 64,
    };
    for (int i = 0; i < 8; i++) bus_cfg.data_gpio_nums[i] = s_data_pins[i];
    err = esp_lcd_new_i80_bus(&bus_cfg, &bus);
    if (err != ESP_OK) return err;

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = GPIO_NUM_NC,   // CS is tied low on the board
        .pclk_hz = CONFIG_LCD_I80_PCLK_MHZ * 1000 * 1000,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_trans_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 1,
        },
        .flags = {
            .swap_color_bytes = 1,   // LVGL renders little-endian RGB565, the bus sends MSB first
        },
    };
    err = esp_lcd_new_panel_io_i80(bus, &io_cfg, &io);
    if (err != ESP_OK) return err;

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7796(io, &panel_cfg, &panel);
    if (err != ESP_OK) return err;
    esp_lcd_panel_reset(panel);   // also resets the touch controller (shared line)
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, true);
    esp_lcd_panel_swap_xy(panel, LCD_SWAP_XY);
    esp_lcd_panel_mirror(panel, LCD_MIRROR_X, LCD_MIRROR_Y);
    esp_lcd_panel_disp_on_off(panel, true);

    vTaskDelay(pdMS_TO_TICKS(200));   // FT6336 start-up after the shared reset
    esp_lcd_touch_handle_t tp = NULL;
    err = touch_init(&tp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FT6336 init failed: %s", esp_err_to_name(err));
        tp = NULL;
    }
    *ret_panel = panel;
    *ret_touch = tp;
    return ESP_OK;
}

#endif // CONFIG_WP_BOARD_WT32_SC01_PLUS
