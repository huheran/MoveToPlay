idf_component_register(SRCS "app_main.c"
                       "imu_lsm6dsv.c"
                   INCLUDE_DIRS "."
                   REQUIRES esp_driver_gpio
                          esp_driver_spi
                          esp_timer)
