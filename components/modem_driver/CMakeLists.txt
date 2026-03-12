idf_component_register(
    SRCS "sms.c" "modem.c" "pdu_codec.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart esp_timer app_core esp_driver_gpio
)
