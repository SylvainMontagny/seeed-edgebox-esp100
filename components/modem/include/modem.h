#ifndef MODEM_H
#define MODEM_H

#include "esp_netif.h"
#include "esp_modem_api.h"
#include "freertos/FreeRTOS.h"
#include "esp_ping.h"
#include "ping/ping_sock.h"

// External variables from main.c
#define MODEM_TX_PIN   48
#define MODEM_RX_PIN   47
#define MODEM_PWR_KEY  21
#define MODEM_PWR_EN   16
#define MODEM_APN      "iot.1nce.net"

// Global variables for modem state
extern volatile bool g_connected;
extern volatile bool g_bacnet_started;
extern esp_netif_t *g_ppp_netif;
extern esp_modem_dce_t *g_ppp_dce;

// Ping-related globals (now defined in modem.c)
extern volatile int g_ping_fail_count;
extern volatile bool g_link_lost;
extern esp_ping_handle_t g_ping_handle;

// Function declarations
bool modem_initialize(void);
bool modem_try_once(void);
void modem_reconnect_task(void *pvParameters);
void modem_start_ping_task(void);

#endif // MODEM_H