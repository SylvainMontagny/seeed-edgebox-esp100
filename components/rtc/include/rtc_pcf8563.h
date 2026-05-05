#ifndef RTC_PCF8563_H
#define RTC_PCF8563_H

#include "esp_err.h"
#include <stdint.h>

// Définitions pour l'EdgeBox-ESP-100
#define RTC_I2C_SDA_IO 20
#define RTC_I2C_SCL_IO 19
#define RTC_I2C_NUM    I2C_NUM_0
#define RTC_ADDR       0x51

// Prototypes
esp_err_t pcf8563_init(void);
esp_err_t rtc_set_time(uint8_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t min, uint8_t sec);
esp_err_t rtc_get_time(uint8_t *year, uint8_t *month, uint8_t *day, uint8_t *hour, uint8_t *min, uint8_t *sec);

#endif