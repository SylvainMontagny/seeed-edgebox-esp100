#ifndef CALENDAR_H
#define CALENDAR_H

#include <stdbool.h>
#include <stdint.h>
#include "bacdef.h"
#include "bacdcode.h"
#include "bacapp.h"
#include "rp.h"
#include "wp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Limites ── */
#ifndef MAX_CALENDARS
#define MAX_CALENDARS 2
#endif

#ifndef MAX_CALENDAR_ENTRIES
#define MAX_CALENDAR_ENTRIES 5
#endif

/* ── Types de CalendarEntry (identiques à la grande stack) ── */
#define BACNET_CALENDAR_DATE       0
#define BACNET_CALENDAR_DATE_RANGE 1
#define BACNET_CALENDAR_WEEK_N_DAY 2

/* ── Structure d'une entrée de date ── */
typedef struct {
    uint8_t tag;    /* BACNET_CALENDAR_DATE / DATE_RANGE / WEEK_N_DAY */
    union {
        BACNET_DATE       Date;
        struct {
            BACNET_DATE startdate;
            BACNET_DATE enddate;
        } DateRange;
        struct {
            uint8_t month;      /* 1-12, 255=any */
            uint8_t weekofmonth;/* 1-5, 6=last, 255=any */
            uint8_t dayofweek;  /* 1=Mon..7=Sun, 255=any */
        } WeekNDay;
    } type;
} CALENDAR_ENTRY;

/* ── Structure principale d'un Calendar ── */
typedef struct {
    char           Name[32];
    CALENDAR_ENTRY Entries[MAX_CALENDAR_ENTRIES];
    uint8_t        Entry_Count;   /* nombre d'entrées actives */
    bool           Present_Value; /* true si date du jour dans la liste */
    bool           Write_Enabled;
} CALENDAR_DESCR;

/* ── Fonctions standard BACnet ── */
void     Calendar_Property_Lists(const int **pRequired,
                                  const int **pOptional,
                                  const int **pProprietary);
void     Calendar_Init(void);
unsigned Calendar_Count(void);
uint32_t Calendar_Index_To_Instance(unsigned index);
unsigned Calendar_Instance_To_Index(uint32_t object_instance);
bool     Calendar_Valid_Instance(uint32_t object_instance);
bool     Calendar_Object_Name(uint32_t object_instance,
                               BACNET_CHARACTER_STRING *object_name);
int      Calendar_Read_Property(BACNET_READ_PROPERTY_DATA *rpdata);
bool     Calendar_Write_Property(BACNET_WRITE_PROPERTY_DATA *wp_data);

/* ── Accès aux données ── */
bool            Calendar_Present_Value(uint32_t object_instance);
bool            Calendar_Date_List_Add(uint32_t object_instance,
                                        const CALENDAR_ENTRY *entry);
bool            Calendar_Date_List_Delete_All(uint32_t object_instance);
int             Calendar_Date_List_Count(uint32_t object_instance);
CALENDAR_ENTRY *Calendar_Date_List_Get(uint32_t object_instance,
                                        uint8_t index);

/* ── Mise à jour Present Value ── */
void Calendar_Update_Present_Value(uint32_t object_instance);

#ifdef __cplusplus
}
#endif
#endif /* CALENDAR_H */