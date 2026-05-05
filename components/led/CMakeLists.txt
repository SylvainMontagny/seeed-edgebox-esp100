idf_component_register(
    SRCS
        "src/led.c"
    INCLUDE_DIRS 
        "." "include" "../../managed_components/espressif__led_strip/include"
    REQUIRES driver led_strip
)