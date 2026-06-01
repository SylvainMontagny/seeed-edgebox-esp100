#pragma once

#include <stdbool.h>

extern bool g_connected;

bool wifi_initialize(void);
bool wifi_is_connected(void);
void wifi_force_reconnect(void);