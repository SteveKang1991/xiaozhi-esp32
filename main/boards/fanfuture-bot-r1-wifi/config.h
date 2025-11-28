#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_INPUT_REFERENCE    true

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_48
#define AUDIO_I2S_GPIO_WS GPIO_NUM_14
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_21
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_13
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_47

#define AUDIO_CODEC_USE_PCA9557
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_1
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_2
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_ES7210_ADDR  0x82

//#define BUILTIN_LED_GPIO        GPIO_NUM_42  // 板载 LED 引脚，用于状态指示
#define BOOT_BUTTON_GPIO          GPIO_NUM_0   // BOOT 按钮引脚，通常用于复位或进入下载模式
#define MODE_BUTTON_GPIO          GPIO_NUM_12  // Mode按钮引脚

#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_41    // 显示屏背光控制引脚
#define DISPLAY_MOSI_PIN      GPIO_NUM_45    // 显示屏 MOSI（主出从入）引脚，用于数据传输
#define DISPLAY_CLK_PIN       GPIO_NUM_40    // 显示屏时钟引脚，用于同步数据传输
#define DISPLAY_DC_PIN        GPIO_NUM_39    // 显示屏数据/命令选择引脚
#define DISPLAY_RST_PIN       GPIO_NUM_38    // 显示屏复位引脚

#ifdef CONFIG_LCD_ST7796_320X480
#define LCD_TYPE_ST7796_SERIAL
#define DISPLAY_WIDTH   320                   // 显示屏宽度（像素）
#define DISPLAY_HEIGHT  480                   // 显示屏高度（像素）
#define DISPLAY_MIRROR_X true                 // 是否水平镜像显示（false 表示不镜像）
#define DISPLAY_MIRROR_Y false                // 是否垂直镜像显示（true 表示镜像）
#define DISPLAY_SWAP_XY false                 // 是否交换 X 和 Y 轴（true 表示交换）
#define DISPLAY_INVERT_COLOR    true          // 是否反转颜色（true 表示反转）
#define DISPLAY_RGB_ORDER  LCD_RGB_ELEMENT_ORDER_RGB  // RGB 颜色顺序（RGB 表示红绿蓝顺序）
#define DISPLAY_OFFSET_X  0                   // 显示屏 X 轴偏移量（像素）
#define DISPLAY_OFFSET_Y  0                   // 显示屏 Y 轴偏移量（像素）
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false // 背光输出是否反转（false 表示不反转）
#define DISPLAY_SPI_MODE 0
#endif

/* Camera pins */
#define CAMERA_PIN_PWDN GPIO_NUM_NC
#define CAMERA_PIN_RESET GPIO_NUM_NC
#define CAMERA_PIN_XCLK GPIO_NUM_5
#define CAMERA_PIN_SIOD GPIO_NUM_1
#define CAMERA_PIN_SIOC GPIO_NUM_2

#define CAMERA_PIN_D7 GPIO_NUM_9
#define CAMERA_PIN_D6 GPIO_NUM_4
#define CAMERA_PIN_D5 GPIO_NUM_6
#define CAMERA_PIN_D4 GPIO_NUM_15
#define CAMERA_PIN_D3 GPIO_NUM_17
#define CAMERA_PIN_D2 GPIO_NUM_8
#define CAMERA_PIN_D1 GPIO_NUM_18
#define CAMERA_PIN_D0 GPIO_NUM_16
#define CAMERA_PIN_VSYNC GPIO_NUM_3
#define CAMERA_PIN_HREF GPIO_NUM_46
#define CAMERA_PIN_PCLK GPIO_NUM_7

#define XCLK_FREQ_HZ 20000000

#endif // _BOARD_CONFIG_H_
