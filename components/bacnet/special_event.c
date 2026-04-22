#include "special_event.h"
#include "bacdcode.h"
#include "bacapp.h"

int bacnet_special_event_encode(uint8_t *apdu, const BACNET_SPECIAL_EVENT *value)
{
    int len = 0;
    int apdu_len = 0;
    unsigned i;

    if (!value) return 0;

    if (value->periodTag == BACNET_SPECIAL_EVENT_PERIOD_CALENDAR_ENTRY) {
        len = encode_opening_tag(apdu, 0);
        apdu_len += len;
        if (apdu) apdu += len;

        len = bacnet_calendarentry_encode(apdu, &value->period.calendarEntry);
        apdu_len += len;
        if (apdu) apdu += len;

        len = encode_closing_tag(apdu, 0);
        apdu_len += len;
        if (apdu) apdu += len;
    } else {
        len = encode_context_object_id(apdu, 1,
            value->period.calendarReference.type,
            value->period.calendarReference.instance);
        apdu_len += len;
        if (apdu) apdu += len;
    }

    /* Tag 2 : timeValues */
    len = encode_opening_tag(apdu, 2);
    apdu_len += len;
    if (apdu) apdu += len;

    for (i = 0; i < value->timeValues.TV_Count; i++) {
        len = bacapp_encode_time_value(apdu,
            (BACNET_TIME_VALUE *)&value->timeValues.Time_Values[i]);
        apdu_len += len;
        if (apdu) apdu += len;
    }

    len = encode_closing_tag(apdu, 2);
    apdu_len += len;
    if (apdu) apdu += len;

    /* Tag 3 : priority */
    len = encode_context_unsigned(apdu, 3, value->priority);
    apdu_len += len;

    return apdu_len;
}

int bacnet_special_event_decode(const uint8_t *apdu, int apdu_size,
    BACNET_SPECIAL_EVENT *value)
{
    int len = 0;
    int apdu_len = 0;
    uint32_t uval = 0;

    if (!apdu || !value || apdu_size <= 0) return -1;

    /* --- Période : tag 0 = CalendarEntry, tag 1 = CalendarReference --- */
    if (decode_is_opening_tag_number((uint8_t *)&apdu[apdu_len], 0)) {
        value->periodTag = BACNET_SPECIAL_EVENT_PERIOD_CALENDAR_ENTRY;
        apdu_len++; /* opening tag 0 */

        len = bacnet_calendarentry_decode(&apdu[apdu_len],
            apdu_size - apdu_len, &value->period.calendarEntry);
        if (len <= 0) return -1;
        apdu_len += len;

        if (decode_is_closing_tag_number((uint8_t *)&apdu[apdu_len], 0))
            apdu_len++;

    } else if (decode_is_context_tag((uint8_t *)&apdu[apdu_len], 1)) {
        value->periodTag = BACNET_SPECIAL_EVENT_PERIOD_CALENDAR_REFERENCE;
        len = decode_context_object_id((uint8_t *)&apdu[apdu_len], 1,
            &value->period.calendarReference.type,
            &value->period.calendarReference.instance);
        if (len <= 0) return -1;
        apdu_len += len;
    } else {
        return -1;
    }

    /* --- Tag 2 : timeValues --- */
    if (!decode_is_opening_tag_number((uint8_t *)&apdu[apdu_len], 2))
        return -1;
    apdu_len++;

    value->timeValues.TV_Count = 0;
    while (apdu_len < apdu_size &&
           !decode_is_closing_tag_number((uint8_t *)&apdu[apdu_len], 2)) {
        if (value->timeValues.TV_Count >= 8) break;
        len = bacapp_decode_time_value(
            (uint8_t *)&apdu[apdu_len],
            &value->timeValues.Time_Values[value->timeValues.TV_Count]);
        if (len <= 0) break;
        apdu_len += len;
        value->timeValues.TV_Count++;
    }

    if (decode_is_closing_tag_number((uint8_t *)&apdu[apdu_len], 2))
        apdu_len++;

    /* --- Tag 3 : priority --- */
    if (apdu_len < apdu_size &&
        decode_is_context_tag((uint8_t *)&apdu[apdu_len], 3)) {
        len = decode_context_unsigned((uint8_t *)&apdu[apdu_len], 3, &uval);
        if (len > 0) {
            value->priority = (uint8_t)uval;
            apdu_len += len;
        }
    } else {
        value->priority = 16;
    }

    return apdu_len;
}

void bacnet_special_event_copy(BACNET_SPECIAL_EVENT *dest,
    const BACNET_SPECIAL_EVENT *src)
{
    if (dest && src) {
        *dest = *src;
    }
}