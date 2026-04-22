/*
 * rtc_fram_manager.h
 *
 * Module de gestion de la persistance BACnet via FRAM + RTC PCF8563.
 *
 * Fonctions principales :
 *  - rfm_init()              : init I2C, vérifie la magic, formate si vierge
 *  - rfm_save_av_state()     : sauvegarde AV0/AV1
 *  - rfm_load_av_state()     : restaure AV0/AV1
 *  - rfm_save_bv_state()     : sauvegarde mode AUTO/MANUEL
 *  - rfm_load_bv_state()     : restaure mode AUTO/MANUEL
 *  - rfm_sync_rtc_from_ntp() : écrit l'heure NTP dans le RTC + FRAM
 *  - rfm_time_init()         : boot : NTP si 4G dispo, sinon RTC
 *  - rfm_log_event()         : ajoute un événement dans le ring buffer
 *  - rfm_dump()              : affiche tout le contenu FRAM dans les logs
 */

#ifndef RTC_FRAM_MANAGER_H
#define RTC_FRAM_MANAGER_H

#include "esp_err.h"
#include "fram_layout.h"
#include <stdbool.h>

/* ─────────────────────────────────────────────────────────────
 * Initialisation
 * ─────────────────────────────────────────────────────────────*/

/**
 * @brief Initialise le bus I2C, le RTC et la FRAM.
 *        Vérifie la magic FRAM. Formate (écriture magic + reset) si vierge.
 * @return ESP_OK si tout est prêt, ESP_FAIL sinon.
 */
esp_err_t rfm_init(void);

/* ─────────────────────────────────────────────────────────────
 * Persistance AV
 * ─────────────────────────────────────────────────────────────*/

/**
 * @brief Sauvegarde les valeurs présentes AV0 et AV1 dans la FRAM.
 */
esp_err_t rfm_save_av_state(float av0, float av1);

/**
 * @brief Restaure AV0 et AV1 depuis la FRAM.
 * @param[out] av0  Valeur restaurée pour AV0
 * @param[out] av1  Valeur restaurée pour AV1
 * @return ESP_OK si les données sont valides, ESP_ERR_NOT_FOUND si zone vide.
 */
esp_err_t rfm_load_av_state(float *av0, float *av1);

/* ─────────────────────────────────────────────────────────────
 * Persistance BV (mode AUTO/MANUEL)
 * ─────────────────────────────────────────────────────────────*/

/**
 * @brief Sauvegarde le mode AUTO/MANUEL dans la FRAM.
 * @param manual  true = MANUEL, false = AUTO
 */
esp_err_t rfm_save_bv_state(bool manual);

/**
 * @brief Restaure le mode AUTO/MANUEL depuis la FRAM.
 * @param[out] manual  true = MANUEL, false = AUTO
 * @return ESP_OK si valide, ESP_ERR_NOT_FOUND si zone vide.
 */
esp_err_t rfm_load_bv_state(bool *manual);

/* ─────────────────────────────────────────────────────────────
 * Gestion du temps : NTP + RTC
 * ─────────────────────────────────────────────────────────────*/

/**
 * @brief Appelée après une synchro NTP réussie.
 *        Écrit l'heure courante dans le RTC PCF8563 ET dans la FRAM.
 */
void rfm_sync_rtc_from_ntp(void);

/**
 * @brief Initialise l'heure système au boot.
 *
 *  - Si @p ntp_available = true : appelle ntp_initialize(), puis rfm_sync_rtc_from_ntp().
 *  - Si @p ntp_available = false : lit le RTC et initialise l'heure système depuis la FRAM.
 *
 * @param ntp_available true si la connexion 4G est disponible
 */
void rfm_time_init(bool ntp_available);

/* ─────────────────────────────────────────────────────────────
 * Ring buffer événements
 * ─────────────────────────────────────────────────────────────*/

/**
 * @brief Ajoute un événement horodaté dans le ring buffer FRAM.
 * @param type       Type d'événement (fram_event_type_t)
 * @param value      Valeur associée (PV pour AV, 0.0 sinon)
 * @param extra_u8   Byte optionnel (ex. mode 0/1 pour EVENT_MODE_CHANGE)
 */
void rfm_log_event(fram_event_type_t type, float value, uint8_t extra_u8);

/* ─────────────────────────────────────────────────────────────
 * Dump FRAM → logs série
 * ─────────────────────────────────────────────────────────────*/

/**
 * @brief Lit et affiche dans les logs ESP toutes les données FRAM :
 *        magic, AV0/AV1, mode BV, backup RTC, et tous les événements
 *        valides du ring buffer (dans l'ordre chronologique).
 *        Appel typique : après rfm_init() au boot, ou sur commande.
 */
void rfm_dump(void);

#endif /* RTC_FRAM_MANAGER_H */