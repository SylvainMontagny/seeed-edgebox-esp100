idf_component_register(
    SRCS
        "src/rtc_fram_manager.c" "src/fram_fm24cl64b.c" 
    INCLUDE_DIRS 
        "." "include" "../rtc/include" 
    REQUIRES driver rtc
)