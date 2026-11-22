/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
// Waveshare ESP32-S3-Touch-LCD-4.3 (vendored from bobscott45/waveshare_esp32_s3_touch_lcd_4_3).
#include "sdkconfig.h"
#if CONFIG_WP_BOARD_WAVESHARE_43

#include "bsp/board.h"
#include "bsp/lvgl_port.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CONFIG_LCD_TOUCH_CONTROLLER_GT911 1

#define I2C_MASTER_SCL_IO       9
#define I2C_MASTER_SDA_IO       8
#define I2C_MASTER_NUM          0
#define I2C_MASTER_FREQ_HZ      400000
#define I2C_MASTER_TIMEOUT_MS   1000

#define GPIO_INPUT_IO_4         4   // GT911 INT, selects the I2C address during reset
#define GPIO_INPUT_PIN_SEL      (1ULL << GPIO_INPUT_IO_4)

#define LCD_PIXEL_CLOCK_HZ      (14 * 1000 * 1000)
#define RGB_DATA_WIDTH          16
#define LCD_IO_RGB_DISP         (-1)
#define LCD_IO_RGB_VSYNC        (GPIO_NUM_3)
#define LCD_IO_RGB_HSYNC        (GPIO_NUM_46)
#define LCD_IO_RGB_DE           (GPIO_NUM_5)
#define LCD_IO_RGB_PCLK         (GPIO_NUM_7)
#define LCD_IO_RGB_DATA0        (GPIO_NUM_14)
#define LCD_IO_RGB_DATA1        (GPIO_NUM_38)
#define LCD_IO_RGB_DATA2        (GPIO_NUM_18)
#define LCD_IO_RGB_DATA3        (GPIO_NUM_17)
#define LCD_IO_RGB_DATA4        (GPIO_NUM_10)
#define LCD_IO_RGB_DATA5        (GPIO_NUM_39)
#define LCD_IO_RGB_DATA6        (GPIO_NUM_0)
#define LCD_IO_RGB_DATA7        (GPIO_NUM_45)
#define LCD_IO_RGB_DATA8        (GPIO_NUM_48)
#define LCD_IO_RGB_DATA9        (GPIO_NUM_47)
#define LCD_IO_RGB_DATA10       (GPIO_NUM_21)
#define LCD_IO_RGB_DATA11       (GPIO_NUM_1)
#define LCD_IO_RGB_DATA12       (GPIO_NUM_2)
#define LCD_IO_RGB_DATA13       (GPIO_NUM_42)
#define LCD_IO_RGB_DATA14       (GPIO_NUM_41)
#define LCD_IO_RGB_DATA15       (GPIO_NUM_40)
#define PIN_NUM_TOUCH_RST       (-1)
#define PIN_NUM_TOUCH_INT       (-1)

static const char *TAG = "DISPLAY";

static i2c_master_bus_handle_t i2c_bus_handle = NULL;
static i2c_master_dev_handle_t ch422g_ctrl_handle = NULL; // 0x24
static i2c_master_dev_handle_t ch422g_out_handle = NULL;  // 0x38


#if CONFIG_LCD_TOUCH_CONTROLLER_GT911
/**
 * @brief I2C master initialization
 */
    static esp_err_t i2c_master_init(void)
    {
        // Prevent double initialization
        if (i2c_bus_handle != NULL) {
            return ESP_OK;
        }

        // Configure the physical I2C bus
        i2c_master_bus_config_t i2c_bus_conf = {
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .i2c_port = I2C_MASTER_NUM,
            .sda_io_num = I2C_MASTER_SDA_IO,
            .scl_io_num = I2C_MASTER_SCL_IO,
            .flags.enable_internal_pullup = true,
        };
        esp_err_t ret = i2c_new_master_bus(&i2c_bus_conf, &i2c_bus_handle);
        if (ret != ESP_OK) {
            return ret;
        }

        // Register CH422G Control interface (Address 0x24)
        i2c_device_config_t ctrl_dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = 0x24,
            .scl_speed_hz = I2C_MASTER_FREQ_HZ,
        };
        ret = i2c_master_bus_add_device(i2c_bus_handle, &ctrl_dev_cfg, &ch422g_ctrl_handle);
        if (ret != ESP_OK) {
            return ret;
        }

        // Register CH422G Output interface (Address 0x38)
        i2c_device_config_t out_dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = 0x38,
            .scl_speed_hz = I2C_MASTER_FREQ_HZ,
        };
        return i2c_master_bus_add_device(i2c_bus_handle, &out_dev_cfg, &ch422g_out_handle);
    }



// GPIO initialization
static esp_err_t gpio_init(void)
{
    // Zero-initialize the config structure
    gpio_config_t io_conf = {};
    // Disable interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    // Bit mask of the pins, use GPIO4 here
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    // Set as input mode
    io_conf.mode = GPIO_MODE_OUTPUT;

    return gpio_config(&io_conf);
}

/*
 * CH422G output bits (Waveshare ESP32-S3-Touch-LCD-4.3 schematic):
 *   EXIO1 TP_RST, EXIO2 DISP (backlight), EXIO3 LCD_RST, EXIO4 SD_CS, EXIO5 USB_SEL.
 * USB_SEL high routes GPIO19/20 to the CAN transceiver and drops the native USB port,
 * so it is kept low at all times. The upstream BSP wrote 0x2C/0x2E here, which set it.
 * Every write goes through this shadow register (1-byte transfers, REQ-HW-3).
 */
#define EXIO_TP_RST  (1u << 1)
#define EXIO_BL      (1u << 2)
#define EXIO_LCD_RST (1u << 3)
#define EXIO_SD_CS   (1u << 4)
#define EXIO_USB_SEL (1u << 5)
static uint8_t s_exio = EXIO_TP_RST | EXIO_LCD_RST | EXIO_SD_CS;

static esp_err_t ch422g_write(uint8_t val)
{
    uint8_t mode = 0x01; /* push-pull outputs enabled */
    esp_err_t ret = i2c_master_transmit(ch422g_ctrl_handle, &mode, 1, I2C_MASTER_TIMEOUT_MS);
    if (ret != ESP_OK) return ret;
    val &= (uint8_t)~EXIO_USB_SEL;
    ret = i2c_master_transmit(ch422g_out_handle, &val, 1, I2C_MASTER_TIMEOUT_MS);
    if (ret == ESP_OK) s_exio = val;
    return ret;
}

// Reset the touch screen; GPIO4 (GT911 INT) low during reset selects address 0x5D.
static esp_err_t waveshare_esp32_s3_touch_reset()
{
    esp_err_t ret = ch422g_write((s_exio & ~EXIO_TP_RST) | EXIO_LCD_RST);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(100));
    ret = gpio_set_level(GPIO_INPUT_IO_4, 0);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(100));
    ret = ch422g_write(s_exio | EXIO_TP_RST);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(200));
    return ESP_OK;
}

#endif

// Initialize RGB LCD
esp_err_t board_display_init(esp_lcd_panel_handle_t *ret_panel, esp_lcd_touch_handle_t *ret_touch)
    {
        ESP_LOGI(TAG, "Install RGB LCD panel driver"); // Log the start of the RGB LCD panel driver installation
        esp_lcd_panel_handle_t panel_handle = NULL; // Declare a handle for the LCD panel
        esp_lcd_rgb_panel_config_t panel_config = {
            .clk_src = LCD_CLK_SRC_DEFAULT, // Set the clock source for the panel
            .timings =  {
                .pclk_hz = LCD_PIXEL_CLOCK_HZ, // Pixel clock frequency
                .h_res = LCD_H_RES, // Horizontal resolution
                .v_res = LCD_V_RES, // Vertical resolution
                .hsync_pulse_width = 4, // Horizontal sync pulse width
                .hsync_back_porch = 8, // Horizontal back porch
                .hsync_front_porch = 8, // Horizontal front porch
                .vsync_pulse_width = 4, // Vertical sync pulse width
                .vsync_back_porch = 8, // Vertical back porch
                .vsync_front_porch = 8, // Vertical front porch
                .flags = {
                    .pclk_active_neg = 1, // Active low pixel clock
                },
            },
            .data_width = RGB_DATA_WIDTH, // Data width for RGB
            .num_fbs = LVGL_PORT_LCD_RGB_BUFFER_NUMS, // Number of frame buffers
            .bounce_buffer_size_px = RGB_BOUNCE_BUFFER_SIZE, // Bounce buffer size in pixels
            .dma_burst_size = 64,
            .hsync_gpio_num = LCD_IO_RGB_HSYNC, // GPIO number for horizontal sync
            .vsync_gpio_num = LCD_IO_RGB_VSYNC, // GPIO number for vertical sync
            .de_gpio_num = LCD_IO_RGB_DE, // GPIO number for data enable
            .pclk_gpio_num = LCD_IO_RGB_PCLK, // GPIO number for pixel clock
            .disp_gpio_num = LCD_IO_RGB_DISP, // GPIO number for display
            .data_gpio_nums = {
                LCD_IO_RGB_DATA0,
                LCD_IO_RGB_DATA1,
                LCD_IO_RGB_DATA2,
                LCD_IO_RGB_DATA3,
                LCD_IO_RGB_DATA4,
                LCD_IO_RGB_DATA5,
                LCD_IO_RGB_DATA6,
                LCD_IO_RGB_DATA7,
                LCD_IO_RGB_DATA8,
                LCD_IO_RGB_DATA9,
                LCD_IO_RGB_DATA10,
                LCD_IO_RGB_DATA11,
                LCD_IO_RGB_DATA12,
                LCD_IO_RGB_DATA13,
                LCD_IO_RGB_DATA14,
                LCD_IO_RGB_DATA15,
            },
            .flags = {
                .fb_in_psram = 1, // Use PSRAM for framebuffer
            },
        };

        // Create a new RGB panel with the specified configuration
        ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &panel_handle));

        ESP_LOGI(TAG, "Initialize RGB LCD panel"); // Log the initialization of the RGB LCD panel
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle)); // Initialize the LCD panel

        esp_lcd_touch_handle_t tp_handle = NULL; // Declare a handle for the touch panel
    #if CONFIG_LCD_TOUCH_CONTROLLER_GT911
        ESP_LOGI(TAG, "Initialize I2C bus"); // Log the initialization of the I2C bus
        esp_err_t ret = i2c_master_init(); // Initialize the I2C master
        if (ret != ESP_OK) return ret;
        ESP_LOGI(TAG, "Initialize GPIO"); // Log GPIO initialization
        ret = gpio_init(); // Initialize GPIO pins
        if ( ret != ESP_OK) return ret; 
        ESP_LOGI(TAG, "Initialize Touch LCD"); // Log touch LCD initialization
        ret = waveshare_esp32_s3_touch_reset(); // Reset the touch panel
        if (ret != ESP_OK) return ret;

        esp_lcd_panel_io_handle_t tp_io_handle = NULL; // Declare a handle for touch panel I/O
        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
        tp_io_config.scl_speed_hz = I2C_MASTER_FREQ_HZ;

        // Configure I2C for GT911 touch controller
        ESP_LOGI(TAG, "Initialize I2C panel IO"); // Log I2C panel I/O initialization
        esp_err_t io_ret = esp_lcd_new_panel_io_i2c(i2c_bus_handle, &tp_io_config, &tp_io_handle);
        if (io_ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_lcd_new_panel_io_i2c failed: %s", esp_err_to_name(io_ret));
        }

        ESP_LOGI(TAG, "Initialize touch controller GT911"); // Log touch controller initialization
        const esp_lcd_touch_config_t tp_cfg = {
            .x_max = LCD_H_RES, // Set maximum X coordinate
            .y_max = LCD_V_RES, // Set maximum Y coordinate
            .rst_gpio_num = PIN_NUM_TOUCH_RST, // GPIO number for reset
            .int_gpio_num = PIN_NUM_TOUCH_INT, // GPIO number for interrupt
            .levels = {
                .reset = 0, // Reset level
                .interrupt = 0, // Interrupt level
            },
            .flags = {
                .swap_xy = 0, // No swap of X and Y
                .mirror_x = 0, // No mirroring of X
                .mirror_y = 0, // No mirroring of Y
            },
        };
        esp_err_t touch_ret = esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &tp_handle);
        if (touch_ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_lcd_touch_new_i2c_gt911 failed: %s", esp_err_to_name(touch_ret));
        }
    #endif // CONFIG_LCD_TOUCH_CONTROLLER_GT911

        if (ret_panel) {
            *ret_panel = panel_handle;
        }
        if (ret_touch) {
            *ret_touch = tp_handle;
        }
        return ESP_OK; // Return success
    }


static esp_err_t bl_on(void)  { return ch422g_write(s_exio | EXIO_BL); }
static esp_err_t bl_off(void) { return ch422g_write(s_exio & ~EXIO_BL); }

// Output register only (the mode register is already set): ~0.1 ms per write.
static esp_err_t ch422g_out(uint8_t val)
{
    val &= (uint8_t)~EXIO_USB_SEL;
    esp_err_t ret = i2c_master_transmit(ch422g_out_handle, &val, 1, I2C_MASTER_TIMEOUT_MS);
    if (ret == ESP_OK) s_exio = val;
    return ret;
}

static void wait_until(int64_t t)
{
    while (esp_timer_get_time() < t) {
    }
}

// Switching the backlight in one step dipped the board's 3.3 V below the brownout level
// (2.44 V) on USB power, resetting the panel (2026-11-22). A 1 ms software PWM whose duty
// ramps from 0 to 100 % (or back) spreads the current step over ramp_ms.
esp_err_t board_backlight(bool on, uint32_t ramp_ms)
{
    if (ramp_ms == 0) return on ? bl_on() : bl_off();
    const int period_us = 1000;
    const int steps = ramp_ms * 1000 / period_us;
    esp_err_t ret = ch422g_write(s_exio);   // make sure the outputs are enabled
    uint8_t hi = s_exio | EXIO_BL, lo = s_exio & ~EXIO_BL;
    for (int i = 1; i <= steps && ret == ESP_OK; i++) {
        int on_us = (on ? i : steps - i) * period_us / steps;
        int64_t t0 = esp_timer_get_time();
        if (on_us > 0) {
            ret = ch422g_out(hi);
            wait_until(t0 + on_us);
        }
        if (ret == ESP_OK && on_us < period_us) {
            ret = ch422g_out(lo);
            wait_until(t0 + period_us);
        }
    }
    if (ret == ESP_OK) ret = ch422g_out(on ? hi : lo);
    return ret;
}

esp_err_t board_framebuffer(esp_lcd_panel_handle_t panel, void **fb)
{
    return esp_lcd_rgb_panel_get_frame_buffer(panel, 1, fb);
}

#endif // CONFIG_WP_BOARD_WAVESHARE_43
