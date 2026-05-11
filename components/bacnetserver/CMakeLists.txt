idf_component_register(
    LDFRAGMENTS 
        "linker.lf"
    SRCS
        "src/server_task.c" 
    INCLUDE_DIRS 
        "." "include" "../led/include" "../httpserver/include" "../solar/include" "../rtc/include" "../modem/include" "../ntp/include" "../fram/include" "../bacnet/include"
    REQUIRES bacnet esp_event driver led esp_wifi esp_netif lwip modem esp_modem httpserver esp_http_server json rtc solar ntp fram gpiooutputs 
)