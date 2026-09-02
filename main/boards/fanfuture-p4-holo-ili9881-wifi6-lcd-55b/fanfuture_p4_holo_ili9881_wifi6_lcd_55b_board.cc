#include "codecs/box_audio_codec.h"
#include "application.h"
#include "fan_mipi55_display.h"
#include "esp_lcd_ili9881c.h"
#include "button.h"
#include "led/single_led.h"
#include "led/circular_strip.h"
#include "config.h"
#include "esp_video.h"

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_lvgl_port.h>
#include <rom/ets_sys.h>
#include "freertos/task.h"

#include "axp2101.h"
#include "sd_scanner.h"
#include "wifi_board.h"

#define TAG "FanFutureP4HoloILI9881WiFi6Lcd55BBoard"

class FanFutureP4HoloILI9881WiFi6Lcd55BBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;  // I2C1: Audio Codec + Camera
    i2c_master_bus_handle_t i2c_bus_touch_;  // I2C0: Touch + AXP2101
    Button boot_button_;
    Button io_button_;
    LcdDisplay *display_;
    Axp2101* pmic_ = nullptr;
    EspVideo* camera_ = nullptr;
    bool aec_device = true;
    int volume_direction_ = -1; // io_button 默认减小方向，撞到边界时反向

    esp_err_t i2c_device_probe(uint8_t addr) {
        return i2c_master_probe(i2c_bus_touch_, addr, 100);
    }

    void InitializeCodecI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {};
        i2c_bus_cfg.i2c_port = I2C_NUM_1;
        i2c_bus_cfg.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
        i2c_bus_cfg.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
        i2c_bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_bus_cfg.glitch_ignore_cnt = 7;
        i2c_bus_cfg.intr_priority = 0;
        i2c_bus_cfg.trans_queue_depth = 0;
        i2c_bus_cfg.flags.enable_internal_pullup = true;
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
        ESP_LOGI(TAG, "🎵 音频编解码器I2C1初始化完成 (SDA: GPIO%d, SCL: GPIO%d)",
                 AUDIO_CODEC_I2C_SDA_PIN, AUDIO_CODEC_I2C_SCL_PIN);

        // Scan for I2C devices (with short timeout per address)
        uint8_t address;
        ESP_LOGI(TAG, "Scanning I2C bus...");
        for (address = 1; address < 127; address++) {
            i2c_master_bus_handle_t cmd = i2c_bus_;
            esp_err_t ret;
            ret = i2c_master_probe(cmd, address, -1);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Found I2C device at address 0x%02x", address);
            }
        }
    }

    void InitializeTouchI2c() {
        ESP_LOGI(TAG, "🔄 初始化I2C0总线 (触摸屏 + AXP2101)...");
        i2c_master_bus_config_t i2c_bus_cfg = {};
        i2c_bus_cfg.i2c_port = I2C_NUM_0;
        i2c_bus_cfg.sda_io_num = TOUCH_SDA_PIN;
        i2c_bus_cfg.scl_io_num = TOUCH_SCL_PIN;
        i2c_bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_bus_cfg.glitch_ignore_cnt = 7;
        i2c_bus_cfg.intr_priority = 0;
        i2c_bus_cfg.trans_queue_depth = 0;
        i2c_bus_cfg.flags.enable_internal_pullup = true;

        esp_err_t ret = i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_touch_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "❌ I2C0总线创建失败: %s", esp_err_to_name(ret));
            return;
        }
        ESP_LOGI(TAG, "✅ I2C0总线初始化成功 (SDA: GPIO%d, SCL: GPIO%d)",
                 TOUCH_SDA_PIN, TOUCH_SCL_PIN);
    }

    void InitializeAXP2101() {
        ESP_LOGI(TAG, "🔌 初始化AXP2101电源管理芯片...");

        vTaskDelay(pdMS_TO_TICKS(100));

        try {
            pmic_ = new Axp2101(i2c_bus_touch_);

            if (!pmic_ || !pmic_->dev_handle) {
                ESP_LOGW(TAG, "⚠️  未检测到AXP2101设备 - 硬件可能不包含此芯片");
                if (pmic_) {
                    delete pmic_;
                    pmic_ = nullptr;
                }
                return;
            }

            uint8_t chip_id = pmic_->GetChipId();
            if (chip_id == 0) {
                ESP_LOGW(TAG, "⚠️  AXP2101无响应 (ID: 0x00)");
                ESP_LOGW(TAG, "     将在无PMIC支持下继续运行...");
                delete pmic_;
                pmic_ = nullptr;
                return;
            }

            /**if (chip_id != 0x4A && chip_id != 0x4B) {
                ESP_LOGW(TAG, "⚠️  AXP2101芯片ID无效: 0x%02X (期望0x4A或0x4B)", chip_id);
                delete pmic_;
                pmic_ = nullptr;
                return;
            }**/

            ESP_LOGI(TAG, "✅ AXP2101芯片ID验证成功: 0x%02X", chip_id);

            ESP_LOGI(TAG, "⚙️  配置电源输出...");
            //ESP_ERROR_CHECK(pmic_->SetDcdc1Voltage(3300));
            //ESP_ERROR_CHECK(pmic_->EnableDcdc1(true));
            //ESP_LOGI(TAG, "   ⚡ DCDC1: 3.3V");

            /**ESP_ERROR_CHECK(pmic_->SetDcdc2Voltage(1200));
            ESP_ERROR_CHECK(pmic_->EnableDcdc2(true));
            ESP_LOGI(TAG, "   ⚡ DCDC2: 1.2V");

            ESP_ERROR_CHECK(pmic_->SetDcdc3Voltage(3400));
            ESP_ERROR_CHECK(pmic_->EnableDcdc3(true));
            ESP_LOGI(TAG, "   ⚡ DCDC3: 3.4V");**/

            ESP_ERROR_CHECK(pmic_->SetDcdc4Voltage(1800));
            ESP_ERROR_CHECK(pmic_->EnableDcdc4(true));
            ESP_LOGI(TAG, "   ⚡ DCDC4: 1.8V");

            ESP_ERROR_CHECK(pmic_->SetAldo1Voltage(3300));
            ESP_ERROR_CHECK(pmic_->EnableAldo1(true));
            ESP_LOGI(TAG, "   💡 ALDO1: 3.3V");

            /**ESP_ERROR_CHECK(pmic_->SetAldo2Voltage(1800));
            ESP_ERROR_CHECK(pmic_->EnableAldo2(true));
            ESP_LOGI(TAG, "   🔋 ALDO2: 1.8V");

            ESP_ERROR_CHECK(pmic_->SetAldo4Voltage(3300));
            ESP_ERROR_CHECK(pmic_->EnableAldo4(true));
            ESP_LOGI(TAG, "   💡 ALDO4: 3.3V");**/

            ESP_ERROR_CHECK(pmic_->EnableBatteryVoltageAdc(true));
            ESP_ERROR_CHECK(pmic_->EnableVbusVoltageAdc(true));
            ESP_ERROR_CHECK(pmic_->EnableSystemVoltageAdc(true));
            ESP_ERROR_CHECK(pmic_->EnableTemperatureAdc(true));

            // Official: disable TS/NTC so floating TS does not suspend/throttle charge.
            ESP_ERROR_CHECK(pmic_->DisableTsPinMeasure());
            // VINDPM 4.12V avoids 5V USB cable drop hitting default 4.36V DPM.
            ESP_ERROR_CHECK(pmic_->SetVbusVoltageLimit(Axp2101VbusVol::VOL_4V12));
            ESP_ERROR_CHECK(pmic_->SetVbusCurrentLimit(Axp2101VbusCurr::LIM_2000MA));
            // AXP2101 ICHG max is 1000mA (not 3000mA). CUR_3000MA overflowed to 100mA.
            ESP_ERROR_CHECK(pmic_->SetChargeCurrent(Axp2101ChgCurr::CUR_1000MA));
            ESP_ERROR_CHECK(pmic_->SetChargeTargetVoltage(Axp2101ChgVol::VOL_4V2));
            ESP_ERROR_CHECK(pmic_->EnableCharging(true));
            ESP_LOGI(TAG, "⚡ 充电配置: ICHG 1000mA @ 4.2V, IINLIM 2000mA");

            ESP_LOGI(TAG, "✅ AXP2101初始化完成");

            uint16_t bat_vol = pmic_->GetBatteryVoltage();
            int bat_percent = pmic_->GetBatteryPercent();
            bool is_charging = pmic_->IsCharging();

            ESP_LOGI(TAG, "========================================");
            ESP_LOGI(TAG, "🔋 电池状态");
            ESP_LOGI(TAG, "========================================");
            ESP_LOGI(TAG, "电量: %d%%", bat_percent);
            ESP_LOGI(TAG, "电压: %u mV", bat_vol);
            ESP_LOGI(TAG, "充电: %s", is_charging ? "✅ 是" : "❌ 否");
            ESP_LOGI(TAG, "========================================");
        } catch (const std::exception& e) {
            ESP_LOGW(TAG, "⚠️  AXP2101初始化异常: %s", e.what());
            ESP_LOGW(TAG, "     将在无PMIC支持下继续运行...");
            if (pmic_) {
                delete pmic_;
                pmic_ = nullptr;
            }
        } catch (...) {
            ESP_LOGW(TAG, "⚠️  AXP2101初始化未知异常");
            ESP_LOGW(TAG, "     将在无PMIC支持下继续运行...");
            if (pmic_) {
                delete pmic_;
                pmic_ = nullptr;
            }
        }
    }

    static esp_err_t bsp_enable_dsi_phy_power(void) {
#if MIPI_DSI_PHY_PWR_LDO_CHAN > 0
        static esp_ldo_channel_handle_t phy_pwr_chan = NULL;
        if (phy_pwr_chan == NULL) {
            esp_ldo_channel_config_t ldo_cfg = {
                .chan_id = MIPI_DSI_PHY_PWR_LDO_CHAN,
                .voltage_mv = MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
            };
            esp_err_t ret = esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr_chan);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "✅ MIPI DSI & CSI PHY LDO已启用 (2.5V, 通道%d)", MIPI_DSI_PHY_PWR_LDO_CHAN);
            } else {
                ESP_LOGE(TAG, "❌ MIPI PHY LDO初始化失败: %s", esp_err_to_name(ret));
                return ret;
            }
        }
#endif
        return ESP_OK;
    }

    void InitializeIli9881cDisplay() {
        ESP_LOGI(TAG, "🖥️  初始化ILI9881C LCD显示屏 (5.5寸 720x1280)...");
        bsp_enable_dsi_phy_power();

        esp_lcd_panel_io_handle_t panel_io = NULL;
        esp_lcd_panel_handle_t panel = NULL;

        ESP_LOGI(TAG, "Install MIPI DSI LCD control panel");
        esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
        esp_lcd_dsi_bus_config_t bus_config = {
            .bus_id             = 0,
            .num_data_lanes     = 2,
            .phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
            .lane_bit_rate_mbps = 480,
        };
        ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus));
        ESP_LOGI(TAG, "   MIPI DSI总线: 2通道, %d Mbps", bus_config.lane_bit_rate_mbps);

        ESP_LOGI(TAG, "Install panel IO");
        esp_lcd_dbi_io_config_t dbi_config = {
            .virtual_channel = 0,
            .lcd_cmd_bits    = 8,
            .lcd_param_bits  = 8,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &panel_io));

        ESP_LOGI(TAG, "Install LCD driver of ili9881c");
        esp_lcd_dpi_panel_config_t dpi_config = {
            .virtual_channel    = 0,
            .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
            .dpi_clock_freq_mhz = 46,
            .pixel_format       = LCD_COLOR_PIXEL_FORMAT_RGB565,
            .num_fbs            = 2,
            .video_timing = {
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
            }
        };

        ili9881c_vendor_config_t vendor_config = {
            .init_cmds      = ili9881c_init_cmds,
            .init_cmds_size = sizeof(ili9881c_init_cmds) / sizeof(ili9881c_init_cmds[0]),
            .mipi_config = {
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

        ESP_LOGI(TAG, "   分辨率: %dx%d", DISPLAY_WIDTH, DISPLAY_HEIGHT);
        ESP_LOGI(TAG, "   DPI时钟: %d MHz", dpi_config.dpi_clock_freq_mhz);

        display_ = new FanMIPI55Display(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X,
                                      DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        ESP_LOGI(TAG, "✅ ILI9881C LCD (5.5寸 720x1280) 初始化完成");
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

        boot_button_.OnLongPress([this]() {
            #if CONFIG_USE_DEVICE_AEC
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);

                aec_device = app.GetAecMode() == kAecOnDeviceSide ? true : false;
                Settings settings("aecMode", true);
                settings.SetBool("aec_device", aec_device);

                auto codec = GetAudioCodec();
                auto volume = codec->output_volume();
                codec->SetOutputVolume(volume);
            }
            #endif
        });

        io_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + volume_direction_ * 10;
            if (volume <= 0) {
                volume = 0;
                volume_direction_ = 1; // 撞到 0，反向增大
            } else if (volume >= 100) {
                volume = 100;
                volume_direction_ = -1; // 撞到 100，反向减小
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });
    }

    void InitializeSdForMjpeg() {
        ESP_LOGI(TAG, "💾 初始化SD卡扫描器(MJPEG播放准备)...");
        const esp_err_t ret = sd_scanner_init_and_scan();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "⚠️ SD卡初始化/扫描失败，MJPEG动画将回退到静态表情: %s", esp_err_to_name(ret));
            return;
        }
        ESP_LOGI(TAG, "✅ SD卡已挂载并完成扫描");
    }

    void InitializeCamera() {
#if CONFIG_ESP_VIDEO_ENABLE_MIPI_CSI_VIDEO_DEVICE
        ESP_LOGI(TAG, "📷 初始化MIPI CSI摄像头...");
        try {
            if (!i2c_bus_) {
                ESP_LOGE(TAG, "❌ I2C1总线未初始化");
                camera_ = nullptr;
                return;
            }

            ESP_LOGI(TAG, "   扫描I2C1总线检测摄像头设备...");
            bool camera_found = false;
            for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
                esp_err_t ret = i2c_master_probe(i2c_bus_, addr, 100);
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "   发现I2C设备: 0x%02X%s", addr,
                             (addr == CAMERA_SCCB_ADDR) ? " [摄像头]" :
                             (addr == AUDIO_CODEC_ES8311_ADDR) ? " [ES8311]" :
                             (addr == AUDIO_CODEC_ES7210_ADDR) ? " [ES7210]" : "");
                    if (addr == CAMERA_SCCB_ADDR) {
                        camera_found = true;
                    }
                }
            }

            if (!camera_found) {
                ESP_LOGW(TAG, "⚠️  未检测到摄像头 @ 0x%02X", CAMERA_SCCB_ADDR);
                camera_ = nullptr;
                return;
            }

            esp_video_init_csi_config_t csi_config = {
                .sccb_config = {
                    .init_sccb = false,
                    .i2c_handle = i2c_bus_,
                    .freq = 100000,
                },
                .reset_pin = (gpio_num_t)CAMERA_RESET_PIN,
                .pwdn_pin = (gpio_num_t)CAMERA_PWDN_PIN,
            };

            esp_video_init_config_t cam_config = {
                .csi = &csi_config,
            };

            camera_ = new EspVideo(cam_config);
            ESP_LOGI(TAG, "✅ 摄像头初始化完成");
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "❌ 摄像头初始化失败: %s", e.what());
            camera_ = nullptr;
        } catch (...) {
            ESP_LOGE(TAG, "❌ 摄像头初始化失败");
            camera_ = nullptr;
        }
#else
        ESP_LOGW(TAG, "⚠️  MIPI CSI未启用，跳过摄像头初始化");
        camera_ = nullptr;
#endif
    }

public:
    FanFutureP4HoloILI9881WiFi6Lcd55BBoard() :
        WifiBoard(),
        boot_button_(BOOT_BUTTON_GPIO),
        io_button_(IO_BUTTON_GPIO) {
        InitializeCodecI2c();
        InitializeTouchI2c();
        InitializeAXP2101();
        InitializeSdForMjpeg();
        InitializeIli9881cDisplay();
        //InitializeCamera();
        InitializeButtons();
        GetBacklight()->RestoreBrightness();

        /**#if CONFIG_USE_DEVICE_AEC
        Settings settings("aecMode", false);
        aec_device = settings.GetBool("aec_device", aec_device);
        auto& app = Application::GetInstance();
        app.SetAecMode(aec_device ? kAecOnDeviceSide : kAecOff);
        #endif**/
    }

    ~FanFutureP4HoloILI9881WiFi6Lcd55BBoard() {
        if (pmic_) {
            delete pmic_;
            pmic_ = nullptr;
        }
    }

    AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE,
            37.0f,  // Physical MIC1 gain (ES7210 ~37.5dB max)
            2,      // Physical MIC3 is the playback reference input
            0.0f);  // reference_gain
        return &audio_codec;
    }

    /**virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }**/

    virtual Led* GetLed() override {
        static CircularStrip led(BUILTIN_LED_GPIO, 16);
        return &led;
    }

    virtual Display *GetDisplay() override {
        return display_;
    }

    /**virtual Camera* GetCamera() override {
        return camera_;
    }**/

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, false);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = pmic_->IsCharging();
        discharging = !pmic_->IsCharging();
        if (discharging != last_discharging) {
            last_discharging = discharging;
        }
        level = pmic_->GetBatteryPercent();

        return true;
    }

#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    virtual void StartWifiConfigMode() override {
        ESP_LOGI(TAG, "Starting BluFi WiFi configuration mode");

        // Call parent implementation
        WifiBoard::StartWifiConfigMode();

        // Show notification on display
        std::string hint = std::string(Lang::Strings::BLUFI_CINFIG) + "Xiaozhi-Blufi";
        Application::GetInstance().Alert(hint.c_str(), Lang::Strings::ENTERING_WIFI_CONFIG_MODE, "gear", Lang::Sounds::OGG_WIFICONFIG);
    }
#endif

};

DECLARE_BOARD(FanFutureP4HoloILI9881WiFi6Lcd55BBoard);
