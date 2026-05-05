#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "bacdef.h"
#include "bacdcode.h"
#include "bactext.h"
#include "proplist.h"
#include "timestamp.h"
#include "debug.h"
#include "device.h"
#include "schedule.h"
#include "bacapp.h"
#include "esp_log.h"
#include "schedule_persist.h"

#ifndef BACNET_ARRAY_INDEX
typedef uint8_t BACNET_ARRAY_INDEX;
#endif

#ifndef BACNET_ARRAY_ALL
#define BACNET_ARRAY_ALL 0xFFFFFFFF
#endif

#ifndef min
#define min(a,b) (((a)<(b))?(a):(b))
#endif

static SCHEDULE_DESCR Schedule_Descr[MAX_SCHEDULES];

static const int Schedule_Properties_Required[] = {
    PROP_OBJECT_IDENTIFIER,
    PROP_OBJECT_NAME,
    PROP_OBJECT_TYPE,
    PROP_PRESENT_VALUE,
    PROP_EFFECTIVE_PERIOD,
    PROP_SCHEDULE_DEFAULT,
    PROP_LIST_OF_OBJECT_PROPERTY_REFERENCES,
    PROP_PRIORITY_FOR_WRITING,
    PROP_STATUS_FLAGS,
    PROP_RELIABILITY,
    PROP_OUT_OF_SERVICE,
    -1
};

static const int Schedule_Properties_Optional[] = {
    PROP_WEEKLY_SCHEDULE,
    PROP_EXCEPTION_SCHEDULE,
    -1
};

static const int Schedule_Properties_Proprietary[] = { -1 };

void Schedule_Property_Lists(
    const int **pRequired,
    const int **pOptional,
    const int **pProprietary)
{
    if (pRequired)    *pRequired    = Schedule_Properties_Required;
    if (pOptional)    *pOptional    = Schedule_Properties_Optional;
    if (pProprietary) *pProprietary = Schedule_Properties_Proprietary;
}

SCHEDULE_DESCR *Schedule_Object(uint32_t object_instance)
{
    unsigned index = Schedule_Instance_To_Index(object_instance);
    if (index < MAX_SCHEDULES)
        return &Schedule_Descr[index];
    return NULL;
}

void Schedule_Init(void)
{
    unsigned i, j;
    BACNET_DATE start_date;
    BACNET_DATE end_date;
    SCHEDULE_DESCR *psched;

    start_date.year  = 255;
    start_date.month = 1;
    start_date.day   = 1;
    start_date.wday  = 255;

    end_date.year  = 255;
    end_date.month = 12;
    end_date.day   = 31;
    end_date.wday  = 255;

    for (i = 0; i < MAX_SCHEDULES; i++) {
        psched = &Schedule_Descr[i];

        psched->Start_Date = start_date;
        psched->End_Date   = end_date;

        for (j = 0; j < BACNET_WEEKLY_SCHEDULE_SIZE; j++)
            psched->Weekly_Schedule[j].TV_Count = 0;

        psched->Schedule_Default.context_specific = false;
        psched->Schedule_Default.tag              = BACNET_APPLICATION_TAG_REAL;
        psched->Schedule_Default.type.Real        = 21.0f;
        psched->Present_Value                     = psched->Schedule_Default;

        psched->obj_prop_ref_cnt     = 0;
        psched->Priority_For_Writing = 16;
        psched->Out_Of_Service       = false;

#if BACNET_EXCEPTION_SCHEDULE_SIZE > 0
        psched->Exception_Count = 0;
        memset(psched->Exception_Schedule, 0,
            sizeof(psched->Exception_Schedule));
#endif
    }
}

unsigned Schedule_Count(void) { return MAX_SCHEDULES; }

uint32_t Schedule_Index_To_Instance(unsigned index) { return index; }

unsigned Schedule_Instance_To_Index(uint32_t instance)
{
    return (instance < MAX_SCHEDULES) ? instance : MAX_SCHEDULES;
}

bool Schedule_Valid_Instance(uint32_t object_instance)
{
    return (Schedule_Instance_To_Index(object_instance) < MAX_SCHEDULES);
}

bool Schedule_Object_Name(uint32_t object_instance,
    BACNET_CHARACTER_STRING *object_name)
{
    char text[32];
    if (Schedule_Valid_Instance(object_instance)) {
        snprintf(text, sizeof(text), "SCHEDULE %u",
            (unsigned)object_instance);
        characterstring_init_ansi(object_name, text);
        return true;
    }
    return false;
}

/* ================================================================
 * Schedule_Exception_Schedule
 * ================================================================ */
#if BACNET_EXCEPTION_SCHEDULE_SIZE > 0
BACNET_SPECIAL_EVENT *Schedule_Exception_Schedule(
    uint32_t object_instance, unsigned array_index)
{
    SCHEDULE_DESCR *pObject = Schedule_Object(object_instance);
    if (pObject && (array_index < BACNET_EXCEPTION_SCHEDULE_SIZE)) {
        return &pObject->Exception_Schedule[array_index];
    }
    return NULL;
}

bool Schedule_Exception_Schedule_Set(
    uint32_t object_instance,
    unsigned array_index,
    const BACNET_SPECIAL_EVENT *value)
{
    SCHEDULE_DESCR *pObject = Schedule_Object(object_instance);
    if (pObject && value && (array_index < BACNET_EXCEPTION_SCHEDULE_SIZE)) {
        memcpy(&pObject->Exception_Schedule[array_index], value,
               sizeof(BACNET_SPECIAL_EVENT));
        if (array_index >= pObject->Exception_Count)
            pObject->Exception_Count = (uint8_t)(array_index + 1);
        return true;
    }
    return false;
}
#endif

static int Schedule_Weekly_Schedule_Encode(uint32_t object_instance,
    BACNET_ARRAY_INDEX array_index, uint8_t *apdu)
{
    SCHEDULE_DESCR *pObject = Schedule_Object(object_instance);
    if (pObject && array_index < BACNET_WEEKLY_SCHEDULE_SIZE) {
        return bacnet_dailyschedule_context_encode(apdu, 0,
            &pObject->Weekly_Schedule[array_index]);
    }
    return BACNET_STATUS_ERROR;
}

int Schedule_Read_Property(BACNET_READ_PROPERTY_DATA *rpdata)
{
    int apdu_len = 0;
    int len = 0;
    SCHEDULE_DESCR *CurrentSC = Schedule_Object(rpdata->object_instance);
    uint8_t *apdu = rpdata->application_data;
    BACNET_BIT_STRING bit_string;
    BACNET_CHARACTER_STRING char_string;

    if (!CurrentSC) return BACNET_STATUS_ERROR;

    switch ((int)rpdata->object_property) {

        case PROP_OBJECT_IDENTIFIER:
            apdu_len = encode_application_object_id(apdu,
                OBJECT_SCHEDULE, rpdata->object_instance);
            break;

        case PROP_OBJECT_NAME:
            Schedule_Object_Name(rpdata->object_instance, &char_string);
            apdu_len = encode_application_character_string(apdu, &char_string);
            break;

        case PROP_OBJECT_TYPE:
            apdu_len = encode_application_enumerated(apdu, OBJECT_SCHEDULE);
            break;

        case PROP_PRESENT_VALUE:
            apdu_len = bacapp_encode_application_data(apdu,
                &CurrentSC->Present_Value);
            break;

        case PROP_EFFECTIVE_PERIOD:
            apdu_len  = encode_application_date(apdu, &CurrentSC->Start_Date);
            apdu_len += encode_application_date(&apdu[apdu_len],
                &CurrentSC->End_Date);
            break;

        case PROP_WEEKLY_SCHEDULE:
            if (rpdata->array_index == 0) {
                apdu_len = encode_application_unsigned(apdu,
                    BACNET_WEEKLY_SCHEDULE_SIZE);
            } else if (rpdata->array_index == BACNET_ARRAY_ALL) {
                int i;
                for (i = 0; i < BACNET_WEEKLY_SCHEDULE_SIZE; i++) {
                    len = Schedule_Weekly_Schedule_Encode(
                        rpdata->object_instance, i,
                        apdu ? &apdu[apdu_len] : NULL);
                    if (len < 0) return BACNET_STATUS_ERROR;
                    apdu_len += len;
                }
            } else {
                if (rpdata->array_index <= BACNET_WEEKLY_SCHEDULE_SIZE) {
                    apdu_len = Schedule_Weekly_Schedule_Encode(
                        rpdata->object_instance,
                        rpdata->array_index - 1, apdu);
                } else {
                    rpdata->error_class = ERROR_CLASS_PROPERTY;
                    rpdata->error_code  = ERROR_CODE_INVALID_ARRAY_INDEX;
                    apdu_len = BACNET_STATUS_ERROR;
                }
            }
            break;

#if BACNET_EXCEPTION_SCHEDULE_SIZE > 0
        case PROP_EXCEPTION_SCHEDULE:
            if (rpdata->array_index == 0) {
                apdu_len = encode_application_unsigned(apdu,
                    CurrentSC->Exception_Count);
            } else if (rpdata->array_index == BACNET_ARRAY_ALL) {
                unsigned i;
                for (i = 0; i < CurrentSC->Exception_Count; i++) {
                    len = bacnet_special_event_encode(
                        apdu ? &apdu[apdu_len] : NULL,
                        &CurrentSC->Exception_Schedule[i]);
                    if (len < 0) return BACNET_STATUS_ERROR;
                    apdu_len += len;
                }
            } else {
                uint32_t idx = rpdata->array_index - 1;
                if (idx < CurrentSC->Exception_Count) {
                    apdu_len = bacnet_special_event_encode(apdu,
                        &CurrentSC->Exception_Schedule[idx]);
                } else {
                    rpdata->error_class = ERROR_CLASS_PROPERTY;
                    rpdata->error_code  = ERROR_CODE_INVALID_ARRAY_INDEX;
                    apdu_len = BACNET_STATUS_ERROR;
                }
            }
            break;
#endif

        case PROP_SCHEDULE_DEFAULT:
            apdu_len = bacapp_encode_application_data(apdu,
                &CurrentSC->Schedule_Default);
            break;

        case PROP_PRIORITY_FOR_WRITING:
            apdu_len = encode_application_unsigned(apdu,
                CurrentSC->Priority_For_Writing);
            break;

        case PROP_STATUS_FLAGS:
            bitstring_init(&bit_string);
            bitstring_set_bit(&bit_string, STATUS_FLAG_IN_ALARM,      false);
            bitstring_set_bit(&bit_string, STATUS_FLAG_FAULT,         false);
            bitstring_set_bit(&bit_string, STATUS_FLAG_OVERRIDDEN,    false);
            bitstring_set_bit(&bit_string, STATUS_FLAG_OUT_OF_SERVICE,
                CurrentSC->Out_Of_Service);
            apdu_len = encode_application_bitstring(apdu, &bit_string);
            break;

        case PROP_RELIABILITY:
            apdu_len = encode_application_enumerated(apdu,
                RELIABILITY_NO_FAULT_DETECTED);
            break;

        case PROP_OUT_OF_SERVICE:
            apdu_len = encode_application_boolean(apdu,
                CurrentSC->Out_Of_Service);
            break;

        case PROP_LIST_OF_OBJECT_PROPERTY_REFERENCES:
            break;

        default:
            rpdata->error_class = ERROR_CLASS_PROPERTY;
            rpdata->error_code  = ERROR_CODE_UNKNOWN_PROPERTY;
            apdu_len = BACNET_STATUS_ERROR;
            break;
    }
    return apdu_len;
}

bool Schedule_Write_Property(BACNET_WRITE_PROPERTY_DATA *wp_data)
{
    SCHEDULE_DESCR *CurrentSC = Schedule_Object(wp_data->object_instance);
    bool status = false;
    int len = 0;
    BACNET_APPLICATION_DATA_VALUE value;

    if (!CurrentSC) return false;

    len = bacapp_decode_application_data(
        wp_data->application_data,
        wp_data->application_data_len,
        &value);

    switch ((int)wp_data->object_property) {

        case PROP_PRESENT_VALUE:
            if (CurrentSC->Out_Of_Service) {
                CurrentSC->Present_Value = value;
                status = true;
            } else {
                wp_data->error_class = ERROR_CLASS_PROPERTY;
                wp_data->error_code  = ERROR_CODE_WRITE_ACCESS_DENIED;
            }
            break;

        case PROP_OUT_OF_SERVICE:
            if (value.tag == BACNET_APPLICATION_TAG_BOOLEAN) {
                CurrentSC->Out_Of_Service = value.type.Boolean;
                status = true;
            }
            break;

        case PROP_WEEKLY_SCHEDULE: {
            if (wp_data->array_index == BACNET_ARRAY_ALL) {
                int offset = 0;
                int day;
                for (day = 0; day < BACNET_WEEKLY_SCHEDULE_SIZE; day++) {
                    int decode_len = bacnet_dailyschedule_context_decode(
                        wp_data->application_data + offset,
                        wp_data->application_data_len - offset,
                        0, &CurrentSC->Weekly_Schedule[day]);
                    if (decode_len <= 0) break;
                    offset += decode_len;
                }
                status = true;
                /* Sauvegarde en FRAM */
                sched_persist_save_weekly(wp_data->object_instance, CurrentSC);
            }
            break;
        }

#if BACNET_EXCEPTION_SCHEDULE_SIZE > 0
        case PROP_EXCEPTION_SCHEDULE: {
            if (wp_data->array_index == BACNET_ARRAY_ALL) {
                int offset = 0;
                int count = 0;

                memset(CurrentSC->Exception_Schedule, 0,
                    sizeof(CurrentSC->Exception_Schedule));

                while (offset < (int)wp_data->application_data_len &&
                       count < BACNET_EXCEPTION_SCHEDULE_SIZE) {

                    int decode_len = bacnet_special_event_decode(
                        wp_data->application_data + offset,
                        wp_data->application_data_len - offset,
                        &CurrentSC->Exception_Schedule[count]);

                    if (decode_len <= 0) break;

                    offset += decode_len;
                    count++;
                }
                CurrentSC->Exception_Count = (uint8_t)count;
                status = true;
                /* Sauvegarde en FRAM */
                sched_persist_save_special_events(
                    wp_data->object_instance, CurrentSC);
            }
            break;
        }
#endif

        case PROP_SCHEDULE_DEFAULT:
            if (len > 0) {
                CurrentSC->Schedule_Default = value;
                status = true;
            }
            break;

        default:
            wp_data->error_class = ERROR_CLASS_PROPERTY;
            wp_data->error_code  = ERROR_CODE_UNKNOWN_PROPERTY;
            break;
    }

    if (status && !CurrentSC->Out_Of_Service) {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        BACNET_TIME btime;

        btime.hour      = (uint8_t)t->tm_hour;
        btime.min       = (uint8_t)t->tm_min;
        btime.sec       = (uint8_t)t->tm_sec;
        btime.hundredths = 0;

        uint8_t bacnet_wday = (t->tm_wday == 0) ? 7 : (uint8_t)t->tm_wday;

        printf("[WRITE] Recalcul immediat suite a mise a jour...\n");
        Schedule_Recalculate_PV(CurrentSC, (BACNET_WEEKDAY)bacnet_wday, &btime);
    }

    return status;
}

/* ================================================================
 * calendar_entry_matches
 * Vérifie si la date système correspond à un CalendarEntry BACnet.
 *
 * IMPORTANT - convention d'année BACnet :
 *   Le champ BACNET_DATE.year stocke (année - 1900).
 *   Ex : 2026 → year = 126.
 *   La valeur 255 signifie "toute année" (wildcard).
 *
 * On compare donc edate->year avec t->tm_year (qui vaut lui aussi
 * année - 1900, soit 126 pour 2026). Les deux sont dans le même
 * référentiel → pas de +1900 nécessaire.
 * ================================================================ */
static bool calendar_entry_matches(const BACNET_CALENDAR_ENTRY *entry,
    struct tm *t)
{
    if (!entry || !t) return false;

    switch (entry->tag) {

        case BACNET_CALENDAR_DATE: {
            const BACNET_DATE *d = &entry->type.Date;
            int year_ok  = (d->year == 255) ||
                           ((int)d->year == t->tm_year) ||
                           ((int)d->year == (t->tm_year + 1900));
            int month_ok = (d->month == 255) || ((int)d->month == (t->tm_mon + 1));
            int day_ok   = (d->day == 255)   || ((int)d->day   == t->tm_mday);
            return (year_ok && month_ok && day_ok);
        }

        case BACNET_CALENDAR_DATE_RANGE: {
            const BACNET_DATE *start = &entry->type.DateRange.startdate;
            const BACNET_DATE *end   = &entry->type.DateRange.enddate;

            int cur = (t->tm_year + 1900) * 10000 +
                      (t->tm_mon + 1) * 100 +
                      t->tm_mday;

            int s_year = (start->year == 255) ?
                         (t->tm_year + 1900) : (int)(start->year + 1900);
            int e_year = (end->year == 255) ?
                         (t->tm_year + 1900) : (int)(end->year + 1900);

            int s = s_year * 10000 + start->month * 100 + start->day;
            int e = e_year * 10000 + end->month   * 100 + end->day;

            return (cur >= s && cur <= e);
        }

        case BACNET_CALENDAR_WEEK_N_DAY: {
            const BACNET_WEEKNDAY *w = &entry->type.WeekNDay;
            int month_ok = (w->month == 255) ||
                           ((int)w->month == (t->tm_mon + 1));
            uint8_t bac_dow = (t->tm_wday == 0) ? 7 : (uint8_t)t->tm_wday;
            int day_ok = (w->dayofweek == 255) ||
                         (w->dayofweek == bac_dow);
            return (month_ok && day_ok);
        }

        default:
            return false;
    }
}

/* ================================================================
 * Schedule_Recalculate_PV
 * Priorité : Special Event > Weekly Schedule > Schedule_Default
 * ================================================================ */
/* ================================================================
 * Schedule_Recalculate_PV
 * Priorité : Special Event > Weekly Schedule > Schedule_Default
 *
 * CORRECTION : parcourt TOUS les Special Events qui matchent la date
 * et garde la valeur associée à l'entrée horaire la plus récente
 * parmi l'ensemble — même si plusieurs SE ont la même date.
 * ================================================================ */
void Schedule_Recalculate_PV(
    SCHEDULE_DESCR *desc,
    BACNET_WEEKDAY wday,
    const BACNET_TIME *btime)
{
    int i, j;
    bool se_found  = false;
    bool wk_found  = false;
    BACNET_TIME se_best_t = {0, 0, 0, 0};
    BACNET_APPLICATION_DATA_VALUE old_value = desc->Present_Value;
    time_t now_t = time(NULL);
    struct tm *t = localtime(&now_t);

    if (!desc || desc->Out_Of_Service) return;

    /* 1. Valeur par défaut */
    desc->Present_Value = desc->Schedule_Default;

#if BACNET_EXCEPTION_SCHEDULE_SIZE > 0
    /* 2. Parcourir TOUS les Special Events */
    for (i = 0; i < (int)desc->Exception_Count; i++) {
        BACNET_SPECIAL_EVENT *se = &desc->Exception_Schedule[i];
        bool date_match = false;

        if (se->periodTag == BACNET_SPECIAL_EVENT_PERIOD_CALENDAR_ENTRY) {
            date_match = calendar_entry_matches(&se->period.calendarEntry, t);
        }

        if (!date_match) continue;

        for (j = 0; j < (int)se->timeValues.TV_Count; j++) {
            BACNET_TIME *et = &se->timeValues.Time_Values[j].Time;

            if (datetime_compare_time((BACNET_TIME *)btime, et) < 0) continue;

            if (!se_found ||
                datetime_compare_time(et, &se_best_t) > 0) {
                se_best_t = *et;
                desc->Present_Value = se->timeValues.Time_Values[j].Value;
                se_found = true;
            }
        }
    }

    if (se_found) {
        if (desc->Present_Value.type.Real != old_value.type.Real) {
            ESP_LOGI("schedule", "SE → %.1f", desc->Present_Value.type.Real);
        }
        return;
    }
#endif

    /* 3. Weekly Schedule */
    if (wday >= 1 && wday <= 7) {
        int day_idx = wday - 1;
        BACNET_TIME wk_best_t = {0, 0, 0, 0};

        for (i = 0; i < (int)desc->Weekly_Schedule[day_idx].TV_Count; i++) {
            BACNET_TIME *et = &desc->Weekly_Schedule[day_idx].Time_Values[i].Time;
            if (datetime_compare_time((BACNET_TIME *)btime, et) >= 0) {
                if (!wk_found ||
                    datetime_compare_time(et, &wk_best_t) > 0) {
                    wk_best_t = *et;
                    desc->Present_Value =
                        desc->Weekly_Schedule[day_idx].Time_Values[i].Value;
                    wk_found = true;
                }
            }
        }
    }

    if (desc->Present_Value.type.Real != old_value.type.Real) {
        ESP_LOGI("schedule", "WK → %.1f", desc->Present_Value.type.Real);
    }

    (void)wk_found;
}