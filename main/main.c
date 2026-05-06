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
#include "client_task.h"
#include "led.h"
#include "device.h"
#include "trendlog.h"
#include "msv.h"
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


#define SERVER_DEVICE_ID 1234
static const char *TAG = "main";
#define GPIO_AV0  GPIO_NUM_42
#define GPIO_AV1  GPIO_NUM_41

/* ================================================================
 * État connexion — partagé entre modem et reconnect_task
 * ================================================================ */

/* Netif PPP — conservé pour pouvoir surveiller l'IP */

extern esp_err_t http_server_start(void);

static object_functions_t Object_Table[] = {
    { OBJECT_DEVICE, NULL, Device_Count, Device_Index_To_Instance,
      Device_Valid_Object_Instance_Number, Device_Object_Name,
      Device_Read_Property_Local, Device_Write_Property_Local,
      Device_Property_Lists, NULL, NULL, NULL, NULL, NULL, NULL },
    { OBJECT_ANALOG_VALUE, Analog_Value_Init, Analog_Value_Count,
      Analog_Value_Index_To_Instance, Analog_Value_Valid_Instance,
      Analog_Value_Object_Name, Analog_Value_Read_Property,
      Analog_Value_Write_Property, Analog_Value_Property_Lists,
      NULL, NULL, NULL, NULL, NULL, NULL },
    { OBJECT_BINARY_VALUE, Binary_Value_Init, Binary_Value_Count,
      Binary_Value_Index_To_Instance, Binary_Value_Valid_Instance,
      Binary_Value_Object_Name, Binary_Value_Read_Property,
      Binary_Value_Write_Property, Binary_Value_Property_Lists,
      NULL, NULL, NULL, NULL, NULL, NULL },
    { OBJECT_SCHEDULE, Schedule_Init, Schedule_Count,
      Schedule_Index_To_Instance, Schedule_Valid_Instance,
      Schedule_Object_Name, Schedule_Read_Property, Schedule_Write_Property,
      (rpm_property_lists_function)Schedule_Property_Lists,
      NULL, NULL, NULL, NULL, NULL, NULL },
    { OBJECT_MULTI_STATE_VALUE, Multistate_Value_Init, Multistate_Value_Count,
      Multistate_Value_Index_To_Instance, Multistate_Value_Valid_Instance,
      Multistate_Value_Object_Name, Multistate_Value_Read_Property,
      Multistate_Value_Write_Property, Multistate_Value_Property_Lists,
      NULL, NULL, NULL, NULL, NULL, NULL },
    { OBJECT_CALENDAR, Calendar_Init, Calendar_Count,
      Calendar_Index_To_Instance, Calendar_Valid_Instance,
      Calendar_Object_Name, Calendar_Read_Property, Calendar_Write_Property,
      (rpm_property_lists_function)Calendar_Property_Lists,
      NULL, NULL, NULL, NULL, NULL, NULL },
    { OBJECT_TRENDLOG, Trend_Log_Init, Trend_Log_Count,
      Trend_Log_Index_To_Instance, Trend_Log_Valid_Instance,
      Trend_Log_Object_Name, Trend_Log_Read_Property, Trend_Log_Write_Property,
      (rpm_property_lists_function)Trend_Log_Property_Lists,
      TrendLogGetRRInfo, NULL, NULL, NULL, NULL, NULL },
    { MAX_BACNET_OBJECT_TYPE, NULL, NULL, NULL, NULL, NULL,
      NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

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

/* ================================================================
 * Modification 1 : Remplir le Weekly Schedule avec les heures solaires
 *
 * Pour chaque jour (lundi=0 … dimanche=6) on programme :
 *   TV[0] = heure coucher soleil → 100.0 % (allumage)
 *   TV[1] = heure lever  soleil → 0.0 %   (extinction)
 *
 * Visible dans YABE → Schedule → Weekly Schedule
 * Mis à jour chaque matin à minuit (invalider cache → recalcul)
 * ================================================================ */
static void schedule_update_solar_times(void)
{
    solar_times_t st = solar_get_today();
    if (!st.valid) {
        ESP_LOGW(TAG, "[SOLAR-SCH] Heures invalides — weekly non mis à jour");
        return;
    }

    ESP_LOGI(TAG, "[SOLAR-SCH] Mise à jour Weekly : coucher %02d:%02d → 100%% | lever %02d:%02d → 0%%",
             st.sunset_h, st.sunset_m, st.sunrise_h, st.sunrise_m);

    for (int sch = 0; sch < 2; sch++) {
        SCHEDULE_DESCR *desc = Schedule_Object((uint32_t)sch);
        if (!desc) continue;

        /* Remplir les 7 jours BACnet (index 0=Lundi … 6=Dimanche)
         * BACNET_WEEKLY_SCHEDULE_SIZE peut être > 7 (défini à 10)
         * On remplit seulement les 7 premiers slots (un par jour)    */
        for (int day = 0; day < 7; day++) {
            BACNET_DAILY_SCHEDULE *ds = &desc->Weekly_Schedule[day];
            ds->TV_Count = 2;

            /* Entrée 0 : coucher du soleil → allumage à 100% */
            ds->Time_Values[0].Time.hour       = st.sunset_h;
            ds->Time_Values[0].Time.min        = st.sunset_m;
            ds->Time_Values[0].Time.sec        = 0;
            ds->Time_Values[0].Time.hundredths = 0;
            ds->Time_Values[0].Value.tag        = BACNET_APPLICATION_TAG_REAL;
            ds->Time_Values[0].Value.type.Real  = 100.0f;

            /* Entrée 1 : lever du soleil → extinction à 0% */
            ds->Time_Values[1].Time.hour       = st.sunrise_h;
            ds->Time_Values[1].Time.min        = st.sunrise_m;
            ds->Time_Values[1].Time.sec        = 0;
            ds->Time_Values[1].Time.hundredths = 0;
            ds->Time_Values[1].Value.tag        = BACNET_APPLICATION_TAG_REAL;
            ds->Time_Values[1].Value.type.Real  = 0.0f;
        }
    }
}

static void Init_Service_Handlers(void)
{
    Device_Init(&Object_Table[0]);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_IS, handler_who_is);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_HAS, handler_who_has);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_I_AM, handler_i_am_bind);
    apdu_set_unrecognized_service_handler_handler(handler_unrecognized_service);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROPERTY,      handler_read_property);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROP_MULTIPLE,  handler_read_property_multiple);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_WRITE_PROPERTY,     handler_write_property);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_WRITE_PROP_MULTIPLE, handler_write_property_multiple);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_RANGE,         handler_read_range);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_REINITIALIZE_DEVICE, handler_reinitialize_device);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_SUBSCRIBE_COV,      handler_cov_subscribe);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_DEVICE_COMMUNICATION_CONTROL, handler_device_communication_control);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_UTC_TIME_SYNCHRONIZATION, handler_timesync_utc);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_TIME_SYNCHRONIZATION,     handler_timesync);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_COV_NOTIFICATION,         handler_ucov_notification);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_PRIVATE_TRANSFER,         handler_unconfirmed_private_transfer);
}

static void ntp_sync_notification_cb(struct timeval *tv)
{
    time_t now = tv->tv_sec;
    struct tm *t = localtime(&now);
    ESP_LOGI(TAG, "[NTP] Synchronise — heure locale: %02d:%02d:%02d",
             t->tm_hour, t->tm_min, t->tm_sec);
    rfm_sync_rtc_from_ntp();
    /* Recalculer les heures solaires après sync NTP */
    solar_invalidate_cache();
    schedule_update_solar_times();
}

void ntp_initialize(void)
{
    ESP_LOGI(TAG, "[NTP] Demarrage...");
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0,"pool.ntp.org");
    esp_sntp_setservername(1,"time.google.com");
    sntp_set_time_sync_notification_cb(ntp_sync_notification_cb);
    esp_sntp_init();
    int retry = 0;
    time_t now = 0;
    while (retry < 20) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        now = time(NULL);
        if (now > 1000000000) {
            struct tm *t = localtime(&now);
            ESP_LOGI(TAG, "[NTP] OK — heure locale: %02d:%02d:%02d",
                     t->tm_hour, t->tm_min, t->tm_sec);
            return;
        }
        ESP_LOGI(TAG, "[NTP] Attente... (%d/20)", retry + 1);
        retry++;
    }
    ESP_LOGW(TAG, "[NTP] Timeout");
}

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

static bool schedule_has_active_entries(SCHEDULE_DESCR *desc, BACNET_WEEKDAY wday)
{
    if (!desc) return false;
#if BACNET_EXCEPTION_SCHEDULE_SIZE > 0
    if (desc->Exception_Count > 0) return true;
#endif
    if (wday >= 1 && wday <= 7) {
        if (desc->Weekly_Schedule[wday - 1].TV_Count > 0) return true;
    }
    return false;
}

/* Valeurs précédentes — accessibles pour forçage depuis Http_server */
static volatile float g_prev_pv0    = -1.0f;
static volatile float g_prev_pv1    = -1.0f;
static volatile bool  g_force_pv0   = false;
static volatile bool  g_force_pv1   = false;
static volatile float g_forced_val0 = 0.0f;
static volatile float g_forced_val1 = 0.0f;

void sched_force_av(int inst, float val)
{
    if (inst == 0) {
        Analog_Value_Present_Value_Set(0, val, 16);
        av_pwm_apply(0, val);
        Multistate_Value_Update_From_AV(0, val);
        rfm_save_av_state(val, Analog_Value_Present_Value(1));
        rfm_log_event(EVENT_AV0_CHANGE, val, 0);
        g_force_pv0   = true;
        g_forced_val0 = val;
        g_prev_pv0    = -1.0f;
        ESP_LOGI("main", "[FORCE] AV0 → %.1f%% (page web)", val);
    } else {
        Analog_Value_Present_Value_Set(1, val, 16);
        av_pwm_apply(1, val);
        Multistate_Value_Update_From_AV(1, val);
        rfm_save_av_state(Analog_Value_Present_Value(0), val);
        rfm_log_event(EVENT_AV1_CHANGE, val, 0);
        g_force_pv1   = true;
        g_forced_val1 = val;
        g_prev_pv1    = -1.0f;
        ESP_LOGI("main", "[FORCE] AV1 → %.1f%% (page web)", val);
    }
}

static void schedule_task(void *pvParameters)
{
    BACNET_TIME btime;
    BACNET_WEEKDAY wday;
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
        /* ---- MOD 1 : Remplir le Weekly Schedule avec les heures solaires ---- */
        schedule_update_solar_times();
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

        wday = (BACNET_WEEKDAY)((t->tm_wday == 0) ? 7 : t->tm_wday);

        /* ---- MOD 1 : Recalcul solaire à minuit → mise à jour Weekly ---- */
        if (t->tm_mday != s_prev_mday) {
            s_prev_mday = t->tm_mday;
            solar_invalidate_cache();
            schedule_update_solar_times();
            ESP_LOGI(TAG, "[SOLAR-SCH] Nouveau jour — Weekly Schedule mis à jour");
        }

        desc0 = Schedule_Object(0);
        if (desc0) { Schedule_Recalculate_PV(desc0, wday, &btime); }
        desc1 = Schedule_Object(1);
        if (desc1) { Schedule_Recalculate_PV(desc1, wday, &btime); }

        {
            const solar_config_t *scfg = solar_get_config();

            if (scfg->enabled) {
                bool  is_night   = solar_is_night_now();
                float solar_val  = is_night ? 100.0f : 0.0f;

                if (btime.hour == 0 && btime.min == 0 && btime.sec == 0) {
                    solar_invalidate_cache();
                }

                static bool s_prev_is_night = false;
                static bool s_first_cycle   = true;
                bool solar_transition = (!s_first_cycle && is_night != s_prev_is_night);
                s_prev_is_night = is_night;
                s_first_cycle   = false;

#if BACNET_EXCEPTION_SCHEDULE_SIZE > 0
                bool has_se0 = false;
                if (is_night && desc0 && desc0->Exception_Count > 0) {
                    float se_pv0  = desc0->Present_Value.type.Real;
                    float se_def0 = desc0->Schedule_Default.type.Real;
                    if (se_pv0 != se_def0 && se_pv0 > 0.0f) {
                        has_se0 = true;
                    }
                }
                bool has_se1 = false;
                if (is_night && desc1 && desc1->Exception_Count > 0) {
                    float se_pv1  = desc1->Present_Value.type.Real;
                    float se_def1 = desc1->Schedule_Default.type.Real;
                    if (se_pv1 != se_def1 && se_pv1 > 0.0f) {
                        has_se1 = true;
                    }
                }
#else
                bool has_se0 = false;
                bool has_se1 = false;
#endif
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
                    else if (has_se0)    { target0 = desc0->Present_Value.type.Real; }
                    else                 { target0 = 100.0f; }
                    apply_zone_a:;
                    if (target0 != g_prev_pv0) {
                        Analog_Value_Present_Value_Set(0, target0, 16);
                        av_pwm_apply(0, target0);
                        Multistate_Value_Update_From_AV(0, target0);
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
                    if (!is_night)       { target1 = 0.0f; }
                    else if (has_se1)    { target1 = desc1->Present_Value.type.Real; }
                    else                 { target1 = 100.0f; }
                    apply_zone_b:;
                    if (target1 != g_prev_pv1) {
                        Analog_Value_Present_Value_Set(1, target1, 16);
                        av_pwm_apply(1, target1);
                        Multistate_Value_Update_From_AV(1, target1);
                        rfm_save_av_state(Analog_Value_Present_Value(0), target1);
                        g_prev_pv1 = target1;
                    }
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
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    led_initialize();

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

    Device_Set_Object_Instance_Number(SERVER_DEVICE_ID);
    ESP_LOGI(TAG, "BACnet Stack %s | Device ID: %lu",
             BACnet_Version, (unsigned long)Device_Object_Instance_Number());
    address_init();
    Init_Service_Handlers();

    if (rfm_init() == ESP_OK) {
        float saved_av0 = 0.0f, saved_av1 = 0.0f;
        bool  saved_manual = false;
        if (rfm_load_av_state(&saved_av0, &saved_av1) == ESP_OK) {
            Analog_Value_Present_Value_Set(0, saved_av0, 16);
            Analog_Value_Present_Value_Set(1, saved_av1, 16);
            av_pwm_apply(0, saved_av0);
            av_pwm_apply(1, saved_av1);
        }
        if (rfm_load_bv_state(&saved_manual) == ESP_OK) {
            Binary_Value_Present_Value_Set(0, saved_manual ? BINARY_ACTIVE : BINARY_INACTIVE);
        }
        ESP_LOGI(TAG, "[BOOT] AV0=%.1f AV1=%.1f", saved_av0, saved_av1);
        sched_persist_restore_all();
        solar_init();
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