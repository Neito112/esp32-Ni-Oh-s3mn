#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// Chế độ Audio I2S Simplex (Mic INMP441 + Loa MAX98357A)
#define AUDIO_I2S_METHOD_SIMPLEX

#ifdef AUDIO_I2S_METHOD_SIMPLEX
#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6

#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16
#endif

// Nút nhấn Boot & Đèn LED tích hợp
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define BUILTIN_LED_GPIO        GPIO_NUM_48
#define TOUCH_BUTTON_GPIO       GPIO_NUM_NC
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_NC
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_NC

// Cấu hình màn hình TFT ST7789 240x240 SPI
#define LCD_TYPE_ST7789_SERIAL
#define DISPLAY_WIDTH           240
#define DISPLAY_HEIGHT          240
#define DISPLAY_MOSI_PIN        GPIO_NUM_13
#define DISPLAY_CLK_PIN         GPIO_NUM_2
#define DISPLAY_CS_PIN          GPIO_NUM_7
#define DISPLAY_DC_PIN          GPIO_NUM_6
#define DISPLAY_RST_PIN         GPIO_NUM_5
#define DISPLAY_BACKLIGHT_PIN   GPIO_NUM_NC
#define DISPLAY_MIRROR_X        false
#define DISPLAY_MIRROR_Y        false
#define DISPLAY_SWAP_XY         false
#define DISPLAY_INVERT_COLOR    true
#define DISPLAY_RGB_ORDER       LCD_RGB_ELEMENT_ORDER_RGB
#define DISPLAY_OFFSET_X        0
#define DISPLAY_OFFSET_Y        0
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false
#define DISPLAY_SPI_MODE        3

// Thẻ nhớ MicroSD (SPI)
#define SD_MISO_GPIO            GPIO_NUM_17
#define SD_MOSI_GPIO            GPIO_NUM_18
#define SD_CLK_GPIO             GPIO_NUM_21
#define SD_CS_GPIO              GPIO_NUM_10

// Relay MCP Tool
#define RELAY_3_GPIO            GPIO_NUM_3
#define RELAY_14_GPIO           GPIO_NUM_14
#define RELAY_15_GPIO           GPIO_NUM_15
#define RELAY_16_GPIO           GPIO_NUM_16

#endif // _BOARD_CONFIG_H_

