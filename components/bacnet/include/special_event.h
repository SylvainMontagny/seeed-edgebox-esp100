#ifndef SPECIAL_EVENT_H
#define SPECIAL_EVENT_H

#include <stdint.h>
#include "bacdef.h"
#include "dailyschedule.h"
#include "calendarentry.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BACNET_SPECIAL_EVENT_PERIOD_CALENDAR_ENTRY = 0,
    BACNET_SPECIAL_EVENT_PERIOD_CALENDAR_REFERENCE = 1
} BACNET_SPECIAL_EVENT_PERIOD;

typedef struct BACnet_Special_Event {
    BACNET_SPECIAL_EVENT_PERIOD periodTag;
    union {
        BACNET_CALENDAR_ENTRY calendarEntry;
        BACNET_OBJECT_ID calendarReference;
    } period;
    BACNET_DAILY_SCHEDULE timeValues;
    uint8_t priority;
} BACNET_SPECIAL_EVENT;

int bacnet_special_event_encode(uint8_t *apdu, const BACNET_SPECIAL_EVENT *value);
int bacnet_special_event_decode(const uint8_t *apdu, int apdu_size, BACNET_SPECIAL_EVENT *value);
void bacnet_special_event_copy(BACNET_SPECIAL_EVENT *dest, const BACNET_SPECIAL_EVENT *src);

#ifdef __cplusplus
}
#endif
#endif