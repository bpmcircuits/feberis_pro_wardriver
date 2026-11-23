idf_component_register(SRCS "app_main.c" "ap_memory.c" "gps_service.c" "status_led.c"
    "core/nmea.c" "core/wire.c" "core/wigle.c" "core/delivery.c" "core/wardrive.c"
    INCLUDE_DIRS "." "core"
    REQUIRES esp_psram esp_wifi esp_event esp_netif nvs_flash esp_timer esp_driver_uart esp_driver_rmt)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
