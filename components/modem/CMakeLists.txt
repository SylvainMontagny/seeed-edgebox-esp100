idf_component_register(
    SRCS
        
    INCLUDE_DIRS "." "include" "../../main"  
    REQUIRES esp_netif esp_event lwip driver esp_wifi
)