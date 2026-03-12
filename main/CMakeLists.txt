idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES 
        esp_driver_gpio
        app_core
        network_manager
        modem_driver
        notification
        web_server
        esp_timer
)
