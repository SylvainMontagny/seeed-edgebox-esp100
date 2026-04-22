/**
 * @file msv.c
 * @brief Multi-State Value object — adapté pour ESP32-S3 / BACnet éclairage public
 * Adapté depuis bacnet-stack/src/bacnet/basic/object/msv.c
 *
 * Adaptations par rapport à la stack originale :
 * - BACNET_STACK_EXPORT supprimé (non défini dans ce projet)
 * - Includes à plat (sans bacnet/...) pour correspondre au projet
 * - keylist supprimé → tableau statique simple comme av.c et bv.c
 * - bacnet_array_encode() absent → encodage State_Text manuel
 * - 3 MSV préconfigurés dans Multistate_Value_Init() :
 *     MSV0 → état zone A (ETEINT/DIMMING/NORMAL/PLEIN ECLAIRAGE)
 *     MSV1 → état zone B (idem)
 *     MSV2 → mode (SCHEDULE/MANUEL)
 * - Fonctions Multistate_Value_Update_From_AV() et _From_BV()
 *   pour mettre à jour les MSV depuis schedule_task()
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "bacdef.h"
#include "bacdcode.h"
#include "bacapp.h"
#include "rp.h"
#include "wp.h"
#include "msv.h"

/* ================================================================
 * Structure interne de chaque objet MSV
 * ================================================================ */
typedef struct {
    const char  *Object_Name;
    const char  *State_Text;   /* chaînes séparées par '\0' */
    uint8_t      Present_Value;
    bool         Out_Of_Service;
    uint8_t      Max_States;
} MSV_DESCR;

/* ================================================================
 * State text — chaînes séparées par '\0', terminées par '\0\0'
 * ================================================================ */
static const char State_Text_Zone[] =
    "ETEINT\0"
    "DIMMING\0"
    "NORMAL\0"
    "PLEIN ECLAIRAGE\0";

static const char State_Text_Mode[] =
    "SCHEDULE\0"
    "MANUEL\0";

/* ================================================================
 * Tableau statique des objets MSV
 * ================================================================ */
static MSV_DESCR MSV_Descr[MAX_MULTI_STATE_VALUES];

/* ================================================================
 * Propriétés BACnet requises / optionnelles / propriétaires
 * ================================================================ */
static const int Properties_Required[] = {
    PROP_OBJECT_IDENTIFIER,
    PROP_OBJECT_NAME,
    PROP_OBJECT_TYPE,
    PROP_PRESENT_VALUE,
    PROP_STATUS_FLAGS,
    PROP_EVENT_STATE,
    PROP_OUT_OF_SERVICE,
    PROP_NUMBER_OF_STATES,
    -1
};

static const int Properties_Optional[] = {
    PROP_DESCRIPTION,
    PROP_STATE_TEXT,
    -1
};

static const int Properties_Proprietary[] = { -1 };

void Multistate_Value_Property_Lists(
    const int **pRequired,
    const int **pOptional,
    const int **pProprietary)
{
    if (pRequired)    *pRequired    = Properties_Required;
    if (pOptional)    *pOptional    = Properties_Optional;
    if (pProprietary) *pProprietary = Properties_Proprietary;
}

/* ================================================================
 * Helpers state text
 * ================================================================ */
static unsigned state_name_count(const char *state_names)
{
    unsigned count = 0;
    int len = 0;
    if (state_names) {
        do {
            len = strlen(state_names);
            if (len > 0) { count++; state_names += len + 1; }
        } while (len > 0);
    }
    return count;
}

static const char *state_name_by_index(const char *state_names, unsigned index)
{
    unsigned count = 0;
    int len = 0;
    if (state_names) {
        do {
            len = strlen(state_names);
            if (len > 0) {
                count++;
                if (index == count) return state_names;
                state_names += len + 1;
            }
        } while (len > 0);
    }
    return NULL;
}

/* ================================================================
 * API de base
 * ================================================================ */
bool Multistate_Value_Valid_Instance(uint32_t object_instance)
{
    return (object_instance < MAX_MULTI_STATE_VALUES);
}

unsigned Multistate_Value_Count(void)
{
    return MAX_MULTI_STATE_VALUES;
}

uint32_t Multistate_Value_Index_To_Instance(unsigned index)
{
    return index;
}

unsigned Multistate_Value_Instance_To_Index(uint32_t object_instance)
{
    if (object_instance < MAX_MULTI_STATE_VALUES)
        return object_instance;
    return MAX_MULTI_STATE_VALUES;
}

uint32_t Multistate_Value_Max_States(uint32_t object_instance)
{
    if (object_instance < MAX_MULTI_STATE_VALUES)
        return MSV_Descr[object_instance].Max_States;
    return 0;
}

const char *Multistate_Value_State_Text(uint32_t object_instance,
                                        uint32_t state_index)
{
    if (object_instance < MAX_MULTI_STATE_VALUES)
        return state_name_by_index(MSV_Descr[object_instance].State_Text,
                                   state_index);
    return NULL;
}

bool Multistate_Value_Object_Name(uint32_t object_instance,
                                  BACNET_CHARACTER_STRING *object_name)
{
    static char text_string[32] = "";
    if (object_instance < MAX_MULTI_STATE_VALUES) {
        if (MSV_Descr[object_instance].Object_Name) {
            return characterstring_init_ansi(object_name,
                MSV_Descr[object_instance].Object_Name);
        } else {
            snprintf(text_string, sizeof(text_string),
                "MULTI-STATE VALUE %lu", (unsigned long)object_instance);
            return characterstring_init_ansi(object_name, text_string);
        }
    }
    return false;
}

uint32_t Multistate_Value_Present_Value(uint32_t object_instance)
{
    if (object_instance < MAX_MULTI_STATE_VALUES)
        return MSV_Descr[object_instance].Present_Value;
    return 1;
}

bool Multistate_Value_Present_Value_Set(uint32_t object_instance,
                                        uint32_t value)
{
    if (object_instance < MAX_MULTI_STATE_VALUES &&
        value >= 1 && value <= MSV_Descr[object_instance].Max_States) {
        MSV_Descr[object_instance].Present_Value = (uint8_t)value;
        return true;
    }
    return false;
}

bool Multistate_Value_Out_Of_Service(uint32_t object_instance)
{
    if (object_instance < MAX_MULTI_STATE_VALUES)
        return MSV_Descr[object_instance].Out_Of_Service;
    return false;
}

void Multistate_Value_Out_Of_Service_Set(uint32_t object_instance, bool value)
{
    if (object_instance < MAX_MULTI_STATE_VALUES)
        MSV_Descr[object_instance].Out_Of_Service = value;
}

/* ================================================================
 * Read Property
 * ================================================================ */
int Multistate_Value_Read_Property(BACNET_READ_PROPERTY_DATA *rpdata)
{
    int apdu_len = 0;
    BACNET_BIT_STRING bit_string;
    BACNET_CHARACTER_STRING char_string;
    uint32_t present_value = 0;
    uint32_t max_states = 0;
    uint32_t state_index = 0;
    const char *pName = NULL;
    uint8_t *apdu = NULL;
    bool state = false;
    int len = 0;

    if (!rpdata || !rpdata->application_data ||
        rpdata->application_data_len == 0)
        return 0;

    if (rpdata->object_instance >= MAX_MULTI_STATE_VALUES)
        return BACNET_STATUS_ERROR;

    apdu = rpdata->application_data;

    switch (rpdata->object_property) {

        case PROP_OBJECT_IDENTIFIER:
            apdu_len = encode_application_object_id(
                &apdu[0], OBJECT_MULTI_STATE_VALUE, rpdata->object_instance);
            break;

        case PROP_OBJECT_NAME:
            Multistate_Value_Object_Name(rpdata->object_instance, &char_string);
            apdu_len = encode_application_character_string(&apdu[0],
                &char_string);
            break;

        case PROP_OBJECT_TYPE:
            apdu_len = encode_application_enumerated(&apdu[0],
                OBJECT_MULTI_STATE_VALUE);
            break;

        case PROP_PRESENT_VALUE:
            present_value = Multistate_Value_Present_Value(
                rpdata->object_instance);
            apdu_len = encode_application_unsigned(&apdu[0], present_value);
            break;

        case PROP_STATUS_FLAGS:
            bitstring_init(&bit_string);
            bitstring_set_bit(&bit_string, STATUS_FLAG_IN_ALARM,   false);
            bitstring_set_bit(&bit_string, STATUS_FLAG_FAULT,       false);
            bitstring_set_bit(&bit_string, STATUS_FLAG_OVERRIDDEN,  false);
            state = Multistate_Value_Out_Of_Service(rpdata->object_instance);
            bitstring_set_bit(&bit_string, STATUS_FLAG_OUT_OF_SERVICE, state);
            apdu_len = encode_application_bitstring(&apdu[0], &bit_string);
            break;

        case PROP_EVENT_STATE:
            apdu_len = encode_application_enumerated(&apdu[0],
                EVENT_STATE_NORMAL);
            break;

        case PROP_OUT_OF_SERVICE:
            state = Multistate_Value_Out_Of_Service(rpdata->object_instance);
            apdu_len = encode_application_boolean(&apdu[0], state);
            break;

        case PROP_NUMBER_OF_STATES:
            apdu_len = encode_application_unsigned(&apdu[0],
                Multistate_Value_Max_States(rpdata->object_instance));
            break;

        case PROP_STATE_TEXT:
            max_states = Multistate_Value_Max_States(rpdata->object_instance);

            if (rpdata->array_index == 0) {
                /* Element 0 = nombre d'états */
                apdu_len = encode_application_unsigned(&apdu[0], max_states);

            } else if (rpdata->array_index == BACNET_ARRAY_ALL) {
                /* Tous les états */
                for (state_index = 1; state_index <= max_states;
                     state_index++) {
                    pName = Multistate_Value_State_Text(
                        rpdata->object_instance, state_index);
                    if (pName) {
                        characterstring_init_ansi(&char_string, pName);
                        len = encode_application_character_string(
                            &apdu[apdu_len], &char_string);
                        apdu_len += len;
                    }
                }
            } else if (rpdata->array_index >= 1 &&
                       rpdata->array_index <= max_states) {
                /* Un état spécifique */
                pName = Multistate_Value_State_Text(
                    rpdata->object_instance, rpdata->array_index);
                if (pName) {
                    characterstring_init_ansi(&char_string, pName);
                    apdu_len = encode_application_character_string(
                        &apdu[0], &char_string);
                } else {
                    rpdata->error_class = ERROR_CLASS_PROPERTY;
                    rpdata->error_code  = ERROR_CODE_INVALID_ARRAY_INDEX;
                    apdu_len = BACNET_STATUS_ERROR;
                }
            } else {
                rpdata->error_class = ERROR_CLASS_PROPERTY;
                rpdata->error_code  = ERROR_CODE_INVALID_ARRAY_INDEX;
                apdu_len = BACNET_STATUS_ERROR;
            }
            break;

        case PROP_DESCRIPTION:
            Multistate_Value_Object_Name(rpdata->object_instance, &char_string);
            apdu_len = encode_application_character_string(&apdu[0],
                &char_string);
            break;

        default:
            rpdata->error_class = ERROR_CLASS_PROPERTY;
            rpdata->error_code  = ERROR_CODE_UNKNOWN_PROPERTY;
            apdu_len = BACNET_STATUS_ERROR;
            break;
    }

    return apdu_len;
}

/* ================================================================
 * Write Property
 * Les MSV sont en lecture seule depuis le réseau BACnet —
 * ils sont mis à jour uniquement par le code interne (schedule_task).
 * ================================================================ */
bool Multistate_Value_Write_Property(BACNET_WRITE_PROPERTY_DATA *wp_data)
{
    wp_data->error_class = ERROR_CLASS_PROPERTY;
    wp_data->error_code  = ERROR_CODE_WRITE_ACCESS_DENIED;
    return false;
}

/* ================================================================
 * Init — configure les 3 MSV
 * ================================================================ */
void Multistate_Value_Init(void)
{
    unsigned i;
    for (i = 0; i < MAX_MULTI_STATE_VALUES; i++) {
        memset(&MSV_Descr[i], 0, sizeof(MSV_DESCR));
        MSV_Descr[i].Present_Value  = 1;
        MSV_Descr[i].Out_Of_Service = false;
    }

    /* MSV0 — état zone A (AV0) */
    MSV_Descr[0].Object_Name = "ETAT ZONE A";
    MSV_Descr[0].State_Text  = State_Text_Zone;
    MSV_Descr[0].Max_States  = (uint8_t)state_name_count(State_Text_Zone);

    /* MSV1 — état zone B (AV1) */
    MSV_Descr[1].Object_Name = "ETAT ZONE B";
    MSV_Descr[1].State_Text  = State_Text_Zone;
    MSV_Descr[1].Max_States  = (uint8_t)state_name_count(State_Text_Zone);

    /* MSV2 — mode pilotage (BV0) */
    MSV_Descr[2].Object_Name = "MODE PILOTAGE";
    MSV_Descr[2].State_Text  = State_Text_Mode;
    MSV_Descr[2].Max_States  = (uint8_t)state_name_count(State_Text_Mode);
}

/* ================================================================
 * Mise à jour MSV0/MSV1 depuis la valeur AV (0.0 à 100.0)
 *
 * AV = 0          → ETEINT          (état 1)
 * AV entre 1..49  → DIMMING         (état 2)
 * AV entre 50..99 → NORMAL          (état 3)
 * AV = 100        → PLEIN ECLAIRAGE (état 4)
 * ================================================================ */
void Multistate_Value_Update_From_AV(uint32_t av_instance, float av_value)
{
    uint32_t msv_instance = av_instance; /* MSV0 ↔ AV0, MSV1 ↔ AV1 */
    uint32_t new_state;

    if (msv_instance >= 2) return; /* sécurité */

    if (av_value <= 0.0f)
        new_state = MSV_STATE_ETEINT;
    else if (av_value < 50.0f)
        new_state = MSV_STATE_DIMMING;
    else if (av_value < 100.0f)
        new_state = MSV_STATE_NORMAL;
    else
        new_state = MSV_STATE_PLEIN_ECLAIRAGE;

    Multistate_Value_Present_Value_Set(msv_instance, new_state);
}

/* ================================================================
 * Mise à jour MSV2 depuis l'état BV0
 *
 * BV0=ACTIVE   (Manuel)   → MANUEL   (état 2)
 * BV0=INACTIVE (Schedule) → SCHEDULE (état 1)
 * ================================================================ */
void Multistate_Value_Update_From_BV(bool control_enabled)
{
    uint32_t new_state = control_enabled ? MSV_STATE_MANUEL
                                         : MSV_STATE_SCHEDULE;
    Multistate_Value_Present_Value_Set(2, new_state);
}