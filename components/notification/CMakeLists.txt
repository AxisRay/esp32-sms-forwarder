idf_component_register(
    SRCS "push.c" "smtp_client.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp-tls mbedtls app_core esp_timer network_manager json
)
