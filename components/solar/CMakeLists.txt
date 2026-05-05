idf_component_register(
    SRCS
        "src/solar.c"
    INCLUDE_DIRS 
        "." "include" "../fram/include"
    REQUIRES driver fram
)