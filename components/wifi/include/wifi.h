/**
 * Initialize WiFi and connect to SSID
 * 
 * @return true if WiFi connection succeeded, false otherwise
 */
#include <stdbool.h>

bool wifi_initialize(void);
extern volatile bool g_connected;