idf_component_register(SRCS "app_main.c"
                       "imu_lsm6dsv.c"
                       "max30102.c"
                       "m2p_espnow.c"
                       "rf_infer.c"
                       "rf_state_infer.c"
                       "generated/rf_model_generated.c"
                       "generated/rf_state_model_generated.c"
                       "usb_keyboard.c"
                       "battery_monitor.c"
                       "status_led.c"
                   INCLUDE_DIRS "."
                   REQUIRES esp_driver_gpio
                          esp_driver_i2c
                          esp_driver_spi
                          esp_driver_rmt
                          esp_driver_usb_serial_jtag
                          esp_adc
                          esp_event
                          esp_http_server
                          esp_netif
                          esp_timer
                          esp_wifi
                          nvs_flash)

# These cache values let each board/profile get an isolated build directory.
# Example: idf.py -B build-dongle-collect -DM2P_BOARD_PROFILE=1 -DM2P_DONGLE_MODE=1 build
set(M2P_BOARD_PROFILE "1" CACHE STRING "1=dongle, 2=blade, 3=chest, 4=right hand, 5=left hand, 6=leg")
set(M2P_DONGLE_MODE "2" CACHE STRING "0=serial view, 1=data collect, 2=play")
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    M2P_BOARD_PROFILE=${M2P_BOARD_PROFILE}
    M2P_DONGLE_MODE=${M2P_DONGLE_MODE})
