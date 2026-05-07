#include "gpiooutputs.h"
#include "driver/ledc.h"
#include "esp_log.h"
static const char *TAG = "gpiooutputs";

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
