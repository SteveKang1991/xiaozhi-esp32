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

#define BOOT_BUTTON_GPIO        GPIO_NUM_35
#define BUILTIN_LED_GPIO        GPIO_NUM_6

#define DISPLAY_WIDTH 720
#define DISPLAY_HEIGHT 1280

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

// ILI9881C 5.5寸720P初始化序列 - HSD5.5+ILI9881C(2).c
static const ili9881c_lcd_init_cmd_t ili9881c_init_cmds[] = {
    /* 第一组命令 - 设置Page 3 */
    {0xFF, (uint8_t[]){0x98, 0x81, 0x03}, 3},

    {0x01, (uint8_t[]){0x00}, 1},
    {0x02, (uint8_t[]){0x00}, 1},
    {0x03, (uint8_t[]){0x72}, 1},
    {0x04, (uint8_t[]){0x00}, 1},
    {0x05, (uint8_t[]){0x00}, 1},
    {0x06, (uint8_t[]){0x09}, 1},
    {0x07, (uint8_t[]){0x00}, 1},
    {0x08, (uint8_t[]){0x00}, 1},
    {0x09, (uint8_t[]){0x01}, 1},
    {0x0A, (uint8_t[]){0x00}, 1},
    {0x0B, (uint8_t[]){0x00}, 1},
    {0x0C, (uint8_t[]){0x01}, 1},
    {0x0D, (uint8_t[]){0x00}, 1},
    {0x0E, (uint8_t[]){0x00}, 1},
    {0x0F, (uint8_t[]){0x00}, 1},
    {0x10, (uint8_t[]){0x00}, 1},
    {0x11, (uint8_t[]){0x00}, 1},
    {0x12, (uint8_t[]){0x00}, 1},
    {0x13, (uint8_t[]){0x00}, 1},
    {0x14, (uint8_t[]){0x00}, 1},
    {0x15, (uint8_t[]){0x00}, 1},
    {0x16, (uint8_t[]){0x00}, 1},
    {0x17, (uint8_t[]){0x00}, 1},
    {0x18, (uint8_t[]){0x00}, 1},
    {0x19, (uint8_t[]){0x00}, 1},
    {0x1A, (uint8_t[]){0x00}, 1},
    {0x1B, (uint8_t[]){0x00}, 1},
    {0x1C, (uint8_t[]){0x00}, 1},
    {0x1D, (uint8_t[]){0x00}, 1},
    {0x1E, (uint8_t[]){0x40}, 1},
    {0x1F, (uint8_t[]){0x80}, 1},
    {0x20, (uint8_t[]){0x05}, 1},
    {0x21, (uint8_t[]){0x02}, 1},
    {0x22, (uint8_t[]){0x00}, 1},
    {0x23, (uint8_t[]){0x00}, 1},
    {0x24, (uint8_t[]){0x00}, 1},
    {0x25, (uint8_t[]){0x00}, 1},
    {0x26, (uint8_t[]){0x00}, 1},
    {0x27, (uint8_t[]){0x00}, 1},
    {0x28, (uint8_t[]){0x33}, 1},
    {0x29, (uint8_t[]){0x02}, 1},
    {0x2A, (uint8_t[]){0x00}, 1},
    {0x2B, (uint8_t[]){0x00}, 1},
    {0x2C, (uint8_t[]){0x00}, 1},
    {0x2D, (uint8_t[]){0x00}, 1},
    {0x2E, (uint8_t[]){0x00}, 1},
    {0x2F, (uint8_t[]){0x00}, 1},
    {0x30, (uint8_t[]){0x00}, 1},
    {0x31, (uint8_t[]){0x00}, 1},
    {0x32, (uint8_t[]){0x00}, 1},
    {0x33, (uint8_t[]){0x00}, 1},
    {0x34, (uint8_t[]){0x04}, 1},
    {0x35, (uint8_t[]){0x00}, 1},
    {0x36, (uint8_t[]){0x00}, 1},
    {0x37, (uint8_t[]){0x00}, 1},
    {0x38, (uint8_t[]){0x3C}, 1},
    {0x39, (uint8_t[]){0x00}, 1},
    {0x3A, (uint8_t[]){0x40}, 1},
    {0x3B, (uint8_t[]){0x40}, 1},
    {0x3C, (uint8_t[]){0x00}, 1},
    {0x3D, (uint8_t[]){0x00}, 1},
    {0x3E, (uint8_t[]){0x00}, 1},
    {0x3F, (uint8_t[]){0x00}, 1},
    {0x40, (uint8_t[]){0x00}, 1},
    {0x41, (uint8_t[]){0x00}, 1},
    {0x42, (uint8_t[]){0x00}, 1},
    {0x43, (uint8_t[]){0x00}, 1},
    {0x44, (uint8_t[]){0x00}, 1},

    /* GIP_2 */
    {0x50, (uint8_t[]){0x01}, 1},
    {0x51, (uint8_t[]){0x23}, 1},
    {0x52, (uint8_t[]){0x45}, 1},
    {0x53, (uint8_t[]){0x67}, 1},
    {0x54, (uint8_t[]){0x89}, 1},
    {0x55, (uint8_t[]){0xAB}, 1},
    {0x56, (uint8_t[]){0x01}, 1},
    {0x57, (uint8_t[]){0x23}, 1},
    {0x58, (uint8_t[]){0x45}, 1},
    {0x59, (uint8_t[]){0x67}, 1},
    {0x5A, (uint8_t[]){0x89}, 1},
    {0x5B, (uint8_t[]){0xAB}, 1},
    {0x5C, (uint8_t[]){0xCD}, 1},
    {0x5D, (uint8_t[]){0xEF}, 1},

    /* GIP_3 */
    {0x5E, (uint8_t[]){0x11}, 1},
    {0x5F, (uint8_t[]){0x01}, 1},
    {0x60, (uint8_t[]){0x00}, 1},
    {0x61, (uint8_t[]){0x15}, 1},
    {0x62, (uint8_t[]){0x14}, 1},
    {0x63, (uint8_t[]){0x0E}, 1},
    {0x64, (uint8_t[]){0x0F}, 1},
    {0x65, (uint8_t[]){0x0C}, 1},
    {0x66, (uint8_t[]){0x0D}, 1},
    {0x67, (uint8_t[]){0x06}, 1},
    {0x68, (uint8_t[]){0x02}, 1},
    {0x69, (uint8_t[]){0x02}, 1},
    {0x6A, (uint8_t[]){0x02}, 1},
    {0x6B, (uint8_t[]){0x02}, 1},
    {0x6C, (uint8_t[]){0x02}, 1},
    {0x6D, (uint8_t[]){0x02}, 1},
    {0x6E, (uint8_t[]){0x07}, 1},
    {0x6F, (uint8_t[]){0x02}, 1},
    {0x70, (uint8_t[]){0x02}, 1},
    {0x71, (uint8_t[]){0x02}, 1},
    {0x72, (uint8_t[]){0x02}, 1},
    {0x73, (uint8_t[]){0x02}, 1},
    {0x74, (uint8_t[]){0x02}, 1},
    {0x75, (uint8_t[]){0x01}, 1},
    {0x76, (uint8_t[]){0x00}, 1},
    {0x77, (uint8_t[]){0x14}, 1},
    {0x78, (uint8_t[]){0x15}, 1},
    {0x79, (uint8_t[]){0x0E}, 1},
    {0x7A, (uint8_t[]){0x0F}, 1},
    {0x7B, (uint8_t[]){0x0C}, 1},
    {0x7C, (uint8_t[]){0x0D}, 1},
    {0x7D, (uint8_t[]){0x06}, 1},
    {0x7E, (uint8_t[]){0x02}, 1},
    {0x7F, (uint8_t[]){0x02}, 1},
    {0x80, (uint8_t[]){0x02}, 1},
    {0x81, (uint8_t[]){0x02}, 1},
    {0x82, (uint8_t[]){0x02}, 1},
    {0x83, (uint8_t[]){0x02}, 1},
    {0x84, (uint8_t[]){0x07}, 1},
    {0x85, (uint8_t[]){0x02}, 1},
    {0x86, (uint8_t[]){0x02}, 1},
    {0x87, (uint8_t[]){0x02}, 1},
    {0x88, (uint8_t[]){0x02}, 1},
    {0x89, (uint8_t[]){0x02}, 1},
    {0x8A, (uint8_t[]){0x02}, 1},

    /* CMD_Page 4 */
    {0xFF, (uint8_t[]){0x98, 0x81, 0x04}, 3},
    {0x00, (uint8_t[]){0x80}, 1},
    {0x6C, (uint8_t[]){0x15}, 1},
    {0x6E, (uint8_t[]){0x2A}, 1},
    {0x6F, (uint8_t[]){0x33}, 1},
    {0x3A, (uint8_t[]){0x94}, 1},
    {0x8D, (uint8_t[]){0x1A}, 1},
    {0x87, (uint8_t[]){0xBA}, 1},
    {0x26, (uint8_t[]){0x76}, 1},
    {0xB2, (uint8_t[]){0xD1}, 1},
    {0xB5, (uint8_t[]){0x06}, 1},
    {0x31, (uint8_t[]){0x75}, 1},
    {0x35, (uint8_t[]){0x1F}, 1},

    /* CMD_Page 1 */
    {0xFF, (uint8_t[]){0x98, 0x81, 0x01}, 3},
    {0x22, (uint8_t[]){0x0A}, 1},
    {0x31, (uint8_t[]){0x00}, 1},
    {0x50, (uint8_t[]){0xAE}, 1},
    {0x51, (uint8_t[]){0xAE}, 1},
    {0x60, (uint8_t[]){0x28}, 1},
    {0x63, (uint8_t[]){0x00}, 1},

    /* Gamma校正参数 */
    {0xA0, (uint8_t[]){0x0F}, 1},
    {0xA1, (uint8_t[]){0x18}, 1},
    {0xA2, (uint8_t[]){0x25}, 1},
    {0xA3, (uint8_t[]){0x15}, 1},
    {0xA4, (uint8_t[]){0x19}, 1},
    {0xA5, (uint8_t[]){0x2A}, 1},
    {0xA6, (uint8_t[]){0x1D}, 1},
    {0xA7, (uint8_t[]){0x1F}, 1},
    {0xA8, (uint8_t[]){0x7A}, 1},
    {0xA9, (uint8_t[]){0x1B}, 1},
    {0xAA, (uint8_t[]){0x27}, 1},
    {0xAB, (uint8_t[]){0x6B}, 1},
    {0xAC, (uint8_t[]){0x1D}, 1},
    {0xAD, (uint8_t[]){0x1C}, 1},
    {0xAE, (uint8_t[]){0x50}, 1},
    {0xAF, (uint8_t[]){0x24}, 1},
    {0xB0, (uint8_t[]){0x2B}, 1},
    {0xB1, (uint8_t[]){0x51}, 1},
    {0xB2, (uint8_t[]){0x60}, 1},
    {0xB3, (uint8_t[]){0x3F}, 1},

    {0xC0, (uint8_t[]){0x04}, 1},
    {0xC1, (uint8_t[]){0x1E}, 1},
    {0xC2, (uint8_t[]){0x2A}, 1},
    {0xC3, (uint8_t[]){0x10}, 1},
    {0xC4, (uint8_t[]){0x12}, 1},
    {0xC5, (uint8_t[]){0x27}, 1},
    {0xC6, (uint8_t[]){0x1C}, 1},
    {0xC7, (uint8_t[]){0x1D}, 1},
    {0xC8, (uint8_t[]){0x85}, 1},
    {0xC9, (uint8_t[]){0x19}, 1},
    {0xCA, (uint8_t[]){0x25}, 1},
    {0xCB, (uint8_t[]){0x7A}, 1},
    {0xCC, (uint8_t[]){0x1C}, 1},
    {0xCD, (uint8_t[]){0x1E}, 1},
    {0xCE, (uint8_t[]){0x55}, 1},
    {0xCF, (uint8_t[]){0x29}, 1},
    {0xD0, (uint8_t[]){0x2B}, 1},
    {0xD1, (uint8_t[]){0x5D}, 1},
    {0xD2, (uint8_t[]){0x6B}, 1},
    {0xD3, (uint8_t[]){0x3F}, 1},

    /* CMD_Page 0 */
    {0xFF, (uint8_t[]){0x98, 0x81, 0x00}, 3},

    /* 显示控制命令 */
    {0x35, (uint8_t[]){}, 0},
    {0x11, (uint8_t[]){}, 0},
    {0x29, (uint8_t[]){}, 0},
};

#endif // _BOARD_CONFIG_H_
