#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "application.h"
#include "fan_mipi50_display.h"
#include "esp_lcd_ili9881c.h"
#include "button.h"
#include "config.h"
#include <wifi_station.h>
#include <esp_log.h>
#include <driver/i2c_master.h>

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#include <esp_lcd_panel_vendor.h>

#define TAG "FanFutureHolo1P450WiFiBoard"

class FanFutureHolo1P450WiFiBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    LcdDisplay* display_;
    Button boot_button_;

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    void InitializeIli9881cDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        ESP_LOGI(TAG, "Turn on the power for MIPI DSI PHY");
        static esp_ldo_channel_handle_t phy_pwr_chan = NULL;
        esp_ldo_channel_config_t ldo_cfg = {
            .chan_id = MIPI_DSI_PHY_PWR_LDO_CHAN,
            .voltage_mv = MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
        };
        esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr_chan);

        ESP_LOGI(TAG, "Install MIPI DSI LCD control panel");
        esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
        esp_lcd_dsi_bus_config_t bus_config = {
            .bus_id             = 0,            /* 总线ID */
            .num_data_lanes     = 2,            /* 2路数据信号 */
            .phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT, /* DPHY时钟源为20M */
            .lane_bit_rate_mbps = 480,          /* 数据通道比特率(Mbps) */
        };
        ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus));

        ESP_LOGI(TAG, "Install panel IO");
        esp_lcd_dbi_io_config_t dbi_config = {
            .virtual_channel = 0,                               /* 虚拟通道(只有一个LCD连接,设置0即可) */
            .lcd_cmd_bits    = 8,                               /* 根据MIPI LCD驱动IC规格设置 命令位宽度 */
            .lcd_param_bits  = 8,                               /* 根据MIPI LCD驱动IC规格设置 参数位宽度 */
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &panel_io));

        ESP_LOGI(TAG, "Install LCD driver of ili9881c");
        esp_lcd_dpi_panel_config_t dpi_config = {.virtual_channel    = 0,
                                                 .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
                                                 .dpi_clock_freq_mhz = 46,
                                                 .pixel_format       = LCD_COLOR_PIXEL_FORMAT_RGB565,
                                                 .num_fbs            = 1,
                                                 .video_timing =
                                                     {
                                                         .h_size            = 720,
                                                         .v_size            = 1280,
                                                         .hsync_pulse_width = 8,
                                                         .hsync_back_porch  = 52,
                                                         .hsync_front_porch = 48,
                                                         .vsync_pulse_width = 5,
                                                         .vsync_back_porch  = 15,
                                                         .vsync_front_porch = 16,
                                                     },
                                                 .flags = {
                                                     .use_dma2d = true,
                                                 }};


        ili9881c_vendor_config_t vendor_config = {
            .init_cmds      = ili9881d_init_cmds,
            .init_cmds_size = sizeof(ili9881d_init_cmds) /
                              sizeof(ili9881d_init_cmds[0]),
            .mipi_config =
                {
                    .dsi_bus    = mipi_dsi_bus,
                    .dpi_config = &dpi_config,
                    .lane_num   = 2,
                },
        };

        esp_lcd_panel_dev_config_t lcd_dev_config = {};
        lcd_dev_config.rgb_ele_order              = LCD_RGB_ELEMENT_ORDER_RGB;
        lcd_dev_config.reset_gpio_num             = PIN_NUM_LCD_RST;
        lcd_dev_config.bits_per_pixel             = LCD_BIT_PER_PIXEL;
        lcd_dev_config.vendor_config              = &vendor_config;

        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9881c(panel_io, &lcd_dev_config, &panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

        display_ = new FanMIPI50Display(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X,
                                      DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState(); 
            });
    }

public:
    FanFutureHolo1P450WiFiBoard() :
        boot_button_(BOOT_BUTTON_GPIO) {
        InitializeCodecI2c();
        InitializeIli9881cDisplay();
        InitializeButtons();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec *GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(codec_i2c_bus_, I2C_NUM_1, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
                                            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, false);
        return &backlight;
    }

};

DECLARE_BOARD(FanFutureHolo1P450WiFiBoard);
