#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t http_server_start(void);
void http_trendlog_record(float av0, float av1);
void      http_server_stop(void);
#ifdef __cplusplus
}
#endif
#endif