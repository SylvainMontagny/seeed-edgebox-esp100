idf_component_register(
    SRCS
        "src/Http_server.c"
    INCLUDE_DIRS 
        "." "include" "../solar/include" "../bacnet/include" "../fram/include"
    REQUIRES esp_http_server esp_event bacnet fram solar json
)