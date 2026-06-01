#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "wifi.h"
#include "sdkconfig.h"

#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "wifi";

static EventGroupHandle_t wifi_event_group = NULL;

static bool wifi_initialized = false;

static void event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
            case WIFI_EVENT_STA_START:

                ESP_LOGI(TAG, "STA_START");
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_DISCONNECTED:

                g_connected = false;

                xEventGroupClearBits(
                    wifi_event_group,
                    WIFI_CONNECTED_BIT);

                ESP_LOGW(TAG,
                         "WiFi disconnected -> reconnecting");

                esp_wifi_connect();

                break;

            default:
                break;
        }
    }

    if (event_base == IP_EVENT &&
        event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;

        ESP_LOGI(TAG,
                 "Got IP: " IPSTR,
                 IP2STR(&event->ip_info.ip));

        g_connected = true;

        xEventGroupSetBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT);

            extern bool g_bacnet_started;
extern uint8_t Handler_Transmit_Buffer[];

if (g_bacnet_started)
{
    char ip_str[32];

    esp_ip4addr_ntoa(
        &event->ip_info.ip,
        ip_str,
        sizeof(ip_str));

    setenv("BACNET_IFACE", ip_str, 1);

    ESP_LOGI(TAG,
             "[BACNET] Interface updated: %s",
             ip_str);


}
    }
}

bool wifi_initialize(void)
{
    if (wifi_initialized)
    {
        ESP_LOGI(TAG,
                 "WiFi already initialized");

        return true;
    }

    wifi_event_group = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg =
        WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(
        esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &event_handler,
            NULL));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &event_handler,
            NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &wifi_config));

    ESP_ERROR_CHECK(
        esp_wifi_start());

    EventBits_t bits =
        xEventGroupWaitBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(15000));

    wifi_initialized = true;

    if (bits & WIFI_CONNECTED_BIT)
    {
        g_connected = true;

        ESP_LOGI(TAG,
                 "Connected to %s",
                 CONFIG_WIFI_SSID);

        return true;
    }

    ESP_LOGW(TAG,
             "Initial connection timeout");

    return false;
}

bool wifi_is_connected(void)
{
    return g_connected;
}

void wifi_force_reconnect(void)
{
    ESP_LOGW(TAG,"Trying to reconnect");

    esp_wifi_disconnect();

    vTaskDelay(pdMS_TO_TICKS(500));

    if(esp_wifi_connect() == ESP_OK)
    {
        ESP_LOGI(TAG,"Reconnect initiated");
        
    }

}