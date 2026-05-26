#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t http_server_start(void);
void http_trendlog_record(uint32_t index, float value);
void      http_server_stop(void);
#ifdef __cplusplus
}
#endif
#endif