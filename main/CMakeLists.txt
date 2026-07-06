idf_component_register(SRCS "app_main.c"
                       "imu_lsm6dsv.c"
                       "m2p_espnow.c"
                       "rf_infer.c"
                       "generated/rf_model_generated.c"
                       "usb_keyboard.c"
                       "battery_monitor.c"
                       "status_led.c"
                   INCLUDE_DIRS "."
                   REQUIRES esp_driver_gpio
                          esp_driver_spi
                          esp_driver_rmt
                          esp_adc
                          esp_event
                          esp_http_server
                          esp_netif
                          esp_timer
                          esp_wifi
                          nvs_flash)
