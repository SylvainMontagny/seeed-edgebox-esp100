/*
 * rtc_pcf8563.c
 *
 *  Created on: 4 mars 2026
 *      Author: user
 */


#include "rtc_pcf8563.h"
#include "driver/i2c.h"
#include "esp_log.h"

static const char *TAG = "PCF8563";

static uint8_t dec_to_bcd(uint8_t val) { return ((val / 10) << 4) | (val % 10); }
static uint8_t bcd_to_dec(uint8_t val) { return ((val >> 4) * 10) + (val & 0x0F); }

esp_err_t pcf8563_init(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = RTC_I2C_SDA_IO,
        .scl_io_num = RTC_I2C_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    i2c_param_config(RTC_I2C_NUM, &conf);
    return i2c_driver_install(RTC_I2C_NUM, conf.mode, 0, 0, 0);
}

esp_err_t rtc_set_time(uint8_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t min, uint8_t sec) {
    uint8_t data[8] = {
        0x02, // Registre de départ
        dec_to_bcd(sec), dec_to_bcd(min), dec_to_bcd(hour),
        dec_to_bcd(day), 0x01, dec_to_bcd(month), dec_to_bcd(year)
    };
    return i2c_master_write_to_device(RTC_I2C_NUM, RTC_ADDR, data, 8, pdMS_TO_TICKS(1000));
}

esp_err_t rtc_get_time(uint8_t *year, uint8_t *month, uint8_t *day, uint8_t *hour, uint8_t *min, uint8_t *sec) {
    uint8_t reg_addr = 0x02;
    uint8_t data[7];
    esp_err_t ret = i2c_master_write_read_device(RTC_I2C_NUM, RTC_ADDR, &reg_addr, 1, data, 7, pdMS_TO_TICKS(1000));
    
    if (ret == ESP_OK) {
        *sec   = bcd_to_dec(data[0] & 0x7F);
        *min   = bcd_to_dec(data[1] & 0x7F);
        *hour  = bcd_to_dec(data[2] & 0x3F);
        *day   = bcd_to_dec(data[3] & 0x3F);
        *month = bcd_to_dec(data[5] & 0x1F);
        *year  = bcd_to_dec(data[6]);
    }
    return ret;
}

