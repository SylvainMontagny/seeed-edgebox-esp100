#include "modem.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "lwip/inet.h"
#include <string.h>
#include "ping/ping_sock.h"
#include "esp_netif.h"
#include "esp_modem_api.h"
#include "server_task.h"
#include "client.h"
#include "solar.h"
#include "rtc_fram_manager.h"
#include "config.h"
#include "dlenv.h"
#include "datalink.h"
#include "iam.h"
#include "tsm.h"
#include "net.h"
#include "txbuf.h"

extern void ntp_initialize(void);
extern void rfm_time_init(bool);
extern void solar_invalidate_cache(void);

static const char *TAG = "modem";


volatile bool g_bacnet_started = false;
esp_netif_t *g_ppp_netif = NULL;
esp_modem_dce_t *g_ppp_dce = NULL;

volatile int g_ping_fail_count = 0;
volatile bool g_link_lost = false;
esp_ping_handle_t g_ping_handle = NULL;


static void modem_cleanup(void)
{
    gpio_reset_pin(MODEM_TX_PIN);
    gpio_reset_pin(MODEM_RX_PIN);
    if (g_ppp_dce) {
        esp_modem_destroy(g_ppp_dce);
        g_ppp_dce = NULL;
    }

    if (g_ppp_netif) {
        esp_netif_destroy(g_ppp_netif);
        g_ppp_netif = NULL;
    }
}


static void ping_success_cb(esp_ping_handle_t hdl, void *args)
{
    g_ping_fail_count = 0;
    g_link_lost = false;
}

static void ping_timeout_cb(esp_ping_handle_t hdl, void *args)
{
    g_ping_fail_count++;

    if (g_ping_fail_count >= 3) {
        g_link_lost = true;
        ESP_LOGW(TAG, "[4G] link lost (ping fail)");
    }
}

void modem_check_task(void *pvParameters)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));

        if (!g_ppp_dce) {
            ESP_LOGW(TAG, "[4G] modem not ready");
            continue;
        }

        int rssi = 0, ber = 0;
        esp_modem_set_mode(g_ppp_dce, ESP_MODEM_MODE_COMMAND);
        if (esp_modem_get_signal_quality(g_ppp_dce, &rssi, &ber) == ESP_OK) {
            ESP_LOGI(TAG, "[4G] RSSI=%d BER=%d", rssi, ber);
        } else {
            ESP_LOGW(TAG, "[4G] signal query failed");
        }
        esp_modem_set_mode(g_ppp_dce, ESP_MODEM_MODE_DATA);
    }
}



static void ping_task(void *pvParameters)
{
    ip_addr_t target_addr;
    inet_pton(AF_INET, "8.8.8.8", &target_addr.u_addr.ip4);
    target_addr.type = IPADDR_TYPE_V4;

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.target_addr = target_addr;
    config.count = ESP_PING_COUNT_INFINITE;
    config.interval_ms = 1800000;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = ping_success_cb,
        .on_ping_timeout = ping_timeout_cb,
    };

    esp_ping_new_session(&config, &cbs, &g_ping_handle);
    esp_ping_start(g_ping_handle);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(3600000));
    }
}

void modem_start_ping_task(void)
{
    xTaskCreate(ping_task, "ping_task", 4096, NULL, 1, NULL);
}


bool modem_try_once(void)
{
    modem_cleanup();

    ESP_LOGI(TAG, "[4G] power cycle modem...");

    gpio_set_level((gpio_num_t)MODEM_PWR_EN,  0); vTaskDelay(pdMS_TO_TICKS(3000));
    gpio_set_level((gpio_num_t)MODEM_PWR_EN,  1); vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level((gpio_num_t)MODEM_PWR_KEY, 1); vTaskDelay(pdMS_TO_TICKS(1500));
    gpio_set_level((gpio_num_t)MODEM_PWR_KEY, 0);

    ESP_LOGI(TAG, "[4G] Attente boot modem (30s)...");
    vTaskDelay(pdMS_TO_TICKS(30000));

    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_config.uart_config.tx_io_num = MODEM_TX_PIN;
    dte_config.uart_config.rx_io_num = MODEM_RX_PIN;
    dte_config.uart_config.port_num = UART_NUM_2;

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_PPP();
    g_ppp_netif = esp_netif_new(&netif_cfg);

    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(CONFIG_MODEM_APN);

    g_ppp_dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600,
                                   &dte_config,
                                   &dce_config,
                                   g_ppp_netif);

    if (!g_ppp_dce) {
        ESP_LOGE(TAG, "[4G] DCE init failed");
        modem_cleanup();
        return false;
    }


    char imei[32] = {0};
    if (esp_modem_get_imei(g_ppp_dce, imei) != ESP_OK) {
        ESP_LOGW(TAG, "[4G] modem not responding");
        modem_cleanup();
        return false;
    }

    ESP_LOGI(TAG, "[4G] IMEI: %s", imei);


    bool network_ok = false;

    for (int i = 0; i < 40; i++) {
        int rssi = 0, ber = 0;

        if (esp_modem_get_signal_quality(g_ppp_dce, &rssi, &ber) == ESP_OK) {
            if (rssi > -90) {
                network_ok = true;
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (!network_ok) {
        ESP_LOGW(TAG, "[4G] no network available");
        modem_cleanup();
        return false;
    }


    if (esp_modem_set_mode(g_ppp_dce, ESP_MODEM_MODE_DATA) != ESP_OK) {
        ESP_LOGE(TAG, "[4G] DATA mode failed");
        modem_cleanup();
        return false;
    }


    esp_netif_ip_info_t ip;
    for (int i = 0; i < 30; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (esp_netif_get_ip_info(g_ppp_netif, &ip) == ESP_OK &&
            ip.ip.addr != 0) {

            ESP_LOGI(TAG, "[4G] IP acquired");
            return true;
        }
    }

    modem_cleanup();
    return false;
}

bool modem_initialize(void)
{
    ESP_LOGI(TAG, "[4G] init modem");

    gpio_set_direction(MODEM_PWR_EN, GPIO_MODE_OUTPUT);
    gpio_set_direction(MODEM_PWR_KEY, GPIO_MODE_OUTPUT);

    for (int i = 0; i < 2; i++) {
        if (modem_try_once()) {
            g_connected = true;
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    ESP_LOGW(TAG, "[4G] offline mode");
    return false;
}

void modem_reconnect_task(void *pvParameters)
{
    if (!g_connected) {

        while (!g_connected) {

            vTaskDelay(pdMS_TO_TICKS(120000));

            ESP_LOGW(TAG, "[RECONNECT] retry modem...");

            if (modem_try_once()) {
                g_connected = true;
                g_link_lost = false;
                g_ping_fail_count = 0;

                ntp_initialize();
                rfm_time_init(true);

                Send_I_Am(&Handler_Transmit_Buffer[0]);

                solar_invalidate_cache();

                if (!g_bacnet_started) {
                    dlenv_init();
                    atexit(datalink_cleanup);
                    xTaskCreate(server_task, "bacnet", 8000, NULL, 1, NULL);
                    g_bacnet_started = true;
                }

                break;
            }
        }
    }

    while (1) {

        vTaskDelay(pdMS_TO_TICKS(1800000));

        if (!g_link_lost)
            continue;

        vTaskDelay(pdMS_TO_TICKS(120000));

        ESP_LOGW(TAG, "[RECONNECT] link lost retry");

        if (modem_try_once()) {

            g_link_lost = false;
            g_ping_fail_count = 0;

            ntp_initialize();
            rfm_time_init(true);

            if (g_bacnet_started) {
                Send_I_Am(&Handler_Transmit_Buffer[0]);
            }

            solar_invalidate_cache();

            if (g_ping_handle) {
                esp_ping_start(g_ping_handle);
            }
        }
    }
}