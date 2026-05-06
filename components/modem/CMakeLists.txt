idf_component_register(
    SRCS "src/modem.c"
      INCLUDE_DIRS
        "." "../../main/." "../led/include" "../bacnetclient/include" "../httpserver/include" "../solar/include" "../fram/include" "../rtc/include" "include" "../NTP/include"
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