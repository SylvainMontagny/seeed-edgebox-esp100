#include "bacdef.h"
#include "device.h"
void server_task(void *arg);
void Init_Service_Handlers(void);
void create_bacnet_object(BACNET_OBJECT_TYPE type, object_functions_t *table, uint16_t IndexNum);