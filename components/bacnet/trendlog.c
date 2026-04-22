/**
 * @file trendlog.c
 * @brief Trend Log object - adapté pour ESP32-S3 EdgeBox éclairage public
 *
 * Modifications par rapport à la stack originale :
 *  - Includes adaptés au projet (pas de "bacnet/..." mais "bacdef.h" etc.)
 *  - MAX_TREND_LOGS = 2  (TL0 → AV0, TL1 → AV1)
 *  - TL_MAX_ENTRIES = 100 (économie RAM)
 *  - Trend_Log_Init() : pas de données de test, config réelle AV0/AV1
 *  - Device_getCurrentDateTime() : disponible dans device.c du projet
 *  - BACNET_STACK_EXPORT supprimé (non défini dans ce projet)
 *  - bacnet_tag_decode / bacnet_*_decode remplacés par decode_* du projet
 *  - local_read_property() utilise Device_Read_Property() du projet
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "bacdef.h"
#include "bacdcode.h"
#include "bacapp.h"
#include "bacenum.h"
#include "apdu.h"
#include "datetime.h"
#include "wp.h"
#include "device.h"
#include "trendlog.h"
#include <time.h>
#include "handlers.h"

/* ---------------------------------------------------------------
 * Stockage statique — 2 logs × 100 entrées
 * Calcul RAM : 2 × 100 × sizeof(TL_DATA_REC) ≈ 2 × 100 × 16 = 3.2 KB
 * --------------------------------------------------------------- */
static TL_DATA_REC  Logs[MAX_TREND_LOGS][TL_MAX_ENTRIES];
static TL_LOG_INFO  LogInfo[MAX_TREND_LOGS];

/* ---------------------------------------------------------------
 * Listes de propriétés (ReadPropertyMultiple)
 * --------------------------------------------------------------- */
static const int Trend_Log_Properties_Required[] = {
    PROP_OBJECT_IDENTIFIER,
    PROP_OBJECT_NAME,
    PROP_OBJECT_TYPE,
    PROP_ENABLE,
    PROP_STOP_WHEN_FULL,
    PROP_BUFFER_SIZE,
    PROP_LOG_BUFFER,
    PROP_RECORD_COUNT,
    PROP_TOTAL_RECORD_COUNT,
    PROP_EVENT_STATE,
    PROP_LOGGING_TYPE,
    PROP_STATUS_FLAGS,
    -1
};

static const int Trend_Log_Properties_Optional[] = {
    PROP_DESCRIPTION,
    PROP_START_TIME,
    PROP_STOP_TIME,
    PROP_LOG_DEVICE_OBJECT_PROPERTY,
    PROP_LOG_INTERVAL,
    PROP_ALIGN_INTERVALS,
    PROP_INTERVAL_OFFSET,
    PROP_TRIGGER,
    -1
};

static const int Trend_Log_Properties_Proprietary[] = { -1 };

void Trend_Log_Property_Lists(
    const int **pRequired,
    const int **pOptional,
    const int **pProprietary)
{
    if (pRequired)    *pRequired    = Trend_Log_Properties_Required;
    if (pOptional)    *pOptional    = Trend_Log_Properties_Optional;
    if (pProprietary) *pProprietary = Trend_Log_Properties_Proprietary;
}

/* ---------------------------------------------------------------
 * Helpers instance
 * --------------------------------------------------------------- */
bool Trend_Log_Valid_Instance(uint32_t object_instance)
{
    return (object_instance < MAX_TREND_LOGS);
}

unsigned Trend_Log_Count(void)
{
    return MAX_TREND_LOGS;
}

uint32_t Trend_Log_Index_To_Instance(unsigned index)
{
    return index;
}

unsigned Trend_Log_Instance_To_Index(uint32_t object_instance)
{
    if (object_instance < MAX_TREND_LOGS)
        return object_instance;
    return MAX_TREND_LOGS; /* invalide */
}

/* ---------------------------------------------------------------
 * Heure courante via Device_getCurrentDateTime() du projet
 * --------------------------------------------------------------- */
static bacnet_time_t Trend_Log_Epoch_Seconds_Now(void)
{
    time_t now = time(NULL);
    return (bacnet_time_t)now;
}

/* ---------------------------------------------------------------
 * Initialisation
 * --------------------------------------------------------------- */
void Trend_Log_Init(void)
{
    static bool initialized = false;
    int i;

    if (initialized) return;
    initialized = true;

    for (i = 0; i < MAX_TREND_LOGS; i++) {
        memset(&Logs[i][0],   0, sizeof(Logs[i]));
        memset(&LogInfo[i],   0, sizeof(TL_LOG_INFO));

        LogInfo[i].bEnable          = true;
        LogInfo[i].bStopWhenFull    = false;
        LogInfo[i].bAlignIntervals  = false;
        LogInfo[i].bTrigger         = false;
        LogInfo[i].LoggingType      = LOGGING_TYPE_POLLED;
        LogInfo[i].ulLogInterval    = 60;
        LogInfo[i].ulIntervalOffset = 0;
        LogInfo[i].iIndex           = 0;
        LogInfo[i].ulRecordCount    = 0;
        LogInfo[i].ulTotalRecordCount = 0;
        LogInfo[i].tLastDataTime    = 0;

        LogInfo[i].ucTimeFlags = TL_T_START_WILD | TL_T_STOP_WILD;
        LogInfo[i].tStartTime  = 0;
        LogInfo[i].tStopTime   = 0xFFFFFFFF;

        /* Correction : deviceIndentifier (typo dans la stack) */
        LogInfo[i].Source.deviceIndentifier.type     = OBJECT_DEVICE;
        LogInfo[i].Source.deviceIndentifier.instance = Device_Object_Instance_Number();
        LogInfo[i].Source.objectIdentifier.type      = OBJECT_ANALOG_VALUE;
        LogInfo[i].Source.objectIdentifier.instance  = i;
        LogInfo[i].Source.propertyIdentifier         = PROP_PRESENT_VALUE;
        LogInfo[i].Source.arrayIndex                 = BACNET_ARRAY_ALL;

        datetime_wildcard_set(&LogInfo[i].StartTime);
        datetime_wildcard_set(&LogInfo[i].StopTime);
    }

    printf("Trend_Log_Init: TL0→AV0, TL1→AV1, interval=60s, entries_max=%d\n",
           TL_MAX_ENTRIES);
}
/* ---------------------------------------------------------------
 * Nom de l'objet
 * --------------------------------------------------------------- */
bool Trend_Log_Object_Name(
    uint32_t object_instance,
    BACNET_CHARACTER_STRING *object_name)
{
    char text[32] = "";
    if (object_instance < MAX_TREND_LOGS) {
        snprintf(text, sizeof(text), "Trend Log %lu",
                 (unsigned long)object_instance);
        return characterstring_init_ansi(object_name, text);
    }
    return false;
}

/* ---------------------------------------------------------------
 * Read Property
 * --------------------------------------------------------------- */
int Trend_Log_Read_Property(BACNET_READ_PROPERTY_DATA *rpdata)
{
    int apdu_len = 0;
    int len = 0;
    BACNET_BIT_STRING bit_string;
    BACNET_CHARACTER_STRING char_string;
    TL_LOG_INFO *CurrentLog;
    uint8_t *apdu = NULL;
    int log_index;

    if (!rpdata || !rpdata->application_data || rpdata->application_data_len == 0)
        return 0;

    apdu = rpdata->application_data;
    log_index = Trend_Log_Instance_To_Index(rpdata->object_instance);
    if (log_index >= MAX_TREND_LOGS) {
        rpdata->error_class = ERROR_CLASS_OBJECT;
        rpdata->error_code  = ERROR_CODE_UNKNOWN_OBJECT;
        return BACNET_STATUS_ERROR;
    }
    CurrentLog = &LogInfo[log_index];

    switch (rpdata->object_property) {
        case PROP_OBJECT_IDENTIFIER:
            apdu_len = encode_application_object_id(
                &apdu[0], OBJECT_TRENDLOG, rpdata->object_instance);
            break;

        case PROP_DESCRIPTION:
        case PROP_OBJECT_NAME:
            Trend_Log_Object_Name(rpdata->object_instance, &char_string);
            apdu_len = encode_application_character_string(&apdu[0], &char_string);
            break;

        case PROP_OBJECT_TYPE:
            apdu_len = encode_application_enumerated(&apdu[0], OBJECT_TRENDLOG);
            break;

        case PROP_ENABLE:
            apdu_len = encode_application_boolean(&apdu[0], CurrentLog->bEnable);
            break;

        case PROP_STOP_WHEN_FULL:
            apdu_len = encode_application_boolean(&apdu[0], CurrentLog->bStopWhenFull);
            break;

        case PROP_BUFFER_SIZE:
            apdu_len = encode_application_unsigned(&apdu[0], TL_MAX_ENTRIES);
            break;

        case PROP_LOG_BUFFER:
            /* Lecture uniquement via ReadRange */
            rpdata->error_class = ERROR_CLASS_PROPERTY;
            rpdata->error_code  = ERROR_CODE_READ_ACCESS_DENIED;
            apdu_len = BACNET_STATUS_ERROR;
            break;

        case PROP_RECORD_COUNT:
            apdu_len = encode_application_unsigned(&apdu[0], CurrentLog->ulRecordCount);
            break;

        case PROP_TOTAL_RECORD_COUNT:
            apdu_len = encode_application_unsigned(&apdu[0], CurrentLog->ulTotalRecordCount);
            break;

        case PROP_EVENT_STATE:
            apdu_len = encode_application_enumerated(&apdu[0], EVENT_STATE_NORMAL);
            break;

        case PROP_LOGGING_TYPE:
            apdu_len = encode_application_enumerated(&apdu[0], CurrentLog->LoggingType);
            break;

        case PROP_STATUS_FLAGS:
            bitstring_init(&bit_string);
            bitstring_set_bit(&bit_string, STATUS_FLAG_IN_ALARM,       false);
            bitstring_set_bit(&bit_string, STATUS_FLAG_FAULT,          false);
            bitstring_set_bit(&bit_string, STATUS_FLAG_OVERRIDDEN,     false);
            bitstring_set_bit(&bit_string, STATUS_FLAG_OUT_OF_SERVICE, false);
            apdu_len = encode_application_bitstring(&apdu[0], &bit_string);
            break;

        case PROP_START_TIME:
            len      = encode_application_date(&apdu[0], &CurrentLog->StartTime.date);
            apdu_len = len;
            apdu_len += encode_application_time(&apdu[apdu_len], &CurrentLog->StartTime.time);
            break;

        case PROP_STOP_TIME:
            len      = encode_application_date(&apdu[0], &CurrentLog->StopTime.date);
            apdu_len = len;
            apdu_len += encode_application_time(&apdu[apdu_len], &CurrentLog->StopTime.time);
            break;

        case PROP_LOG_DEVICE_OBJECT_PROPERTY:
            apdu_len = bacapp_encode_device_obj_property_ref(&apdu[0], &CurrentLog->Source);
            break;

        case PROP_LOG_INTERVAL:
            /* BACnet exprime l'intervalle en centièmes de seconde */
            apdu_len = encode_application_unsigned(&apdu[0], CurrentLog->ulLogInterval * 100);
            break;

        case PROP_ALIGN_INTERVALS:
            apdu_len = encode_application_boolean(&apdu[0], CurrentLog->bAlignIntervals);
            break;

        case PROP_INTERVAL_OFFSET:
            apdu_len = encode_application_unsigned(&apdu[0], CurrentLog->ulIntervalOffset * 100);
            break;

        case PROP_TRIGGER:
            apdu_len = encode_application_boolean(&apdu[0], CurrentLog->bTrigger);
            break;

        default:
            rpdata->error_class = ERROR_CLASS_PROPERTY;
            rpdata->error_code  = ERROR_CODE_UNKNOWN_PROPERTY;
            apdu_len = BACNET_STATUS_ERROR;
            break;
    }

    return apdu_len;
}

/* ---------------------------------------------------------------
 * Write Property
 * --------------------------------------------------------------- */
bool Trend_Log_Write_Property(BACNET_WRITE_PROPERTY_DATA *wp_data)
{
    bool status = false;
    int len = 0;
    BACNET_APPLICATION_DATA_VALUE value = { 0 };
    TL_LOG_INFO *CurrentLog;
    BACNET_DATE start_date, stop_date;
    BACNET_DEVICE_OBJECT_PROPERTY_REFERENCE TempSource;
    bool bEffectiveEnable;
    int log_index;

    log_index = Trend_Log_Instance_To_Index(wp_data->object_instance);
    if (log_index >= MAX_TREND_LOGS) {
        wp_data->error_class = ERROR_CLASS_OBJECT;
        wp_data->error_code  = ERROR_CODE_UNKNOWN_OBJECT;
        return false;
    }
    CurrentLog = &LogInfo[log_index];

    len = bacapp_decode_application_data(
        wp_data->application_data, wp_data->application_data_len, &value);
    if (len < 0) {
        wp_data->error_class = ERROR_CLASS_PROPERTY;
        wp_data->error_code  = ERROR_CODE_VALUE_OUT_OF_RANGE;
        return false;
    }

    switch (wp_data->object_property) {
        case PROP_ENABLE:
            status = WPValidateArgType(&value, BACNET_APPLICATION_TAG_BOOLEAN,
                                       &wp_data->error_class, &wp_data->error_code);
            if (status) {
                if ((CurrentLog->bEnable == false) &&
                    (CurrentLog->bStopWhenFull == true) &&
                    (CurrentLog->ulRecordCount == TL_MAX_ENTRIES) &&
                    (value.type.Boolean == true)) {
                    status = false;
                    wp_data->error_class = ERROR_CLASS_OBJECT;
                    wp_data->error_code  = ERROR_CODE_LOG_BUFFER_FULL;
                    break;
                }
                if (CurrentLog->bEnable != value.type.Boolean) {
                    bEffectiveEnable = TL_Is_Enabled(log_index);
                    CurrentLog->bEnable = value.type.Boolean;
                    if (!value.type.Boolean) {
                        if (bEffectiveEnable)
                            TL_Insert_Status_Rec(log_index, LOG_STATUS_LOG_DISABLED, true);
                    } else {
                        if (TL_Is_Enabled(log_index))
                            TL_Insert_Status_Rec(log_index, LOG_STATUS_LOG_DISABLED, false);
                    }
                }
            }
            break;

        case PROP_STOP_WHEN_FULL:
            status = WPValidateArgType(&value, BACNET_APPLICATION_TAG_BOOLEAN,
                                       &wp_data->error_class, &wp_data->error_code);
            if (status) {
                if (CurrentLog->bStopWhenFull != value.type.Boolean) {
                    CurrentLog->bStopWhenFull = value.type.Boolean;
                    if (value.type.Boolean &&
                        (CurrentLog->ulRecordCount == TL_MAX_ENTRIES) &&
                        CurrentLog->bEnable) {
                        CurrentLog->bEnable = false;
                        TL_Insert_Status_Rec(log_index, LOG_STATUS_LOG_DISABLED, true);
                    }
                }
            }
            break;

        case PROP_BUFFER_SIZE:
            wp_data->error_class = ERROR_CLASS_PROPERTY;
            wp_data->error_code  = ERROR_CODE_WRITE_ACCESS_DENIED;
            break;

        case PROP_RECORD_COUNT:
            status = WPValidateArgType(&value, BACNET_APPLICATION_TAG_UNSIGNED_INT,
                                       &wp_data->error_class, &wp_data->error_code);
            if (status && value.type.Unsigned_Int == 0) {
                CurrentLog->ulRecordCount = 0;
                CurrentLog->iIndex = 0;
                TL_Insert_Status_Rec(log_index, LOG_STATUS_BUFFER_PURGED, true);
            }
            break;

        case PROP_LOGGING_TYPE:
            status = WPValidateArgType(&value, BACNET_APPLICATION_TAG_ENUMERATED,
                                       &wp_data->error_class, &wp_data->error_code);
            if (status) {
                if (value.type.Enumerated != LOGGING_TYPE_COV) {
                    CurrentLog->LoggingType = (BACNET_LOGGING_TYPE)value.type.Enumerated;
                    if (value.type.Enumerated == LOGGING_TYPE_POLLED &&
                        CurrentLog->ulLogInterval == 0)
                        CurrentLog->ulLogInterval = 60;
                    if (value.type.Enumerated == LOGGING_TYPE_TRIGGERED)
                        CurrentLog->ulLogInterval = 0;
                } else {
                    status = false;
                    wp_data->error_class = ERROR_CLASS_PROPERTY;
                    wp_data->error_code  = ERROR_CODE_OPTIONAL_FUNCTIONALITY_NOT_SUPPORTED;
                }
            }
            break;

        case PROP_START_TIME:
            status = WPValidateArgType(&value, BACNET_APPLICATION_TAG_DATE,
                                       &wp_data->error_class, &wp_data->error_code);
            if (!status) break;
            start_date = value.type.Date;
            len = bacapp_decode_application_data(
                wp_data->application_data + len,
                wp_data->application_data_len - len, &value);
            if (len) {
                status = WPValidateArgType(&value, BACNET_APPLICATION_TAG_TIME,
                                           &wp_data->error_class, &wp_data->error_code);
                if (!status) break;
                bEffectiveEnable = TL_Is_Enabled(log_index);
                CurrentLog->StartTime.date = start_date;
                CurrentLog->StartTime.time = value.type.Time;
                if (datetime_wildcard_present(&CurrentLog->StartTime)) {
                    CurrentLog->ucTimeFlags |= TL_T_START_WILD;
                    CurrentLog->tStartTime = 0;
                } else {
                    CurrentLog->ucTimeFlags &= ~TL_T_START_WILD;
                    CurrentLog->tStartTime = TL_BAC_Time_To_Local(&CurrentLog->StartTime);
                }
                if (bEffectiveEnable != TL_Is_Enabled(log_index))
                    TL_Insert_Status_Rec(log_index, LOG_STATUS_LOG_DISABLED, bEffectiveEnable);
            }
            break;

        case PROP_STOP_TIME:
            status = WPValidateArgType(&value, BACNET_APPLICATION_TAG_DATE,
                                       &wp_data->error_class, &wp_data->error_code);
            if (!status) break;
            stop_date = value.type.Date;
            len = bacapp_decode_application_data(
                wp_data->application_data + len,
                wp_data->application_data_len - len, &value);
            if (len) {
                status = WPValidateArgType(&value, BACNET_APPLICATION_TAG_TIME,
                                           &wp_data->error_class, &wp_data->error_code);
                if (!status) break;
                bEffectiveEnable = TL_Is_Enabled(log_index);
                CurrentLog->StopTime.date = stop_date;
                CurrentLog->StopTime.time = value.type.Time;
                if (datetime_wildcard_present(&CurrentLog->StopTime)) {
                    CurrentLog->ucTimeFlags |= TL_T_STOP_WILD;
                    CurrentLog->tStopTime = 0xFFFFFFFF;
                } else {
                    CurrentLog->ucTimeFlags &= ~TL_T_STOP_WILD;
                    CurrentLog->tStopTime = TL_BAC_Time_To_Local(&CurrentLog->StopTime);
                }
                if (bEffectiveEnable != TL_Is_Enabled(log_index))
                    TL_Insert_Status_Rec(log_index, LOG_STATUS_LOG_DISABLED, bEffectiveEnable);
            }
            break;

        case PROP_LOG_DEVICE_OBJECT_PROPERTY:
            /* Correction : bacapp_decode_device_obj_property_ref */
            len = bacapp_decode_device_obj_property_ref(
                wp_data->application_data, &TempSource);
            if (len <= 0) {
                wp_data->error_class = ERROR_CLASS_PROPERTY;
                wp_data->error_code  = ERROR_CODE_OTHER;
                break;
            }
            /* Correction : deviceIndentifier (typo dans la stack) */
            if ((TempSource.deviceIndentifier.type == OBJECT_DEVICE) &&
                (TempSource.deviceIndentifier.instance != Device_Object_Instance_Number())) {
                wp_data->error_class = ERROR_CLASS_PROPERTY;
                wp_data->error_code  = ERROR_CODE_OPTIONAL_FUNCTIONALITY_NOT_SUPPORTED;
                break;
            }
            if (memcmp(&TempSource, &CurrentLog->Source,
                       sizeof(BACNET_DEVICE_OBJECT_PROPERTY_REFERENCE)) != 0) {
                CurrentLog->ulRecordCount = 0;
                CurrentLog->iIndex = 0;
                TL_Insert_Status_Rec(log_index, LOG_STATUS_BUFFER_PURGED, true);
            }
            CurrentLog->Source = TempSource;
            status = true;
            break;

        case PROP_LOG_INTERVAL:
            if (CurrentLog->LoggingType == LOGGING_TYPE_TRIGGERED) {
                wp_data->error_class = ERROR_CLASS_PROPERTY;
                wp_data->error_code  = ERROR_CODE_WRITE_ACCESS_DENIED;
                break;
            }
            status = WPValidateArgType(&value, BACNET_APPLICATION_TAG_UNSIGNED_INT,
                                       &wp_data->error_class, &wp_data->error_code);
            if (status) {
                CurrentLog->ulLogInterval = value.type.Unsigned_Int / 100;
                if (CurrentLog->ulLogInterval == 0)
                    CurrentLog->ulLogInterval = 1;
            }
            break;

        case PROP_ALIGN_INTERVALS:
            status = WPValidateArgType(&value, BACNET_APPLICATION_TAG_BOOLEAN,
                                       &wp_data->error_class, &wp_data->error_code);
            if (status) CurrentLog->bAlignIntervals = value.type.Boolean;
            break;

        case PROP_INTERVAL_OFFSET:
            status = WPValidateArgType(&value, BACNET_APPLICATION_TAG_UNSIGNED_INT,
                                       &wp_data->error_class, &wp_data->error_code);
            if (status)
                CurrentLog->ulIntervalOffset = value.type.Unsigned_Int / 100;
            break;

        case PROP_TRIGGER:
            status = WPValidateArgType(&value, BACNET_APPLICATION_TAG_BOOLEAN,
                                       &wp_data->error_class, &wp_data->error_code);
            if (status) {
                if ((CurrentLog->LoggingType == LOGGING_TYPE_POLLED) &&
                    CurrentLog->bAlignIntervals) {
                    wp_data->error_class = ERROR_CLASS_PROPERTY;
                    wp_data->error_code  = ERROR_CODE_NOT_CONFIGURED_FOR_TRIGGERED_LOGGING;
                    status = false;
                } else {
                    CurrentLog->bTrigger = value.type.Boolean;
                }
            }
            break;

        default:
            wp_data->error_class = ERROR_CLASS_PROPERTY;
            wp_data->error_code  = ERROR_CODE_WRITE_ACCESS_DENIED;
            break;
    }

    return status;
}

/* ---------------------------------------------------------------
 * ReadRange glue
 * --------------------------------------------------------------- */
bool TrendLogGetRRInfo(BACNET_READ_RANGE_DATA *pRequest, RR_PROP_INFO *pInfo)
{
    int log_index = Trend_Log_Instance_To_Index(pRequest->object_instance);
    if (log_index >= MAX_TREND_LOGS) {
        pRequest->error_class = ERROR_CLASS_OBJECT;
        pRequest->error_code  = ERROR_CODE_UNKNOWN_OBJECT;
        return false;
    }
    if (pRequest->object_property == PROP_LOG_BUFFER) {
        pInfo->RequestTypes = RR_BY_POSITION | RR_BY_TIME | RR_BY_SEQUENCE;
        pInfo->Handler = rr_trend_log_encode;
        return true;
    }
    pRequest->error_class = ERROR_CLASS_SERVICES;
    pRequest->error_code  = ERROR_CODE_PROPERTY_IS_NOT_A_LIST;
    return false;
}

/* ---------------------------------------------------------------
 * Insertion d'un enregistrement de statut
 * --------------------------------------------------------------- */
void TL_Insert_Status_Rec(int iLog, BACNET_LOG_STATUS eStatus, bool bState)
{
    TL_LOG_INFO *CurrentLog = &LogInfo[iLog];
    TL_DATA_REC TempRec;

    TempRec.tTimeStamp       = Trend_Log_Epoch_Seconds_Now();
    TempRec.ucRecType        = TL_TYPE_STATUS;
    TempRec.ucStatus         = 0;
    TempRec.Datum.ucLogStatus = 0;

    switch (eStatus) {
        case LOG_STATUS_LOG_DISABLED:
            if (bState) TempRec.Datum.ucLogStatus = 1 << LOG_STATUS_LOG_DISABLED;
            break;
        case LOG_STATUS_BUFFER_PURGED:
            if (bState) TempRec.Datum.ucLogStatus = 1 << LOG_STATUS_BUFFER_PURGED;
            break;
        case LOG_STATUS_LOG_INTERRUPTED:
            TempRec.Datum.ucLogStatus = 1 << LOG_STATUS_LOG_INTERRUPTED;
            break;
        default:
            break;
    }

    Logs[iLog][CurrentLog->iIndex++] = TempRec;
    if (CurrentLog->iIndex >= TL_MAX_ENTRIES) CurrentLog->iIndex = 0;
    CurrentLog->ulTotalRecordCount++;
    if (CurrentLog->ulRecordCount < TL_MAX_ENTRIES) CurrentLog->ulRecordCount++;
}

/* ---------------------------------------------------------------
 * Vérifie si le log est actuellement actif
 * --------------------------------------------------------------- */
bool TL_Is_Enabled(int iLog)
{
    TL_LOG_INFO *CurrentLog = &LogInfo[iLog];
    bacnet_time_t tNow;

    if (!CurrentLog->bEnable) return false;

    /* Les deux wildcards → toujours actif */
    if (CurrentLog->ucTimeFlags == (TL_T_START_WILD | TL_T_STOP_WILD))
        return true;

    if ((CurrentLog->ucTimeFlags == 0) &&
        (CurrentLog->tStopTime < CurrentLog->tStartTime))
        return false;

    tNow = Trend_Log_Epoch_Seconds_Now();

    if (CurrentLog->ucTimeFlags & TL_T_START_WILD)
        return (tNow <= CurrentLog->tStopTime);

    if (CurrentLog->ucTimeFlags & TL_T_STOP_WILD)
        return (tNow >= CurrentLog->tStartTime);

    return (tNow >= CurrentLog->tStartTime && tNow <= CurrentLog->tStopTime);
}

/* ---------------------------------------------------------------
 * Conversions temps
 * --------------------------------------------------------------- */
bacnet_time_t TL_BAC_Time_To_Local(const BACNET_DATE_TIME *bdatetime)
{
    struct tm t = {0};
    t.tm_year = bdatetime->date.year - 1900;
    t.tm_mon  = bdatetime->date.month - 1;
    t.tm_mday = bdatetime->date.day;
    t.tm_hour = bdatetime->time.hour;
    t.tm_min  = bdatetime->time.min;
    t.tm_sec  = bdatetime->time.sec;
    t.tm_isdst = -1;
    return (bacnet_time_t)mktime(&t);
}

void TL_Local_Time_To_BAC(BACNET_DATE_TIME *bdatetime, bacnet_time_t seconds)
{
    time_t t = (time_t)seconds;
    struct tm *tm_info = localtime(&t);
    bdatetime->date.year      = (uint16_t)(tm_info->tm_year + 1900);
    bdatetime->date.month     = (uint8_t)(tm_info->tm_mon + 1);
    bdatetime->date.day       = (uint8_t)(tm_info->tm_mday);
    bdatetime->date.wday      = (uint8_t)(tm_info->tm_wday == 0 ? 7 : tm_info->tm_wday);
    bdatetime->time.hour      = (uint8_t)tm_info->tm_hour;
    bdatetime->time.min       = (uint8_t)tm_info->tm_min;
    bdatetime->time.sec       = (uint8_t)tm_info->tm_sec;
    bdatetime->time.hundredths = 0;
}

/* ---------------------------------------------------------------
 * Lecture locale de la valeur surveillée (AV0 ou AV1)
 * Utilise Device_Read_Property() disponible dans device.c
 * --------------------------------------------------------------- */
static int local_read_property(
    uint8_t *value_buf,
    uint8_t *status_buf,
    const BACNET_DEVICE_OBJECT_PROPERTY_REFERENCE *Source,
    BACNET_ERROR_CLASS *error_class,
    BACNET_ERROR_CODE  *error_code)
{
    int len = 0;
    BACNET_READ_PROPERTY_DATA rpdata;

    if (value_buf) {
        rpdata.application_data     = value_buf;
        rpdata.application_data_len = MAX_APDU;
        rpdata.object_type          = Source->objectIdentifier.type;
        rpdata.object_instance      = Source->objectIdentifier.instance;
        rpdata.object_property      = Source->propertyIdentifier;
        rpdata.array_index          = Source->arrayIndex;
        len = Device_Read_Property(&rpdata);
        if (len < 0) {
            *error_class = rpdata.error_class;
            *error_code  = rpdata.error_code;
        }
    }

    if ((len >= 0) && status_buf) {
        rpdata.application_data     = status_buf;
        rpdata.application_data_len = MAX_APDU;
        rpdata.object_property      = PROP_STATUS_FLAGS;
        rpdata.array_index          = BACNET_ARRAY_ALL;
        len = Device_Read_Property(&rpdata);
        if (len < 0) {
            *error_class = rpdata.error_class;
            *error_code  = rpdata.error_code;
        }
    }

    return len;
}

/* ---------------------------------------------------------------
 * Lecture et stockage de la valeur AV surveillée
 * --------------------------------------------------------------- */
static void TL_fetch_property(int iLog)
{
    uint8_t ValueBuf[MAX_APDU];
    uint8_t StatusBuf[4];
    BACNET_ERROR_CLASS error_class = ERROR_CLASS_SERVICES;
    BACNET_ERROR_CODE  error_code  = ERROR_CODE_OTHER;
    int iLen;
    TL_LOG_INFO *CurrentLog;
    TL_DATA_REC TempRec;
    BACNET_BIT_STRING TempBits;
    uint8_t tag_number  = 0;
    uint32_t len_value  = 0;

    CurrentLog = &LogInfo[iLog];
    TempRec.tTimeStamp        = Trend_Log_Epoch_Seconds_Now();
    CurrentLog->tLastDataTime = TempRec.tTimeStamp;
    TempRec.ucStatus          = 0;

    iLen = local_read_property(ValueBuf, StatusBuf,
                               &LogInfo[iLog].Source,
                               &error_class, &error_code);

    if (iLen < 0) {
        TempRec.Datum.Error.usClass = error_class;
        TempRec.Datum.Error.usCode  = error_code;
        TempRec.ucRecType = TL_TYPE_ERROR;
        goto store;
    }

    /* Décode le tag de la valeur retournée */
    iLen = decode_tag_number_and_value(ValueBuf, &tag_number, &len_value);

    switch (tag_number) {
        case BACNET_APPLICATION_TAG_REAL: {
            float fVal = 0.0f;
            decode_real(&ValueBuf[iLen], &fVal);
            TempRec.ucRecType     = TL_TYPE_REAL;
            TempRec.Datum.fReal   = fVal;
            break;
        }
        case BACNET_APPLICATION_TAG_UNSIGNED_INT: {
            uint32_t uVal = 0;
            decode_unsigned(&ValueBuf[iLen], len_value, &uVal);
            TempRec.ucRecType       = TL_TYPE_UNSIGN;
            TempRec.Datum.ulUValue  = uVal;
            break;
        }
        case BACNET_APPLICATION_TAG_NULL:
            TempRec.ucRecType = TL_TYPE_NULL;
            break;
        default:
            TempRec.Datum.Error.usClass = ERROR_CLASS_PROPERTY;
            TempRec.Datum.Error.usCode  = ERROR_CODE_DATATYPE_NOT_SUPPORTED;
            TempRec.ucRecType = TL_TYPE_ERROR;
            break;
    }

    /* Status flags */
    {
        int slen = decode_tag_number_and_value(StatusBuf, &tag_number, &len_value);
        if (slen > 0 && tag_number == BACNET_APPLICATION_TAG_BIT_STRING) {
            bitstring_init(&TempBits);
            decode_bitstring(&StatusBuf[slen], len_value, &TempBits);
            TempRec.ucStatus = 128 | bitstring_octet(&TempBits, 0);
        }
    }

store:
    Logs[iLog][CurrentLog->iIndex++] = TempRec;
    if (CurrentLog->iIndex >= TL_MAX_ENTRIES) CurrentLog->iIndex = 0;
    CurrentLog->ulTotalRecordCount++;
    if (CurrentLog->ulRecordCount < TL_MAX_ENTRIES) CurrentLog->ulRecordCount++;
}

/* ---------------------------------------------------------------
 * Timer — à appeler toutes les secondes depuis schedule_task ou main
 * --------------------------------------------------------------- */
void trend_log_timer(uint16_t uSeconds)
{
    TL_LOG_INFO *CurrentLog;
    int i;
    bacnet_time_t tNow;

    (void)uSeconds;
    tNow = Trend_Log_Epoch_Seconds_Now();

    for (i = 0; i < MAX_TREND_LOGS; i++) {
        CurrentLog = &LogInfo[i];
        if (!TL_Is_Enabled(i)) continue;

        if (CurrentLog->LoggingType == LOGGING_TYPE_POLLED) {
            if (CurrentLog->bAlignIntervals) {
                if ((tNow % CurrentLog->ulLogInterval) ==
                    (CurrentLog->ulIntervalOffset % CurrentLog->ulLogInterval)) {
                    TL_fetch_property(i);
                } else if ((tNow - CurrentLog->tLastDataTime) > CurrentLog->ulLogInterval) {
                    TL_fetch_property(i);
                }
            } else if (((tNow - CurrentLog->tLastDataTime) >= CurrentLog->ulLogInterval) ||
                       CurrentLog->bTrigger) {
                TL_fetch_property(i);
            }
            CurrentLog->bTrigger = false;

        } else if (CurrentLog->LoggingType == LOGGING_TYPE_TRIGGERED) {
            if (CurrentLog->bTrigger) {
                TL_fetch_property(i);
                CurrentLog->bTrigger = false;
            }
        }
    }
}

/* ---------------------------------------------------------------
 * Encodage ReadRange
 * --------------------------------------------------------------- */
#define TL_MAX_ENC 23  /* taille max d'une entrée encodée */

int rr_trend_log_encode(uint8_t *apdu, BACNET_READ_RANGE_DATA *pRequest)
{
    int log_index;

    if (!pRequest) return 0;

    bitstring_init(&pRequest->ResultFlags);
    bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_FIRST_ITEM, false);
    bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_LAST_ITEM,  false);
    bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_MORE_ITEMS, false);
    pRequest->ItemCount = 0;

    log_index = Trend_Log_Instance_To_Index(pRequest->object_instance);
    if (log_index >= MAX_TREND_LOGS) return 0;
    if (LogInfo[log_index].ulRecordCount == 0) return 0;

    if ((pRequest->RequestType == RR_BY_POSITION) ||
        (pRequest->RequestType == RR_READ_ALL))
        return TL_encode_by_position(apdu, pRequest);

    if (pRequest->RequestType == RR_BY_SEQUENCE)
        return TL_encode_by_sequence(apdu, pRequest);

    return TL_encode_by_time(apdu, pRequest);
}

/* ------- encode_by_position ------- */
int TL_encode_by_position(uint8_t *apdu, BACNET_READ_RANGE_DATA *pRequest)
{
    int log_index = Trend_Log_Instance_To_Index(pRequest->object_instance);
    TL_LOG_INFO *CurrentLog = &LogInfo[log_index];
    int iLen = 0;
    int32_t iTemp;
    uint32_t uiIndex, uiFirst, uiLast, uiTarget;
    uint32_t uiRemaining = MAX_APDU - pRequest->Overhead;

    if (pRequest->RequestType == RR_READ_ALL) {
        pRequest->Count = CurrentLog->ulRecordCount;
        pRequest->Range.RefIndex = 1;
    }

    if (pRequest->Count < 0) {
        iTemp = pRequest->Range.RefIndex + pRequest->Count + 1;
        if (iTemp < 1) {
            pRequest->Count = pRequest->Range.RefIndex;
            pRequest->Range.RefIndex = 1;
        } else {
            pRequest->Range.RefIndex = iTemp;
            pRequest->Count = -pRequest->Count;
        }
    }

    if (pRequest->Range.RefIndex > CurrentLog->ulRecordCount) return 0;

    uiTarget = pRequest->Range.RefIndex + pRequest->Count - 1;
    if (uiTarget > CurrentLog->ulRecordCount) uiTarget = CurrentLog->ulRecordCount;

    uiIndex = pRequest->Range.RefIndex;
    uiFirst = uiIndex;
    uiLast  = uiIndex;

    while (uiIndex <= uiTarget) {
        if (uiRemaining < TL_MAX_ENC) {
            bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_MORE_ITEMS, true);
            break;
        }
        iTemp = TL_encode_entry(&apdu[iLen], log_index, uiIndex);
        uiRemaining -= iTemp;
        iLen += iTemp;
        uiLast = uiIndex++;
        pRequest->ItemCount++;
    }

    if (uiFirst == 1)
        bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_FIRST_ITEM, true);
    if (uiLast == CurrentLog->ulRecordCount)
        bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_LAST_ITEM, true);

    return iLen;
}

/* ------- encode_by_sequence ------- */
int TL_encode_by_sequence(uint8_t *apdu, BACNET_READ_RANGE_DATA *pRequest)
{
    int log_index = Trend_Log_Instance_To_Index(pRequest->object_instance);
    TL_LOG_INFO *CurrentLog = &LogInfo[log_index];
    int iLen = 0;
    int32_t iTemp;
    uint32_t uiIndex, uiFirst, uiLast, uiSequence;
    uint32_t uiRemaining = MAX_APDU - pRequest->Overhead;
    uint32_t uiFirstSeq  = CurrentLog->ulTotalRecordCount - (CurrentLog->ulRecordCount - 1);
    uint32_t uiBegin, uiEnd;

    if (pRequest->Count < 0) {
        uiBegin = pRequest->Range.RefSeqNum + pRequest->Count + 1;
        uiEnd   = pRequest->Range.RefSeqNum;
    } else {
        uiBegin = pRequest->Range.RefSeqNum;
        uiEnd   = pRequest->Range.RefSeqNum + pRequest->Count - 1;
    }

    /* Clamp to buffer range */
    if (uiEnd < uiFirstSeq || uiBegin > CurrentLog->ulTotalRecordCount) return 0;
    if (uiBegin < uiFirstSeq)                   uiBegin = uiFirstSeq;
    if (uiEnd   > CurrentLog->ulTotalRecordCount) uiEnd  = CurrentLog->ulTotalRecordCount;

    uiIndex    = uiBegin - uiFirstSeq + 1;
    uiSequence = uiBegin;
    uiFirst    = uiIndex;
    uiLast     = uiIndex;

    while (uiSequence != uiEnd + 1) {
        if (uiRemaining < TL_MAX_ENC) {
            bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_MORE_ITEMS, true);
            break;
        }
        iTemp = TL_encode_entry(&apdu[iLen], log_index, uiIndex);
        uiRemaining -= iTemp;
        iLen += iTemp;
        uiLast = uiIndex++;
        uiSequence++;
        pRequest->ItemCount++;
    }

    if (uiFirst == 1)
        bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_FIRST_ITEM, true);
    if (uiLast == CurrentLog->ulRecordCount)
        bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_LAST_ITEM, true);

    pRequest->FirstSequence = uiBegin;
    return iLen;
}

/* ------- encode_by_time ------- */
int TL_encode_by_time(uint8_t *apdu, BACNET_READ_RANGE_DATA *pRequest)
{
    int log_index = Trend_Log_Instance_To_Index(pRequest->object_instance);
    TL_LOG_INFO *CurrentLog = &LogInfo[log_index];
    int iLen = 0, iCount = 0;
    int32_t iTemp;
    uint32_t uiIndex, uiFirst, uiLast;
    uint32_t uiRemaining = MAX_APDU - pRequest->Overhead;
    uint32_t uiFirstSeq;
    bacnet_time_t tRefTime = TL_BAC_Time_To_Local(&pRequest->Range.RefTime);

    uiIndex = (CurrentLog->ulRecordCount < TL_MAX_ENTRIES) ? 0 : CurrentLog->iIndex;

    if (pRequest->Count < 0) {
        iCount = CurrentLog->ulRecordCount - 1;
        uiFirstSeq = CurrentLog->ulTotalRecordCount;
        for (;;) {
            if (Logs[log_index][(uiIndex + iCount) % TL_MAX_ENTRIES].tTimeStamp < tRefTime) break;
            uiFirstSeq--;
            iCount--;
            if (iCount < 0) return 0;
        }
        pRequest->Count = -pRequest->Count;
        iTemp = pRequest->Count - 1;
        if (iTemp > iCount) { uiFirstSeq -= iCount; pRequest->Count = iCount + 1; iCount = 0; }
        else                { uiFirstSeq -= iTemp;  iCount -= iTemp; }
    } else {
        iCount = 0;
        uiFirstSeq = CurrentLog->ulTotalRecordCount - (CurrentLog->ulRecordCount - 1);
        for (;;) {
            if (Logs[log_index][(uiIndex + iCount) % TL_MAX_ENTRIES].tTimeStamp > tRefTime) break;
            uiFirstSeq++;
            iCount++;
            if ((uint32_t)iCount == CurrentLog->ulRecordCount) return 0;
        }
    }

    uiIndex = iCount + 1;
    uiFirst = uiIndex;
    uiLast  = uiIndex;
    iCount  = pRequest->Count;

    while (iCount != 0) {
        if (uiRemaining < TL_MAX_ENC) {
            bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_MORE_ITEMS, true);
            break;
        }
        iTemp = TL_encode_entry(&apdu[iLen], log_index, uiIndex);
        uiRemaining -= iTemp;
        iLen += iTemp;
        uiLast = uiIndex++;
        pRequest->ItemCount++;
        iCount--;
        if (uiIndex > CurrentLog->ulRecordCount) break;
    }

    if (uiFirst == 1)
        bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_FIRST_ITEM, true);
    if (uiLast == CurrentLog->ulRecordCount)
        bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_LAST_ITEM, true);

    pRequest->FirstSequence = uiFirstSeq;
    return iLen;
}

/* ------- Encodage d'une entrée ------- */
int TL_encode_entry(uint8_t *apdu, int iLog, int iEntry)
{
    int iLen = 0;
    TL_DATA_REC *pSource;
    BACNET_BIT_STRING TempBits;
    uint8_t ucCount;
    BACNET_DATE_TIME TempTime;

    if (LogInfo[iLog].ulRecordCount < TL_MAX_ENTRIES)
        pSource = &Logs[iLog][(iEntry - 1) % TL_MAX_ENTRIES];
    else
        pSource = &Logs[iLog][(LogInfo[iLog].iIndex + iEntry - 1) % TL_MAX_ENTRIES];

    /* Tag [0] : horodatage */
    TL_Local_Time_To_BAC(&TempTime, pSource->tTimeStamp);
    iLen += bacapp_encode_context_datetime(apdu, 0, &TempTime);

    /* Tag [1] : valeur */
    iLen += encode_opening_tag(&apdu[iLen], 1);

    switch (pSource->ucRecType) {
        case TL_TYPE_STATUS:
            bitstring_init(&TempBits);
            bitstring_set_bits_used(&TempBits, 1, 5);
            bitstring_set_octet(&TempBits, 0, pSource->Datum.ucLogStatus);
            iLen += encode_context_bitstring(&apdu[iLen], TL_TYPE_STATUS, &TempBits);
            break;
        case TL_TYPE_BOOL:
            iLen += encode_context_boolean(&apdu[iLen], TL_TYPE_BOOL, pSource->Datum.ucBoolean);
            break;
        case TL_TYPE_REAL:
            iLen += encode_context_real(&apdu[iLen], TL_TYPE_REAL, pSource->Datum.fReal);
            break;
        case TL_TYPE_ENUM:
            iLen += encode_context_enumerated(&apdu[iLen], TL_TYPE_ENUM, pSource->Datum.ulEnum);
            break;
        case TL_TYPE_UNSIGN:
            iLen += encode_context_unsigned(&apdu[iLen], TL_TYPE_UNSIGN, pSource->Datum.ulUValue);
            break;
        case TL_TYPE_SIGN:
            iLen += encode_context_signed(&apdu[iLen], TL_TYPE_SIGN, pSource->Datum.lSValue);
            break;
        case TL_TYPE_BITS:
            bitstring_init(&TempBits);
            bitstring_set_bits_used(&TempBits,
                (pSource->Datum.Bits.ucLen >> 4) & 0x0F,
                pSource->Datum.Bits.ucLen & 0x0F);
            for (ucCount = pSource->Datum.Bits.ucLen >> 4; ucCount > 0; ucCount--)
                bitstring_set_octet(&TempBits, ucCount - 1,
                    pSource->Datum.Bits.ucStore[ucCount - 1]);
            iLen += encode_context_bitstring(&apdu[iLen], TL_TYPE_BITS, &TempBits);
            break;
        case TL_TYPE_NULL:
            iLen += encode_context_null(&apdu[iLen], TL_TYPE_NULL);
            break;
        case TL_TYPE_ERROR:
            iLen += encode_opening_tag(&apdu[iLen], TL_TYPE_ERROR);
            iLen += encode_application_enumerated(&apdu[iLen], pSource->Datum.Error.usClass);
            iLen += encode_application_enumerated(&apdu[iLen], pSource->Datum.Error.usCode);
            iLen += encode_closing_tag(&apdu[iLen], TL_TYPE_ERROR);
            break;
        case TL_TYPE_DELTA:
            iLen += encode_context_real(&apdu[iLen], TL_TYPE_DELTA, pSource->Datum.fTime);
            break;
        default:
            break;
    }

    iLen += encode_closing_tag(&apdu[iLen], 1);

    /* Tag [2] : status flags optionnels */
    if ((pSource->ucStatus & 128) == 128) {
        bitstring_init(&TempBits);
        bitstring_set_bits_used(&TempBits, 1, 4);
        bitstring_set_octet(&TempBits, 0, pSource->ucStatus & 0x0F);
        iLen += encode_context_bitstring(&apdu[iLen], 2, &TempBits);
    }

    return iLen;
}
