/**
 * @file trendlog.h
 * @brief Trend Log object - adapté pour ESP32-S3 EdgeBox éclairage public
 * @note  TL0 surveille AV0 (sortie PWM voie 0)
 *        TL1 surveille AV1 (sortie PWM voie 1)
 *        MAX_TREND_LOGS = 2, TL_MAX_ENTRIES = 100 (RAM limitée)
 */
#ifndef TRENDLOG_H
#define TRENDLOG_H

#include <stdbool.h>
#include <stdint.h>
#include "bacdef.h"
#include "datetime.h"
#include "readrange.h"
#include "rp.h"
#include "wp.h"

typedef uint32_t bacnet_time_t;

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------
 * Constantes projet
 * --------------------------------------------------------------- */
#define MAX_TREND_LOGS   2    /* TL0 = AV0, TL1 = AV1             */
#define TL_MAX_ENTRIES   100  /* 100 entrées/log → RAM raisonnable */

/* Flags pour les wildcards de temps */
#define TL_T_START_WILD  1
#define TL_T_STOP_WILD   2

/* ---------------------------------------------------------------
 * Types de données enregistrés dans le buffer
 * (= numéros de tag BACnet pour l'encodage)
 * --------------------------------------------------------------- */
#define TL_TYPE_STATUS   0
#define TL_TYPE_BOOL     1
#define TL_TYPE_REAL     2
#define TL_TYPE_ENUM     3
#define TL_TYPE_UNSIGN   4
#define TL_TYPE_SIGN     5
#define TL_TYPE_BITS     6
#define TL_TYPE_NULL     7
#define TL_TYPE_ERROR    8
#define TL_TYPE_DELTA    9
#define TL_TYPE_ANY      10  /* non supporté */

/* ---------------------------------------------------------------
 * Structures de données
 * --------------------------------------------------------------- */

/* Code d'erreur stocké dans le log */
typedef struct tl_error {
    uint16_t usClass;
    uint16_t usCode;
} TL_ERROR;

/* Bitstring jusqu'à 32 bits */
typedef struct tl_bits {
    uint8_t ucLen;        /* octets utilisés (nibble haut) / bits libres (nibble bas) */
    uint8_t ucStore[4];
} TL_BITS;

/* Un enregistrement dans le buffer circulaire */
typedef struct tl_data_record {
    bacnet_time_t tTimeStamp;   /* Horodatage (secondes depuis epoch) */
    uint8_t       ucRecType;    /* Type TL_TYPE_xxx                   */
    uint8_t       ucStatus;     /* b7=1 si status flags présents      */
    union {
        uint8_t  ucLogStatus;   /* Changement d'état du log           */
        uint8_t  ucBoolean;
        float    fReal;         /* ← utilisé pour AV0/AV1 (0..100 %)  */
        uint32_t ulEnum;
        uint32_t ulUValue;
        int32_t  lSValue;
        TL_BITS  Bits;
        TL_ERROR Error;
        float    fTime;         /* Delta time                         */
    } Datum;
} TL_DATA_REC;

/* Configuration et état d'un Trend Log */
typedef struct tl_log_info {
    bool              bEnable;
    BACNET_DATE_TIME  StartTime;
    bacnet_time_t     tStartTime;
    BACNET_DATE_TIME  StopTime;
    bacnet_time_t     tStopTime;
    uint8_t           ucTimeFlags;
    BACNET_DEVICE_OBJECT_PROPERTY_REFERENCE Source; /* objet surveillé */
    uint32_t          ulLogInterval;      /* secondes entre lectures   */
    bool              bStopWhenFull;
    uint32_t          ulRecordCount;      /* entrées actuellement dans le buffer */
    uint32_t          ulTotalRecordCount; /* total historique          */
    BACNET_LOGGING_TYPE LoggingType;
    bool              bAlignIntervals;
    uint32_t          ulIntervalOffset;
    bool              bTrigger;
    int               iIndex;            /* prochain index d'insertion */
    bacnet_time_t     tLastDataTime;
} TL_LOG_INFO;

/* ---------------------------------------------------------------
 * API publique
 * --------------------------------------------------------------- */

void     Trend_Log_Init(void);
void     Trend_Log_Property_Lists(const int **pRequired,
                                  const int **pOptional,
                                  const int **pProprietary);
bool     Trend_Log_Valid_Instance(uint32_t object_instance);
unsigned Trend_Log_Count(void);
uint32_t Trend_Log_Index_To_Instance(unsigned index);
unsigned Trend_Log_Instance_To_Index(uint32_t object_instance);
bool     Trend_Log_Object_Name(uint32_t object_instance,
                               BACNET_CHARACTER_STRING *object_name);
int      Trend_Log_Read_Property(BACNET_READ_PROPERTY_DATA *rpdata);
bool     Trend_Log_Write_Property(BACNET_WRITE_PROPERTY_DATA *wp_data);

/* Insertion d'un enregistrement de statut */
void     TL_Insert_Status_Rec(int iLog, BACNET_LOG_STATUS eStatus, bool bState);

/* Vérifie si le log est actif (enable + plage horaire) */
bool     TL_Is_Enabled(int iLog);

/* Conversions heure BACnet <-> secondes locales */
bacnet_time_t TL_BAC_Time_To_Local(const BACNET_DATE_TIME *bdatetime);
void          TL_Local_Time_To_BAC(BACNET_DATE_TIME *bdatetime, bacnet_time_t seconds);

/* Encodage ReadRange */
int  TL_encode_entry(uint8_t *apdu, int iLog, int iEntry);
int  TL_encode_by_position(uint8_t *apdu, BACNET_READ_RANGE_DATA *pRequest);
int  TL_encode_by_sequence(uint8_t *apdu, BACNET_READ_RANGE_DATA *pRequest);
int  TL_encode_by_time(uint8_t *apdu, BACNET_READ_RANGE_DATA *pRequest);
bool TrendLogGetRRInfo(BACNET_READ_RANGE_DATA *pRequest, RR_PROP_INFO *pInfo);
int  rr_trend_log_encode(uint8_t *apdu, BACNET_READ_RANGE_DATA *pRequest);

/* Timer à appeler régulièrement (ex: toutes les secondes depuis main) */
void trend_log_timer(uint16_t uSeconds);

#ifdef __cplusplus
}
#endif

#endif /* TRENDLOG_H */
