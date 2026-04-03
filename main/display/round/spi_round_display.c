/**
 * @file spi_round_display.c
 * @brief Driver for GC9A01 round SPI display (240x240)
 */

#include "spi_round_display.h"
#include "display_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_gc9a01.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

static const char *TAG = "SPI_DISP";

static lv_disp_t *s_disp = NULL;
static bool s_display_available = false;
static display_hw_config_t s_cfg;

bool spi_display_init(const display_hw_config_t *cfg)
{
    if (cfg == NULL || cfg->bus_type != DISPLAY_BUS_SPI) {
        ESP_LOGE(TAG, "Invalid config or not SPI bus type");
        return false;
    }
    
    s_cfg = *cfg;
    
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_handle_t panel_handle = NULL;

    ESP_LOGI(TAG, "Initialize SPI bus for GC9A01");
    spi_bus_config_t buscfg = {
        .sclk_io_num = cfg->spi.sclk,
        .mosi_io_num = cfg->spi.mosi,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = cfg->width * cfg->height * sizeof(uint16_t) + 100,
    };
    esp_err_t ret = spi_bus_initialize(cfg->spi.host, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return false;
    }

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = cfg->spi.dc,
        .cs_gpio_num = cfg->spi.cs,
        .pclk_hz = cfg->pixel_clock_hz,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .flags = {
            .dc_low_on_data = 0,
            .octal_mode = 0,
            .lsb_first = 0,
        },
    };
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)cfg->spi.host, &io_config, &io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel IO init failed: %s", esp_err_to_name(ret));
        return false;
    }

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = cfg->reset_pin,
        .color_space = ESP_LCD_COLOR_SPACE_RGB,
        .bits_per_pixel = 16,
    };
    ret = esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GC9A01 panel init failed: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    
    /* GC9A01 specific settings */
    esp_lcd_panel_invert_color(panel_handle, true);
    esp_lcd_panel_swap_xy(panel_handle, false);
    esp_lcd_panel_mirror(panel_handle, false, false);
    
    /* Backlight on if configured */
    if (cfg->spi.backlight != GPIO_NUM_NC) {
        gpio_set_direction(cfg->spi.backlight, GPIO_MODE_OUTPUT);
        gpio_set_level(cfg->spi.backlight, 1);
    }

    ESP_LOGI(TAG, "Initialize LVGL for round display");
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 1,       /* Below keyboard (prio 3) to avoid lag */
        .task_stack = 6144,
        .task_affinity = 1,       /* Run on core 1, keyboard scans on core 0 */
        .task_max_sleep_ms = 100,
        .timer_period_ms = 33,    /* ~30 fps max */
    };
    lvgl_port_init(&lvgl_cfg);

    /* Quarter-screen buffer: saves RAM for BLE stack */
    uint32_t buf_size = cfg->width * 60;

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = buf_size,
        .double_buffer = false,
        .hres = cfg->width,
        .vres = cfg->height,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = true,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = 1,
            .sw_rotate = false,
        }
    };
    
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return false;
    }

    lv_disp_set_rotation(s_disp, LV_DISP_ROT_NONE);

    s_display_available = true;
    ESP_LOGI(TAG, "GC9A01 round display initialized successfully");
    
    return true;
}

lv_disp_t *spi_display_get_disp(void)
{
    return s_disp;
}

bool spi_display_is_available(void)
{
    return s_display_available;
}

void spi_display_clear(void)
{
    if (!s_display_available) return;
    
    if (lvgl_port_lock(100)) {
        lv_obj_t *scr = lv_scr_act();
        lv_obj_clean(scr);
        lvgl_port_unlock();
    }
}

void spi_display_test_text(const char *text)
{
    if (!s_display_available) return;
    
    if (lvgl_port_lock(100)) {
        lv_obj_t *label = lv_label_create(lv_scr_act());
        lv_label_set_text(label, text);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        lvgl_port_unlock();
    }
}
