#include "ntp.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "rtc_fram_manager.h"
#include "solar.h"
#include "schedule.h"

#include <sys/time.h>
#include <time.h>


const char *TAG = "ntp";
void ntp_sync_notification_cb(struct timeval *tv)
{
    time_t now = tv->tv_sec;
    struct tm *t = localtime(&now);
    ESP_LOGI(TAG, "[NTP] Synchronized local time: %02d:%02d:%02d",
             t->tm_hour, t->tm_min, t->tm_sec);
    rfm_sync_rtc_from_ntp();
    solar_invalidate_cache();
}

void ntp_initialize(void)
{
    ESP_LOGI(TAG, "[NTP] Demarrage...");
    
    /*get timezone */
    const solar_config_t *solar_cfg = solar_get_config();
    if (solar_cfg && solar_cfg->valid == 0xAA) {
        const char *tz_string = solar_get_timezone_posix(solar_cfg->latitude, solar_cfg->longitude);
        if (tz_string) {
            ESP_LOGI(TAG, "[NTP] Setting timezone based on GPS: lat=%.2f lon=%.2f -> %s",
                     solar_cfg->latitude, solar_cfg->longitude, tz_string);
            setenv("TZ", tz_string, 1);
        } else {
            ESP_LOGW(TAG, "[NTP] Failed to get timezone, using default CET");
            setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
        }
    } else {
        ESP_LOGW(TAG, "[NTP] No valid solar config, using default CET");
        setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    }
    
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