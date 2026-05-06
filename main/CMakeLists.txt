idf_component_register(
    SRCS
        "main.c"
    INCLUDE_DIRS
        "." "../components/NTP/include" "../components/led/include" "../components/bacnetclient/include" "../components/httpserver/include" "../components/solar/include" "../components/fram/include" "../components/rtc/include"
    REQUIRES
        esp_driver_spi
        esp_wifi
        esp_netif
        lwip
        nvs_flash
        driver
        esp_event
        bacnet
        bacnetclient
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
        NTP
        

        
)