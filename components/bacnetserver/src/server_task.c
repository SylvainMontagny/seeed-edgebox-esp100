#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_netif.h"
#include "ping/ping_sock.h"
#include "lwip/inet.h"
#include "server_task.h"
#include "led.h"
#include "device.h"
#include "trendlog.h"
#include "msv.h"
#include "calendar.h"
#include "config.h"
#include "address.h"
#include "bacdef.h"
#include "handlers.h"
#include "client.h"
#include "dlenv.h"
#include "bacdcode.h"
#include "npdu.h"
#include "apdu.h"
#include "iam.h"
#include "tsm.h"
#include "datalink.h"
#include "dcc.h"
#include "getevent.h"
#include "net.h"
#include "txbuf.h"
#include "version.h"
#include "av.h"
#include "bv.h"
#include "schedule.h"
#include "sdkconfig.h"
#include "driver/ledc.h"
#include <time.h>
#include "rtc_fram_manager.h"
#include "schedule_persist.h"
#include "solar.h"
#include "Http_server.h"
#include "wifi.h"
#include "modem.h"
#include "ntp.h"
#include "gpiooutputs.h"


static const char *TAG = "server";

/** Buffer used for receiving */
static uint8_t rx_buffer[MAX_MPDU] = { 0 };

void server_task(void *arg)
{
	// Structure pour stocker l'adresse de celui qui nous envoie un message
    BACNET_ADDRESS src = {
        0
    }; 
    
    // Variable pour stocker la taille du message reçu
    uint16_t pdu_len = 0;
	
    for (;;) {
		// On attend de recevoir des données sur le réseau
        pdu_len = datalink_receive(&src, &rx_buffer[0], MAX_MPDU, 5000);

		// Si on a reçu quelque chose (taille > 0)
        if (pdu_len) {
			// On envoie le message au gestionnaire BACnet (Handler)
            npdu_handler(&src, &rx_buffer[0], pdu_len);
				//printf("Paquet recu ! Analog Value (0) = %f\n", Analog_Value_Present_Value(0));

           // if (Analog_Value_Present_Value(0) == 1)
            //{
           //     led_on();
            //}
            //else
           // {
           //     led_off();
           // }
        }
    }
}


/* ================================================================
 * Modification 1 : Remplir le Weekly Schedule avec les heures solaires
 *
 * Pour chaque jour (lundi=0 … dimanche=6) on programme :
 *   TV[0] = heure coucher soleil → 100.0 % (allumage)
 *   TV[1] = heure lever  soleil → 0.0 %   (extinction)
 *
 * Visible dans YABE → Schedule → Weekly Schedule
 * Mis à jour chaque matin à minuit (invalider cache → recalcul)
 * ================================================================ */
 void schedule_update_solar_times(void)
{
    solar_times_t st = solar_get_today();
    if (!st.valid) {
        ESP_LOGW(TAG, "[SOLAR-SCH] Heures invalides — weekly non mis à jour");
        return;
    }

    ESP_LOGI(TAG, "[SOLAR-SCH] Mise à jour Weekly : coucher %02d:%02d → 100%% | lever %02d:%02d → 0%%",
             st.sunset_h, st.sunset_m, st.sunrise_h, st.sunrise_m);

    for (int sch = 0; sch < 2; sch++) {
        SCHEDULE_DESCR *desc = Schedule_Object((uint32_t)sch);
        if (!desc) continue;

        /* Remplir les 7 jours BACnet (index 0=Lundi … 6=Dimanche)
         * BACNET_WEEKLY_SCHEDULE_SIZE peut être > 7 (défini à 10)
         * On remplit seulement les 7 premiers slots (un par jour)    */
        for (int day = 0; day < 7; day++) {
            BACNET_DAILY_SCHEDULE *ds = &desc->Weekly_Schedule[day];
            ds->TV_Count = 2;

            /* Entrée 0 : coucher du soleil → allumage à 100% */
            ds->Time_Values[0].Time.hour       = st.sunset_h;
            ds->Time_Values[0].Time.min        = st.sunset_m;
            ds->Time_Values[0].Time.sec        = 0;
            ds->Time_Values[0].Time.hundredths = 0;
            ds->Time_Values[0].Value.tag        = BACNET_APPLICATION_TAG_REAL;
            ds->Time_Values[0].Value.type.Real  = 100.0f;

            /* Entrée 1 : lever du soleil → extinction à 0% */
            ds->Time_Values[1].Time.hour       = st.sunrise_h;
            ds->Time_Values[1].Time.min        = st.sunrise_m;
            ds->Time_Values[1].Time.sec        = 0;
            ds->Time_Values[1].Time.hundredths = 0;
            ds->Time_Values[1].Value.tag        = BACNET_APPLICATION_TAG_REAL;
            ds->Time_Values[1].Value.type.Real  = 0.0f;
        }
    }
}





void Init_Service_Handlers(void)
{

    static object_functions_t Object_Table[] = {
    { OBJECT_DEVICE, NULL, Device_Count, Device_Index_To_Instance,
      Device_Valid_Object_Instance_Number, Device_Object_Name,
      Device_Read_Property_Local, Device_Write_Property_Local,
      Device_Property_Lists, NULL, NULL, NULL, NULL, NULL, NULL },
    { OBJECT_ANALOG_VALUE, Analog_Value_Init, Analog_Value_Count,
      Analog_Value_Index_To_Instance, Analog_Value_Valid_Instance,
      Analog_Value_Object_Name, Analog_Value_Read_Property,
      Analog_Value_Write_Property, Analog_Value_Property_Lists,
      NULL, NULL, NULL, NULL, NULL, NULL },
    { OBJECT_BINARY_VALUE, Binary_Value_Init, Binary_Value_Count,
      Binary_Value_Index_To_Instance, Binary_Value_Valid_Instance,
      Binary_Value_Object_Name, Binary_Value_Read_Property,
      Binary_Value_Write_Property, Binary_Value_Property_Lists,
      NULL, NULL, NULL, NULL, NULL, NULL },
    { OBJECT_SCHEDULE, Schedule_Init, Schedule_Count,
      Schedule_Index_To_Instance, Schedule_Valid_Instance,
      Schedule_Object_Name, Schedule_Read_Property, Schedule_Write_Property,
      (rpm_property_lists_function)Schedule_Property_Lists,
      NULL, NULL, NULL, NULL, NULL, NULL },
    { OBJECT_MULTI_STATE_VALUE, Multistate_Value_Init, Multistate_Value_Count,
      Multistate_Value_Index_To_Instance, Multistate_Value_Valid_Instance,
      Multistate_Value_Object_Name, Multistate_Value_Read_Property,
      Multistate_Value_Write_Property, Multistate_Value_Property_Lists,
      NULL, NULL, NULL, NULL, NULL, NULL },
    { OBJECT_CALENDAR, Calendar_Init, Calendar_Count,
      Calendar_Index_To_Instance, Calendar_Valid_Instance,
      Calendar_Object_Name, Calendar_Read_Property, Calendar_Write_Property,
      (rpm_property_lists_function)Calendar_Property_Lists,
      NULL, NULL, NULL, NULL, NULL, NULL },
    { OBJECT_TRENDLOG, Trend_Log_Init, Trend_Log_Count,
      Trend_Log_Index_To_Instance, Trend_Log_Valid_Instance,
      Trend_Log_Object_Name, Trend_Log_Read_Property, Trend_Log_Write_Property,
      (rpm_property_lists_function)Trend_Log_Property_Lists,
      TrendLogGetRRInfo, NULL, NULL, NULL, NULL, NULL },
    { MAX_BACNET_OBJECT_TYPE, NULL, NULL, NULL, NULL, NULL,
      NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};
    Device_Init(&Object_Table[0]);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_IS, handler_who_is);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_HAS, handler_who_has);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_I_AM, handler_i_am_bind);
    apdu_set_unrecognized_service_handler_handler(handler_unrecognized_service);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROPERTY,      handler_read_property);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROP_MULTIPLE,  handler_read_property_multiple);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_WRITE_PROPERTY,     handler_write_property);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_WRITE_PROP_MULTIPLE, handler_write_property_multiple);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_RANGE,         handler_read_range);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_REINITIALIZE_DEVICE, handler_reinitialize_device);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_SUBSCRIBE_COV,      handler_cov_subscribe);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_DEVICE_COMMUNICATION_CONTROL, handler_device_communication_control);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_UTC_TIME_SYNCHRONIZATION, handler_timesync_utc);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_TIME_SYNCHRONIZATION,     handler_timesync);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_COV_NOTIFICATION,         handler_ucov_notification);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_PRIVATE_TRANSFER,         handler_unconfirmed_private_transfer);
}
