idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES 
        app_core
        app_update
        esp_driver_gpio
        esp_timer
        modem_driver
        network_manager
        notification
        web_server
)
