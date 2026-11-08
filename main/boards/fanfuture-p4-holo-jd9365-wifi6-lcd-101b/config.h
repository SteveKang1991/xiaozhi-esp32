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

#define DISPLAY_WIDTH 800
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

// JD9365DA-H3 10.1寸 800x1280，BOE TV101WX0 / PC101CS3D01
// 初始化来自 1BOE10.1_N91 残影序列 20250804。0x80 改为 0x01，配合本板 2 lane DSI。
static const jd9365_lcd_init_cmd_t jd9365_init_cmds[] = {
    {0xE0, (uint8_t[]){0x00}, 1, 0},
    {0xE1, (uint8_t[]){0x93}, 1, 0},
    {0xE2, (uint8_t[]){0x65}, 1, 0},
    {0xE3, (uint8_t[]){0xF8}, 1, 0},
    {0x80, (uint8_t[]){0x01}, 1, 0},  // 2 lane. Vendor file writes 0x03 (4 lane); ESP32-P4 DSI is 2 lanes.
    {0xE0, (uint8_t[]){0x01}, 1, 0},
    {0x00, (uint8_t[]){0x00}, 1, 0},
    {0x01, (uint8_t[]){0x3E}, 1, 0},
    {0x03, (uint8_t[]){0x00}, 1, 0},
    {0x04, (uint8_t[]){0x38}, 1, 0},
    {0x0C, (uint8_t[]){0x74}, 1, 0},
    {0x17, (uint8_t[]){0x00}, 1, 0},
    {0x18, (uint8_t[]){0xAF}, 1, 0},
    {0x19, (uint8_t[]){0x00}, 1, 0},
    {0x1A, (uint8_t[]){0x00}, 1, 0},
    {0x1B, (uint8_t[]){0xAF}, 1, 0},
    {0x1C, (uint8_t[]){0x00}, 1, 0},
    {0x35, (uint8_t[]){0x26}, 1, 0},
    {0x37, (uint8_t[]){0x09}, 1, 0},
    {0x38, (uint8_t[]){0x04}, 1, 0},
    {0x39, (uint8_t[]){0x00}, 1, 0},
    {0x3A, (uint8_t[]){0x01}, 1, 0},
    {0x3C, (uint8_t[]){0x78}, 1, 0},
    {0x3D, (uint8_t[]){0xFF}, 1, 0},
    {0x3E, (uint8_t[]){0xFF}, 1, 0},
    {0x3F, (uint8_t[]){0x7F}, 1, 0},
    {0x40, (uint8_t[]){0x06}, 1, 0},
    {0x41, (uint8_t[]){0xA0}, 1, 0},
    {0x42, (uint8_t[]){0x81}, 1, 0},
    {0x43, (uint8_t[]){0x18}, 1, 0},
    {0x44, (uint8_t[]){0x0B}, 1, 0},
    {0x45, (uint8_t[]){0x28}, 1, 0},
    {0x55, (uint8_t[]){0x0D}, 1, 0},
    {0x57, (uint8_t[]){0x69}, 1, 0},
    {0x59, (uint8_t[]){0x0A}, 1, 0},
    {0x5A, (uint8_t[]){0x2A}, 1, 0},
    {0x5B, (uint8_t[]){0x17}, 1, 0},
    {0x5D, (uint8_t[]){0x7F}, 1, 0},
    {0x5E, (uint8_t[]){0x6B}, 1, 0},
    {0x5F, (uint8_t[]){0x5C}, 1, 0},
    {0x60, (uint8_t[]){0x4F}, 1, 0},
    {0x61, (uint8_t[]){0x4D}, 1, 0},
    {0x62, (uint8_t[]){0x3F}, 1, 0},
    {0x63, (uint8_t[]){0x42}, 1, 0},
    {0x64, (uint8_t[]){0x2B}, 1, 0},
    {0x65, (uint8_t[]){0x44}, 1, 0},
    {0x66, (uint8_t[]){0x43}, 1, 0},
    {0x67, (uint8_t[]){0x43}, 1, 0},
    {0x68, (uint8_t[]){0x63}, 1, 0},
    {0x69, (uint8_t[]){0x52}, 1, 0},
    {0x6A, (uint8_t[]){0x5A}, 1, 0},
    {0x6B, (uint8_t[]){0x4F}, 1, 0},
    {0x6C, (uint8_t[]){0x4E}, 1, 0},
    {0x6D, (uint8_t[]){0x20}, 1, 0},
    {0x6E, (uint8_t[]){0x0F}, 1, 0},
    {0x6F, (uint8_t[]){0x00}, 1, 0},
    {0x70, (uint8_t[]){0x7F}, 1, 0},
    {0x71, (uint8_t[]){0x6B}, 1, 0},
    {0x72, (uint8_t[]){0x5C}, 1, 0},
    {0x73, (uint8_t[]){0x4F}, 1, 0},
    {0x74, (uint8_t[]){0x4D}, 1, 0},
    {0x75, (uint8_t[]){0x3F}, 1, 0},
    {0x76, (uint8_t[]){0x42}, 1, 0},
    {0x77, (uint8_t[]){0x2B}, 1, 0},
    {0x78, (uint8_t[]){0x44}, 1, 0},
    {0x79, (uint8_t[]){0x43}, 1, 0},
    {0x7A, (uint8_t[]){0x43}, 1, 0},
    {0x7B, (uint8_t[]){0x63}, 1, 0},
    {0x7C, (uint8_t[]){0x52}, 1, 0},
    {0x7D, (uint8_t[]){0x5A}, 1, 0},
    {0x7E, (uint8_t[]){0x4F}, 1, 0},
    {0x7F, (uint8_t[]){0x4E}, 1, 0},
    {0x80, (uint8_t[]){0x20}, 1, 0},
    {0x81, (uint8_t[]){0x0F}, 1, 0},
    {0x82, (uint8_t[]){0x00}, 1, 0},
    {0xE0, (uint8_t[]){0x02}, 1, 0},
    {0x00, (uint8_t[]){0x42}, 1, 0},
    {0x01, (uint8_t[]){0x42}, 1, 0},
    {0x02, (uint8_t[]){0x40}, 1, 0},
    {0x03, (uint8_t[]){0x40}, 1, 0},
    {0x04, (uint8_t[]){0x5E}, 1, 0},
    {0x05, (uint8_t[]){0x5E}, 1, 0},
    {0x06, (uint8_t[]){0x5F}, 1, 0},
    {0x07, (uint8_t[]){0x5F}, 1, 0},
    {0x08, (uint8_t[]){0x5F}, 1, 0},
    {0x09, (uint8_t[]){0x57}, 1, 0},
    {0x0A, (uint8_t[]){0x57}, 1, 0},
    {0x0B, (uint8_t[]){0x77}, 1, 0},
    {0x0C, (uint8_t[]){0x77}, 1, 0},
    {0x0D, (uint8_t[]){0x47}, 1, 0},
    {0x0E, (uint8_t[]){0x47}, 1, 0},
    {0x0F, (uint8_t[]){0x45}, 1, 0},
    {0x10, (uint8_t[]){0x45}, 1, 0},
    {0x11, (uint8_t[]){0x4B}, 1, 0},
    {0x12, (uint8_t[]){0x4B}, 1, 0},
    {0x13, (uint8_t[]){0x49}, 1, 0},
    {0x14, (uint8_t[]){0x49}, 1, 0},
    {0x15, (uint8_t[]){0x5F}, 1, 0},
    {0x16, (uint8_t[]){0x41}, 1, 0},
    {0x17, (uint8_t[]){0x41}, 1, 0},
    {0x18, (uint8_t[]){0x40}, 1, 0},
    {0x19, (uint8_t[]){0x40}, 1, 0},
    {0x1A, (uint8_t[]){0x5E}, 1, 0},
    {0x1B, (uint8_t[]){0x5E}, 1, 0},
    {0x1C, (uint8_t[]){0x5F}, 1, 0},
    {0x1D, (uint8_t[]){0x5F}, 1, 0},
    {0x1E, (uint8_t[]){0x5F}, 1, 0},
    {0x1F, (uint8_t[]){0x57}, 1, 0},
    {0x20, (uint8_t[]){0x57}, 1, 0},
    {0x21, (uint8_t[]){0x77}, 1, 0},
    {0x22, (uint8_t[]){0x77}, 1, 0},
    {0x23, (uint8_t[]){0x46}, 1, 0},
    {0x24, (uint8_t[]){0x46}, 1, 0},
    {0x25, (uint8_t[]){0x44}, 1, 0},
    {0x26, (uint8_t[]){0x44}, 1, 0},
    {0x27, (uint8_t[]){0x4A}, 1, 0},
    {0x28, (uint8_t[]){0x4A}, 1, 0},
    {0x29, (uint8_t[]){0x48}, 1, 0},
    {0x2A, (uint8_t[]){0x48}, 1, 0},
    {0x2B, (uint8_t[]){0x5F}, 1, 0},
    {0x2C, (uint8_t[]){0x01}, 1, 0},
    {0x2D, (uint8_t[]){0x01}, 1, 0},
    {0x2E, (uint8_t[]){0x00}, 1, 0},
    {0x2F, (uint8_t[]){0x00}, 1, 0},
    {0x30, (uint8_t[]){0x1F}, 1, 0},
    {0x31, (uint8_t[]){0x1F}, 1, 0},
    {0x32, (uint8_t[]){0x1E}, 1, 0},
    {0x33, (uint8_t[]){0x1E}, 1, 0},
    {0x34, (uint8_t[]){0x1F}, 1, 0},
    {0x35, (uint8_t[]){0x17}, 1, 0},
    {0x36, (uint8_t[]){0x17}, 1, 0},
    {0x37, (uint8_t[]){0x37}, 1, 0},
    {0x38, (uint8_t[]){0x37}, 1, 0},
    {0x39, (uint8_t[]){0x08}, 1, 0},
    {0x3A, (uint8_t[]){0x08}, 1, 0},
    {0x3B, (uint8_t[]){0x0A}, 1, 0},
    {0x3C, (uint8_t[]){0x0A}, 1, 0},
    {0x3D, (uint8_t[]){0x04}, 1, 0},
    {0x3E, (uint8_t[]){0x04}, 1, 0},
    {0x3F, (uint8_t[]){0x06}, 1, 0},
    {0x40, (uint8_t[]){0x06}, 1, 0},
    {0x41, (uint8_t[]){0x1F}, 1, 0},
    {0x42, (uint8_t[]){0x02}, 1, 0},
    {0x43, (uint8_t[]){0x02}, 1, 0},
    {0x44, (uint8_t[]){0x00}, 1, 0},
    {0x45, (uint8_t[]){0x00}, 1, 0},
    {0x46, (uint8_t[]){0x1F}, 1, 0},
    {0x47, (uint8_t[]){0x1F}, 1, 0},
    {0x48, (uint8_t[]){0x1E}, 1, 0},
    {0x49, (uint8_t[]){0x1E}, 1, 0},
    {0x4A, (uint8_t[]){0x1F}, 1, 0},
    {0x4B, (uint8_t[]){0x17}, 1, 0},
    {0x4C, (uint8_t[]){0x17}, 1, 0},
    {0x4D, (uint8_t[]){0x37}, 1, 0},
    {0x4E, (uint8_t[]){0x37}, 1, 0},
    {0x4F, (uint8_t[]){0x09}, 1, 0},
    {0x50, (uint8_t[]){0x09}, 1, 0},
    {0x51, (uint8_t[]){0x0B}, 1, 0},
    {0x52, (uint8_t[]){0x0B}, 1, 0},
    {0x53, (uint8_t[]){0x05}, 1, 0},
    {0x54, (uint8_t[]){0x05}, 1, 0},
    {0x55, (uint8_t[]){0x07}, 1, 0},
    {0x56, (uint8_t[]){0x07}, 1, 0},
    {0x57, (uint8_t[]){0x1F}, 1, 0},
    {0x58, (uint8_t[]){0x40}, 1, 0},
    {0x5B, (uint8_t[]){0x30}, 1, 0},
    {0x5C, (uint8_t[]){0x00}, 1, 0},
    {0x5D, (uint8_t[]){0x34}, 1, 0},
    {0x5E, (uint8_t[]){0x05}, 1, 0},
    {0x5F, (uint8_t[]){0x02}, 1, 0},
    {0x63, (uint8_t[]){0x00}, 1, 0},
    {0x64, (uint8_t[]){0x6A}, 1, 0},
    {0x67, (uint8_t[]){0x73}, 1, 0},
    {0x68, (uint8_t[]){0x07}, 1, 0},  // 2 lane。残影文件里的 0x05 是 4 lane 时序
    {0x69, (uint8_t[]){0x08}, 1, 0},
    {0x6A, (uint8_t[]){0x6A}, 1, 0},
    {0x6B, (uint8_t[]){0x08}, 1, 0},
    {0x6C, (uint8_t[]){0x00}, 1, 0},
    {0x6D, (uint8_t[]){0x00}, 1, 0},
    {0x6E, (uint8_t[]){0x00}, 1, 0},
    {0x6F, (uint8_t[]){0x88}, 1, 0},
    {0x75, (uint8_t[]){0xFF}, 1, 0},
    {0x77, (uint8_t[]){0xDD}, 1, 0},
    {0x78, (uint8_t[]){0x2C}, 1, 0},  // 2 lane。残影文件里的 0x24 是 4 lane 时序
    {0x79, (uint8_t[]){0x15}, 1, 0},
    {0x7A, (uint8_t[]){0x17}, 1, 0},
    {0x7D, (uint8_t[]){0x14}, 1, 0},
    {0x7E, (uint8_t[]){0x82}, 1, 0},
    {0xE0, (uint8_t[]){0x04}, 1, 0},
    {0x00, (uint8_t[]){0x0E}, 1, 0},
    {0x02, (uint8_t[]){0xB3}, 1, 0},
    {0x09, (uint8_t[]){0x61}, 1, 0},
    {0x0E, (uint8_t[]){0x48}, 1, 0},
    {0x37, (uint8_t[]){0x58}, 1, 0},  // 2 lane
    {0x2B, (uint8_t[]){0x0F}, 1, 0},  // 2 lane。残影文件的 0x36=0x48 会在 2 lane 上拉出竖条
    {0xE0, (uint8_t[]){0x00}, 1, 0},
    {0xE6, (uint8_t[]){0x02}, 1, 0},
    {0xE7, (uint8_t[]){0x0C}, 1, 0},
    {0xE0, (uint8_t[]){0x00}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 20},
    {0x35, (uint8_t[]){0x00}, 1, 0},
};

#endif // _BOARD_CONFIG_H_

