#ifndef SCHEDULE_PERSIST_H
#define SCHEDULE_PERSIST_H

#include <stdint.h>
#include "esp_err.h"
#include "schedule.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Sauvegarde en FRAM
 * Appelées depuis Schedule_Write_Property() à chaque écriture
 * ================================================================ */

/* Sauvegarde le Weekly Schedule d'un schedule (instance 0 ou 1) */
esp_err_t sched_persist_save_weekly(uint32_t instance,
                                     SCHEDULE_DESCR *desc);

/* Sauvegarde les Special Events d'un schedule (instance 0 ou 1) */
esp_err_t sched_persist_save_special_events(uint32_t instance,
                                             SCHEDULE_DESCR *desc);

/* ================================================================
 * Restauration depuis FRAM
 * Appelée dans app_main() après Init_Service_Handlers()
 * ================================================================ */

/* Restaure Weekly Schedule + Special Events des 2 schedules */
esp_err_t sched_persist_restore_all(void);

#ifdef __cplusplus
}
#endif
#endif /* SCHEDULE_PERSIST_H */