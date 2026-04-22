/**
 * @file
 * @brief BACnetWeeklySchedule complex data type encode and decode
 * @author Ondřej Hruška <ondra@ondrovo.com>
 * @author Steve Karg <skarg@users.sourceforge.net>
 * @date September 2022
 * @copyright SPDX-License-Identifier: GPL-2.0-or-later WITH GCC-exception-2.0
 */
#ifndef BACNET_WEEKLY_SCHEDULE_H
#define BACNET_WEEKLY_SCHEDULE_H

#include <stdint.h>
#include <stdbool.h>
/* CORRIGÉ: suppression du préfixe "bacnet/" — headers directement dans include/ */
#include "bacdef.h"
#include "dailyschedule.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BACNET_WEEKLY_SCHEDULE_SIZE
#define BACNET_WEEKLY_SCHEDULE_SIZE 7
#endif

typedef struct BACnet_Weekly_Schedule {
    BACNET_DAILY_SCHEDULE weeklySchedule[BACNET_WEEKLY_SCHEDULE_SIZE];
    bool singleDay;
} BACNET_WEEKLY_SCHEDULE;

int bacnet_weeklyschedule_encode(
    uint8_t *apdu, const BACNET_WEEKLY_SCHEDULE *value);

int bacnet_weeklyschedule_decode(
    const uint8_t *apdu, int apdu_size, BACNET_WEEKLY_SCHEDULE *value);

int bacnet_weeklyschedule_context_encode(
    uint8_t *apdu, uint8_t tag_number, const BACNET_WEEKLY_SCHEDULE *value);

int bacnet_weeklyschedule_context_decode(
    const uint8_t *apdu,
    int apdu_size,
    uint8_t tag_number,
    BACNET_WEEKLY_SCHEDULE *value);

bool bacnet_weeklyschedule_same(
    const BACNET_WEEKLY_SCHEDULE *value1,
    const BACNET_WEEKLY_SCHEDULE *value2);

#ifdef __cplusplus
}
#endif

#endif /* BACNET_WEEKLY_SCHEDULE_H */