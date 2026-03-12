idf_component_register(
    SRCS "config.c" "utils.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_event mbedtls
)
