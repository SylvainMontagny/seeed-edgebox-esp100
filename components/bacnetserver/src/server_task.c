#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>

#include "esp_log.h"
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
#include "device.h"
#include "bi.h"
#include "ai.h"
#include "bv.h"
#include "av.h"
#include "led.h"
#include "server_task.h"
#include "schedule.h"
#include "solar.h"
#include "NTP.h"
#include "rtc_fram_manager.h"

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
static void schedule_update_solar_times(void)
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

