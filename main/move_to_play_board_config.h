#pragma once

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

#ifndef MOVE_TO_PLAY_TRACKER_BOARD_STYLE
#error "Define MOVE_TO_PLAY_TRACKER_BOARD_STYLE in app_main.c before including this file"
#endif

#define MOVE_TO_PLAY_TRACKER_BOARD_CURRENT 0
#define MOVE_TO_PLAY_TRACKER_BOARD_NEW     1

#if (MOVE_TO_PLAY_TRACKER_BOARD_STYLE != MOVE_TO_PLAY_TRACKER_BOARD_CURRENT) && \
    (MOVE_TO_PLAY_TRACKER_BOARD_STYLE != MOVE_TO_PLAY_TRACKER_BOARD_NEW)
#error "MOVE_TO_PLAY_TRACKER_BOARD_STYLE must be 0(current) or 1(new)"
#endif

#if MOVE_TO_PLAY_TRACKER_BOARD_STYLE == MOVE_TO_PLAY_TRACKER_BOARD_CURRENT
#define MOVE_TO_PLAY_TRACKER_BOARD_NAME "current"
#define IMU_SPI_SCLK_GPIO              GPIO_NUM_12
#define IMU_SPI_MOSI_GPIO              GPIO_NUM_11
#define IMU_SPI_MISO_GPIO              GPIO_NUM_9
#define IMU_SPI_CS_GPIO                GPIO_NUM_10
#define STATUS_LED_SK6812_GPIO         GPIO_NUM_38
#define BATTERY_ADC_GPIO               GPIO_NUM_4
#define BATTERY_ADC_UNIT               ADC_UNIT_1
#define BATTERY_ADC_CHANNEL            ADC_CHANNEL_3
#else
#define MOVE_TO_PLAY_TRACKER_BOARD_NAME "new"
#define IMU_SPI_SCLK_GPIO              GPIO_NUM_13
#define IMU_SPI_MOSI_GPIO              GPIO_NUM_12
#define IMU_SPI_MISO_GPIO              GPIO_NUM_11
#define IMU_SPI_CS_GPIO                GPIO_NUM_14
#define STATUS_LED_SK6812_GPIO         GPIO_NUM_38
#define BATTERY_ADC_GPIO               GPIO_NUM_2
#define BATTERY_ADC_UNIT               ADC_UNIT_1
#define BATTERY_ADC_CHANNEL            ADC_CHANNEL_1
#endif
