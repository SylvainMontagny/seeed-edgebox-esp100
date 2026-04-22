#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "bacdef.h"
#include "bacdcode.h"
#include "bacapp.h"
#include "rp.h"
#include "wp.h"
#include "calendar.h"
#include "esp_log.h"

static const char *TAG = "calendar";

/* ── Tableau statique — pas de malloc, pas de keylist ── */
static CALENDAR_DESCR Calendar_Descr[MAX_CALENDARS];

/* ── Propriétés BACnet ── */
static const int Calendar_Properties_Required[] = {
    PROP_OBJECT_IDENTIFIER,
    PROP_OBJECT_NAME,
    PROP_OBJECT_TYPE,
    PROP_PRESENT_VALUE,
    PROP_DATE_LIST,
    -1
};

static const int Calendar_Properties_Optional[] = {
    PROP_DESCRIPTION,
    -1
};

static const int Calendar_Properties_Proprietary[] = { -1 };

void Calendar_Property_Lists(
    const int **pRequired,
    const int **pOptional,
    const int **pProprietary)
{
    if (pRequired)    *pRequired    = Calendar_Properties_Required;
    if (pOptional)    *pOptional    = Calendar_Properties_Optional;
    if (pProprietary) *pProprietary = Calendar_Properties_Proprietary;
}

/* ================================================================
 * Calendar_Init
 * Initialise tous les calendriers + ajoute des dates de test
 * ================================================================ */
void Calendar_Init(void)
{
    unsigned i;
    for (i = 0; i < MAX_CALENDARS; i++) {
        snprintf(Calendar_Descr[i].Name, sizeof(Calendar_Descr[i].Name),
                 "CALENDAR %u", i);
        Calendar_Descr[i].Entry_Count   = 0;
        Calendar_Descr[i].Present_Value = false;
        Calendar_Descr[i].Write_Enabled = true;

        memset(Calendar_Descr[i].Entries, 0,
               sizeof(Calendar_Descr[i].Entries));
    }

    /* === Dates de test codées en dur dans Calendar 0 ===
     * Exemple : 14 juillet (récurrent tous les ans) et 25 décembre
     * Décommenter pour tester
    {
        CALENDAR_ENTRY e;

        // 14 juillet — récurrent (year=255 = wildcard)
        e.tag = BACNET_CALENDAR_DATE;
        e.type.Date.year  = 255;   // wildcard = tous les ans
        e.type.Date.month = 7;
        e.type.Date.day   = 14;
        e.type.Date.wday  = 255;   // wildcard
        Calendar_Date_List_Add(0, &e);

        // 25 décembre — récurrent
        e.tag = BACNET_CALENDAR_DATE;
        e.type.Date.year  = 255;
        e.type.Date.month = 12;
        e.type.Date.day   = 25;
        e.type.Date.wday  = 255;
        Calendar_Date_List_Add(0, &e);

        Calendar_Update_Present_Value(0);
    }
    */

    ESP_LOGI(TAG, "Calendar_Init : %d calendar(s) initialisé(s)", MAX_CALENDARS);
}

/* ── Fonctions de navigation ── */
unsigned Calendar_Count(void) { return MAX_CALENDARS; }

uint32_t Calendar_Index_To_Instance(unsigned index)
{
    return (index < MAX_CALENDARS) ? index : MAX_CALENDARS;
}

unsigned Calendar_Instance_To_Index(uint32_t object_instance)
{
    return (object_instance < MAX_CALENDARS) ? object_instance : MAX_CALENDARS;
}

bool Calendar_Valid_Instance(uint32_t object_instance)
{
    return (object_instance < MAX_CALENDARS);
}

bool Calendar_Object_Name(uint32_t object_instance,
    BACNET_CHARACTER_STRING *object_name)
{
    if (!Calendar_Valid_Instance(object_instance)) return false;
    characterstring_init_ansi(object_name,
        Calendar_Descr[object_instance].Name);
    return true;
}

/* ── Accès à la liste de dates ── */
int Calendar_Date_List_Count(uint32_t object_instance)
{
    if (!Calendar_Valid_Instance(object_instance)) return 0;
    return Calendar_Descr[object_instance].Entry_Count;
}

CALENDAR_ENTRY *Calendar_Date_List_Get(uint32_t object_instance, uint8_t index)
{
    if (!Calendar_Valid_Instance(object_instance)) return NULL;
    if (index >= Calendar_Descr[object_instance].Entry_Count) return NULL;
    return &Calendar_Descr[object_instance].Entries[index];
}

bool Calendar_Date_List_Add(uint32_t object_instance,
    const CALENDAR_ENTRY *entry)
{
    CALENDAR_DESCR *cal;
    if (!Calendar_Valid_Instance(object_instance)) return false;
    cal = &Calendar_Descr[object_instance];
    if (cal->Entry_Count >= MAX_CALENDAR_ENTRIES) return false;
    cal->Entries[cal->Entry_Count] = *entry;
    cal->Entry_Count++;
    return true;
}

bool Calendar_Date_List_Delete_All(uint32_t object_instance)
{
    if (!Calendar_Valid_Instance(object_instance)) return false;
    Calendar_Descr[object_instance].Entry_Count = 0;
    memset(Calendar_Descr[object_instance].Entries, 0,
           sizeof(Calendar_Descr[object_instance].Entries));
    return true;
}

/* ================================================================
 * calendar_entry_matches
 * Vérifie si la date du jour correspond à une CalendarEntry
 * ================================================================ */
static bool calendar_entry_matches(const CALENDAR_ENTRY *entry,
    struct tm *t)
{
    if (!entry || !t) return false;

    switch (entry->tag) {

        case BACNET_CALENDAR_DATE: {
            const BACNET_DATE *d = &entry->type.Date;
            int year_ok  = (d->year == 255) ||
                           ((int)(d->year + 1900) == (t->tm_year + 1900));
            int month_ok = (d->month == 255) ||
                           (d->month == (uint8_t)(t->tm_mon + 1));
            int day_ok   = (d->day == 255) ||
                           (d->day == (uint8_t)t->tm_mday);
            return (year_ok && month_ok && day_ok);
        }

        case BACNET_CALENDAR_DATE_RANGE: {
            int cur = (t->tm_year + 1900) * 10000 +
                      (t->tm_mon + 1) * 100 +
                      t->tm_mday;

            const BACNET_DATE *s = &entry->type.DateRange.startdate;
            const BACNET_DATE *e = &entry->type.DateRange.enddate;

            int sy = (s->year == 255) ? (t->tm_year + 1900) : (int)(s->year + 1900);
            int ey = (e->year == 255) ? (t->tm_year + 1900) : (int)(e->year + 1900);

            int start = sy * 10000 + s->month * 100 + s->day;
            int end   = ey * 10000 + e->month * 100 + e->day;

            return (cur >= start && cur <= end);
        }

        case BACNET_CALENDAR_WEEK_N_DAY: {
            int month_ok = (entry->type.WeekNDay.month == 255) ||
                           (entry->type.WeekNDay.month ==
                            (uint8_t)(t->tm_mon + 1));
            uint8_t bac_dow = (t->tm_wday == 0) ? 7 : (uint8_t)t->tm_wday;
            int day_ok = (entry->type.WeekNDay.dayofweek == 255) ||
                         (entry->type.WeekNDay.dayofweek == bac_dow);
            return (month_ok && day_ok);
        }

        default:
            return false;
    }
}

/* ================================================================
 * Calendar_Update_Present_Value
 * Recalcule Present_Value en comparant la date du jour aux entrées
 * Appelée depuis schedule_task() toutes les secondes
 * ================================================================ */
void Calendar_Update_Present_Value(uint32_t object_instance)
{
    unsigned i;
    time_t now;
    struct tm *t;

    if (!Calendar_Valid_Instance(object_instance)) return;

    now = time(NULL);
    t   = localtime(&now);

    Calendar_Descr[object_instance].Present_Value = false;

    for (i = 0; i < Calendar_Descr[object_instance].Entry_Count; i++) {
        if (calendar_entry_matches(
                &Calendar_Descr[object_instance].Entries[i], t)) {
            Calendar_Descr[object_instance].Present_Value = true;
            break;
        }
    }
}

/* ================================================================
 * Calendar_Present_Value
 * Retourne la Present Value calculée (BOOLEAN BACnet)
 * ================================================================ */
bool Calendar_Present_Value(uint32_t object_instance)
{
    if (!Calendar_Valid_Instance(object_instance)) return false;
    return Calendar_Descr[object_instance].Present_Value;
}

/* ================================================================
 * Encodage manuel d'une CalendarEntry dans un buffer APDU
 * ================================================================ */
static int encode_calendar_entry(uint8_t *apdu,
    const CALENDAR_ENTRY *entry)
{
    int len = 0;
    if (!entry) return 0;

    switch (entry->tag) {
        case BACNET_CALENDAR_DATE:
            /* context tag 0 — date */
            if (apdu) {
                len += encode_context_date(apdu + len, 0,
                    (BACNET_DATE *)&entry->type.Date);
            } else {
                len += encode_context_date(NULL, 0,
                    (BACNET_DATE *)&entry->type.Date);
            }
            break;

        case BACNET_CALENDAR_DATE_RANGE:
            /* context tag 1 — opening + startdate + enddate + closing */
            if (apdu) {
                len += encode_opening_tag(apdu + len, 1);
                len += encode_application_date(apdu + len,
                    (BACNET_DATE *)&entry->type.DateRange.startdate);
                len += encode_application_date(apdu + len,
                    (BACNET_DATE *)&entry->type.DateRange.enddate);
                len += encode_closing_tag(apdu + len, 1);
            } else {
                len += encode_opening_tag(NULL, 1);
                len += encode_application_date(NULL,
                    (BACNET_DATE *)&entry->type.DateRange.startdate);
                len += encode_application_date(NULL,
                    (BACNET_DATE *)&entry->type.DateRange.enddate);
                len += encode_closing_tag(NULL, 1);
            }
            break;

        case BACNET_CALENDAR_WEEK_N_DAY:
            /* context tag 2 — octet string de 3 octets */
            if (apdu) {
                apdu[len++] = 0x2C; /* context tag 2, length 3 */
                apdu[len++] = entry->type.WeekNDay.month;
                apdu[len++] = entry->type.WeekNDay.weekofmonth;
                apdu[len++] = entry->type.WeekNDay.dayofweek;
            } else {
                len += 4;
            }
            break;

        default:
            break;
    }
    return len;
}

/* ================================================================
 * Décodage d'une CalendarEntry depuis un buffer APDU
 * Retourne le nombre d'octets consommés, ou -1 sur erreur
 * ================================================================ */
static int decode_calendar_entry(const uint8_t *apdu, int apdu_len,
    CALENDAR_ENTRY *entry)
{
    uint8_t tag_number = 0;
    uint32_t len_value = 0;
    int len = 0;
    int tag_len = 0;

    if (!apdu || apdu_len <= 0 || !entry) return -1;

    tag_len = decode_tag_number_and_value(
        (uint8_t *)apdu + len, &tag_number, &len_value);
    len += tag_len;

    switch (tag_number) {
        case 0: /* date */
            entry->tag = BACNET_CALENDAR_DATE;
            len += decode_date((uint8_t *)apdu + len,
                &entry->type.Date);
            break;

        case 1: /* date range — opening tag */
            entry->tag = BACNET_CALENDAR_DATE_RANGE;
            /* startdate */
            tag_len = decode_tag_number_and_value(
                (uint8_t *)apdu + len, &tag_number, &len_value);
            len += tag_len;
            len += decode_date((uint8_t *)apdu + len,
                &entry->type.DateRange.startdate);
            /* enddate */
            tag_len = decode_tag_number_and_value(
                (uint8_t *)apdu + len, &tag_number, &len_value);
            len += tag_len;
            len += decode_date((uint8_t *)apdu + len,
                &entry->type.DateRange.enddate);
            /* closing tag 1 */
            len++;
            break;

        case 2: /* week-n-day — 3 octets */
            entry->tag = BACNET_CALENDAR_WEEK_N_DAY;
            if (len_value >= 3) {
                entry->type.WeekNDay.month       = apdu[len];
                entry->type.WeekNDay.weekofmonth = apdu[len + 1];
                entry->type.WeekNDay.dayofweek   = apdu[len + 2];
                len += 3;
            }
            break;

        default:
            return -1;
    }
    return len;
}

/* ================================================================
 * Calendar_Read_Property
 * ================================================================ */
int Calendar_Read_Property(BACNET_READ_PROPERTY_DATA *rpdata)
{
    int apdu_len = 0;
    int len = 0;
    uint8_t *apdu;
    BACNET_CHARACTER_STRING char_string;
    BACNET_BIT_STRING bit_string;
    CALENDAR_DESCR *cal;
    unsigned i;

    if (!rpdata || !rpdata->application_data ||
        rpdata->application_data_len == 0)
        return 0;

    if (!Calendar_Valid_Instance(rpdata->object_instance))
        return BACNET_STATUS_ERROR;

    cal  = &Calendar_Descr[rpdata->object_instance];
    apdu = rpdata->application_data;

    switch ((int)rpdata->object_property) {

        case PROP_OBJECT_IDENTIFIER:
            apdu_len = encode_application_object_id(apdu,
                OBJECT_CALENDAR, rpdata->object_instance);
            break;

        case PROP_OBJECT_NAME:
            characterstring_init_ansi(&char_string, cal->Name);
            apdu_len = encode_application_character_string(apdu, &char_string);
            break;

        case PROP_OBJECT_TYPE:
            apdu_len = encode_application_enumerated(apdu, OBJECT_CALENDAR);
            break;

        case PROP_PRESENT_VALUE:
            /* Present_Value est un BOOLEAN BACnet */
            apdu_len = encode_application_boolean(apdu, cal->Present_Value);
            break;

        case PROP_DATE_LIST:
            /* Encode toutes les entrées de la liste */
            for (i = 0; i < cal->Entry_Count; i++) {
                len = encode_calendar_entry(
                    apdu ? apdu + apdu_len : NULL,
                    &cal->Entries[i]);
                if (len < 0) return BACNET_STATUS_ERROR;
                apdu_len += len;
            }
            break;

        case PROP_DESCRIPTION:
            characterstring_init_ansi(&char_string, "Jours feries");
            apdu_len = encode_application_character_string(apdu, &char_string);
            break;

        case PROP_STATUS_FLAGS:
            bitstring_init(&bit_string);
            bitstring_set_bit(&bit_string, STATUS_FLAG_IN_ALARM,      false);
            bitstring_set_bit(&bit_string, STATUS_FLAG_FAULT,         false);
            bitstring_set_bit(&bit_string, STATUS_FLAG_OVERRIDDEN,    false);
            bitstring_set_bit(&bit_string, STATUS_FLAG_OUT_OF_SERVICE, false);
            apdu_len = encode_application_bitstring(apdu, &bit_string);
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
 * Calendar_Write_Property
 * Seule PROP_DATE_LIST est écrivable
 * ================================================================ */
bool Calendar_Write_Property(BACNET_WRITE_PROPERTY_DATA *wp_data)
{
    CALENDAR_DESCR *cal;
    int offset = 0;
    int decode_len = 0;
    CALENDAR_ENTRY entry;

    if (!wp_data) return false;
    if (!Calendar_Valid_Instance(wp_data->object_instance)) return false;

    cal = &Calendar_Descr[wp_data->object_instance];

    switch ((int)wp_data->object_property) {

        case PROP_DATE_LIST:
            if (!cal->Write_Enabled) {
                wp_data->error_class = ERROR_CLASS_PROPERTY;
                wp_data->error_code  = ERROR_CODE_WRITE_ACCESS_DENIED;
                return false;
            }
            /* Effacer la liste existante puis décoder les nouvelles entrées */
            Calendar_Date_List_Delete_All(wp_data->object_instance);
            offset = 0;
            while (offset < wp_data->application_data_len &&
                   cal->Entry_Count < MAX_CALENDAR_ENTRIES) {
                decode_len = decode_calendar_entry(
                    wp_data->application_data + offset,
                    wp_data->application_data_len - offset,
                    &entry);
                if (decode_len <= 0) break;
                Calendar_Date_List_Add(wp_data->object_instance, &entry);
                offset += decode_len;
            }
            /* Recalculer la Present Value immédiatement */
            Calendar_Update_Present_Value(wp_data->object_instance);
            ESP_LOGI(TAG, "Calendar %lu mis à jour — %d entrée(s)",
                     (unsigned long)wp_data->object_instance,
                     cal->Entry_Count);
            return true;

        default:
            wp_data->error_class = ERROR_CLASS_PROPERTY;
            wp_data->error_code  = ERROR_CODE_WRITE_ACCESS_DENIED;
            return false;
    }
}