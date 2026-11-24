# Use the MQTT 3.1.1 client shipped with ESP-IDF's lwIP, without extra packages.
get_filename_component(MQTT_SOURCE
    "$ENV{IDF_PATH}/components/lwip/lwip/src/apps/mqtt/mqtt.c" REALPATH)
idf_component_register(
    SRCS "main.c" "bus.c" "bme280.c" "rtc.c" "display.c" "flash_store.c" "network.c"
         "${MQTT_SOURCE}"
    INCLUDE_DIRS "../include"
    REQUIRES esp_driver_i2c esp_driver_uart esp_wifi esp_event
             esp_netif nvs_flash esp_partition esp_pm esp_timer lwip
)
