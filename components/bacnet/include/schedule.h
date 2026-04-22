#ifndef SCHEDULE_H
#define SCHEDULE_H

#include <stdint.h>
#include <stdbool.h>
#include "bacdef.h"
#include "bacstr.h"
#include "timestamp.h"
#include "dailyschedule.h"
#include "special_event.h"
#include "rp.h"
#include "wp.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MAX_SCHEDULES
#define MAX_SCHEDULES 2
#endif

#ifndef BACNET_WEEKLY_SCHEDULE_SIZE
#define BACNET_WEEKLY_SCHEDULE_SIZE 7
#endif

/* 1 Special Event par Schedule pour commencer */
#ifndef BACNET_EXCEPTION_SCHEDULE_SIZE
#define BACNET_EXCEPTION_SCHEDULE_SIZE 5
#endif

typedef struct Schedule_Descr {
    BACNET_DATE Start_Date;
    BACNET_DATE End_Date;
    BACNET_DAILY_SCHEDULE Weekly_Schedule[BACNET_WEEKLY_SCHEDULE_SIZE];
    BACNET_APPLICATION_DATA_VALUE Schedule_Default;
    BACNET_APPLICATION_DATA_VALUE Present_Value;
    bool Out_Of_Service;
    uint8_t Priority_For_Writing;

    BACNET_DEVICE_OBJECT_PROPERTY_REFERENCE Object_Property_References[2];
    uint8_t obj_prop_ref_cnt;

#if BACNET_EXCEPTION_SCHEDULE_SIZE > 0
    BACNET_SPECIAL_EVENT Exception_Schedule[BACNET_EXCEPTION_SCHEDULE_SIZE];
    uint8_t Exception_Count; /* nombre de Special Events actifs */
#endif
} SCHEDULE_DESCR;

void Schedule_Property_Lists(
    const int **pRequired,
    const int **pOptional,
    const int **pProprietary);

void Schedule_Init(void);
unsigned Schedule_Count(void);
uint32_t Schedule_Index_To_Instance(unsigned index);
unsigned Schedule_Instance_To_Index(uint32_t instance);
bool Schedule_Valid_Instance(uint32_t object_instance);
bool Schedule_Object_Name(uint32_t object_instance,
    BACNET_CHARACTER_STRING *object_name);
int Schedule_Read_Property(BACNET_READ_PROPERTY_DATA *rpdata);
bool Schedule_Write_Property(BACNET_WRITE_PROPERTY_DATA *wp_data);
SCHEDULE_DESCR *Schedule_Object(uint32_t object_instance);
void Schedule_Recalculate_PV(SCHEDULE_DESCR *desc, BACNET_WEEKDAY wday,
    const BACNET_TIME *time);

/* ----------------------------------------------------------------
 * Accesseurs Special Events — nommage identique à la grande stack
 * (demandés par le prof : Schedule_Exception_Schedule_Set /
 *  Schedule_Exception_Schedule)
 * ---------------------------------------------------------------- */
#if BACNET_EXCEPTION_SCHEDULE_SIZE > 0
BACNET_SPECIAL_EVENT *Schedule_Exception_Schedule(
    uint32_t object_instance, unsigned array_index);

bool Schedule_Exception_Schedule_Set(
    uint32_t object_instance,
    unsigned array_index,
    const BACNET_SPECIAL_EVENT *value);
#endif

#ifdef __cplusplus
}
#endif
#endif