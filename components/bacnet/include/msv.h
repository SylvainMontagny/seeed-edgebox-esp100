/**
 * @file msv.h
 * @brief Multi-State Value object — adapté pour ESP32-S3 / BACnet éclairage public
 * Adapté depuis bacnet-stack/src/bacnet/basic/object/msv.h
 * - BACNET_STACK_EXPORT supprimé
 * - Includes à plat (sans bacnet/...)
 * - Tableau statique simple (sans keylist)
 * - 3 objets MSV :
 *     MSV0 → état zone A (AV0)
 *     MSV1 → état zone B (AV1)
 *     MSV2 → mode BV0 (SCHEDULE / MANUEL)
 */
#ifndef BACNET_BASIC_OBJECT_MULTI_STATE_VALUE_H
#define BACNET_BASIC_OBJECT_MULTI_STATE_VALUE_H

#include <stdbool.h>
#include <stdint.h>
#include "bacdef.h"
#include "bacerror.h"
#include "rp.h"
#include "wp.h"

#ifndef MAX_MULTI_STATE_VALUES
#define MAX_MULTI_STATE_VALUES 3
#endif

/* États MSV0 et MSV1 (zones A et B) */
#define MSV_STATE_ETEINT          1
#define MSV_STATE_DIMMING         2
#define MSV_STATE_NORMAL          3
#define MSV_STATE_PLEIN_ECLAIRAGE 4

/* États MSV2 (mode BV0) */
#define MSV_STATE_SCHEDULE        1
#define MSV_STATE_MANUEL          2

#ifdef __cplusplus
extern "C" {
#endif

void Multistate_Value_Property_Lists(
    const int **pRequired,
    const int **pOptional,
    const int **pProprietary);

bool Multistate_Value_Valid_Instance(uint32_t object_instance);
unsigned Multistate_Value_Count(void);
uint32_t Multistate_Value_Index_To_Instance(unsigned index);
unsigned Multistate_Value_Instance_To_Index(uint32_t instance);

int  Multistate_Value_Read_Property(BACNET_READ_PROPERTY_DATA *rpdata);
bool Multistate_Value_Write_Property(BACNET_WRITE_PROPERTY_DATA *wp_data);

bool Multistate_Value_Object_Name(
    uint32_t object_instance,
    BACNET_CHARACTER_STRING *object_name);

uint32_t Multistate_Value_Present_Value(uint32_t object_instance);
bool     Multistate_Value_Present_Value_Set(uint32_t object_instance,
                                            uint32_t value);

bool Multistate_Value_Out_Of_Service(uint32_t object_instance);
void Multistate_Value_Out_Of_Service_Set(uint32_t object_instance, bool value);

uint32_t    Multistate_Value_Max_States(uint32_t object_instance);
const char *Multistate_Value_State_Text(uint32_t object_instance,
                                        uint32_t state_index);

void Multistate_Value_Init(void);

/**
 * @brief Met à jour MSV0 ou MSV1 en fonction de la valeur d'un AV (0..100)
 * @param av_instance  0 → MSV0, 1 → MSV1
 * @param av_value     valeur présente de l'AV (0.0 à 100.0)
 */
void Multistate_Value_Update_From_AV(uint32_t av_instance, float av_value);

/**
 * @brief Met à jour MSV2 en fonction de l'état de BV0
 * @param control_enabled  true = MANUEL (BV0=ACTIVE), false = SCHEDULE
 */
void Multistate_Value_Update_From_BV(bool control_enabled);

#ifdef __cplusplus
}
#endif
#endif /* BACNET_BASIC_OBJECT_MULTI_STATE_VALUE_H */