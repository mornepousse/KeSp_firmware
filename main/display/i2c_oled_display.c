#include "i2c_oled_display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/i2c.h"
#include "driver/gpio.h"

static const char *TAG_DISP = "DISPL"; 
// Déclaration préalable
bool test_oled_presence(void);

lv_disp_t *disp;
bool display_available = true;
static display_hw_config_t current_cfg = {
    .bus_type = DISPLAY_BUS_I2C,
    .width = 128,
    .height = 64,
    .pixel_clock_hz = 400 * 1000,
    .reset_pin = GPIO_NUM_NC,
    .i2c = {
        .host = I2C_NUM_0,
        .sda = GPIO_NUM_15,
        .scl = GPIO_NUM_7,
        .address = 0x3C,
        .enable_internal_pullups = true,
    },
};

void display_set_hw_config(const display_hw_config_t *config)
{
    if (config == NULL)
        return;
    current_cfg = *config;
}

const display_hw_config_t *display_get_hw_config(void)
{
    return &current_cfg;
}
// Bit number used to represent command and parameter
#define DISPL_LCD_CMD_BITS 8
#define DISPL_LCD_PARAM_BITS 8

void example_lvgl_demo_ui(lv_disp_t *disp)
{
    if(display_available == false) return;
    lv_obj_t *scr = lv_disp_get_scr_act(disp);
    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR); /* Circular scroll */
    lv_label_set_text(label, "Hello, my name is KaSe. I am a keyboard.");
    /* Size of the screen (if you use rotation 90 or 270, please set disp->driver->ver_res) */
    lv_obj_set_width(label, disp->driver->hor_res);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 0);
}

void display_test_text(char *text)
{
    if(display_available == false) return;
    ESP_LOGI(TAG_DISP, "Displaying test text on OLED");

    // Lock the mutex due to the LVGL APIs are not thread-safe
    if (lvgl_port_lock(0))
    {
        lv_obj_t *label = lv_label_create(lv_scr_act());
        lv_label_set_text(label, text);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

        // Release the mutex
        lvgl_port_unlock();
    }
    else
    {
        ESP_LOGE(TAG_DISP, "Failed to lock LVGL port");
    }
}

void display_clear_screen(void)
{
    if(display_available == false) return;
    ESP_LOGI(TAG_DISP, "Clearing OLED display");
    if (lvgl_port_lock(0))
    {
        lv_obj_t *scr = lv_scr_act();
        lv_obj_clean(scr);
        lvgl_port_unlock();
    }
    else
    {
        ESP_LOGE(TAG_DISP, "Failed to lock LVGL port");
    }
}

void init_display(void)
{
    const display_hw_config_t *cfg = &current_cfg;
    if (cfg->bus_type != DISPLAY_BUS_I2C)
    {
        ESP_LOGE(TAG_DISP, "Unsupported display bus type: %d", cfg->bus_type);
        display_available = false;
        return;
    }
    // Tester d'abord si l'écran répond sur le bus I2C
    if (!test_oled_presence())
    {
        ESP_LOGW(TAG_DISP, "OLED non détecté - on saute l'initialisation graphique");
        display_available = false;
        return;
    }

    ESP_LOGI(TAG_DISP, "Initialize I2C bus");
    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port = cfg->i2c.host,
        .sda_io_num = cfg->i2c.sda,
        .scl_io_num = cfg->i2c.scl,
        .flags.enable_internal_pullup = cfg->i2c.enable_internal_pullups,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &i2c_bus));
    ESP_LOGI(TAG_DISP, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = cfg->i2c.address,
        .scl_speed_hz = cfg->pixel_clock_hz,
        .control_phase_bytes = 1,               // According to SSD1306 datasheet
        .lcd_cmd_bits = DISPL_LCD_CMD_BITS,     // According to SSD1306 datasheet
        .lcd_param_bits = DISPL_LCD_PARAM_BITS, // According to SSD1306 datasheet
        .dc_bit_offset = 6,                     // According to SSD1306 datasheet

    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &io_config, &io_handle));
    ESP_LOGI(TAG_DISP, "esp_lcd_new_panel_io_i2c");
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .bits_per_pixel = 1,
        .reset_gpio_num = cfg->reset_pin,
        .color_space = ESP_LCD_COLOR_SPACE_MONOCHROME,
    };
    esp_lcd_panel_ssd1306_config_t ssd1306_config = {
        .height = cfg->height,
    };

    panel_config.vendor_config = &ssd1306_config;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(io_handle, &panel_config, &panel_handle));

    

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG_DISP, "Initialize LVGL");
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&lvgl_cfg);

    ESP_LOGI(TAG_DISP, "Initialized LVGL");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = cfg->width * cfg->height,
        .double_buffer = true,
        .hres = cfg->width,
        .vres = cfg->height,
        .monochrome = true,

        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .sw_rotate = false,
        }};
    disp = lvgl_port_add_disp(&disp_cfg);

    ESP_LOGI(TAG_DISP, "Display LVGL Scroll Text");
    if (display_available)
    {
        display_available = true;
        display_test_text("KaSe_V2");
    }
}

// ------------------------------------------------------------------------------------
// Fonction dédiée : test rapide de présence de l'écran (SSD1306) avant initialisation
// Retourne true si un ACK / write simple réussit, sinon false.
// N'utilise pas l'infrastructure esp_lcd pour isoler les erreurs bas niveau.
// ------------------------------------------------------------------------------------
bool test_oled_presence(void)
{
    //if(display_available == false) return false;
    esp_err_t err;
    i2c_master_bus_handle_t bus = NULL;
    const display_hw_config_t *cfg = &current_cfg;
    if (cfg->bus_type != DISPLAY_BUS_I2C)
        return false;
    i2c_master_bus_config_t bus_config = (i2c_master_bus_config_t){
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port = cfg->i2c.host,
        .sda_io_num = cfg->i2c.sda,
        .scl_io_num = cfg->i2c.scl,
        .flags.enable_internal_pullup = cfg->i2c.enable_internal_pullups,
    };
    err = i2c_new_master_bus(&bus_config, &bus);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_DISP, "Création bus I2C échec: %s", esp_err_to_name(err));
        display_available = false;
        return false;
    }

    i2c_master_dev_handle_t dev = NULL;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg->i2c.address,
        .scl_speed_hz = cfg->pixel_clock_hz,
    };
    err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG_DISP, "Ajout device OLED (0x%02X) échec: %s", cfg->i2c.address, esp_err_to_name(err));
        i2c_del_master_bus(bus);
        display_available = false;
        return false;
    }

    // Paquet minimal : 0x00 (control byte) + 0xAE (DISPLAY OFF) commande inoffensive
    uint8_t pkt[] = {0x00, 0xAE};
    err = i2c_master_transmit(dev, pkt, sizeof(pkt), 50); // timeout ms
    bool ok = (err == ESP_OK);
    if (!ok)
    {
        ESP_LOGW(TAG_DISP, "Aucun ACK OLED @0x%02X (%s)", cfg->i2c.address, esp_err_to_name(err));
    }
    else
    {
        ESP_LOGI(TAG_DISP, "OLED détecté @0x%02X", cfg->i2c.address);
    }

    // Nettoyage (on recréera le bus dans init_display si présent)
    i2c_master_bus_rm_device(dev);
    i2c_del_master_bus(bus);

    display_available = ok;
    return ok;
}

void erase_rectangle(lv_obj_t *rect)
{
    if(display_available == false) return;
    ESP_LOGI(TAG_DISP, "Erasing rectangle");
    if (lvgl_port_lock(0))
    {
        lv_obj_del(rect);
        lvgl_port_unlock();
    }
    else
    {
        ESP_LOGE(TAG_DISP, "Failed to lock LVGL port");
    }
}

void write_text_to_display_centre(const char *text, int x, int y)
{
    if(display_available == false) return;
    ESP_LOGI(TAG_DISP, "Writing text to display: %s", text);
    if (lvgl_port_lock(0))
    {
        lv_obj_t *label = lv_label_create(lv_scr_act());
        lv_label_set_text(label, text);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        lvgl_port_unlock();
    }
    else
    {
        ESP_LOGE(TAG_DISP, "Failed to lock LVGL port");
    }
}
void write_text_to_display(const char *text, int x, int y)
{
    if(display_available == false) return;
    ESP_LOGI(TAG_DISP, "Writing text to display: %s", text);
    if (lvgl_port_lock(0))
    {
        lv_obj_t *label = lv_label_create(lv_scr_act());
        lv_label_set_text(label, text);
        lv_obj_set_pos(label, x, y);
        lvgl_port_unlock();
    }
    else
    {
        ESP_LOGE(TAG_DISP, "Failed to lock LVGL port");
    }
}

void draw_rectangle(int x, int y, int w, int h)
{
    if(display_available == false) return;
    ESP_LOGI(TAG_DISP, "Drawing rectangle at (%d,%d) size %dx%d", x, y, w, h);
    if (lvgl_port_lock(0))
    {
        lv_obj_t *rect = lv_obj_create(lv_scr_act());
        lv_obj_set_size(rect, w, h);
        lv_obj_set_pos(rect, x, y);
        lv_obj_set_style_bg_color(rect, lv_color_white(), 0); // Black background 
        lvgl_port_unlock();
    }
    else
    {
        ESP_LOGE(TAG_DISP, "Failed to lock LVGL port");
    }
}

void draw_rectangle_White(int x, int y, int w, int h)
{
    if(display_available == false) return;
    ESP_LOGI(TAG_DISP, "Drawing rectangle at (%d,%d) size %dx%d", x, y, w, h);
    if (lvgl_port_lock(0))
    {
        lv_obj_t *rect = lv_obj_create(lv_scr_act());
        lv_obj_set_size(rect, w, h);
        lv_obj_set_pos(rect, x, y);
        lv_obj_set_style_bg_color(rect, lv_color_black(), 0); // Black background 
        lvgl_port_unlock();
    }
    else
    {
        ESP_LOGE(TAG_DISP, "Failed to lock LVGL port");
    }
}