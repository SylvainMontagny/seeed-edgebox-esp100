idf_component_register(
    SRCS
        "src/gpiooutputs.c"
    INCLUDE_DIRS 
    "." "include" "../NTP/include" "../led/include"  "../httpserver/include" "../solar/include" "../fram/include" "../rtc/include" "../gpiooutputs/include" "../modem/include"
    REQUIRES 
        esp_driver_spi
        esp_wifi
        esp_netif
        lwip
        nvs_flash
        driver
        esp_event
        bacnet
       
        bacnetserver
        fram
        gpiooutputs
        esp_modem
        modem
        esp_http_server
        httpserver
        json
        led
        rtc
        solar
        wifi
        ntp
        
)