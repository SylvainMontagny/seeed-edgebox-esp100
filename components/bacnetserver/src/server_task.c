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

#define OBJECT_TABLE_BYTE_SIZE 4194304


static const char *TAG = "server";

void Setup_Default_Weekly_Schedule(void);

/* Buffer used for receiving */
static uint8_t rx_buffer[MAX_MPDU];
object_functions_t added_object;
void server_task(void *arg)
{
    /* Source address of incoming packet */
    BACNET_ADDRESS src = { 0 };

    /* Received PDU length */
    uint16_t pdu_len = 0;
	
    for (;;) {
        pdu_len = datalink_receive(&src, rx_buffer, MAX_MPDU, 5000);
        if (pdu_len) {
            npdu_handler(&src, rx_buffer, pdu_len);
        }
    }
}

void Init_Service_Handlers(void)
{

    heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Free heap before creating object table: %d bytes", heap_caps_get_free_size(MALLOC_CAP_8BIT));

  
    object_functions_t *Object_Table = (object_functions_t *)heap_caps_malloc(
        OBJECT_TABLE_BYTE_SIZE,
        MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM
    );

    create_bacnet_object(OBJECT_DEVICE, &Object_Table[0], 0);
    create_bacnet_object(OBJECT_SCHEDULE, &Object_Table[0], 1);
    create_bacnet_object(OBJECT_TRENDLOG, &Object_Table[0], 2);
    create_bacnet_object(OBJECT_ANALOG_VALUE, &Object_Table[0], 3);

    Device_Init(&Object_Table[0]);

    heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Free heap after creating object table: %d bytes", heap_caps_get_free_size(MALLOC_CAP_8BIT));

    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_IS, handler_who_is);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_HAS, handler_who_has);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_I_AM, handler_i_am_bind);
    apdu_set_unrecognized_service_handler_handler(handler_unrecognized_service);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROPERTY, handler_read_property);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROP_MULTIPLE, handler_read_property_multiple);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_WRITE_PROPERTY, handler_write_property);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_WRITE_PROP_MULTIPLE, handler_write_property_multiple);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_RANGE, handler_read_range);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_REINITIALIZE_DEVICE, handler_reinitialize_device);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_SUBSCRIBE_COV, handler_cov_subscribe);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_DEVICE_COMMUNICATION_CONTROL, handler_device_communication_control);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_UTC_TIME_SYNCHRONIZATION, handler_timesync_utc);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_TIME_SYNCHRONIZATION,     handler_timesync);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_COV_NOTIFICATION,         handler_ucov_notification);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_PRIVATE_TRANSFER,         handler_unconfirmed_private_transfer);

    /* Populate a default weekly schedule on server start */
    Setup_Default_Weekly_Schedule();
}

/**
 * @brief Creates and initializes a BACnet object based on the specified object type.
 *
 * This function initializes the appropriate BACnet object by calling its corresponding
 * initialization function. It supports various BACnet object types such as Device,
 * Analog Value, Binary Value, Schedule, Calendar, Trend Log, and Multi-State Value.
 *
 * @param object_type The BACnet object type to create and initialize.
 * @param object_functions Pointer to the object functions structure.
 * @param IndexNum The index number where the object should be added in the table.
 */
void create_bacnet_object(BACNET_OBJECT_TYPE object_type, object_functions_t *object_table, uint16_t IndexNum)
{
    switch (object_type)
    {      
        case OBJECT_DEVICE:
          added_object = (object_functions_t){OBJECT_DEVICE, NULL, Device_Count, Device_Index_To_Instance,
          Device_Valid_Object_Instance_Number, Device_Object_Name,
          Device_Read_Property_Local, Device_Write_Property_Local,
          Device_Property_Lists, NULL,NULL, NULL, NULL, NULL, NULL };
            break;
        case OBJECT_ANALOG_VALUE:
             added_object  =  (object_functions_t){OBJECT_ANALOG_VALUE, Analog_Value_Init, Analog_Value_Count,
            Analog_Value_Index_To_Instance, Analog_Value_Valid_Instance,
            Analog_Value_Object_Name, Analog_Value_Read_Property,
            Analog_Value_Write_Property, Analog_Value_Property_Lists,
            NULL,NULL, NULL, NULL, NULL, NULL };
            break;
        case OBJECT_SCHEDULE:
             added_object  = (object_functions_t){OBJECT_SCHEDULE, Schedule_Init, Schedule_Count,
      Schedule_Index_To_Instance, Schedule_Valid_Instance,
      Schedule_Object_Name, Schedule_Read_Property, Schedule_Write_Property,
      (rpm_property_lists_function)Schedule_Property_Lists,
      NULL,NULL, NULL, NULL, NULL, NULL };
            break;

        case OBJECT_TRENDLOG:
             added_object  =  (object_functions_t){OBJECT_TRENDLOG, Trend_Log_Init, Trend_Log_Count,
      Trend_Log_Index_To_Instance, Trend_Log_Valid_Instance,
      Trend_Log_Object_Name, Trend_Log_Read_Property, Trend_Log_Write_Property,
      (rpm_property_lists_function)Trend_Log_Property_Lists,
      TrendLogGetRRInfo,NULL, NULL, NULL, NULL, NULL };
    
            break;
        default:
            ESP_LOGW(TAG, "Unknown BACnet object type: %d", object_type);
            break;
    }
        object_table[IndexNum] = added_object; 
}

/*  Setup_Default_Weekly_Schedule
 * Creates a weekly schedule for each schedule object with the
 * following active periods (every day):
 *  - 00:00 to 01:00
 *  - 05:00 to 10:00
 *  - 17:00 to 23:59 (continues into next day's 00:00-01:00) */
void Setup_Default_Weekly_Schedule(void)
{
    unsigned sch_idx;
    unsigned sch_count = Schedule_Count();

    for (sch_idx = 0; sch_idx < sch_count; sch_idx++) {
        SCHEDULE_DESCR *desc = Schedule_Object((uint32_t)sch_idx);
        if (!desc) continue;

        for (int day = 0; day < BACNET_WEEKLY_SCHEDULE_SIZE; day++) {
            BACNET_DAILY_SCHEDULE *ds = &desc->Weekly_Schedule[day];
            ds->TV_Count = 0;
            int idx = 0;

            /* 00:00 -> ON (100%) */
            ds->Time_Values[idx].Time.hour = 0; ds->Time_Values[idx].Time.min = 0;
            ds->Time_Values[idx].Time.sec = 0; ds->Time_Values[idx].Time.hundredths = 0;
            ds->Time_Values[idx].Value.tag = BACNET_APPLICATION_TAG_REAL;
            ds->Time_Values[idx].Value.type.Real = 100.0f; idx++;

            /* 01:00 -> OFF (0%) */
            ds->Time_Values[idx].Time.hour = 1; ds->Time_Values[idx].Time.min = 0;
            ds->Time_Values[idx].Time.sec = 0; ds->Time_Values[idx].Time.hundredths = 0;
            ds->Time_Values[idx].Value.tag = BACNET_APPLICATION_TAG_REAL;
            ds->Time_Values[idx].Value.type.Real = 0.0f; idx++;

            /* 05:00 -> ON (100%) */
            ds->Time_Values[idx].Time.hour = 5; ds->Time_Values[idx].Time.min = 0;
            ds->Time_Values[idx].Time.sec = 0; ds->Time_Values[idx].Time.hundredths = 0;
            ds->Time_Values[idx].Value.tag = BACNET_APPLICATION_TAG_REAL;
            ds->Time_Values[idx].Value.type.Real = 100.0f; idx++;

            /* 10:00 -> OFF (0%) */
            ds->Time_Values[idx].Time.hour = 10; ds->Time_Values[idx].Time.min = 0;
            ds->Time_Values[idx].Time.sec = 0; ds->Time_Values[idx].Time.hundredths = 0;
            ds->Time_Values[idx].Value.tag = BACNET_APPLICATION_TAG_REAL;
            ds->Time_Values[idx].Value.type.Real = 0.0f; idx++;

            /* 17:00 -> ON (100%) — remains ON until next day's 01:00 */
            if (idx < (int)(sizeof(ds->Time_Values) / sizeof(ds->Time_Values[0]))) {
                ds->Time_Values[idx].Time.hour = 17; ds->Time_Values[idx].Time.min = 0;
                ds->Time_Values[idx].Time.sec = 0; ds->Time_Values[idx].Time.hundredths = 0;
                ds->Time_Values[idx].Value.tag = BACNET_APPLICATION_TAG_REAL;
                ds->Time_Values[idx].Value.type.Real = 100.0f; idx++;
            }

            ds->TV_Count = (uint16_t)idx;
        }
    }
    ESP_LOGI(TAG, "Default weekly schedules set for %u schedule(s)", sch_count);
}
