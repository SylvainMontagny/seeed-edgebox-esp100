#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_netif.h"
#include "ping/ping_sock.h"
#include "lwip/inet.h"
#include "server_task.h"
#include "led.h"
#include "device.h"
#include "trendlog.h"
#include "calendar.h"
#include "config.h"
#include "address.h"
#include "bacdef.h"
#include "handlers.h"
#include "client.h"
#include "dlenv.h"
#include "bacdcode.h"
#include "npdu.h"
#include "apdu.h"
#include "iam.h"
#include "tsm.h"
#include "datalink.h"
#include "dcc.h"
#include "getevent.h"
#include "net.h"
#include "txbuf.h"
#include "version.h"
#include "av.h"
#include "bv.h"
#include "schedule.h"
#include "sdkconfig.h"
#include "driver/ledc.h"
#include <time.h>
#include "rtc_fram_manager.h"
#include "schedule_persist.h"
#include "solar.h"
#include "Http_server.h"
#include "wifi.h"
#include "modem.h"
#include "ntp.h"
#include "gpiooutputs.h"


#define SERVER_DEVICE_ID 1234
static const char *TAG = "main";


/* ================================================================
 * État connexion — partagé entre modem et reconnect_task
 * ================================================================ */

/* Netif PPP — conservé pour pouvoir surveiller l'IP */

extern esp_err_t http_server_start(void);


static void reconnect_task(void *pvParameters)
{
    ESP_LOGI(TAG, "[RECONNECT] Mode WiFi — reconnexion automatique via event handler");

    if (!g_connected) {
        /* Attendre la connexion WiFi initiale */
        ESP_LOGW(TAG, "[RECONNECT] En attente de connexion WiFi...");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            if (g_connected) break;
        }

        ntp_initialize();
        rfm_time_init(true);

        /* === Configure BACnet for WiFi interface === */
        /* Get WiFi IP and set as BACNET interface for proper discovery */
        esp_netif_ip_info_t ip_info;
        esp_netif_t *wifi_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (wifi_netif && esp_netif_get_ip_info(wifi_netif, &ip_info) == ESP_OK
            && ip_info.ip.addr != 0) {
            char ip_str[32];
            esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
            setenv("BACNET_IFACE", ip_str, 1);
            ESP_LOGI(TAG, "[BACNET] Interface WiFi configurée: %s", ip_str);
        }

        dlenv_init();
        atexit(datalink_cleanup);
        Send_I_Am(&Handler_Transmit_Buffer[0]);
        xTaskCreate(server_task, "bacnet_server", 8000, NULL, 1, NULL);
        g_bacnet_started = true;
        ESP_LOGI(TAG, "[RECONNECT] BACnet lances");
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

static void update_solar_offsets_from_av(void)
{
    int16_t offset_before = (int16_t)Analog_Value_Present_Value(2);
    int16_t offset_after  = (int16_t)Analog_Value_Present_Value(3);
    
    solar_set_offsets(offset_before, offset_after);
}

static volatile float g_prev_pv0    = -1.0f;
static volatile float g_prev_pv1    = -1.0f;
static volatile bool  g_force_pv0   = false;
static volatile bool  g_force_pv1   = false;
static volatile float g_forced_val0 = 0.0f;
static volatile float g_forced_val1 = 0.0f;

static void schedule_task(void *pvParameters)
{
    BACNET_TIME btime;
    SCHEDULE_DESCR *desc0 = NULL;
    SCHEDULE_DESCR *desc1 = NULL;
    struct tm *t;
    time_t now;

    rfm_load_av_state((float*)&g_prev_pv0, (float*)&g_prev_pv1);

    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("PPP_DEF");
    vTaskDelay(pdMS_TO_TICKS(2000));

    char ip_str[32] = "0.0.0.0";
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
    }

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "Schedule Task demarre | IP 4G: %s", ip_str);
    ESP_LOGI(TAG, "SCH-0 → AV0/GPIO42 | SCH-1 → AV1/GPIO41");
    ESP_LOGI(TAG, "================================================");

    solar_invalidate_cache();
    solar_times_t st_init = solar_get_today();
    if (st_init.valid) {
        ESP_LOGI(TAG, "[SOLAR] Lever %02d:%02d / Coucher %02d:%02d — is_night=%d",
                 st_init.sunrise_h, st_init.sunrise_m,
                 st_init.sunset_h,  st_init.sunset_m,
                 (int)solar_is_night_now());
    } else {
        ESP_LOGW(TAG, "[SOLAR] Calcul invalide — configurer lat/lon sur la page web");
    }

    /* Jour précédent pour détecter le changement de jour → recalcul solaire */
    int s_prev_mday = -1;

    for (;;) {
        now = time(NULL);
        t   = localtime(&now);

        btime.hour       = (uint8_t)t->tm_hour;
        btime.min        = (uint8_t)t->tm_min;
        btime.sec        = (uint8_t)t->tm_sec;
        btime.hundredths = 0;
        uint8_t bacnet_wday = (t->tm_wday == 0) ? 7 : (uint8_t)t->tm_wday;

        desc0 = Schedule_Object(0);
        desc1 = Schedule_Object(1);

        if (desc0) {
            Schedule_Recalculate_PV(desc0, (BACNET_WEEKDAY)bacnet_wday, &btime);
        }
        if (desc1) {
            Schedule_Recalculate_PV(desc1, (BACNET_WEEKDAY)bacnet_wday, &btime);
        }

        update_solar_offsets_from_av();
        const solar_config_t *scfg = solar_get_config();

        if (scfg->enabled) {
                bool  is_night   = solar_is_night_now();

                if (btime.hour == 0 && btime.min == 0 && btime.sec == 0) {
                    solar_invalidate_cache();
                }

                static bool s_prev_is_night = false;
                static bool s_first_cycle   = true;
                bool solar_transition = (!s_first_cycle && is_night != s_prev_is_night);
                s_prev_is_night = is_night;
                s_first_cycle   = false;
                bool has_se0 = false;
                if (is_night && desc0 && desc0->Exception_Count > 0) {
                    float se_pv0  = desc0->Present_Value.type.Real;
                    float se_def0 = desc0->Schedule_Default.type.Real;
                    if (se_pv0 != se_def0) {
                        has_se0 = true;
                    }
                }
                bool has_se1 = false;
                if (is_night && desc1 && desc1->Exception_Count > 0) {
                    float se_pv1  = desc1->Present_Value.type.Real;
                    float se_def1 = desc1->Schedule_Default.type.Real;
                    if (se_pv1 != se_def1) {
                        has_se1 = true;
                    }
                }
                /* ── Zone A (AV0 / SCH0) ── */
                {
                    float target0;
                    if (g_force_pv0) {
                        if (solar_transition || has_se0) {
                            g_force_pv0 = false;
                            ESP_LOGI(TAG, "[FORCE] Zone A : force annule");
                        } else {
                            target0 = g_forced_val0;
                            goto apply_zone_a;
                        }
                    }
                    if (!is_night)       { target0 = 0.0f; }
                    else if (has_se0)    
                    { 
                        target0 = desc0->Present_Value.type.Real; }
                    else                 
                    { 
                        target0 = 100.0f; 
                    }
                    apply_zone_a:;
                    if (target0 != g_prev_pv0) {
                        Analog_Value_Present_Value_Set(0, target0, 16);
                        av_pwm_apply(0, target0);
                    
                        if (!is_night && !g_force_pv0) {
                            ESP_LOGI(TAG, "[SOLAR] Zone A → 0%% (JOUR)");
                            rfm_log_event(EVENT_SOLAR_OFF, 0.0f, 0);
                        } else if (has_se0 && !g_force_pv0) {
                            ESP_LOGI(TAG, "[SE] Zone A → %.1f%% (NUIT)", target0);
                            rfm_log_event(EVENT_AV0_CHANGE, target0, 0);
                        } else if (!g_force_pv0) {
                            ESP_LOGI(TAG, "[SOLAR] Zone A → 100%% (NUIT)");
                            rfm_log_event(EVENT_SOLAR_ON, 100.0f, 0);
                        }
                        rfm_save_av_state(target0, Analog_Value_Present_Value(1));
                        g_prev_pv0 = target0;
                    }
                }

                /* ── Zone B (AV1 / SCH1) ── */
                {
                    float target1;
                    if (g_force_pv1) {
                        if (solar_transition || has_se1) {
                            g_force_pv1 = false;
                            ESP_LOGI(TAG, "[FORCE] Zone B : force annule");
                        } else {
                            target1 = g_forced_val1;
                            goto apply_zone_b;
                        }
                    }
                    if (!is_night)       
                    { 
                        target1 = 0.0f; 
                    }
                    else if (has_se1)    
                    { 
                        target1 = desc1->Present_Value.type.Real; 
                    }
                    else                
                    { 
                        target1 = 100.0f; 
                    }
                    apply_zone_b:;
                    if (target1 != g_prev_pv1) {
                        Analog_Value_Present_Value_Set(1, target1, 16);
                        av_pwm_apply(1, target1);
                        rfm_save_av_state(Analog_Value_Present_Value(0), target1);
                        g_prev_pv1 = target1;
                    }
                }
            }
        

        trend_log_timer(1);
        Calendar_Update_Present_Value(0);

        if (btime.min == 0 && btime.sec < 2) {
            http_trendlog_record(
                Analog_Value_Present_Value(0),
                Analog_Value_Present_Value(1));
        }

    vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#ifdef CONFIG_NETWORK_CONNECTION_4G

#endif /* CONFIG_NETWORK_CONNECTION_4G */

/**
 * Initialize network connection based on menuconfig selection.
 * Returns true if connected, false otherwise.
 */
static bool network_initialize(void)
{
#ifdef CONFIG_NETWORK_CONNECTION_WIFI
    ESP_LOGI(TAG, "[NETWORK] Initialisation WiFi...");
    return wifi_initialize();
#else
    ESP_LOGI(TAG, "[NETWORK] Initialisation 4G/LTE...");
    return modem_initialize();
#endif
}

void app_main(void)
{

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) 
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    led_initialize();
    ledc_initialize();
    Device_Set_Object_Instance_Number(SERVER_DEVICE_ID);
    ESP_LOGI(TAG, "BACnet Stack %s | Device ID: %lu",
             BACnet_Version, (unsigned long)Device_Object_Instance_Number());
    address_init();
    Init_Service_Handlers();

    if (rfm_init() == ESP_OK) {
        float saved_av0 = 0.0f, saved_av1 = 0.0f;
        if (rfm_load_av_state(&saved_av0, &saved_av1) == ESP_OK) {
            Analog_Value_Present_Value_Set(0, saved_av0, 16);
            Analog_Value_Present_Value_Set(1, saved_av1, 16);
            av_pwm_apply(0, saved_av0);
            av_pwm_apply(1, saved_av1);
        }
        ESP_LOGI(TAG, "[BOOT] AV0=%.1f AV1=%.1f", saved_av0, saved_av1);
        
        /*initialise   les offsets solaires dans AV:2 et AV:3 */
        sched_persist_restore_all();
        solar_init();
        const solar_config_t *scfg = solar_get_config();
        Analog_Value_Present_Value_Set(2, (float)scfg->offset_before_sunset, 16);
        Analog_Value_Present_Value_Set(3, (float)scfg->offset_after_sunrise, 16);
        ESP_LOGI(TAG, "[BOOT] Solar offsets: AV2(before_sunset)=%d, AV3(after_sunrise)=%d",
                 scfg->offset_before_sunset, scfg->offset_after_sunrise);
        
        setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
        tzset();
        rfm_time_init(false);
        rfm_log_event(EVENT_BOOT, 0.0f, 0);
        rfm_dump();
    } else {
        ESP_LOGE(TAG, "[BOOT] FRAM/RTC indisponible");
    }

    bool connected = network_initialize();

    /* Serveur HTTP toujours actif */
    http_server_start();

    if (connected) {
        ntp_initialize();
        rfm_time_init(true);
        
        /* === Configure BACnet with current network interface === */
#ifdef CONFIG_NETWORK_CONNECTION_4G
        /* Get PPP IP address */
        esp_netif_ip_info_t ip_info;
        if (g_ppp_netif && esp_netif_get_ip_info(g_ppp_netif, &ip_info) == ESP_OK 
            && ip_info.ip.addr != 0) {
            char ip_str[32];
            esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
            setenv("BACNET_IFACE", ip_str, 1);
            ESP_LOGI(TAG, "[BACNET] Interface PPP configurée: %s", ip_str);
        }
#else
        /* Get WiFi IP address */
        esp_netif_ip_info_t ip_info;
        esp_netif_t *wifi_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (wifi_netif && esp_netif_get_ip_info(wifi_netif, &ip_info) == ESP_OK 
            && ip_info.ip.addr != 0) {
            char ip_str[32];
            esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
            setenv("BACNET_IFACE", ip_str, 1);
            ESP_LOGI(TAG, "[BACNET] Interface WiFi configurée: %s", ip_str);
        }
#endif
        
        dlenv_init();
        atexit(datalink_cleanup);
        Send_I_Am(&Handler_Transmit_Buffer[0]);

        xTaskCreate(server_task, "bacnet_server", 8000, NULL, 1, NULL);
        
#ifdef CONFIG_NETWORK_CONNECTION_4G
        modem_start_ping_task();
#endif

        g_bacnet_started = true;
        ESP_LOGI(TAG, "=== Mode CONNECTE — BACnet + Schedule actifs ===");
    } else {
        ESP_LOGW(TAG, "=== Mode HORS-LIGNE — Schedule actif, BACnet inactif ===");
        ESP_LOGW(TAG, "=== reconnect_task retentera toutes les 2 min ===");
    }

    /* Tâche locale — toujours active */
    xTaskCreate(schedule_task, "schedule_task", 4096, NULL, 1, NULL);

    /* ---- MOD 2 : Tâche de reconnexion automatique — toujours active ---- */
#ifdef CONFIG_NETWORK_CONNECTION_4G
    xTaskCreate(modem_reconnect_task, "reconnect_task", 4096, NULL, 1, NULL);
#else
    xTaskCreate(reconnect_task, "reconnect_task", 4096, NULL, 1, NULL);
#endif

    ESP_LOGI(TAG, "AV0 -> GPIO42 | AV1 -> GPIO41");
    
}