#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_2
#define AUDIO_I2S_GPIO_WS GPIO_NUM_6
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_4
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_5
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_3

#define AUDIO_CODEC_PA_PIN       GPIO_NUM_11
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_28
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_29
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR

#define BOOT_BUTTON_GPIO        GPIO_NUM_35

#define DISPLAY_WIDTH 720
#define DISPLAY_HEIGHT 1280

#define LCD_BIT_PER_PIXEL          (16)
#define PIN_NUM_LCD_RST            GPIO_NUM_22

#define DELAY_TIME_MS                      (3000)
#define LCD_MIPI_DSI_LANE_NUM          (2)    // 2 data lanes

#define MIPI_DSI_PHY_PWR_LDO_CHAN          (3)
#define MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV    (2500)

#define DISPLAY_SWAP_XY false
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false

#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0

#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_23
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

/* 5.0寸720P ILI9881D初始化序列 */
static const ili9881c_lcd_init_cmd_t ili9881d_init_cmds[] = {
    /* 第一组命令 - 设置Page 3 */
    {0xFF, (uint8_t[]){0x98, 0x81, 0x03}, 3},
    
    {0x01, (uint8_t[]){0x00}, 1},
    {0x02, (uint8_t[]){0x00}, 1},
    {0x03, (uint8_t[]){0x73}, 1},
    {0x04, (uint8_t[]){0x00}, 1},
    {0x05, (uint8_t[]){0x00}, 1},
    {0x06, (uint8_t[]){0x0A}, 1},
    {0x07, (uint8_t[]){0x00}, 1},
    {0x08, (uint8_t[]){0x00}, 1},
    {0x09, (uint8_t[]){0x01}, 1},
    {0x0A, (uint8_t[]){0x00}, 1},
    {0x0B, (uint8_t[]){0x00}, 1},
    {0x0C, (uint8_t[]){0x01}, 1},
    {0x0D, (uint8_t[]){0x00}, 1},
    {0x0E, (uint8_t[]){0x00}, 1},
    {0x0F, (uint8_t[]){0x1D}, 1},
    {0x10, (uint8_t[]){0x1D}, 1},
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
    {0x20, (uint8_t[]){0x06}, 1},
    {0x21, (uint8_t[]){0x02}, 1},
    {0x22, (uint8_t[]){0x00}, 1},
    {0x23, (uint8_t[]){0x00}, 1},
    {0x24, (uint8_t[]){0x00}, 1},
    {0x25, (uint8_t[]){0x00}, 1},
    {0x26, (uint8_t[]){0x00}, 1},
    {0x27, (uint8_t[]){0x00}, 1},
    {0x28, (uint8_t[]){0x33}, 1},
    {0x29, (uint8_t[]){0x03}, 1},
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
    {0x39, (uint8_t[]){0x35}, 1},
    {0x3A, (uint8_t[]){0x01}, 1},
    {0x3B, (uint8_t[]){0x40}, 1},
    {0x3C, (uint8_t[]){0x00}, 1},
    {0x3D, (uint8_t[]){0x01}, 1},
    {0x3E, (uint8_t[]){0x00}, 1},
    {0x3F, (uint8_t[]){0x00}, 1},
    {0x40, (uint8_t[]){0x00}, 1},
    {0x41, (uint8_t[]){0x88}, 1},
    {0x42, (uint8_t[]){0x00}, 1},
    {0x43, (uint8_t[]){0x00}, 1},
    {0x44, (uint8_t[]){0x1F}, 1},
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
    {0x69, (uint8_t[]){0x07}, 1},
    {0x6A, (uint8_t[]){0x02}, 1},
    {0x6B, (uint8_t[]){0x02}, 1},
    {0x6C, (uint8_t[]){0x02}, 1},
    {0x6D, (uint8_t[]){0x02}, 1},
    {0x6E, (uint8_t[]){0x02}, 1},
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
    {0x7F, (uint8_t[]){0x07}, 1},
    {0x80, (uint8_t[]){0x02}, 1},
    {0x81, (uint8_t[]){0x02}, 1},
    {0x82, (uint8_t[]){0x02}, 1},
    {0x83, (uint8_t[]){0x02}, 1},
    {0x84, (uint8_t[]){0x02}, 1},
    {0x85, (uint8_t[]){0x02}, 1},
    {0x86, (uint8_t[]){0x02}, 1},
    {0x87, (uint8_t[]){0x02}, 1},
    {0x88, (uint8_t[]){0x02}, 1},
    {0x89, (uint8_t[]){0x02}, 1},
    {0x8A, (uint8_t[]){0x02}, 1},
    
    /* 设置Page 4 */
    {0xFF, (uint8_t[]){0x98, 0x81, 0x04}, 3},
    
    /* Page 4命令 */
    {0x70, (uint8_t[]){0x00}, 1},
    {0x71, (uint8_t[]){0x00}, 1},
    {0x82, (uint8_t[]){0x0F}, 1},  // VGH_MOD clamp level=15v
    {0x84, (uint8_t[]){0x0F}, 1},  // VGH clamp level 15V
    {0x85, (uint8_t[]){0x0D}, 1},  // VGL clamp level (-10V)
    {0x32, (uint8_t[]){0xAC}, 1},
    {0x8C, (uint8_t[]){0x80}, 1},
    {0x3C, (uint8_t[]){0xF5}, 1},
    {0xB5, (uint8_t[]){0x07}, 1},  // GAMMA OP
    {0x31, (uint8_t[]){0x45}, 1},  // SOURCE OP
    {0x3A, (uint8_t[]){0x24}, 1},  // PS_EN OFF
    {0x88, (uint8_t[]){0x33}, 1},  // LVD
    
    /* 设置Page 1 */
    {0xFF, (uint8_t[]){0x98, 0x81, 0x01}, 3},
    
    /* Page 1命令 */
    {0x22, (uint8_t[]){0x09}, 1},  // BGR SS GS
    {0x31, (uint8_t[]){0x01}, 1},  // 1-dot inversion
    {0x53, (uint8_t[]){0x8A}, 1},  // VCOM1
    {0x55, (uint8_t[]){0xA2}, 1},  // VCOM2
    {0x50, (uint8_t[]){0x81}, 1},  // VREG1OUT=5V
    {0x51, (uint8_t[]){0x85}, 1},  // VREG2OUT=-5V
    {0x62, (uint8_t[]){0x0D}, 1},  // EQT Time setting
    
    /* 再次设置Page 1 */
    {0xFF, (uint8_t[]){0x98, 0x81, 0x01}, 3},
    
    /* 伽马校正参数 */
    {0xA0, (uint8_t[]){0x00}, 1},
    {0xA1, (uint8_t[]){0x1A}, 1},
    {0xA2, (uint8_t[]){0x28}, 1},
    {0xA3, (uint8_t[]){0x13}, 1},
    {0xA4, (uint8_t[]){0x16}, 1},
    {0xA5, (uint8_t[]){0x29}, 1},
    {0xA6, (uint8_t[]){0x1D}, 1},
    {0xA7, (uint8_t[]){0x1E}, 1},
    {0xA8, (uint8_t[]){0x84}, 1},
    {0xA9, (uint8_t[]){0x1C}, 1},
    {0xAA, (uint8_t[]){0x28}, 1},
    {0xAB, (uint8_t[]){0x75}, 1},
    {0xAC, (uint8_t[]){0x1A}, 1},
    {0xAD, (uint8_t[]){0x19}, 1},
    {0xAE, (uint8_t[]){0x4D}, 1},
    {0xAF, (uint8_t[]){0x22}, 1},
    {0xB0, (uint8_t[]){0x28}, 1},
    {0xB1, (uint8_t[]){0x54}, 1},
    {0xB2, (uint8_t[]){0x66}, 1},
    {0xB3, (uint8_t[]){0x39}, 1},
    {0xC0, (uint8_t[]){0x00}, 1},
    {0xC1, (uint8_t[]){0x1A}, 1},
    {0xC2, (uint8_t[]){0x28}, 1},
    {0xC3, (uint8_t[]){0x13}, 1},
    {0xC4, (uint8_t[]){0x16}, 1},
    {0xC5, (uint8_t[]){0x29}, 1},
    {0xC6, (uint8_t[]){0x1D}, 1},
    {0xC7, (uint8_t[]){0x1E}, 1},
    {0xC8, (uint8_t[]){0x84}, 1},
    {0xC9, (uint8_t[]){0x1C}, 1},
    {0xCA, (uint8_t[]){0x28}, 1},
    {0xCB, (uint8_t[]){0x75}, 1},
    {0xCC, (uint8_t[]){0x1A}, 1},
    {0xCD, (uint8_t[]){0x19}, 1},
    {0xCE, (uint8_t[]){0x4D}, 1},
    {0xCF, (uint8_t[]){0x22}, 1},
    {0xD0, (uint8_t[]){0x28}, 1},
    {0xD1, (uint8_t[]){0x54}, 1},
    {0xD2, (uint8_t[]){0x66}, 1},
    {0xD3, (uint8_t[]){0x39}, 1},
    
    /* 设置Page 0 */
    {0xFF, (uint8_t[]){0x98, 0x81, 0x00}, 3},
    
    /* 显示控制命令 */
    {0x35, (uint8_t[]){}, 0},
    {0x36, (uint8_t[]){0x03}, 1},
    {0x11, (uint8_t[]){}, 0},  // Sleep-out
    {0x29, (uint8_t[]){}, 0},  // Display on
};


#endif // _BOARD_CONFIG_H_
