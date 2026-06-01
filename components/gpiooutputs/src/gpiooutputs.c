#include "gpiooutputs.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "gpiooutputs";
#define GPIO_AV0  GPIO_NUM_42
#define GPIO_AV1  GPIO_NUM_41


void av_pwm_apply(uint32_t instance, float percent)
{
    if (percent < 0.0f)   percent = 0.0f;
    if (percent > 100.0f) percent = 100.0f;
    uint32_t duty = (uint32_t)((percent / 100.0f) * 255.0f);
    
    if (instance == 0) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        ESP_LOGI(TAG, "[AV0] PWM → GPIO42 : %.1f%% (duty=%lu)", percent, (unsigned long)duty);
    } else if (instance == 1) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        ESP_LOGI(TAG, "[AV1] PWM → GPIO41 : %.1f%% (duty=%lu)", percent, (unsigned long)duty);
    }
}


void ledc_initialize(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT, .freq_hz = 4000,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel0 = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0, .intr_type = LEDC_INTR_DISABLE,
        .gpio_num   = GPIO_AV0, .duty = 0, .hpoint = 0
    };
    ledc_channel_config(&ledc_channel0);

    ledc_channel_config_t ledc_channel1 = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_1,
        .timer_sel  = LEDC_TIMER_0, .intr_type = LEDC_INTR_DISABLE,
        .gpio_num   = GPIO_AV1, .duty = 0, .hpoint = 0
    };
    ledc_channel_config(&ledc_channel1);
}