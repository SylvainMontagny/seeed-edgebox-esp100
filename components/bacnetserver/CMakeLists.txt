idf_component_register(
    SRCS
        "src/server_task.c"
    INCLUDE_DIRS 
        "." "include" "../led/include"
    REQUIRES bacnet esp_event driver led
)