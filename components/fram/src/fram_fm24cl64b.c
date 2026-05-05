#include "fram_fm24cl64b.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include <stdlib.h>

// On utilise le même numéro de bus I2C que le RTC
#define I2C_MASTER_NUM I2C_NUM_0 

esp_err_t fram_write(uint16_t mem_addr, const uint8_t *data, size_t len) {
    // Le protocole I2C pour l'EEPROM/FRAM demande d'envoyer l'adresse mémoire (2 octets) 
    // suivie immédiatement des données à écrire.
    uint8_t *buffer = malloc(2 + len);
    if (!buffer) return ESP_ERR_NO_MEM;

    buffer[0] = (uint8_t)(mem_addr >> 8);   // Octet de poids fort (MSB) de l'adresse
    buffer[1] = (uint8_t)(mem_addr & 0xFF); // Octet de poids faible (LSB) de l'adresse
    
    // On copie les données à la suite
    for (size_t i = 0; i < len; i++) {
        buffer[2 + i] = data[i];
    }

    esp_err_t ret = i2c_master_write_to_device(I2C_MASTER_NUM, FRAM_I2C_ADDR, buffer, 2 + len, pdMS_TO_TICKS(1000));
    free(buffer);
    return ret;
}

esp_err_t fram_read(uint16_t mem_addr, uint8_t *data, size_t len) {
    uint8_t addr_buf[2];
    addr_buf[0] = (uint8_t)(mem_addr >> 8);
    addr_buf[1] = (uint8_t)(mem_addr & 0xFF);

    // On écrit l'adresse mémoire qu'on veut lire, puis on lit les données
    return i2c_master_write_read_device(I2C_MASTER_NUM, FRAM_I2C_ADDR, addr_buf, 2, data, len, pdMS_TO_TICKS(1000));
}