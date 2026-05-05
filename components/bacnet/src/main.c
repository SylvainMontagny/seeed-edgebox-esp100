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
#include "esp_modem_api.h"        /* ← 4G : remplace esp_wifi.h + wifi.h */
#include "ping/ping_sock.h"       /* ← Ping */
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

/* ★ RTC + FRAM */
#include "rtc_fram_manager.h"

#define SERVER_DEVICE_ID 1234

static const char *TAG = "main";

/* ================================================================
 * GPIO ESP32-S3 EdgeBox
 * AO0 → IO42  (AV0 PWM)
 * AO1 → IO41  (AV1 PWM)
 * ================================================================ */
#define GPIO_AV0  GPIO_NUM_42
#define GPIO_AV1  GPIO_NUM_41

/* ================================================================
 * Modem 4G — SIM7600 sur EdgeBox ESP32-S3
 * ================================================================ */

#define MODEM_TX_PIN   48
#define MODEM_RX_PIN   47
#define MODEM_PWR_KEY  21
#define MODEM_PWR_EN   16
#define MODEM_APN      "iot.1nce.net"   /* ← adaptez votre APN si nécessaire */

/* ================================================================
 * Object Table (inchangée)
 * ================================================================ */
static object_functions_t Object_Table[] = {
    {
        OBJECT_DEVICE, NULL,
        Device_Count, Device_Index_To_Instance,
        Device_Valid_Object_Instance_Number, Device_Object_Name,
        Device_Read_Property_Local, Device_Write_Property_Local,
        Device_Property_Lists,
        NULL, NULL, NULL, NULL, NULL, NULL
    },
    {
        OBJECT_ANALOG_VALUE, Analog_Value_Init,
        Analog_Value_Count, Analog_Value_Index_To_Instance,
        Analog_Value_Valid_Instance, Analog_Value_Object_Name,
        Analog_Value_Read_Property, Analog_Value_Write_Property,
        Analog_Value_Property_Lists,
        NULL, NULL, NULL, NULL, NULL, NULL
    },
    {
        OBJECT_BINARY_VALUE, Binary_Value_Init,
        Binary_Value_Count, Binary_Value_Index_To_Instance,
        Binary_Value_Valid_Instance, Binary_Value_Object_Name,
        Binary_Value_Read_Property, Binary_Value_Write_Property,
        Binary_Value_Property_Lists,
        NULL, NULL, NULL, NULL, NULL, NULL
    },
    {
        OBJECT_SCHEDULE, Schedule_Init,
        Schedule_Count, Schedule_Index_To_Instance,
        Schedule_Valid_Instance, Schedule_Object_Name,
        Schedule_Read_Property, Schedule_Write_Property,
        (rpm_property_lists_function)Schedule_Property_Lists,
        NULL, NULL, NULL, NULL, NULL, NULL
    },
    {
        OBJECT_MULTI_STATE_VALUE, Multistate_Value_Init,
        Multistate_Value_Count, Multistate_Value_Index_To_Instance,
        Multistate_Value_Valid_Instance, Multistate_Value_Object_Name,
        Multistate_Value_Read_Property, Multistate_Value_Write_Property,
        Multistate_Value_Property_Lists,
        NULL, NULL, NULL, NULL, NULL, NULL
    },
    {
        OBJECT_CALENDAR, Calendar_Init,
        Calendar_Count, Calendar_Index_To_Instance,
        Calendar_Valid_Instance, Calendar_Object_Name,
        Calendar_Read_Property, Calendar_Write_Property,
        (rpm_property_lists_function)Calendar_Property_Lists,
        NULL, NULL, NULL, NULL, NULL, NULL
    },
    {
        OBJECT_TRENDLOG, Trend_Log_Init,
        Trend_Log_Count, Trend_Log_Index_To_Instance,
        Trend_Log_Valid_Instance, Trend_Log_Object_Name,
        Trend_Log_Read_Property, Trend_Log_Write_Property,
        (rpm_property_lists_function)Trend_Log_Property_Lists,
        TrendLogGetRRInfo, NULL, NULL, NULL, NULL, NULL
    },
    {
        MAX_BACNET_OBJECT_TYPE, NULL,
        NULL, NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL
    }
};

/* ================================================================
 * Fonction PWM centralisée
 * ================================================================ */
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
 * Init handlers BACnet (inchangé)
 * ================================================================ */
static void Init_Service_Handlers(void)
{
    Device_Init(&Object_Table[0]);

    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_IS, handler_who_is);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_HAS, handler_who_has);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_I_AM, handler_i_am_bind);
    apdu_set_unrecognized_service_handler_handler(handler_unrecognized_service);

    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROPERTY,
        handler_read_property);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROP_MULTIPLE,
        handler_read_property_multiple);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_WRITE_PROPERTY,
        handler_write_property);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_WRITE_PROP_MULTIPLE,
        handler_write_property_multiple);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_RANGE,
        handler_read_range);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_REINITIALIZE_DEVICE,
        handler_reinitialize_device);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_SUBSCRIBE_COV,
        handler_cov_subscribe);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_DEVICE_COMMUNICATION_CONTROL,
        handler_device_communication_control);

    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_UTC_TIME_SYNCHRONIZATION,
        handler_timesync_utc);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_TIME_SYNCHRONIZATION,
        handler_timesync);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_COV_NOTIFICATION,
        handler_ucov_notification);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_PRIVATE_TRANSFER,
        handler_unconfirmed_private_transfer);
}

/* ================================================================
 * Callback NTP
 * ★ Sauvegarde l'heure dans le RTC matériel + FRAM à chaque synchro
 * ================================================================ */
static void ntp_sync_notification_cb(struct timeval *tv)
{
    time_t now = tv->tv_sec;
    struct tm *t = localtime(&now);
    ESP_LOGI(TAG, "[NTP] ✓ Synchronisé — heure locale: %02d:%02d:%02d",
             t->tm_hour, t->tm_min, t->tm_sec);

    /* ★ Sauvegarde heure NTP → RTC PCF8563 + backup FRAM */
    rfm_sync_rtc_from_ntp();
}

/* ================================================================
 * Initialisation NTP — appelée APRÈS connexion 4G
 * ================================================================ */
static void ntp_initialize(void)
{
    ESP_LOGI(TAG, "[NTP] Démarrage...");

    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
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
    ESP_LOGW(TAG, "[NTP] Timeout — heure peut être incorrecte");
}

/* ================================================================
 * Callbacks Ping
 * ================================================================ */
static void ping_success_cb(esp_ping_handle_t hdl, void *args)
{
    uint32_t elapsed_time;
    uint8_t ttl;
    uint16_t seq;
    uint32_t recv_len;
    ip_addr_t target_addr;

    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP,  &elapsed_time, sizeof(elapsed_time));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL,       &ttl,          sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO,     &seq,          sizeof(seq));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE,      &recv_len,     sizeof(recv_len));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR,    &target_addr,  sizeof(target_addr));

    ESP_LOGI(TAG, "[PING] %lu bytes from %s : seq=%d ttl=%d time=%lu ms",
             recv_len,
             inet_ntoa(target_addr.u_addr.ip4),
             seq, ttl, elapsed_time);
}

static void ping_timeout_cb(esp_ping_handle_t hdl, void *args)
{
    uint16_t seq;
    ip_addr_t target_addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO,  &seq,         sizeof(seq));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    ESP_LOGW(TAG, "[PING] Timeout — seq=%d vers %s",
             seq, inet_ntoa(target_addr.u_addr.ip4));
}

/* ================================================================
 * ping_task — ping 8.8.8.8 toutes les 900 secondes
 * Démarre après connexion 4G établie.
 * ================================================================ */
static void ping_task(void *pvParameters)
{
    /* Résolution IP cible */
    ip_addr_t target_addr;
    inet_pton(AF_INET, "8.8.8.8", &target_addr.u_addr.ip4);
    target_addr.type = IPADDR_TYPE_V4;

    /* Config ping */
    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.target_addr        = target_addr;
    config.count              = ESP_PING_COUNT_INFINITE; /* ping en continu */
    config.interval_ms        = 900000;                   /* toutes les 900s  */
    config.timeout_ms         = 3000;                    /* timeout 3s      */
    config.task_stack_size    = 4096;
    config.task_prio          = 1;

    /* Callbacks */
    esp_ping_callbacks_t cbs = {
        .on_ping_success = ping_success_cb,
        .on_ping_timeout = ping_timeout_cb,
        .on_ping_end     = NULL,
        .cb_args         = NULL
    };

    esp_ping_handle_t ping;
    esp_err_t err = esp_ping_new_session(&config, &cbs, &ping);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[PING] Échec création session : %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    esp_ping_start(ping);
    ESP_LOGI(TAG, "[PING] Démarré → 8.8.8.8 toutes les 900s");

    /* La tâche reste vivante — le ping tourne en callbacks */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

/* ================================================================
 * Tâche Schedule
 *
 * Schedule 0 → AV0 (GPIO42)
 * Schedule 1 → AV1 (GPIO41)
 *
 * Mode AUTO   (BV0=INACTIVE) : Schedule pilote AV + PWM + MSV
 * Mode MANUEL (BV0=ACTIVE)   : opérateur pilote AV via YABE + MSV
 *
 * Log uniquement sur changement de PV ou de mode.
 * ★ Sauvegarde FRAM sur chaque changement de PV ou de mode.
 * ================================================================ */
static void schedule_task(void *pvParameters)
{
    BACNET_TIME btime;
    BACNET_WEEKDAY wday;
    SCHEDULE_DESCR *desc0 = NULL;
    SCHEDULE_DESCR *desc1 = NULL;
    struct tm *t;
    time_t now;

    /* ★ Init depuis FRAM : evite sauvegarde parasite de 0.0 au 1er cycle */
    float prev_pv0 = 0.0f;
    float prev_pv1 = 0.0f;
    bool  prev_manual = false;
    rfm_load_av_state(&prev_pv0, &prev_pv1);
    rfm_load_bv_state(&prev_manual);

    /* Récupération IP sur interface PPP (4G) */
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("PPP_DEF");
    vTaskDelay(pdMS_TO_TICKS(2000));

    char ip_str[32] = "0.0.0.0";
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
    }

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "Schedule Task démarrée | IP 4G: %s", ip_str);
    ESP_LOGI(TAG, "SCH-0 → AV0/GPIO42 | SCH-1 → AV1/GPIO41");
    ESP_LOGI(TAG, "================================================");

    for (;;) {
        now = time(NULL);
        t   = localtime(&now);

        btime.hour       = (uint8_t)t->tm_hour;
        btime.min        = (uint8_t)t->tm_min;
        btime.sec        = (uint8_t)t->tm_sec;
        btime.hundredths = 0;

        wday = (BACNET_WEEKDAY)((t->tm_wday == 0) ? 7 : t->tm_wday);

        /* ── 1. Recalcul des schedules ── */
        desc0 = Schedule_Object(0);
        if (desc0) Schedule_Recalculate_PV(desc0, wday, &btime);

        desc1 = Schedule_Object(1);
        if (desc1) Schedule_Recalculate_PV(desc1, wday, &btime);

        /* ── 2. Pilotage des sorties ── */
        bool manual = Binary_Value_Is_Control_Enabled();

        if (!manual) {
            if (desc0) {
                float pv0 = desc0->Present_Value.type.Real;
                Analog_Value_Present_Value_Set(0, pv0, 16);
                av_pwm_apply(0, pv0);
                Multistate_Value_Update_From_AV(0, pv0);
                if (pv0 != prev_pv0) {
                    ESP_LOGI(TAG, "[EVENT] SCH-0 PV: %.1f → %.1f | %02d:%02d:%02d",
                             prev_pv0,
                             pv0, btime.hour, btime.min, btime.sec);
                    prev_pv0 = pv0;
                    /* ★ Sauvegarde AV0 dans FRAM + log événement */
                    rfm_save_av_state(pv0, Analog_Value_Present_Value(1));
                    rfm_log_event(EVENT_AV0_CHANGE, pv0, 0);
                }
            }
            if (desc1) {
                float pv1 = desc1->Present_Value.type.Real;
                Analog_Value_Present_Value_Set(1, pv1, 16);
                av_pwm_apply(1, pv1);
                Multistate_Value_Update_From_AV(1, pv1);
                if (pv1 != prev_pv1) {
                    ESP_LOGI(TAG, "[EVENT] SCH-1 PV: %.1f → %.1f | %02d:%02d:%02d",
                             prev_pv1,
                             pv1, btime.hour, btime.min, btime.sec);
                    prev_pv1 = pv1;
                    /* ★ Sauvegarde AV1 dans FRAM + log événement */
                    rfm_save_av_state(Analog_Value_Present_Value(0), pv1);
                    rfm_log_event(EVENT_AV1_CHANGE, pv1, 0);
                }
            }
        } else {
            /* Mode MANUEL : l'opérateur écrit AV via YABE */
            float pv0_man = Analog_Value_Present_Value(0);
            float pv1_man = Analog_Value_Present_Value(1);
            Multistate_Value_Update_From_AV(0, pv0_man);
            Multistate_Value_Update_From_AV(1, pv1_man);
            /* ★ Sauvegarde FRAM si une valeur a changé en mode MANUEL */
            if (pv0_man != prev_pv0 || pv1_man != prev_pv1) {
                rfm_save_av_state(pv0_man, pv1_man);
                if (pv0_man != prev_pv0) {
                    rfm_log_event(EVENT_AV0_CHANGE, pv0_man, 0);
                    ESP_LOGI(TAG, "[MANUEL] AV0: %.1f → %.1f sauvegardé",
                             prev_pv0, pv0_man);
                    prev_pv0 = pv0_man;
                }
                if (pv1_man != prev_pv1) {
                    rfm_log_event(EVENT_AV1_CHANGE, pv1_man, 0);
                    ESP_LOGI(TAG, "[MANUEL] AV1: %.1f → %.1f sauvegardé",
                             prev_pv1, pv1_man);
                    prev_pv1 = pv1_man;
                }
            }
        }

        if (manual != prev_manual) {
            ESP_LOGI(TAG, "[EVENT] Mode → %s | %02d:%02d:%02d",
                     manual ? "MANUEL" : "AUTO",
                     btime.hour, btime.min, btime.sec);
            prev_manual = manual;
            /* ★ Sauvegarde mode AUTO/MANUEL dans FRAM + log événement */
            rfm_save_bv_state(manual);
            rfm_log_event(EVENT_MODE_CHANGE, 0.0f, manual ? 1 : 0);
        }

        /* ── 3. Maintenance ── */
        Multistate_Value_Update_From_BV(manual);
        trend_log_timer(1);
        Calendar_Update_Present_Value(0);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ================================================================
 * modem_initialize — remplace wifi_initialize()
 *
 * Bloque jusqu'à IP PPP obtenue. Retry infini avec power cycle.
 * ================================================================ */
static void modem_initialize(void)
{
    ESP_LOGI(TAG, "[4G] Initialisation modem SIM7600...");

    gpio_set_direction((gpio_num_t)MODEM_PWR_EN,  GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)MODEM_PWR_KEY, GPIO_MODE_OUTPUT);

    int attempt = 0;

    while (1) {
        attempt++;
        ESP_LOGI(TAG, "[4G] Tentative %d — Power cycle...", attempt);

        gpio_set_level((gpio_num_t)MODEM_PWR_EN, 0);
        vTaskDelay(pdMS_TO_TICKS(3000));
        gpio_set_level((gpio_num_t)MODEM_PWR_EN, 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
        gpio_set_level((gpio_num_t)MODEM_PWR_KEY, 1);
        vTaskDelay(pdMS_TO_TICKS(1500));
        gpio_set_level((gpio_num_t)MODEM_PWR_KEY, 0);

        ESP_LOGI(TAG, "[4G] Attente boot modem (45s)...");
        vTaskDelay(pdMS_TO_TICKS(45000));

        esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
        dte_config.uart_config.tx_io_num  = MODEM_TX_PIN;
        dte_config.uart_config.rx_io_num  = MODEM_RX_PIN;
        dte_config.uart_config.port_num   = UART_NUM_2;

        esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
        esp_netif_t *esp_netif = esp_netif_new(&netif_ppp_config);

        esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(MODEM_APN);
        esp_modem_dce_t *dce = esp_modem_new_dev(
            ESP_MODEM_DCE_SIM7600, &dte_config, &dce_config, esp_netif);

        if (!dce) {
            ESP_LOGE(TAG, "[4G] Échec création DCE — retry...");
            if (esp_netif) esp_netif_destroy(esp_netif);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        ESP_LOGI(TAG, "[4G] Passage en mode DATA (PPP)...");
        if (esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA) != ESP_OK) {
            ESP_LOGE(TAG, "[4G] Échec mode DATA — retry...");
            esp_modem_destroy(dce);
            esp_netif_destroy(esp_netif);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        ESP_LOGI(TAG, "[4G] Attente IP PPP...");
        esp_netif_ip_info_t ip_info;
        int ip_retry = 0;
        while (ip_retry < 30) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (esp_netif_get_ip_info(esp_netif, &ip_info) == ESP_OK &&
                ip_info.ip.addr != 0) {
                char ip_str[32];
                esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
                ESP_LOGI(TAG, "[4G] ✓ CONNECTÉ ! IP: %s | APN: %s", ip_str, MODEM_APN);
                return;
            }
            ESP_LOGI(TAG, "[4G] Attente IP... (%d/30)", ++ip_retry);
        }

        ESP_LOGW(TAG, "[4G] Pas d'IP obtenue — retry complet...");
        esp_modem_destroy(dce);
        esp_netif_destroy(esp_netif);
    }
}

/* ================================================================
 * app_main
 *
 * Séquence :
 * 1. Init système
 * 2. Init FRAM + RTC  ★ NOUVEAU
 * 3. Init PWM
 * 4. Init BACnet stack + handlers
 * 5. Restauration état AV/BV depuis FRAM  ★ NOUVEAU
 * 6. modem_initialize()  ← bloque jusqu'à 4G connectée
 * 7. ntp_initialize()    ← sync heure via 4G (callback → RTC + FRAM)
 * 8. dlenv_init()        ← bind socket BACnet sur PPP
 * 9. Send_I_Am()         ← annonce BACnet
 * 10. Lancement tâches (schedule + ping)
 * ================================================================ */
 void app_main(void)
 {
     /* ── 1. Système ── */
     esp_err_t ret = nvs_flash_init();
     if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
         ESP_ERROR_CHECK(nvs_flash_erase());
         ret = nvs_flash_init();
     }
     ESP_ERROR_CHECK(ret);
     ESP_ERROR_CHECK(esp_netif_init());
     ESP_ERROR_CHECK(esp_event_loop_create_default());

     led_initialize();

     /* ── 2. PWM ── */
     ledc_timer_config_t ledc_timer = {
         .speed_mode      = LEDC_LOW_SPEED_MODE,
         .timer_num       = LEDC_TIMER_0,
         .duty_resolution = LEDC_TIMER_8_BIT,
         .freq_hz         = 4000,
         .clk_cfg         = LEDC_AUTO_CLK
     };
     ledc_timer_config(&ledc_timer);

     ledc_channel_config_t ledc_channel0 = {
         .speed_mode = LEDC_LOW_SPEED_MODE,
         .channel    = LEDC_CHANNEL_0,
         .timer_sel  = LEDC_TIMER_0,
         .intr_type  = LEDC_INTR_DISABLE,
         .gpio_num   = GPIO_AV0,
         .duty       = 0,
         .hpoint     = 0
     };
     ledc_channel_config(&ledc_channel0);

     ledc_channel_config_t ledc_channel1 = {
         .speed_mode = LEDC_LOW_SPEED_MODE,
         .channel    = LEDC_CHANNEL_1,
         .timer_sel  = LEDC_TIMER_0,
         .intr_type  = LEDC_INTR_DISABLE,
         .gpio_num   = GPIO_AV1,
         .duty       = 0,
         .hpoint     = 0
     };
     ledc_channel_config(&ledc_channel1);

     /* ── 3. BACnet stack (Initialisation de base AVANT de restaurer la FRAM) ── */
     Device_Set_Object_Instance_Number(SERVER_DEVICE_ID);
     ESP_LOGI(TAG, "BACnet Stack %s | Device ID: %lu | Max APDU: %d",
              BACnet_Version,
              (unsigned long)Device_Object_Instance_Number(),
              MAX_APDU);
     address_init();
     Init_Service_Handlers(); /* Cette fonction met tout à 0 par défaut */

     /* ★ ── 4. Init FRAM + Restauration (APRÈS l'init BACnet) ── */
     if (rfm_init() == ESP_OK) {
         /* Restauration état AV0 / AV1 */
         float saved_av0 = 0.0f, saved_av1 = 0.0f;
         if (rfm_load_av_state(&saved_av0, &saved_av1) == ESP_OK) {
             /* On écrase le 0.0 par défaut de BACnet avec nos valeurs FRAM */
             Analog_Value_Present_Value_Set(0, saved_av0, 16);
             Analog_Value_Present_Value_Set(1, saved_av1, 16);
             av_pwm_apply(0, saved_av0);
             av_pwm_apply(1, saved_av1);
         }

         /* Restauration mode AUTO/MANUEL dans le Binary Value 0 */
		 /* Restauration mode AUTO/MANUEL dans le Binary Value 0 */
		         bool saved_manual = false;
		         if (rfm_load_bv_state(&saved_manual) == ESP_OK) {
		             /* On déclare explicitement la fonction BACnet pour éviter l'erreur "implicit declaration" */
		             
		             
		             /* On force la valeur avec les bonnes constantes BACnet */
		             Binary_Value_Present_Value_Set(0, saved_manual ? BINARY_ACTIVE : BINARY_INACTIVE);
		         }
         ESP_LOGI(TAG, "[BOOT] État restauré — AV0=%.1f AV1=%.1f mode=%s",
                  saved_av0, saved_av1, saved_manual ? "MANUEL" : "AUTO");

         rfm_log_event(EVENT_BOOT, 0.0f, 0);
         rfm_dump();
     } else {
         ESP_LOGE(TAG, "[BOOT] FRAM/RTC indisponible — démarrage sans restauration");
     }

     /* ── 5. 4G ── bloque jusqu'à connexion établie ── */
     modem_initialize();

     /* ── 6. NTP ── sync heure via 4G (le callback rfm_sync_rtc_from_ntp() se déclenche) ── */
     ntp_initialize();

     /* ── 7. dlenv_init APRÈS 4G ── socket BACnet bindé sur PPP ── */
     dlenv_init();
     atexit(datalink_cleanup);
     Send_I_Am(&Handler_Transmit_Buffer[0]);

     /* ── 8. Tâches FreeRTOS ── */
     xTaskCreate(server_task,   "bacnet_server", 8000, NULL, 1, NULL);
     xTaskCreate(schedule_task, "schedule_task", 4096, NULL, 1, NULL);
     xTaskCreate(ping_task,     "ping_task",     4096, NULL, 1, NULL);

     ESP_LOGI(TAG, "=== BACnet éclairage public prêt (ESP32-S3 / 4G) ===");
     ESP_LOGI(TAG, "AV0 → GPIO42 (Schedule 0) | AV1 → GPIO41 (Schedule 1)");
     ESP_LOGI(TAG, "BV0=INACTIVE → AUTO | BV0=ACTIVE → MANUEL");
 }