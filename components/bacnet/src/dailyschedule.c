#include "dailyschedule.h"
#include "bacdcode.h"
#include "bactimevalue.h" // Nécessaire pour les fonctions bacapp_...

int bacnet_dailyschedule_context_encode(uint8_t *apdu, uint8_t tag_number, const BACNET_DAILY_SCHEDULE *value) {
    int len = 0;
    int apdu_len = 0;
    unsigned i = 0;

    if (value) {
        len = encode_opening_tag(apdu, tag_number);
        apdu_len += len;
        if (apdu) apdu += len;

        for (i = 0; i < value->TV_Count; i++) {
            // CORRECTION: bacnet_time_value_encode -> bacapp_encode_time_value
            len = bacapp_encode_time_value(apdu, &value->Time_Values[i]);
            apdu_len += len;
            if (apdu) apdu += len;
        }

        len = encode_closing_tag(apdu, tag_number);
        apdu_len += len;
    }
    return apdu_len;
}

int bacnet_dailyschedule_context_decode(const uint8_t *apdu, int apdu_size, uint8_t tag_number, BACNET_DAILY_SCHEDULE *value) {
    int len = 0;
    int apdu_len = 0;

    // CORRECTION: bacnet_is_opening_tag_number -> decode_is_opening_tag_number
    if (decode_is_opening_tag_number((uint8_t*)&apdu[0], tag_number)) {
        // decode_is_... ne retourne pas la longueur, on doit la calculer/avancer manuellement (1 octet pour le tag)
        apdu_len += 1; 
        
        value->TV_Count = 0;
        
        // CORRECTION: bacnet_is_closing_tag_number -> decode_is_closing_tag_number
        while ((apdu_len < apdu_size) && !decode_is_closing_tag_number((uint8_t*)&apdu[apdu_len], tag_number)) {
            // CORRECTION: bacnet_time_value_decode -> bacapp_decode_time_value
            len = bacapp_decode_time_value((uint8_t*)&apdu[apdu_len], &value->Time_Values[value->TV_Count]);
            if (len > 0) {
                value->TV_Count++;
                apdu_len += len;
            } else {
                break;
            }
        }
        
        if (decode_is_closing_tag_number((uint8_t*)&apdu[apdu_len], tag_number)) {
            apdu_len += 1; // Avancer pour le closing tag
        }
    }
    return apdu_len;
}