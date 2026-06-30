#pragma once

#include "driver/gpio.h"

namespace printsphere::board {

constexpr int kDisplayWidth = 480;
constexpr int kDisplayHeight = 480;

constexpr gpio_num_t kI2cScl = GPIO_NUM_7;
constexpr gpio_num_t kI2cSda = GPIO_NUM_15;

constexpr gpio_num_t kTouchInterrupt = GPIO_NUM_16;
constexpr gpio_num_t kTouchReset = GPIO_NUM_NC;

constexpr gpio_num_t kQspiClk = GPIO_NUM_41;
constexpr gpio_num_t kQspiData0 = GPIO_NUM_5;
constexpr gpio_num_t kQspiData1 = GPIO_NUM_45;
constexpr gpio_num_t kQspiData2 = GPIO_NUM_48;
constexpr gpio_num_t kQspiData3 = GPIO_NUM_47;

constexpr int kAxp2101Address = 0;
constexpr char kBoardName[] = "ESP32-S3-Touch-LCD-2.8C";

}  // namespace printsphere::board
