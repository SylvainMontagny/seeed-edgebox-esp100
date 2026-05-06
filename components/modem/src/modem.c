#include "modem.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "ping/ping_sock.h"
#include "server_task.h"
#include "client_task.h"
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

// External functions
extern void ntp_initialize(void);
extern void rfm_time_init(bool);
extern void solar_invalidate_cache(void);
extern void schedule_update_solar_times(void);

// Fallback stub when the solar scheduling implementation is missing.
static const char *TAG = "modem";
void schedule_update_solar_times(void) __attribute__((weak));
void schedule_update_solar_times(void)
{
    ESP_LOGW(TAG, "[SOLAR] schedule_update_solar_times() unavailable - fallback no-op");
}

// Local TAG for logging


// Global variables
volatile bool g_connected = false;
volatile bool g_bacnet_started = false;
esp_netif_t *g_ppp_netif = NULL;
esp_modem_dce_t *g_ppp_dce = NULL;

// Ping-related globals
volatile int  g_ping_fail_count  = 0;
volatile bool g_link_lost        = false;
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
    /* Ping OK → réinitialiser le compteur d'échecs */
    g_ping_fail_count = 0;
    g_link_lost = false;
}

static void ping_timeout_cb(esp_ping_handle_t hdl, void *args)
{
    uint16_t seq; ip_addr_t target_addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO,  &seq,         sizeof(seq));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    ESP_LOGW(TAG, "[PING] Timeout — seq=%d vers %s", seq, inet_ntoa(target_addr.u_addr.ip4));
    g_ping_fail_count++;
    if (g_ping_fail_count >= 3 && !g_link_lost) {
        g_link_lost = true;
        g_connected = false;
        ESP_LOGE(TAG, "[4G] CONNEXION PERDUE (3 pings KO) — mode NO_DATA");
        rfm_log_event(EVENT_4G_LOST, 0.0f, 0);
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
    config.interval_ms     = 30000;   /* toutes les 30s */
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
    ESP_LOGI(TAG, "[PING] Demarre → 8.8.8.8 toutes les 30s");
    for (;;) { vTaskDelay(pdMS_TO_TICKS(60000)); }
}

void modem_start_ping_task(void)
{
    xTaskCreate(ping_task, "ping_task", 4096, NULL, 1, NULL);
}

/* Tente un seul power-cycle + connexion PPP.
 * Retourne true si une IP est obtenue. */
bool modem_try_once(void)
{
    gpio_reset_pin(MODEM_TX_PIN);
    gpio_reset_pin(MODEM_RX_PIN);
    ESP_LOGI(TAG, "[4G] Power cycle...");
    gpio_set_level((gpio_num_t)MODEM_PWR_EN,  0); vTaskDelay(pdMS_TO_TICKS(3000));
    gpio_set_level((gpio_num_t)MODEM_PWR_EN,  1); vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level((gpio_num_t)MODEM_PWR_KEY, 1); vTaskDelay(pdMS_TO_TICKS(1500));
    gpio_set_level((gpio_num_t)MODEM_PWR_KEY, 0);

    ESP_LOGI(TAG, "[4G] Attente boot modem (45s)...");
    vTaskDelay(pdMS_TO_TICKS(45000));

    /* Nettoyer l'ancien DCE/netif si existant */
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

    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(MODEM_APN);
    g_ppp_dce = esp_modem_new_dev(
        ESP_MODEM_DCE_SIM7600, &dte_config, &dce_config, g_ppp_netif);

    if (!g_ppp_dce) {
        ESP_LOGE(TAG, "[4G] Echec DCE");
        if (g_ppp_netif) { esp_netif_destroy(g_ppp_netif); g_ppp_netif = NULL; }
        return false;
    }

    if (esp_modem_set_mode(g_ppp_dce, ESP_MODEM_MODE_DATA) != ESP_OK) {
        ESP_LOGE(TAG, "[4G] Echec DATA");
        esp_modem_destroy(g_ppp_dce);  g_ppp_dce = NULL;
        esp_netif_destroy(g_ppp_netif); g_ppp_netif = NULL;
        return false;
    }

    esp_netif_ip_info_t ip_info;
    for (int i = 0; i < 30; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (esp_netif_get_ip_info(g_ppp_netif, &ip_info) == ESP_OK
            && ip_info.ip.addr != 0) {
            char ip_str[32];
            esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
            ESP_LOGI(TAG, "[4G] CONNECTE ! IP: %s", ip_str);
            return true;
        }
        ESP_LOGI(TAG, "[4G] Attente IP... (%d/30)", i+1);
    }

    esp_modem_destroy(g_ppp_dce);  g_ppp_dce = NULL;
    esp_netif_destroy(g_ppp_netif); g_ppp_netif = NULL;
    return false;
}

bool modem_initialize(void)
{
    ESP_LOGI(TAG, "[4G] Initialisation modem SIM7600...");
    gpio_set_direction((gpio_num_t)MODEM_PWR_EN,  GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)MODEM_PWR_KEY, GPIO_MODE_OUTPUT);

    for (int attempt = 1; attempt <= 2; attempt++) {
        ESP_LOGI(TAG, "[4G] Tentative %d/2", attempt);
        if (modem_try_once()) {
            g_connected = true;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    ESP_LOGW(TAG, "[4G] Echec — mode hors-ligne");
    return false;
}

/* ================================================================
 * Tâche de reconnexion automatique pour 4G
 *
 * - Toutes les 30s : vérifie l'état (ping déjà géré par ping_task)
 * - Si g_link_lost : attend 2 minutes puis tente modem_try_once()
 * - Si succès : NTP + I_Am BACnet → reprend normalement
 * ================================================================ */
void modem_reconnect_task(void *pvParameters)
{
    if (!g_connected) {
        /* ── Cas 2 : hors-ligne dès le boot ── */
        ESP_LOGW(TAG, "[RECONNECT] Demarrage hors-ligne — tentatives toutes les 2 min");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(120000));  /* 2 minutes */

            if (g_connected) break;             /* connexion établie entre-temps */

            ESP_LOGI(TAG, "[RECONNECT] Tentative de connexion (hors-ligne)...");

            bool ok = modem_try_once();
            if (ok) {
                g_connected       = true;
                g_link_lost       = false;
                g_ping_fail_count = 0;

                ntp_initialize();
                rfm_time_init(true);

                if (!g_bacnet_started) {
                    /*get PPP IP address and set it as BACNET interface */
                    esp_netif_ip_info_t ip_info;
                    if (g_ppp_netif && esp_netif_get_ip_info(g_ppp_netif, &ip_info) == ESP_OK 
                        && ip_info.ip.addr != 0) {
                        char ip_str[32];
                        esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
                        setenv("BACNET_IFACE", ip_str, 1);
                        ESP_LOGI(TAG, "[BACNET] Interface configurée: %s", ip_str);
                    }
                    
                    dlenv_init();
                    atexit(datalink_cleanup);
                    Send_I_Am(&Handler_Transmit_Buffer[0]);
                    xTaskCreate(server_task, "bacnet_server", 8000, NULL, 1, NULL);
                    xTaskCreate(ping_task,   "ping_task",     4096, NULL, 1, NULL);
                    g_bacnet_started = true;
                    ESP_LOGI(TAG, "[RECONNECT] BACnet + Ping lances (premiere connexion)");
                } else {
                    Send_I_Am(&Handler_Transmit_Buffer[0]);
                }

                solar_invalidate_cache();
                schedule_update_solar_times();
                rfm_log_event(EVENT_NTP_SYNC, 0.0f, 0);
                ESP_LOGI(TAG, "[RECONNECT] ✅ Connexion etablie depuis hors-ligne !");
                break;  /* sortir de la boucle hors-ligne → tomber dans la boucle surveillance */
            } else {
                ESP_LOGW(TAG, "[RECONNECT] Echec — prochaine tentative dans 2 min");
            }
        }
    }

    /* ── Cas 1 (ou suite du Cas 2 après reconnexion) : surveillance ── */
    ESP_LOGI(TAG, "[RECONNECT] Surveillance connexion active");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));   /* check toutes les 30s */

        if (!g_link_lost) continue;         /* tout va bien */

        /* Connexion perdue — attendre 2 minutes avant de retenter */
        ESP_LOGW(TAG, "[RECONNECT] Connexion perdue — attente 2 min avant retentative...");
        vTaskDelay(pdMS_TO_TICKS(120000));  /* 2 minutes */

        if (!g_link_lost) continue;         /* reconnecté entre-temps ? */

        ESP_LOGI(TAG, "[RECONNECT] Tentative de reconnexion...");

        /* Stopper le ping pendant la reconnexion */
        if (g_ping_handle) {
            esp_ping_stop(g_ping_handle);
        }

        bool ok = modem_try_once();

        if (ok) {
            ESP_LOGI(TAG, "[RECONNECT] ✅ Reconnexion réussie !");
            g_connected     = true;
            g_link_lost     = false;
            g_ping_fail_count = 0;

            /* Re-synchroniser NTP */
            ntp_initialize();
            rfm_time_init(true);

            /* Re-annoncer le device BACnet sur le réseau */
            if (g_bacnet_started) {
                Send_I_Am(&Handler_Transmit_Buffer[0]);
                ESP_LOGI(TAG, "[RECONNECT] BACnet I_Am envoyé");
            } else {
                /* === Configure BACnet for PPP interface === */
                esp_netif_ip_info_t ip_info;
                if (g_ppp_netif && esp_netif_get_ip_info(g_ppp_netif, &ip_info) == ESP_OK 
                    && ip_info.ip.addr != 0) {
                    char ip_str[32];
                    esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
                    setenv("BACNET_IFACE", ip_str, 1);
                    ESP_LOGI(TAG, "[BACNET] Interface configurée: %s", ip_str);
                }
                
                /* BACnet n'avait jamais démarré (hors-ligne au boot) → le lancer */
                dlenv_init();
                atexit(datalink_cleanup);
                Send_I_Am(&Handler_Transmit_Buffer[0]);
                xTaskCreate(server_task, "bacnet_server", 8000, NULL, 1, NULL);
                g_bacnet_started = true;
                ESP_LOGI(TAG, "[RECONNECT] BACnet démarré (première connexion)");
            }

            /* Relancer le ping */
            if (g_ping_handle) {
                esp_ping_start(g_ping_handle);
            }

            /* Recalculer les heures solaires avec la nouvelle heure NTP */
            solar_invalidate_cache();
            schedule_update_solar_times();

            rfm_log_event(EVENT_NTP_SYNC, 0.0f, 0);

        } else {
            ESP_LOGW(TAG, "[RECONNECT] ❌ Echec — nouvelle tentative dans 2 min");
            /* g_link_lost reste true → la boucle retentera dans 2 min */
        }
    }
}

