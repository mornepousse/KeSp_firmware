#include "ICSH044A.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "matrix.h"
#include "keyboard_manager.h"
#include "key_definitions.h"

static const char *BALL_TAG = "BALL";
#define CURSOR_UP (GPIO_NUM_7)
#define CURSOR_DWN (GPIO_NUM_15)
#define CURSOR_LFT (GPIO_NUM_5)
#define CURSOR_RHT (GPIO_NUM_6)
#define CURSOR_LED_RED_PIN (GPIO_NUM_8)
#define CURSOR_LED_GRN_PIN (GPIO_NUM_17)
#define CURSOR_LED_BLU_PIN (GPIO_NUM_18)
#define CURSOR_LED_WHT_PIN (GPIO_NUM_16)


#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_OUTPUT_IO          (CURSOR_LED_GRN_PIN) // Define the output GPIO
#define LEDC_CHANNEL            LEDC_CHANNEL_0
#define LEDC_DUTY_RES           LEDC_TIMER_13_BIT // Set duty resolution to 13 bits
#define LEDC_DUTY               (4096) // Set duty to 50%. (2 ** 13) * 50% = 4096
#define LEDC_FREQUENCY          (4000) // Frequency in Hertz. Set frequency at 4 kHz

uint16_t Led_I_GRN = 0;
uint8_t Led_UP_DOWN_GRN = 0;
uint16_t Led_I_RED = 0;
uint8_t Led_UP_DOWN_RED = 0;

uint8_t cursor_states[4] = {0, 0, 0, 0}; // up, down, left, right
uint8_t cursor_pressed_state = 0;


void initISH044A()
{
    gpio_set_direction(CURSOR_UP, GPIO_MODE_INPUT);
    gpio_set_direction(CURSOR_DWN, GPIO_MODE_INPUT);
    gpio_set_direction(CURSOR_LFT, GPIO_MODE_INPUT);
    gpio_set_direction(CURSOR_RHT, GPIO_MODE_INPUT);

    gpio_set_direction(CURSOR_LED_RED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(CURSOR_LED_GRN_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(CURSOR_LED_BLU_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(CURSOR_LED_WHT_PIN, GPIO_MODE_OUTPUT);

    gpio_set_level(CURSOR_LED_RED_PIN, 0);
    gpio_set_level(CURSOR_LED_GRN_PIN, 0);
    gpio_set_level(CURSOR_LED_BLU_PIN, 0);
    gpio_set_level(CURSOR_LED_WHT_PIN, 0);
}

void getLvltrackball(uint8_t CURSOR_Dir, uint8_t key, uint8_t cursor_Selected_state)
{
    uint8_t state = gpio_get_level(CURSOR_Dir);
    if (cursor_states[cursor_Selected_state] != state)
    {

        cursor_states[cursor_Selected_state] = state;
        if (state == 1)
        {

            for (uint8_t i = 0; i < 6; i++)
            {
                if (keycodes[0] == 0)
                {
                    keycodes[0] = key;
                    break;
                }
            }

            send_hid_key(keycodes);
            if (cursor_pressed_state == 0)
            {
                cursor_pressed_state = 1;
            }
        }
        else
        {
            for (uint8_t i = 0; i < 6; i++)
            {
                if (keycodes[0] == key)
                {
                    keycodes[0] = 0;
                    break;
                }
            }
            cursor_pressed_state = 0;
            send_hid_key(keycodes);
        }
    }
}

void vTaskTrackBall(void *pvParameters) // ICSH044A
{
    for (;;)
    {
        // ESP_LOGI(TAG, "test %d", gpio_get_level(CURSOR_UP));

        if (tud_mounted())
        {
            getLvltrackball(CURSOR_UP, K_UP, 0);
            getLvltrackball(CURSOR_DWN, K_DOWN, 1);
            getLvltrackball(CURSOR_LFT, K_LEFT, 2);
            getLvltrackball(CURSOR_RHT, K_RIGHT, 3);

            if (cursor_pressed_state != 0)
            {
                cursor_pressed_state++;
                ESP_LOGI(BALL_TAG, "state cursor");
            }
            if (cursor_pressed_state >= 100)
            {
                ESP_LOGI(BALL_TAG, "cursor_pressed_state %d", cursor_pressed_state);
                cursor_pressed_state = 0;
                for (uint8_t i = 0; i < 6; i++)
                {
                    if (keycodes[i] == K_UP || keycodes[i] == K_DOWN 
                    || keycodes[i] == K_LEFT || keycodes[i] == K_RIGHT)
                    {
                        keycodes[i] = 0;
                    }
                }
            }
            send_hid_key(keycodes);
        }

        // Get the next x and y delta in the draw square pattern
        // mouse_draw_square_next_delta(&delta_x, &delta_y);
        // tud_hid_mouse_report(HID_ITF_PROTOCOL_MOUSE, 0x00, delta_x, delta_y, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(4));
    }
}

void example_ledc_init(void)
{
    // Prepare and then apply the LEDC PWM timer configuration
    ledc_timer_config_t ledc_timer_green = {
        .speed_mode       = LEDC_MODE,
        .duty_resolution  = LEDC_DUTY_RES,
        .timer_num        = LEDC_TIMER,
        .freq_hz          = LEDC_FREQUENCY,  // Set output frequency at 4 kHz
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer_green));

    // Prepare and then apply the LEDC PWM channel configuration
    ledc_channel_config_t ledc_channel_green = {
        .speed_mode     = LEDC_MODE,
        .channel        = LEDC_CHANNEL,
        .timer_sel      = LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = LEDC_OUTPUT_IO,
        .duty           = 0, // Set duty to 0%
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel_green));

    //eeeeeeeeeeeeeeeeeeeeeeeeee

    // Prepare and then apply the LEDC PWM timer configuration
    ledc_timer_config_t ledc_timer_red = {
        .speed_mode       = LEDC_MODE,
        .duty_resolution  = LEDC_DUTY_RES,
        .timer_num        = LEDC_TIMER,
        .freq_hz          = LEDC_FREQUENCY,  // Set output frequency at 4 kHz
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer_red));

    // Prepare and then apply the LEDC PWM channel configuration
    ledc_channel_config_t ledc_channel_red = {
        .speed_mode     = LEDC_MODE,
        .channel        = LEDC_CHANNEL_1,
        .timer_sel      = LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = CURSOR_LED_WHT_PIN,
        .duty           = 0, // Set duty to 0%
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel_red));
}

void vTaskLED_Animation(void *pvParameters)
{
    for (;;)
    {
        if(Led_UP_DOWN_GRN == 0)
        {
            Led_I_GRN += 10;
            if(Led_I_GRN >= 4096)
            {
                Led_UP_DOWN_GRN = 1;
            }
        }
        else
        {
            Led_I_GRN -= 10;
            if(Led_I_GRN <= 1024)
            {
                Led_UP_DOWN_GRN = 0;
            }
        }
        ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, Led_I_GRN));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));
        if(Led_UP_DOWN_RED == 0)
        {
            Led_I_RED += 20;
            if(Led_I_RED >= 2096)
            {
                Led_UP_DOWN_RED = 1;
            }
        }
        else
        {
            Led_I_RED -= 20;
            if(Led_I_RED <= 512)
            {
                Led_UP_DOWN_RED = 0;
            }
        }
        ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_1, Led_I_RED));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_1));
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
