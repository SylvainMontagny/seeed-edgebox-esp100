#ifndef CALENDAR_ENTRY_H
#define CALENDAR_ENTRY_H

#include <stdint.h>
#include <stdbool.h>
#include "bacdef.h"
#include "datetime.h" /* <--- AJOUT CRITIQUE pour BACNET_DATE */

#ifdef __cplusplus
extern "C" {
#endif

/* Définition des Tags pour le choix (CHOICE) */
#ifndef BACNET_CALENDAR_DATE
#define BACNET_CALENDAR_DATE 0
#endif
#ifndef BACNET_CALENDAR_DATE_RANGE
#define BACNET_CALENDAR_DATE_RANGE 1
#endif
#ifndef BACNET_CALENDAR_WEEK_N_DAY
#define BACNET_CALENDAR_WEEK_N_DAY 2
#endif

/* Structures manquantes dans votre version */
typedef struct BACnet_Date_Range {
    BACNET_DATE startdate;
    BACNET_DATE enddate;
} BACNET_DATE_RANGE;

typedef struct BACnet_WeekNDay {
    uint8_t month;       /* 1..12, 13=Odd, 14=Even, 255=Any */
    uint8_t weekofmonth; /* 1..5, 255=Any */
    uint8_t dayofweek;   /* 1..7, 255=Any */
} BACNET_WEEKNDAY;

typedef struct BACnet_Calendar_Entry {
    uint8_t tag; /* BACNET_CALENDAR_DATE, RANGE, ou WEEK_N_DAY */
    union {
        BACNET_DATE Date;
        BACNET_DATE_RANGE DateRange;
        BACNET_WEEKNDAY WeekNDay;
    } type;
    struct BACnet_Calendar_Entry *next;
} BACNET_CALENDAR_ENTRY;

int bacnet_calendarentry_encode(uint8_t *apdu, const BACNET_CALENDAR_ENTRY *value);
int bacnet_calendarentry_decode(const uint8_t *apdu, int apdu_size, BACNET_CALENDAR_ENTRY *value);

#ifdef __cplusplus
}
#endif
#endif