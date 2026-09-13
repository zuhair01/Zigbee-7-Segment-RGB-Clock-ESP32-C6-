idf_component_register(
    SRCS
        "main.c"
        "clock_render.c"
        "zb_device.c"
        "settings.c"
        "ambient.c"
        "dht.c"
    INCLUDE_DIRS "."
    REQUIRES esp_adc esp_driver_gpio esp_timer
)
