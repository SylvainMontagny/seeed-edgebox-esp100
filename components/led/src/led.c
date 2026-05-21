#include "led.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "led_strip.h"
#include "sdkconfig.h"

static const char *TAG = "led";

#define LED_GPIO CONFIG_LED_GPIO

#ifdef CONFIG_LED_TYPE_RMT

static led_strip_handle_t led_strip;

void led_on(void)
{
    led_strip_set_pixel(led_strip, 0, 16, 16, 16);
    led_strip_refresh(led_strip);
}

void led_off(void)
{
    led_strip_clear(led_strip);
}

void led_initialize(void)
{
    ESP_LOGI(TAG, "Example configured to blink addressable LED!");
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    /* Set all LED off to clear all pixels */
    led_strip_clear(led_strip);
}

#elif CONFIG_LED_TYPE_GPIO

void led_on(void)
{
    gpio_set_level(LED_GPIO, 1);
}

void led_off(void)
{
    gpio_set_level(LED_GPIO, 0);
}

void led_initialize(void)
{
    ESP_LOGI(TAG, "Example configured to blink GPIO LED!");
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
}

#endif