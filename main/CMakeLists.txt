idf_component_register(
    SRCS "main.c" "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_event
        nvs_flash
        lwip
        driver
        esp_timer
        esp_system
)