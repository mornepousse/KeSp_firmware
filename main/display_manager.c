#include "display_manager.h"
#include "esp_lcd_gc9a01.h"
#include <stdio.h>
#include "lvgl.h"
#include "font/lv_font.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "core/lv_obj.h"
#include "esp_err.h"
#include "esp_log.h"
#include "imgs.h"
#include "misc/lv_area.h"
#include "graphic_manager.h"


static const char *TAG_DM = "Display_Manager";

#define LCD_HOST               SPI2_HOST
#define LCD_H_RES              (240)
#define LCD_V_RES              (240)
#define LCD_BIT_PER_PIXEL      (16)
#define LCD_PIXEL_CLOCK_HZ     (40 * 1000 * 1000) // 40 MHz
#define LCD_CMD_BITS           8
#define LCD_PARAM_BITS         8

#define PIN_NUM_LCD_CS         (GPIO_NUM_1)
#define PIN_NUM_LCD_PCLK       (GPIO_NUM_41)
#define PIN_NUM_LCD_DATA0      (GPIO_NUM_42)
#define PIN_NUM_LCD_RST        (GPIO_NUM_44)
#define PIN_NUM_LCD_DC         (GPIO_NUM_2)

#define DISP_LVGL_TICK_PERIOD_MS    2
esp_lcd_panel_handle_t panel_handle = NULL;
lv_disp_drv_t disp_drv;  // contains callback functions


void lvgl_time_task(void* param);

bool display_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}
static void disp_lvgl_port_update_callback(lv_disp_drv_t *drv)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;

    switch (drv->rotated) {
    case LV_DISP_ROT_NONE:
        // Rotate LCD display
        esp_lcd_panel_swap_xy(panel_handle, false);
        esp_lcd_panel_mirror(panel_handle, true, false);

        break;
    case LV_DISP_ROT_90:
        // Rotate LCD display
        esp_lcd_panel_swap_xy(panel_handle, true);
        esp_lcd_panel_mirror(panel_handle, true, true);

        break;
    case LV_DISP_ROT_180:
        // Rotate LCD display
        esp_lcd_panel_swap_xy(panel_handle, false);
        esp_lcd_panel_mirror(panel_handle, false, true);

        break;
    case LV_DISP_ROT_270:
        // Rotate LCD display
        esp_lcd_panel_swap_xy(panel_handle, true);
        esp_lcd_panel_mirror(panel_handle, false, false);

        break;
    }
}
void gc9a01_displayInit(void)
{
    ESP_LOGI(TAG_DM, "Initialize SPI bus");
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_LCD_PCLK,
        .mosi_io_num = PIN_NUM_LCD_DATA0,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG_DM, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_LCD_DC,
        .cs_gpio_num = PIN_NUM_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = display_notify_lvgl_flush_ready,
        .user_ctx = &disp_drv,
    };
    // Attach the LCD to the SPI bus
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    ESP_LOGI(TAG_DM, "Install GC9A01 panel driver");

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_LCD_RST,

        //.rgb_endian = LCD_RGB_ENDIAN_RGB, LCD_RGB_ENDIAN_BGR

		//.rgb_ele_order = LCD_RGB_ENDIAN_RGB,
        .rgb_endian = LCD_RGB_ENDIAN_BGR,

        .bits_per_pixel = 16,
    };


    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle));


    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));

    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));

    // user can flush pre-defined pattern to the screen before we turn on the screen or backlight
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

}
static void disp_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;
    // copy a buffer's content to a specific area of the display
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
}

static void disp_increase_lvgl_tick(void *arg)
{
    /* Tell LVGL how many milliseconds has elapsed */
    lv_tick_inc(DISP_LVGL_TICK_PERIOD_MS);
}

void init_display()
{
    gc9a01_displayInit();
    static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)

    ESP_LOGI(TAG_DM, "Initialize LVGL library");
    lv_init();
    // alloc draw buffers used by LVGL
    // it's recommended to choose the size of the draw buffer(s) to be at least 1/10 screen sized
    lv_color_t *buf1 = heap_caps_malloc(LCD_H_RES * 20 * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf1);
    lv_color_t *buf2 = heap_caps_malloc(LCD_H_RES * 20 * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf2);
    // initialize LVGL draw buffers
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, LCD_H_RES * 20);

    ESP_LOGI(TAG_DM, "Register display driver to LVGL");
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = disp_lvgl_flush_cb;
    disp_drv.drv_update_cb = disp_lvgl_port_update_callback;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

    ESP_LOGI(TAG_DM, "Install LVGL tick timer");
    // Tick interface for LVGL (using esp_timer to generate 2ms periodic event)
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &disp_increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, DISP_LVGL_TICK_PERIOD_MS * 1000));


    ESP_LOGI(TAG_DM, "Display LVGL Meter Widget");

    xTaskCreatePinnedToCore(lvgl_time_task, "lvgl_time_task", 10000, NULL, 4, NULL, 1);

    //example_lvgl_demo_ui(disp);
    // fill color
    lv_color_t color = LV_COLOR_MAKE(0xFF, 0x00, 0x00);
    lv_obj_t *label = lv_label_create(lv_scr_act());
    
    lv_label_set_text(label, "Mae");
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0); // Use a larger font size
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);


}

void write_txt(const char *txt, int x, int y)
{
    lv_color_t color = LV_COLOR_MAKE(0xFF, 0x00, 0x00);
    lv_color_t colorf = LV_COLOR_MAKE(0x00, 0x00, 0x00);
    lv_obj_t *label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, txt);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_bg_color(label, colorf, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0); // Use a larger font size
    lv_obj_align(label, LV_ALIGN_CENTER, x, y);
}

#define SYSTEM_BUFFER_SIZE					4
typedef struct
{
	uint8_t packet_size;
	uint16_t data[SYSTEM_BUFFER_SIZE];
}system_packet;
void lvgl_time_task(void* param)
{
	TickType_t xLastWakeTime = xTaskGetTickCount();

	system_packet system_buffer = {0};
	while(1)
	{
        graphic_update();
        lv_timer_handler();
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(10));
	}
}

