#include "modem.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "ping/ping_sock.h"
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
#include "sdkconfig.h"

extern void ntp_initialize(void);
extern void rfm_time_init(bool);
extern void solar_invalidate_cache(void);

static const char *TAG = "modem";

volatile bool g_bacnet_started = false;
esp_netif_t *g_ppp_netif = NULL;
esp_modem_dce_t *g_ppp_dce = NULL;

volatile int  g_ping_fail_count = 0;
volatile bool g_link_lost = false;
esp_ping_handle_t g_ping_handle = NULL;

static void ping_success_cb(esp_ping_handle_t hdl, void *args)
{
    uint32_t elapsed_time; uint8_t ttl; uint16_t seq;
    uint32_t recv_len; ip_addr_t target_addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_time, sizeof(elapsed_time));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL,     &ttl,          sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO,   &seq,          sizeof(seq));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE,    &recv_len,     sizeof(recv_len));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR,  &target_addr,  sizeof(target_addr));
    ESP_LOGI(TAG, "[PING] %lu bytes from %s : seq=%d ttl=%d time=%lu ms",
             recv_len, inet_ntoa(target_addr.u_addr.ip4), seq, ttl, elapsed_time);
    g_ping_fail_count = 0;
    g_link_lost = false;
}

static void ping_timeout_cb(esp_ping_handle_t hdl, void *args)
{
    uint16_t seq; ip_addr_t target_addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO,  &seq,         sizeof(seq));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    ESP_LOGW(TAG, "[PING] Timeout - seq=%d to %s", seq, inet_ntoa(target_addr.u_addr.ip4));
    g_ping_fail_count++;
    if (g_ping_fail_count >= 3 && !g_link_lost) {
        g_link_lost = true;
        g_connected = false;
        ESP_LOGE(TAG, "[4G] Connection lost (3 ping failures)");
        rfm_log_event(EVENT_4G_LOST, 0.0f, 0);
    }
}


void modem_check_task(void *pvParameters)
{
 
    for (;;) {
        int rssi = -1;
        int ber = 0;
        vTaskDelay(pdMS_TO_TICKS(10000));  /* check every 10 seconds */
       if(esp_modem_get_signal_quality(g_ppp_dce, &rssi, &ber) != ESP_OK) {
            ESP_LOGW(TAG, "[4G] Failed to get signal quality");
        } else {
            ESP_LOGI(TAG, "[4G] Signal quality - RSSI: %d dBm, BER: %d", rssi, ber);
        }
    }
}

static void ping_task(void *pvParameters)
{
    ip_addr_t target_addr;
    inet_pton(AF_INET, "8.8.8.8", &target_addr.u_addr.ip4);
    target_addr.type = IPADDR_TYPE_V4;
    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.target_addr     = target_addr;
    config.count           = ESP_PING_COUNT_INFINITE;
    config.interval_ms     = 1800000;   /* 30 minutes */
    config.timeout_ms      = 5000;
    config.task_stack_size = 4096;
    config.task_prio       = 1;
    esp_ping_callbacks_t cbs = {
        .on_ping_success = ping_success_cb,
        .on_ping_timeout = ping_timeout_cb,
        .on_ping_end     = NULL,
        .cb_args         = NULL
    };
    esp_err_t err = esp_ping_new_session(&config, &cbs, &g_ping_handle);
    if (err != ESP_OK) { vTaskDelete(NULL); return; }
    esp_ping_start(g_ping_handle);
    ESP_LOGI(TAG, "[PING] Started - 8.8.8.8 every 30 minutes");
    for (;;) { vTaskDelay(pdMS_TO_TICKS(3600000)); }
}

void modem_start_ping_task(void)
{
    xTaskCreate(ping_task, "ping_task", 4096, NULL, 1, NULL);
}


/* Try a single power-cycle + PPP connection. Returns true if an IP is obtained. */
bool modem_try_once(void)
{
    gpio_reset_pin(MODEM_TX_PIN);
    gpio_reset_pin(MODEM_RX_PIN);
    ESP_LOGI(TAG, "[4G] Power cycling modem...");
    gpio_set_level((gpio_num_t)MODEM_PWR_EN,  0); 
    vTaskDelay(pdMS_TO_TICKS(3000));
    gpio_set_level((gpio_num_t)MODEM_PWR_EN,  1); 
    vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level((gpio_num_t)MODEM_PWR_KEY, 1); 
    vTaskDelay(pdMS_TO_TICKS(1500));
    gpio_set_level((gpio_num_t)MODEM_PWR_KEY, 0);

    ESP_LOGI(TAG, "[4G] Waiting for modem boot (15s)...");
    vTaskDelay(pdMS_TO_TICKS(15000));

    if (g_ppp_dce) {
        esp_modem_destroy(g_ppp_dce);
        g_ppp_dce = NULL;
    }
    if (g_ppp_netif) {
        esp_netif_destroy(g_ppp_netif);
        g_ppp_netif = NULL;
    }

    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_config.uart_config.tx_io_num = MODEM_TX_PIN;
    dte_config.uart_config.rx_io_num = MODEM_RX_PIN;
    dte_config.uart_config.port_num  = UART_NUM_2;

    esp_netif_config_t ppp_cfg = ESP_NETIF_DEFAULT_PPP();
    g_ppp_netif = esp_netif_new(&ppp_cfg);

    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(CONFIG_MODEM_APN);
    g_ppp_dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte_config, &dce_config, g_ppp_netif);

    if (!g_ppp_dce) {
        ESP_LOGE(TAG, "[4G] DCE initialization failed");
        if (g_ppp_netif) { esp_netif_destroy(g_ppp_netif); g_ppp_netif = NULL; }
        return false;
    }
    esp_err_t err = esp_modem_set_mode(g_ppp_dce, ESP_MODEM_MODE_DATA);
    if (err != ESP_OK) {
        switch(err) {
            case ESP_ERR_NOT_SUPPORTED: ESP_LOGE(TAG, "[4G] Modem does not support PPP mode"); break;
            case ESP_FAIL: ESP_LOGE(TAG, "[4G] Modem switch to DATA mode fail"); break;    
            default: ESP_LOGE(TAG, "[4G] Failed to set modem mode: %d", err); break;
        }
        esp_modem_destroy(g_ppp_dce);   g_ppp_dce = NULL;
        esp_netif_destroy(g_ppp_netif); g_ppp_netif = NULL;
        return false;
        
    }

    ESP_LOGI(TAG, "[4G] Waiting for IP address (30s)...");
    esp_netif_ip_info_t ip_info;
    for (int i = 0; i < 30; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (esp_netif_get_ip_info(g_ppp_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            char ip_str[32];
            esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
            ESP_LOGI(TAG, "[4G] CONNECTED - IP: %s", ip_str);
            return true;
        }
    }

    esp_modem_destroy(g_ppp_dce);  g_ppp_dce = NULL;
    esp_netif_destroy(g_ppp_netif); g_ppp_netif = NULL;
    return false;
}

bool modem_initialize(void)
{
    ESP_LOGI(TAG, "[4G] Initializing SIM7600 modem...");
    gpio_set_direction((gpio_num_t)MODEM_PWR_EN,  GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)MODEM_PWR_KEY, GPIO_MODE_OUTPUT);

    for (int attempt = 1; attempt <= 2; attempt++) {
        ESP_LOGI(TAG, "[4G] Attempt %d/2", attempt);
        if (modem_try_once()) {
            g_connected = true;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    ESP_LOGW(TAG, "[4G] Failed - offline mode");
    return false;
}

/* ================================================================
 * Automatic reconnect task for 4G
 *
 * - Every 1800s: check status (ping handled by ping_task)
 * - If g_link_lost: wait 2 minutes then call modem_try_once()
 * - On success: NTP + BACnet I_Am -> resume normal operation
 * ================================================================ */
void modem_reconnect_task(void *pvParameters)
{
    if (!g_connected) {

        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(10000));  /* 10 seconds */

            if (!g_connected)
            {
            /* Offline at boot */
            ESP_LOGW(TAG, "[RECONNECT] Starting offline - attempts every 10 seconds");
            bool ok = modem_try_once();
            if (ok) {
                g_connected       = true;
                g_link_lost       = false;
                g_ping_fail_count = 0;

                ntp_initialize();
                rfm_time_init(true);
                Send_I_Am(&Handler_Transmit_Buffer[0]);
                solar_invalidate_cache();
                rfm_log_event(EVENT_NTP_SYNC, 0.0f, 0);
                ESP_LOGI(TAG, "[RECONNECT] Connection established from offline state");
                break;  /* exit offline loop and continue monitoring */
            } else {
                ESP_LOGW(TAG, "[RECONNECT] Failed - next attempt in 2 minutes");
            }
        }
        }
    }

    /* Monitoring loop */
    ESP_LOGI(TAG, "[RECONNECT] Monitoring active connection");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1800000));  /* check every 30 min */

        if (!g_link_lost) continue;

        /* Lost connection - wait 2 minutes before retry */
        ESP_LOGW(TAG, "[RECONNECT] Connection lost - waiting 2 minutes before retry...");
        vTaskDelay(pdMS_TO_TICKS(1800000));  /* 30 minutes */  

        if (!g_link_lost) continue;

        ESP_LOGI(TAG, "[RECONNECT] Attempting reconnection...");

        /* Stop ping during reconnection */
        if (g_ping_handle) {
            esp_ping_stop(g_ping_handle);
        }

        bool ok = modem_try_once();

        if (ok) {
            ESP_LOGI(TAG, "[RECONNECT] Reconnection successful");
            g_connected     = true;
            g_link_lost     = false;
            g_ping_fail_count = 0;

            /* Resync NTP */
            ntp_initialize();
            rfm_time_init(true);

            /* Re-announce BACnet device on network */
            if (g_bacnet_started) {
                Send_I_Am(&Handler_Transmit_Buffer[0]);
                ESP_LOGI(TAG, "[RECONNECT] BACnet I_Am sent");
            } else {
                /* Configure BACnet for PPP interface */
                esp_netif_ip_info_t ip_info;
                if (g_ppp_netif && esp_netif_get_ip_info(g_ppp_netif, &ip_info) == ESP_OK 
                    && ip_info.ip.addr != 0) {
                    char ip_str[32];
                    esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
                    setenv("BACNET_IFACE", ip_str, 1);
                    ESP_LOGI(TAG, "[BACNET] Interface set: %s", ip_str);
                }

                dlenv_init();
                atexit(datalink_cleanup);
                Send_I_Am(&Handler_Transmit_Buffer[0]);
                xTaskCreate(server_task, "bacnet_server", 8000, NULL, 1, NULL);
                g_bacnet_started = true;
                ESP_LOGI(TAG, "[RECONNECT] BACnet started (first connection)");
            }

            /* Restart ping */
            if (g_ping_handle) 
            {
                esp_ping_start(g_ping_handle);
            }

            /* Recalculate solar times after NTP update */
            solar_invalidate_cache();
            rfm_log_event(EVENT_NTP_SYNC, 0.0f, 0);

        } else {
            ESP_LOGW(TAG, "[RECONNECT] Failed - will retry in 30 minutes");
            /* g_link_lost remains true -> loop will retry in 30 min */
        }
    }
}

