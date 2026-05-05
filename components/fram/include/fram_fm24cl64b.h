#ifndef FRAM_FM24CL64B_H
#define FRAM_FM24CL64B_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#define FRAM_I2C_ADDR 0x50

// Écrit des données dans la FRAM à une adresse mémoire donnée (de 0x0000 à 0x1FFF)
esp_err_t fram_write(uint16_t mem_addr, const uint8_t *data, size_t len);

// Lit des données depuis la FRAM
esp_err_t fram_read(uint16_t mem_addr, uint8_t *data, size_t len);

#endif