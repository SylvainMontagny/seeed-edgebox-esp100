#ifndef SOLAR_H
#define SOLAR_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float   latitude;
    float   longitude;
    int16_t offset_before_sunset;   /* min avant coucher → allumage (0 = pile) */
    int16_t offset_after_sunrise;   /* min après lever   → extinction (0 = pile) */
    uint8_t enabled;
    uint8_t valid;
    uint8_t _pad[2];
} solar_config_t;   /* 16 bytes */

typedef struct {
    uint8_t sunrise_h;
    uint8_t sunrise_m;
    uint8_t sunset_h;
    uint8_t sunset_m;
    bool    valid;
} solar_times_t;

esp_err_t        solar_init(void);
esp_err_t        solar_save_config(const solar_config_t *cfg);
const solar_config_t *solar_get_config(void);
solar_times_t    solar_calc(int year, int month, int day);
solar_times_t    solar_calc_by_coords(int year, int month, int day,
                                      float latitude, float longitude,
                                      int16_t offset_before_sunset,
                                      int16_t offset_after_sunrise);
bool             solar_get_local_time(float latitude, float longitude,
                                      struct tm *local_time);
bool             solar_is_night_now(void);
solar_times_t    solar_get_today(void);
void             solar_invalidate_cache(void);
void             solar_get_offsets(int16_t *offset_before_sunset, int16_t *offset_after_sunrise);
void             solar_set_offsets(int16_t offset_before_sunset, int16_t offset_after_sunrise);
const char*      solar_get_timezone_posix(float latitude, float longitude);
void             solar_update_timezone_env(void);

#ifdef __cplusplus
}
#endif
#endif