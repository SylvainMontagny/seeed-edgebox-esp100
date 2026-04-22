#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "config.h"
#include "txbuf.h"
#include "bacdef.h"
#include "bacdcode.h"
#include "bacerror.h"
#include "apdu.h"
#include "npdu.h"
#include "abort.h"
#include "wp.h"
#include "device.h"
#include "handlers.h"

void handler_write_property(
    uint8_t * service_request,
    uint16_t service_len,
    BACNET_ADDRESS * src,
    BACNET_CONFIRMED_SERVICE_DATA * service_data)
{
    printf("*** WP RECU len=%d ***\n", service_len);

    BACNET_WRITE_PROPERTY_DATA wp_data;
    int len = 0;
    int pdu_len = 0;
    BACNET_NPDU_DATA npdu_data;
    int bytes_sent = 0;
    BACNET_ADDRESS my_address;

    datalink_get_my_address(&my_address);
    npdu_encode_npdu_data(&npdu_data, false, MESSAGE_PRIORITY_NORMAL);
    pdu_len = npdu_encode_pdu(&Handler_Transmit_Buffer[0], src, &my_address, &npdu_data);

    if (service_data->segmented_message) {
        len = abort_encode_apdu(&Handler_Transmit_Buffer[pdu_len],
            service_data->invoke_id, ABORT_REASON_SEGMENTATION_NOT_SUPPORTED, true);
        goto WP_ABORT;
    }

    len = wp_decode_service_request(service_request, service_len, &wp_data);

    if (len > 0) {
        printf("*** WP: type=%d instance=%d prop=%d array=%d len_data=%d ***\n",
            (int)wp_data.object_type,
            (int)wp_data.object_instance,
            (int)wp_data.object_property,
            (int)wp_data.array_index,
            (int)wp_data.application_data_len);
    } else {
        printf("*** WP: decode failed len=%d ***\n", len);
    }

    if (len <= 0) {
        len = abort_encode_apdu(&Handler_Transmit_Buffer[pdu_len],
            service_data->invoke_id, ABORT_REASON_OTHER, true);
        goto WP_ABORT;
    }

    if (Device_Write_Property(&wp_data)) {
        printf("*** WP: ACK OK ***\n");
        len = encode_simple_ack(&Handler_Transmit_Buffer[pdu_len],
            service_data->invoke_id, SERVICE_CONFIRMED_WRITE_PROPERTY);
    } else {
        printf("*** WP: ERROR class=%d code=%d ***\n",
            (int)wp_data.error_class, (int)wp_data.error_code);
        len = bacerror_encode_apdu(&Handler_Transmit_Buffer[pdu_len],
            service_data->invoke_id, SERVICE_CONFIRMED_WRITE_PROPERTY,
            wp_data.error_class, wp_data.error_code);
    }

WP_ABORT:
    pdu_len += len;
    bytes_sent = datalink_send_pdu(src, &npdu_data, &Handler_Transmit_Buffer[0], pdu_len);
    (void)bytes_sent;
    return;
}

bool WPValidateString(
    BACNET_APPLICATION_DATA_VALUE * pValue,
    int iMaxLen,
    bool bEmptyAllowed,
    BACNET_ERROR_CLASS * pErrorClass,
    BACNET_ERROR_CODE * pErrorCode)
{
    bool bResult = false;
    *pErrorClass = ERROR_CLASS_PROPERTY;

    if (pValue->tag == BACNET_APPLICATION_TAG_CHARACTER_STRING) {
        if (characterstring_encoding(&pValue->type.Character_String) == CHARACTER_ANSI_X34) {
            if ((bEmptyAllowed == false) &&
                (characterstring_length(&pValue->type.Character_String) == 0)) {
                *pErrorCode = ERROR_CODE_VALUE_OUT_OF_RANGE;
            } else if ((bEmptyAllowed == false) &&
                (!characterstring_printable(&pValue->type.Character_String))) {
                *pErrorCode = ERROR_CODE_VALUE_OUT_OF_RANGE;
            } else if (characterstring_length(&pValue->type.Character_String) > (uint16_t)iMaxLen) {
                *pErrorClass = ERROR_CLASS_RESOURCES;
                *pErrorCode = ERROR_CODE_NO_SPACE_TO_WRITE_PROPERTY;
            } else {
                bResult = true;
            }
        } else {
            *pErrorCode = ERROR_CODE_CHARACTER_SET_NOT_SUPPORTED;
        }
    } else {
        *pErrorCode = ERROR_CODE_INVALID_DATA_TYPE;
    }
    return bResult;
}

bool WPValidateArgType(
    BACNET_APPLICATION_DATA_VALUE * pValue,
    uint8_t ucExpectedTag,
    BACNET_ERROR_CLASS * pErrorClass,
    BACNET_ERROR_CODE * pErrorCode)
{
    bool bResult = true;
    if (pValue->tag != ucExpectedTag) {
        bResult = false;
        *pErrorClass = ERROR_CLASS_PROPERTY;
        *pErrorCode = ERROR_CODE_INVALID_DATA_TYPE;
    }
    return bResult;
}