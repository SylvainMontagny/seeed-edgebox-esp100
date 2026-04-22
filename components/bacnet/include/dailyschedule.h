#ifndef DAILY_SCHEDULE_H
#define DAILY_SCHEDULE_H
#include <stdint.h>
#include "bacdef.h"
#include "bactimevalue.h"

typedef struct BACnet_Daily_Schedule {
    BACNET_TIME_VALUE Time_Values[4]; // Max 8 changements par jour
    uint16_t TV_Count;
} BACNET_DAILY_SCHEDULE;

int bacnet_dailyschedule_context_encode(uint8_t *apdu, uint8_t tag_number, const BACNET_DAILY_SCHEDULE *value);
int bacnet_dailyschedule_context_decode(const uint8_t *apdu, int apdu_size, uint8_t tag_number, BACNET_DAILY_SCHEDULE *value);
#endif