/*
 * rtc_fram_manager.c
 *
 * Module de gestion de la persistance BACnet via FRAM FM24CL64B + RTC PCF8563.
 *
 * Dépendances :
 *   - rtc_pcf8563.h / rtc_pcf8563.c
 *   - fram_fm24cl64b.h / fram_fm24cl64b.c
 *   - fram_layout.h
 *   - esp_sntp.h, esp_log.h, time.h (ESP-IDF)
 */

#include "rtc_fram_manager.h"
#include "rtc_pcf8563.h"
#include "fram_fm24cl64b.h"
#include "fram_layout.h"

#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <time.h>

static const char *TAG = "RFM";

/* ══════════════════════════════════════════════════════════════
 * Helpers internes
 * ══════════════════════════════════════════════════════════════*/

/** Lit l'heure courante du système et remplit une structure rtc */
static void _get_local_time(fram_rtc_backup_t *out)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    out->year  = (uint8_t)(t->tm_year - 100); /* tm_year = années depuis 1900 */
    out->month = (uint8_t)(t->tm_mon + 1);
    out->day   = (uint8_t)t->tm_mday;
    out->hour  = (uint8_t)t->tm_hour;
    out->min   = (uint8_t)t->tm_min;
    out->sec   = (uint8_t)t->tm_sec;
    out->valid = 0xAA;
}

/** Applique une structure rtc_backup_t à l'heure système (settimeofday) */
static void _apply_rtc_to_system(const fram_rtc_backup_t *rtc)
{
    struct tm t = {
        .tm_year  = rtc->year + 100, /* 2026 → 126 */
        .tm_mon   = rtc->month - 1,
        .tm_mday  = rtc->day,
        .tm_hour  = rtc->hour,
        .tm_min   = rtc->min,
        .tm_sec   = rtc->sec,
        .tm_isdst = -1
    };
    time_t epoch = mktime(&t);
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "[RTC→SYS] %02d/%02d/%02d %02d:%02d:%02d",
             rtc->day, rtc->month, rtc->year,
             rtc->hour, rtc->min, rtc->sec);
}

/* ══════════════════════════════════════════════════════════════
 * rfm_init
 * ══════════════════════════════════════════════════════════════*/
esp_err_t rfm_init(void)
{
    /* Init I2C + RTC */
    esp_err_t ret = pcf8563_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Échec init PCF8563 : %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "PCF8563 OK");

    /* Vérification magic FRAM */
    uint32_t magic = 0;
    ret = fram_read(FRAM_MAGIC_ADDR, (uint8_t *)&magic, sizeof(magic));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Échec lecture FRAM magic : %s", esp_err_to_name(ret));
        return ret;
    }

    if (magic != FRAM_MAGIC_VALUE) {
        ESP_LOGW(TAG, "FRAM vierge (magic=0x%08lX) — formatage...", (unsigned long)magic);

        /* Écriture de la magic */
        uint32_t m = FRAM_MAGIC_VALUE;
        ret = fram_write(FRAM_MAGIC_ADDR, (uint8_t *)&m, sizeof(m));
        if (ret != ESP_OK) return ret;

        /* Remise à zéro des zones d'état */
        uint8_t zeros[16] = {0};
        fram_write(FRAM_AV_STATE_ADDR, zeros, sizeof(fram_av_state_t));
        fram_write(FRAM_BV_STATE_ADDR, zeros, sizeof(fram_bv_state_t));
        fram_write(FRAM_RTC_BACKUP_ADDR, zeros, sizeof(fram_rtc_backup_t));

        /* Head du ring buffer à 0 */
        uint16_t head = 0;
        fram_write(FRAM_EVENT_HEAD_ADDR, (uint8_t *)&head, sizeof(head));

        ESP_LOGI(TAG, "FRAM formatée — magic=0x%08lX", (unsigned long)FRAM_MAGIC_VALUE);
    } else {
        ESP_LOGI(TAG, "FRAM OK — magic valide");
    }

    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════
 * Persistance AV
 * ══════════════════════════════════════════════════════════════*/
esp_err_t rfm_save_av_state(float av0, float av1)
{
    fram_av_state_t s = {
        .av0   = av0,
        .av1   = av1,
        .valid = 0xAA,
        ._pad  = {0}
    };
    esp_err_t ret = fram_write(FRAM_AV_STATE_ADDR, (uint8_t *)&s, sizeof(s));
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "[AV SAVE] AV0=%.1f AV1=%.1f", av0, av1);
    }
    return ret;
}

esp_err_t rfm_load_av_state(float *av0, float *av1)
{
    fram_av_state_t s;
    esp_err_t ret = fram_read(FRAM_AV_STATE_ADDR, (uint8_t *)&s, sizeof(s));
    if (ret != ESP_OK) return ret;

    if (s.valid != 0xAA) {
        ESP_LOGW(TAG, "[AV LOAD] Données invalides — valeurs par défaut (0.0)");
        *av0 = 0.0f;
        *av1 = 0.0f;
        return ESP_ERR_NOT_FOUND;
    }

    *av0 = s.av0;
    *av1 = s.av1;
    ESP_LOGI(TAG, "[AV LOAD] AV0=%.1f AV1=%.1f", *av0, *av1);
    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════
 * Persistance BV
 * ══════════════════════════════════════════════════════════════*/
esp_err_t rfm_save_bv_state(bool manual)
{
    fram_bv_state_t s = {
        .manual_mode = manual ? 1 : 0,
        .valid       = 0xAA,
        ._pad        = {0}
    };
    esp_err_t ret = fram_write(FRAM_BV_STATE_ADDR, (uint8_t *)&s, sizeof(s));
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "[BV SAVE] mode=%s", manual ? "MANUEL" : "AUTO");
    }
    return ret;
}

esp_err_t rfm_load_bv_state(bool *manual)
{
    fram_bv_state_t s;
    esp_err_t ret = fram_read(FRAM_BV_STATE_ADDR, (uint8_t *)&s, sizeof(s));
    if (ret != ESP_OK) return ret;

    if (s.valid != 0xAA) {
        ESP_LOGW(TAG, "[BV LOAD] Données invalides — mode AUTO par défaut");
        *manual = false;
        return ESP_ERR_NOT_FOUND;
    }

    *manual = (s.manual_mode == 1);
    ESP_LOGI(TAG, "[BV LOAD] mode=%s", *manual ? "MANUEL" : "AUTO");
    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════
 * Gestion du temps
 * ══════════════════════════════════════════════════════════════*/
void rfm_sync_rtc_from_ntp(void)
{
    /* Écriture dans le RTC matériel */
    fram_rtc_backup_t t;
    _get_local_time(&t);

    esp_err_t ret = rtc_set_time(t.year, t.month, t.day, t.hour, t.min, t.sec);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[NTP→RTC] Échec écriture RTC : %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "[NTP→RTC] %02d/%02d/%02d %02d:%02d:%02d",
                 t.day, t.month, t.year, t.hour, t.min, t.sec);
    }

    /* Sauvegarde en FRAM */
    ret = fram_write(FRAM_RTC_BACKUP_ADDR, (uint8_t *)&t, sizeof(t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[NTP→FRAM] Échec sauvegarde RTC : %s", esp_err_to_name(ret));
    }

    rfm_log_event(EVENT_NTP_SYNC, 0.0f, 0);
}

void rfm_time_init(bool ntp_available)
{
    if (ntp_available) {
        /*
         * NTP disponible : la synchro NTP est déjà faite dans modem_initialize().
         * On sauvegarde juste l'heure dans le RTC + FRAM.
         */
        ESP_LOGI(TAG, "[TIME INIT] Source : NTP (4G connectée)");
        rfm_sync_rtc_from_ntp();
        return;
    }

    /*
     * Pas de 4G : on essaie d'abord le RTC matériel, puis le backup FRAM.
     */
    ESP_LOGW(TAG, "[TIME INIT] 4G indisponible — tentative restauration heure locale...");

    uint8_t y, mo, d, h, mi, s;
    esp_err_t ret = rtc_get_time(&y, &mo, &d, &h, &mi, &s);

    if (ret == ESP_OK && y > 0) {
        /* RTC matériel valide */
        fram_rtc_backup_t rtc = {
            .year = y, .month = mo, .day = d,
            .hour = h, .min   = mi, .sec = s,
            .valid = 0xAA
        };
        _apply_rtc_to_system(&rtc);
        ESP_LOGI(TAG, "[TIME INIT] Source : RTC matériel (PCF8563)");
        rfm_log_event(EVENT_4G_LOST, 0.0f, 0);
        return;
    }

    /* RTC matériel invalide : lecture backup FRAM */
    ESP_LOGW(TAG, "[TIME INIT] RTC invalide — lecture backup FRAM...");
    fram_rtc_backup_t backup;
    ret = fram_read(FRAM_RTC_BACKUP_ADDR, (uint8_t *)&backup, sizeof(backup));
    if (ret == ESP_OK && backup.valid == 0xAA) {
        _apply_rtc_to_system(&backup);
        ESP_LOGI(TAG, "[TIME INIT] Source : backup FRAM");
        rfm_log_event(EVENT_4G_LOST, 0.0f, 0);
    } else {
        ESP_LOGE(TAG, "[TIME INIT] Aucune source d'heure disponible — heure système non définie !");
    }
}

/* ══════════════════════════════════════════════════════════════
 * Ring buffer événements
 * ══════════════════════════════════════════════════════════════*/
void rfm_log_event(fram_event_type_t type, float value, uint8_t extra_u8)
{
    /* 1. Lecture du head courant */
    uint16_t head = 0;
    if (fram_read(FRAM_EVENT_HEAD_ADDR, (uint8_t *)&head, sizeof(head)) != ESP_OK) {
        ESP_LOGE(TAG, "[LOG] Échec lecture head");
        return;
    }
    if (head >= FRAM_EVENT_MAX_ENTRIES) head = 0; /* protection débordement */

    /* 2. Construction de l'entrée horodatée */
    fram_event_entry_t entry;
    memset(&entry, 0, sizeof(entry));

    fram_rtc_backup_t now;
    _get_local_time(&now);
    entry.year       = now.year;
    entry.month      = now.month;
    entry.day        = now.day;
    entry.hour       = now.hour;
    entry.min        = now.min;
    entry.sec        = now.sec;
    entry.event_type = (uint8_t)type;
    entry.extra_u8   = extra_u8;
    entry.value      = value;
    entry.valid      = 0xAA;

    /* 3. Écriture à l'adresse calculée dans le ring buffer */
    uint16_t offset = (uint16_t)(head * FRAM_EVENT_ENTRY_SIZE);
    uint16_t addr   = FRAM_EVENT_LOG_ADDR + offset;

    if (fram_write(addr, (uint8_t *)&entry, sizeof(entry)) != ESP_OK) {
        ESP_LOGE(TAG, "[LOG] Échec écriture entrée #%u", head);
        return;
    }

    /* 4. Mise à jour du head (ring) */
    head = (head + 1) % FRAM_EVENT_MAX_ENTRIES;
    fram_write(FRAM_EVENT_HEAD_ADDR, (uint8_t *)&head, sizeof(head));

    ESP_LOGD(TAG, "[LOG] evt=0x%02X val=%.1f extra=%u @%04X",
             type, value, extra_u8, addr);
}

/* ══════════════════════════════════════════════════════════════
 * rfm_dump — affiche tout le contenu FRAM dans les logs
 * ══════════════════════════════════════════════════════════════*/

/* Noms lisibles pour les types d'événement */
static const char *_event_name(uint8_t type)
{
    switch (type) {
        case EVENT_AV0_CHANGE:  return "AV0_CHANGE";
        case EVENT_AV1_CHANGE:  return "AV1_CHANGE";
        case EVENT_MODE_CHANGE: return "MODE_CHANGE";
        case EVENT_BOOT:        return "BOOT";
        case EVENT_NTP_SYNC:    return "NTP_SYNC";
        case EVENT_4G_LOST:     return "4G_LOST";
        default:                return "INCONNU";
    }
}

void rfm_dump(void)
{
    ESP_LOGI(TAG, "+------------------------------------------+");
    ESP_LOGI(TAG, "|         DUMP FRAM FM24CL64B              |");
    ESP_LOGI(TAG, "+------------------------------------------+");

    /* ── 1. Magic ── */
    uint32_t magic = 0;
    if (fram_read(FRAM_MAGIC_ADDR, (uint8_t *)&magic, sizeof(magic)) == ESP_OK) {
        ESP_LOGI(TAG, "[MAGIC]  0x%08lX  %s",
                 (unsigned long)magic,
                 magic == FRAM_MAGIC_VALUE ? "OK valide" : "!! INVALIDE");
    } else {
        ESP_LOGE(TAG, "[MAGIC]  Erreur lecture");
    }

    /* ── 2. AV state ── */
    fram_av_state_t av;
    if (fram_read(FRAM_AV_STATE_ADDR, (uint8_t *)&av, sizeof(av)) == ESP_OK) {
        if (av.valid == 0xAA) {
            ESP_LOGI(TAG, "[AV]     AV0 = %.2f %%   AV1 = %.2f %%", av.av0, av.av1);
        } else {
            ESP_LOGW(TAG, "[AV]     Zone vide (valid=0x%02X)", av.valid);
        }
    } else {
        ESP_LOGE(TAG, "[AV]     Erreur lecture");
    }

    /* ── 3. BV state ── */
    fram_bv_state_t bv;
    if (fram_read(FRAM_BV_STATE_ADDR, (uint8_t *)&bv, sizeof(bv)) == ESP_OK) {
        if (bv.valid == 0xAA) {
            ESP_LOGI(TAG, "[BV]     Mode = %s", bv.manual_mode ? "MANUEL" : "AUTO");
        } else {
            ESP_LOGW(TAG, "[BV]     Zone vide (valid=0x%02X)", bv.valid);
        }
    } else {
        ESP_LOGE(TAG, "[BV]     Erreur lecture");
    }

    /* ── 4. RTC backup ── */
    fram_rtc_backup_t rtc;
    if (fram_read(FRAM_RTC_BACKUP_ADDR, (uint8_t *)&rtc, sizeof(rtc)) == ESP_OK) {
        if (rtc.valid == 0xAA) {
            ESP_LOGI(TAG, "[RTC]    Backup : 20%02d-%02d-%02d %02d:%02d:%02d",
                     rtc.year, rtc.month, rtc.day,
                     rtc.hour, rtc.min, rtc.sec);
        } else {
            ESP_LOGW(TAG, "[RTC]    Zone vide (valid=0x%02X)", rtc.valid);
        }
    } else {
        ESP_LOGE(TAG, "[RTC]    Erreur lecture");
    }

    /* ── 5. Ring buffer événements ── */
    uint16_t head = 0;
    if (fram_read(FRAM_EVENT_HEAD_ADDR, (uint8_t *)&head, sizeof(head)) != ESP_OK) {
        ESP_LOGE(TAG, "[LOG]    Erreur lecture head");
        return;
    }
    if (head >= FRAM_EVENT_MAX_ENTRIES) head = 0;

    /* Comptage des entrées valides */
    uint16_t count = 0;
    for (uint16_t i = 0; i < FRAM_EVENT_MAX_ENTRIES; i++) {
        fram_event_entry_t e;
        uint16_t addr = FRAM_EVENT_LOG_ADDR + i * FRAM_EVENT_ENTRY_SIZE;
        if (fram_read(addr, (uint8_t *)&e, sizeof(e)) == ESP_OK && e.valid == 0xAA) {
            count++;
        }
    }

    ESP_LOGI(TAG, "[LOG]    Ring buffer : %u entrée(s) valide(s) / %u  (head=%u)",
             count, FRAM_EVENT_MAX_ENTRIES, head);

    if (count == 0) {
        ESP_LOGI(TAG, "[LOG]    (aucun événement enregistré)");
        ESP_LOGI(TAG, "+------------------------------------------+");
        return;
    }

    ESP_LOGI(TAG, "[LOG]    %-4s  %-10s  %-19s  %-7s  %s",
             "#", "TYPE", "HORODATAGE", "VALEUR", "EXTRA");
    ESP_LOGI(TAG, "[LOG]    %-4s  %-10s  %-19s  %-7s  %s",
             "----", "----------", "-------------------", "-------", "-----");

    /*
     * Lecture dans l'ordre chronologique :
     * le head pointe sur la PROCHAINE case à écrire,
     * donc la plus ancienne est à l'index head (si elle est valide).
     * On parcourt depuis head en tournant sur FRAM_EVENT_MAX_ENTRIES.
     */
    uint16_t displayed = 0;
    for (uint16_t i = 0; i < FRAM_EVENT_MAX_ENTRIES; i++) {
        uint16_t idx  = (head + i) % FRAM_EVENT_MAX_ENTRIES;
        uint16_t addr = FRAM_EVENT_LOG_ADDR + idx * FRAM_EVENT_ENTRY_SIZE;

        fram_event_entry_t e;
        if (fram_read(addr, (uint8_t *)&e, sizeof(e)) != ESP_OK) continue;
        if (e.valid != 0xAA) continue;

        /* Colonne EXTRA : pour MODE_CHANGE on affiche AUTO/MANUEL */
        char extra_str[12] = "-";
        if (e.event_type == EVENT_MODE_CHANGE) {
            snprintf(extra_str, sizeof(extra_str), "%s",
                     e.extra_u8 ? "MANUEL" : "AUTO");
        }

        ESP_LOGI(TAG, "[LOG]    %-4u  %-12s  20%02d-%02d-%02d %02d:%02d:%02d  %6.1f%%  %s",
                 displayed + 1,
                 _event_name(e.event_type),
                 e.year, e.month, e.day,
                 e.hour, e.min, e.sec,
                 e.value,
                 extra_str);
        displayed++;
    }

    ESP_LOGI(TAG, "+------------------------------------------+");
}