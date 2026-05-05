#include "schedule_persist.h"
#include "fram_layout.h"
#include "fram_fm24cl64b.h"
#include "schedule.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "sched_persist";

/* ================================================================
 * sched_persist_save_weekly
 * Sauvegarde le Weekly Schedule d'un schedule en FRAM.
 * Format compact : pour chaque jour → TV_Count + entrées time/val.
 * ================================================================ */
esp_err_t sched_persist_save_weekly(uint32_t instance, SCHEDULE_DESCR *desc)
{
    if (!desc || instance >= 2) return ESP_ERR_INVALID_ARG;

    uint8_t  buf[FRAM_SCH_DAY_SIZE];
    uint16_t base_addr = FRAM_SCH_ADDR(instance);
    int day, tv;
    esp_err_t ret;

    for (day = 0; day < FRAM_SCH_DAYS; day++) {
        BACNET_DAILY_SCHEDULE *ds = &desc->Weekly_Schedule[day];
        uint8_t count = (ds->TV_Count < FRAM_SCH_MAX_TV)
                        ? (uint8_t)ds->TV_Count
                        : FRAM_SCH_MAX_TV;

        memset(buf, 0, sizeof(buf));
        buf[0] = count;

        for (tv = 0; tv < count; tv++) {
            fram_tv_entry_t entry;
            entry.hour       = ds->Time_Values[tv].Time.hour;
            entry.min        = ds->Time_Values[tv].Time.min;
            entry.sec        = ds->Time_Values[tv].Time.sec;
            entry.hundredths = ds->Time_Values[tv].Time.hundredths;
            entry.value      = ds->Time_Values[tv].Value.type.Real;
            memcpy(&buf[1 + tv * FRAM_SCH_TV_SIZE], &entry,
                   sizeof(fram_tv_entry_t));
        }

        uint16_t addr = base_addr + (uint16_t)(day * FRAM_SCH_DAY_SIZE);
        ret = fram_write(addr, buf, FRAM_SCH_DAY_SIZE);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Erreur écriture Weekly SCH%lu jour%d : %s",
                     (unsigned long)instance, day, esp_err_to_name(ret));
            return ret;
        }
    }

    ESP_LOGI(TAG, "Weekly Schedule SCH%lu sauvegardé",
             (unsigned long)instance);
    return ESP_OK;
}

/* ================================================================
 * sched_persist_save_special_events
 * Sauvegarde les Special Events d'un schedule en FRAM.
 * Seuls les CalendarEntry de type DATE sont supportés.
 * ================================================================ */
esp_err_t sched_persist_save_special_events(uint32_t instance,
                                             SCHEDULE_DESCR *desc)
{
    if (!desc || instance >= 2) return ESP_ERR_INVALID_ARG;

#if BACNET_EXCEPTION_SCHEDULE_SIZE > 0
    uint8_t count = (desc->Exception_Count < FRAM_SE_MAX_COUNT)
                    ? desc->Exception_Count
                    : FRAM_SE_MAX_COUNT;
    int i, tv;
    esp_err_t ret;

    /* Sauvegarder le compteur */
    uint8_t cnt_byte = count;
    ret = fram_write(FRAM_SE_COUNT_ADDR(instance), &cnt_byte, 1);
    if (ret != ESP_OK) return ret;

    /* Sauvegarder chaque Special Event */
    for (i = 0; i < count; i++) {
        BACNET_SPECIAL_EVENT *se = &desc->Exception_Schedule[i];
        fram_se_entry_t entry;
        memset(&entry, 0, sizeof(entry));

        /* Seul CALENDAR_ENTRY / DATE supporté */
        if (se->periodTag != BACNET_SPECIAL_EVENT_PERIOD_CALENDAR_ENTRY) {
            ESP_LOGW(TAG, "SE[%d] type non supporté pour FRAM — ignoré", i);
            continue;
        }
        if (se->period.calendarEntry.tag != BACNET_CALENDAR_DATE) {
            ESP_LOGW(TAG, "SE[%d] CalendarEntry non DATE — ignoré", i);
            continue;
        }

        entry.valid      = 0xAA;
        entry.period_tag = (uint8_t)se->periodTag;
        entry.year       = se->period.calendarEntry.type.Date.year;
        entry.month      = se->period.calendarEntry.type.Date.month;
        entry.day        = se->period.calendarEntry.type.Date.day;
        entry.wday       = se->period.calendarEntry.type.Date.wday;
        entry.priority   = se->priority;

        uint8_t tv_count = (se->timeValues.TV_Count < FRAM_SE_MAX_TV)
                           ? (uint8_t)se->timeValues.TV_Count
                           : FRAM_SE_MAX_TV;
        entry.tv_count = tv_count;

        for (tv = 0; tv < tv_count; tv++) {
            entry.tv[tv].hour       = se->timeValues.Time_Values[tv].Time.hour;
            entry.tv[tv].min        = se->timeValues.Time_Values[tv].Time.min;
            entry.tv[tv].sec        = se->timeValues.Time_Values[tv].Time.sec;
            entry.tv[tv].hundredths = se->timeValues.Time_Values[tv].Time.hundredths;
            entry.tv[tv].value      = se->timeValues.Time_Values[tv].Value.type.Real;
        }

        uint16_t addr = FRAM_SE_ENTRY_ADDR(instance, i);
        ret = fram_write(addr, (uint8_t *)&entry, sizeof(fram_se_entry_t));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Erreur écriture SE[%d] SCH%lu : %s",
                     i, (unsigned long)instance, esp_err_to_name(ret));
            return ret;
        }
    }

    ESP_LOGI(TAG, "Special Events SCH%lu sauvegardés (%d entrées)",
             (unsigned long)instance, count);
    return ESP_OK;
#else
    return ESP_OK;
#endif
}

/* ================================================================
 * sched_persist_restore_weekly
 * Restaure le Weekly Schedule d'un schedule depuis la FRAM.
 * ================================================================ */
static esp_err_t restore_weekly(uint32_t instance)
{
    SCHEDULE_DESCR *desc = Schedule_Object(instance);
    if (!desc) return ESP_ERR_INVALID_ARG;

    uint8_t  buf[FRAM_SCH_DAY_SIZE];
    uint16_t base_addr = FRAM_SCH_ADDR(instance);
    int day, tv;
    esp_err_t ret;
    bool has_entries = false;

    for (day = 0; day < FRAM_SCH_DAYS; day++) {
        uint16_t addr = base_addr + (uint16_t)(day * FRAM_SCH_DAY_SIZE);
        ret = fram_read(addr, buf, FRAM_SCH_DAY_SIZE);
        if (ret != ESP_OK) return ret;

        uint8_t count = buf[0];
        if (count > FRAM_SCH_MAX_TV) count = 0; /* données corrompues */

        desc->Weekly_Schedule[day].TV_Count = count;
        for (tv = 0; tv < count; tv++) {
            fram_tv_entry_t entry;
            memcpy(&entry, &buf[1 + tv * FRAM_SCH_TV_SIZE],
                   sizeof(fram_tv_entry_t));
            desc->Weekly_Schedule[day].Time_Values[tv].Time.hour       = entry.hour;
            desc->Weekly_Schedule[day].Time_Values[tv].Time.min        = entry.min;
            desc->Weekly_Schedule[day].Time_Values[tv].Time.sec        = entry.sec;
            desc->Weekly_Schedule[day].Time_Values[tv].Time.hundredths = entry.hundredths;
            desc->Weekly_Schedule[day].Time_Values[tv].Value.tag       = BACNET_APPLICATION_TAG_REAL;
            desc->Weekly_Schedule[day].Time_Values[tv].Value.type.Real = entry.value;
            has_entries = true;
        }
    }

    if (has_entries)
        ESP_LOGI(TAG, "Weekly Schedule SCH%lu restauré",
                 (unsigned long)instance);
    return ESP_OK;
}

/* ================================================================
 * restore_special_events
 * Restaure les Special Events d'un schedule depuis la FRAM.
 * ================================================================ */
static esp_err_t restore_special_events(uint32_t instance)
{
#if BACNET_EXCEPTION_SCHEDULE_SIZE > 0
    SCHEDULE_DESCR *desc = Schedule_Object(instance);
    if (!desc) return ESP_ERR_INVALID_ARG;

    esp_err_t ret;
    uint8_t count = 0;

    ret = fram_read(FRAM_SE_COUNT_ADDR(instance), &count, 1);
    if (ret != ESP_OK) return ret;
    if (count > FRAM_SE_MAX_COUNT) count = 0;
    if (count == 0) return ESP_OK;

    int i, tv;
    desc->Exception_Count = 0;
    memset(desc->Exception_Schedule, 0,
           sizeof(desc->Exception_Schedule));

    for (i = 0; i < count; i++) {
        if (i >= BACNET_EXCEPTION_SCHEDULE_SIZE) break;

        fram_se_entry_t entry;
        uint16_t addr = FRAM_SE_ENTRY_ADDR(instance, i);
        ret = fram_read(addr, (uint8_t *)&entry, sizeof(fram_se_entry_t));
        if (ret != ESP_OK) continue;
        if (entry.valid != 0xAA) continue;

        BACNET_SPECIAL_EVENT *se = &desc->Exception_Schedule[i];

        se->periodTag = BACNET_SPECIAL_EVENT_PERIOD_CALENDAR_ENTRY;
        se->period.calendarEntry.tag              = BACNET_CALENDAR_DATE;
        se->period.calendarEntry.type.Date.year   = entry.year;
        se->period.calendarEntry.type.Date.month  = entry.month;
        se->period.calendarEntry.type.Date.day    = entry.day;
        se->period.calendarEntry.type.Date.wday   = entry.wday;
        se->priority = entry.priority;

        uint8_t tv_count = (entry.tv_count < FRAM_SE_MAX_TV)
                           ? entry.tv_count
                           : FRAM_SE_MAX_TV;
        se->timeValues.TV_Count = tv_count;

        for (tv = 0; tv < tv_count; tv++) {
            se->timeValues.Time_Values[tv].Time.hour       = entry.tv[tv].hour;
            se->timeValues.Time_Values[tv].Time.min        = entry.tv[tv].min;
            se->timeValues.Time_Values[tv].Time.sec        = entry.tv[tv].sec;
            se->timeValues.Time_Values[tv].Time.hundredths = entry.tv[tv].hundredths;
            se->timeValues.Time_Values[tv].Value.tag       = BACNET_APPLICATION_TAG_REAL;
            se->timeValues.Time_Values[tv].Value.type.Real = entry.tv[tv].value;
        }

        desc->Exception_Count++;
    }

    ESP_LOGI(TAG, "Special Events SCH%lu restaurés (%d entrées)",
             (unsigned long)instance, desc->Exception_Count);
    return ESP_OK;
#else
    return ESP_OK;
#endif
}

/* ================================================================
 * sched_persist_restore_all
 * Restaure Weekly Schedule + Special Events des 2 schedules.
 * Appelée dans app_main() après Init_Service_Handlers().
 * ================================================================ */
esp_err_t sched_persist_restore_all(void)
{
    esp_err_t ret;

    ret = restore_weekly(0);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "Weekly SCH0 non restauré : %s", esp_err_to_name(ret));

    ret = restore_weekly(1);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "Weekly SCH1 non restauré : %s", esp_err_to_name(ret));

    ret = restore_special_events(0);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "SE SCH0 non restaurés : %s", esp_err_to_name(ret));

    ret = restore_special_events(1);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "SE SCH1 non restaurés : %s", esp_err_to_name(ret));

    return ESP_OK;
}