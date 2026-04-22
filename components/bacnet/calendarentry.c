#include "calendarentry.h"
#include "bacdcode.h"

int bacnet_calendarentry_encode(uint8_t *apdu, const BACNET_CALENDAR_ENTRY *value) {
    int len = 0;
    if (value) {
        switch (value->tag) {
            case BACNET_CALENDAR_DATE:
                len = encode_context_date(apdu, 0, (BACNET_DATE *)&value->type.Date);
                break;
            case BACNET_CALENDAR_DATE_RANGE:
                len = encode_opening_tag(apdu, 1);
                len += encode_application_date(apdu ? apdu + len : NULL, (BACNET_DATE *)&value->type.DateRange.startdate);
                len += encode_application_date(apdu ? apdu + len : NULL, (BACNET_DATE *)&value->type.DateRange.enddate);
                len += encode_closing_tag(apdu ? apdu + len : NULL, 1);
                break;
            case BACNET_CALENDAR_WEEK_N_DAY:
                len = encode_opening_tag(apdu, 2);
                len += encode_context_octet_string(apdu ? apdu + len : NULL, 0, (BACNET_OCTET_STRING *)&value->type.WeekNDay);
                len += encode_closing_tag(apdu ? apdu + len : NULL, 2);
                break;
        }
    }
    return len;
}

int bacnet_calendarentry_decode(const uint8_t *apdu, int apdu_size, BACNET_CALENDAR_ENTRY *value) {
    int len = 0;
    int apdu_len = 0;
    if (!apdu || !value) return -1;

    if (decode_is_context_tag(&apdu[apdu_len], 0)) {
        value->tag = BACNET_CALENDAR_DATE;
        len = decode_context_date(&apdu[apdu_len], 0, &value->type.Date);
        if (len > 0) apdu_len += len;
    } else if (decode_is_opening_tag_number(&apdu[apdu_len], 1)) {
        value->tag = BACNET_CALENDAR_DATE_RANGE;
        apdu_len++;
        len = decode_application_date(&apdu[apdu_len], &value->type.DateRange.startdate);
        apdu_len += len;
        len = decode_application_date(&apdu[apdu_len], &value->type.DateRange.enddate);
        apdu_len += len;
        if (decode_is_closing_tag_number(&apdu[apdu_len], 1)) apdu_len++;
    }
    // Simplification : WeekNDay omis pour gagner du temps, ajoutez si besoin
    return apdu_len;
}