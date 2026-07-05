idf_component_register(
    SRCS "camera_hal.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_timer
        freertos
)
