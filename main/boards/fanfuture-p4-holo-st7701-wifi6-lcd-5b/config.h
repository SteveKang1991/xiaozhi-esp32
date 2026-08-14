#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_INPUT_REFERENCE    true

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_13
#define AUDIO_I2S_GPIO_WS GPIO_NUM_10
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_12
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_11
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_9

#define AUDIO_CODEC_PA_PIN       GPIO_NUM_20
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_7
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_8
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_ES7210_ADDR  0x82

#define BOOT_BUTTON_GPIO        GPIO_NUM_35     // M键引脚
#define IO_BUTTON_GPIO          GPIO_NUM_21     // IO按钮引脚
#define BUILTIN_LED_GPIO        GPIO_NUM_6

#define DISPLAY_WIDTH 480
#define DISPLAY_HEIGHT 854

#define LCD_BIT_PER_PIXEL          (16)
#define PIN_NUM_LCD_RST            GPIO_NUM_26

#define DELAY_TIME_MS                      (3000)
#define LCD_MIPI_DSI_LANE_NUM          (2)    // 2 data lanes

#define MIPI_DSI_PHY_PWR_LDO_CHAN          (3)
#define MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV    (2500)

#define DISPLAY_SWAP_XY false
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false

#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0

#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_27
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

// Touch screen configuration
#define TOUCH_SCL_PIN       GPIO_NUM_29
#define TOUCH_SDA_PIN       GPIO_NUM_28
#define TOUCH_RST_PIN       GPIO_NUM_30
#define TOUCH_INT_PIN       GPIO_NUM_31

// ML307 4G module configuration
#define ML307_TX_PIN        GPIO_NUM_32
#define ML307_RX_PIN        GPIO_NUM_33
#define ML307_UART_NUM      UART_NUM_1
#define ML307_POWER_PIN     GPIO_NUM_NC  // Using AXP2101 DCDC3 instead

// Camera configuration - OV2710 MIPI CSI
// Note: Camera shares I2C1 with ES8311 audio codec (GPIO7/8)
#define CAMERA_I2C_SDA      AUDIO_CODEC_I2C_SDA_PIN  // GPIO_NUM_7 (shared with ES8311)
#define CAMERA_I2C_SCL      AUDIO_CODEC_I2C_SCL_PIN  // GPIO_NUM_8 (shared with ES8311)
#define CAMERA_RESET_PIN    GPIO_NUM_NC    // No reset pin used
#define CAMERA_PWDN_PIN     GPIO_NUM_NC    // No power-down pin used
#define CAMERA_SENSOR_NAME  "OV2710"
#define CAMERA_SCCB_ADDR    0x36           // OV2710 I2C address

// ST7701 初始化序列 - HS5IPS(HSD050B8W8-C00) 5寸MIPI屏 480x854
// 面板信息: 分辨率 480x854, 反转 2dot, VBP≧17, VFP≧20, 行时间 19us, 帧率 60Hz
static const st7701_lcd_init_cmd_t st7701_init_cmds[] = {
    // ST7701S 复位序列
    {0x11, (uint8_t[]){0x00}, 0, 120},  // Sleep Out, 等待120ms

    // Bank0 设置 - 显示控制
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0xE9, 0x03}, 2, 0},
    {0xC1, (uint8_t[]){0x11, 0x02}, 2, 0},
    {0xC2, (uint8_t[]){0x31, 0x08}, 2, 0},
    {0xCC, (uint8_t[]){0x10}, 1, 0},

    // Gamma 曲线设置
    {0xB0, (uint8_t[]){0x00, 0x0D, 0x14, 0x0D, 0x10, 0x05, 0x02, 0x08, 0x08, 0x1E, 0x05, 0x13, 0x11, 0xA3, 0x29, 0x18}, 16, 0},
    {0xB1, (uint8_t[]){0x00, 0x0C, 0x14, 0x0C, 0x10, 0x05, 0x03, 0x08, 0x07, 0x20, 0x05, 0x13, 0x11, 0xA4, 0x29, 0x18}, 16, 0},

    // Bank1 设置 - 电源控制
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t[]){0x6C}, 1, 0},
    {0xB1, (uint8_t[]){0x43}, 1, 0},
    {0xB2, (uint8_t[]){0x07}, 1, 0},
    {0xB3, (uint8_t[]){0x80}, 1, 0},
    {0xB5, (uint8_t[]){0x47}, 1, 0},
    {0xB7, (uint8_t[]){0x85}, 1, 0},
    {0xB8, (uint8_t[]){0x20}, 1, 0},
    {0xB9, (uint8_t[]){0x10}, 1, 0},
    {0xC1, (uint8_t[]){0x78}, 1, 0},
    {0xC2, (uint8_t[]){0x78}, 1, 0},
    {0xD0, (uint8_t[]){0x88}, 1, 0},

    // GIP 设置
    {0xE0, (uint8_t[]){0x00, 0x00, 0x02}, 3, 0},
    {0xE1, (uint8_t[]){0x08, 0x00, 0x0A, 0x00, 0x07, 0x00, 0x09, 0x00, 0x00, 0x33, 0x33}, 11, 0},
    {0xE2, (uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 13, 0},
    {0xE3, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE4, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t[]){0x0E, 0x60, 0xA0, 0xA0, 0x10, 0x60, 0xA0, 0xA0, 0x0A, 0x60, 0xA0, 0xA0, 0x0C, 0x60, 0xA0, 0xA0}, 16, 0},
    {0xE6, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE7, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t[]){0x0D, 0x60, 0xA0, 0xA0, 0x0F, 0x60, 0xA0, 0xA0, 0x09, 0x60, 0xA0, 0xA0, 0x0B, 0x60, 0xA0, 0xA0}, 16, 0},
    {0xEB, (uint8_t[]){0x02, 0x01, 0xE4, 0xE4, 0x44, 0x00, 0x40}, 7, 0},
    {0xEC, (uint8_t[]){0x02, 0x01}, 2, 0},
    {0xED, (uint8_t[]){0xAB, 0x89, 0x76, 0x54, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x10, 0x45, 0x67, 0x98, 0xBA}, 16, 0},

    // 退出 Bank1
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x3A, (uint8_t[]){0x05}, 1, 0},  // COLMOD: RGB565
    {0x29, (uint8_t[]){0x00}, 0, 0},  // Display On
};

#endif // _BOARD_CONFIG_H_
