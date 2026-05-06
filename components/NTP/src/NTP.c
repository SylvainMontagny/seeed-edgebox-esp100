#include "NTP.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "rtc_fram_manager.h"
#include "solar.h"
#include "schedule.h"

#include <sys/time.h>
#include <time.h>

extern void schedule_update_solar_times(void);

const char *TAG = "ntp";
void ntp_sync_notification_cb(struct timeval *tv)
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