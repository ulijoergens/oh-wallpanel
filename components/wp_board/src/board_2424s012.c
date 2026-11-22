// ESP32-2424S012: ESP32-C3 (4 MB flash, no PSRAM), round 1.28" 240x240 GC9A01 on SPI,
// CST816D capacitive touch, backlight on GPIO3. Pinout from community configurations
// (ESPHome / LVGL forum); the LCD has no reset line (software reset).
#include "sdkconfig.h"
#if CONFIG_WP_BOARD_ESP32_2424S012

#include "bsp/board.h"
#include "bsp/lvgl_port.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

#define LCD_HOST        SPI2_HOST
#define PIN_LCD_SCLK    GPIO_NUM_6
#define PIN_LCD_MOSI    GPIO_NUM_7
#define PIN_LCD_CS      GPIO_NUM_10
#define PIN_LCD_DC      GPIO_NUM_2     // a strapping pin; only driven after boot
#define PIN_LCD_BL      GPIO_NUM_3
#define LCD_PCLK_HZ     (40 * 1000 * 1000)

#define PIN_TP_SDA      GPIO_NUM_4
#define PIN_TP_SCL      GPIO_NUM_5
#define PIN_TP_RST      GPIO_NUM_1
#define TP_I2C_HZ       400000
#define CST816_REG_DIS_AUTO_SLEEP 0xFE   // non-zero: stay awake, so polling always gets an ACK

// Orientation with the USB-C socket at the bottom. To be verified on the board (console
// 'touchtest'): the dot must follow the finger.
#define LCD_MIRROR_X    false
#define LCD_MIRROR_Y    false
#define TP_SWAP_XY      false
#define TP_MIRROR_X     false
#define TP_MIRROR_Y     false

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
    return ESP_ERR_NOT_SUPPORTED;   // the frame memory is inside the GC9A01, write-only
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
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    io_cfg.scl_speed_hz = TP_I2C_HZ;
    err = esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io);
    if (err != ESP_OK) return err;

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = PIN_TP_RST,
        .int_gpio_num = GPIO_NUM_NC,   // polled
        .flags = {
            .swap_xy = TP_SWAP_XY,
            .mirror_x = TP_MIRROR_X,
            .mirror_y = TP_MIRROR_Y,
        },
    };
    err = esp_lcd_touch_new_i2c_cst816s(io, &tp_cfg, ret_touch);
    if (err != ESP_OK) return err;
    // The CST816 sleeps a few seconds after the last touch and then NACKs every poll.
    uint8_t one = 1;
    if (esp_lcd_panel_io_tx_param(io, CST816_REG_DIS_AUTO_SLEEP, &one, 1) != ESP_OK) {
        ESP_LOGW(TAG, "CST816: could not disable auto-sleep");
    }
    return ESP_OK;
}

esp_err_t board_display_init(esp_lcd_panel_handle_t *ret_panel, esp_lcd_touch_handle_t *ret_touch)
{
    esp_err_t err = backlight_init();
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "GC9A01 on SPI, %d MHz", LCD_PCLK_HZ / 1000000);
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H_RES * LVGL_PORT_BUFFER_HEIGHT * 2,
    };
    err = spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_trans_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io);
    if (err != ESP_OK) return err;

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_gc9a01(io, &panel_cfg, &panel);
    if (err != ESP_OK) return err;
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, true);
    esp_lcd_panel_mirror(panel, LCD_MIRROR_X, LCD_MIRROR_Y);
    esp_lcd_panel_disp_on_off(panel, true);

    esp_lcd_touch_handle_t tp = NULL;
    err = touch_init(&tp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CST816D init failed: %s", esp_err_to_name(err));
        tp = NULL;
    }
    *ret_panel = panel;
    *ret_touch = tp;
    return ESP_OK;
}

#endif // CONFIG_WP_BOARD_ESP32_2424S012
