#include "codecs/box_audio_codec.h"
#include "application.h"
#include "fan_mipi70_display.h"
#include "button.h"
#include "config.h"
#include "esp_video.h"

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"

#include "esp_lcd_jd9165.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_lvgl_port.h>
#include <rom/ets_sys.h>
#include "esp_lcd_touch_gt911.h"
#include "freertos/task.h"

#include "axp2101.h"
#include "sd_scanner.h"
#include "wifi_board.h"

#define TAG "FanFutureP4JD9165WiFi6TouchLcd7BBoard"

class FanFutureP4JD9165WiFi6TouchLcd7BBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;  // I2C1: Audio Codec + Camera
    i2c_master_bus_handle_t i2c_bus_touch_;  // I2C0: Touch + AXP2101
    Button boot_button_;
    LcdDisplay *display_;
    Axp2101* pmic_ = nullptr;
    EspVideo* camera_ = nullptr;

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

            if (chip_id != 0x4A && chip_id != 0x4B) {
                ESP_LOGW(TAG, "⚠️  AXP2101芯片ID无效: 0x%02X (期望0x4A或0x4B)", chip_id);
                delete pmic_;
                pmic_ = nullptr;
                return;
            }

            ESP_LOGI(TAG, "✅ AXP2101芯片ID验证成功: 0x%02X", chip_id);

            ESP_LOGI(TAG, "⚙️  配置电源输出...");
            ESP_ERROR_CHECK(pmic_->SetDcdc4Voltage(1800));
            ESP_ERROR_CHECK(pmic_->EnableDcdc4(true));
            ESP_LOGI(TAG, "   📟 DCDC4: 1.8V (系统电压)");

            ESP_ERROR_CHECK(pmic_->SetAldo2Voltage(1800));
            ESP_ERROR_CHECK(pmic_->EnableAldo2(true));
            ESP_LOGI(TAG, "   🔋 ALDO2: 1.8V");

            ESP_ERROR_CHECK(pmic_->SetAldo4Voltage(3300));
            ESP_ERROR_CHECK(pmic_->EnableAldo4(true));
            ESP_LOGI(TAG, "   💡 ALDO4: 3.3V");

            ESP_LOGI(TAG, "   📡 DCDC3: 已禁用 (仅WiFi模式，无需4G供电)");

            ESP_ERROR_CHECK(pmic_->EnableBatteryVoltageAdc(true));
            ESP_ERROR_CHECK(pmic_->EnableVbusVoltageAdc(true));
            ESP_ERROR_CHECK(pmic_->EnableSystemVoltageAdc(true));
            ESP_ERROR_CHECK(pmic_->EnableTemperatureAdc(true));

            ESP_ERROR_CHECK(pmic_->SetChargeCurrent(Axp2101ChgCurr::CUR_3000MA));
            ESP_ERROR_CHECK(pmic_->SetChargeTargetVoltage(Axp2101ChgVol::VOL_4V2));
            ESP_ERROR_CHECK(pmic_->EnableCharging(true));
            ESP_LOGI(TAG, "⚡ 充电配置: 3000mA @ 4.2V");

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

    void InitializeLCD() {
        ESP_LOGI(TAG, "🖥️  初始化JD9165 LCD显示屏...");
        bsp_enable_dsi_phy_power();
        esp_lcd_panel_io_handle_t io = NULL;
        esp_lcd_panel_handle_t disp_panel = NULL;

        esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
        esp_lcd_dsi_bus_config_t bus_config = {
            .bus_id = 0,
            .num_data_lanes = 2,
            .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
            .lane_bit_rate_mbps = 614,
        };
        esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus);
        ESP_LOGI(TAG, "   MIPI DSI总线: 2通道, %d Mbps", bus_config.lane_bit_rate_mbps);

        ESP_LOGI(TAG, "Install MIPI DSI LCD control panel");
        esp_lcd_dbi_io_config_t dbi_config = JD9165_PANEL_IO_DBI_CONFIG();
        esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &io);

        esp_lcd_dpi_panel_config_t dpi_config = {};
        dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
        dpi_config.dpi_clock_freq_mhz = 51;
        dpi_config.virtual_channel = 0;
        dpi_config.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
        /* 双帧缓冲：一边扫描输出时，另一块可供 CPU/DMA2D 写入，显著降低
         * "previous draw operation is not finished" 与 MJPEG+LVGL 抢同一提交槽的概率 */
        dpi_config.num_fbs = 2;
        dpi_config.video_timing.h_size = 1024;
        dpi_config.video_timing.v_size = 600;
        dpi_config.video_timing.hsync_pulse_width = 24;
        dpi_config.video_timing.hsync_back_porch = 136;
        dpi_config.video_timing.hsync_front_porch = 160;
        dpi_config.video_timing.vsync_pulse_width = 2;
        dpi_config.video_timing.vsync_back_porch = 21;
        dpi_config.video_timing.vsync_front_porch = 12;
        dpi_config.flags.use_dma2d = true;

        jd9165_vendor_config_t vendor_config = {
            .mipi_config = {
                .dsi_bus = mipi_dsi_bus,
                .dpi_config = &dpi_config,
            },
        };

        const esp_lcd_panel_dev_config_t lcd_dev_config = {
            .reset_gpio_num = PIN_NUM_LCD_RST,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = 16,
            .vendor_config = &vendor_config,
        };
        esp_lcd_new_panel_jd9165(io, &lcd_dev_config, &disp_panel);
        esp_lcd_panel_reset(disp_panel);
        esp_lcd_panel_init(disp_panel);
        ESP_LOGI(TAG, "   分辨率: %dx%d", DISPLAY_WIDTH, DISPLAY_HEIGHT);
        ESP_LOGI(TAG, "   DPI时钟: %d MHz", dpi_config.dpi_clock_freq_mhz);

        display_ = new FanMIPI70Display(io, disp_panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X,
                                      DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        ESP_LOGI(TAG, "✅ JD9165 LCD初始化完成");
    }
    void ResetTouchGT911()
    {
        ESP_LOGI(TAG, "🔄 复位GT911触摸控制器...");

        gpio_config_t int_conf = {};
        int_conf.pin_bit_mask = (1ULL << TOUCH_INT_PIN);
        int_conf.mode = GPIO_MODE_OUTPUT;
        int_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        int_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        gpio_config(&int_conf);
        gpio_set_level(TOUCH_INT_PIN, 0);

        gpio_config_t rst_conf = {};
        rst_conf.pin_bit_mask = (1ULL << TOUCH_RST_PIN);
        rst_conf.mode = GPIO_MODE_OUTPUT;
        rst_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        rst_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        gpio_config(&rst_conf);

        gpio_set_level(TOUCH_RST_PIN, 0);
        esp_rom_delay_us(20000);
        gpio_set_level(TOUCH_RST_PIN, 1);
        esp_rom_delay_us(100000);

        int_conf.mode = GPIO_MODE_INPUT;
        gpio_config(&int_conf);
        esp_rom_delay_us(50000);
        ESP_LOGI(TAG, "✅ GT911复位完成 (I2C地址: 0x5D)");
    }

    void InitializeTouch()
    {
        ESP_LOGI(TAG, "👆 初始化GT911触摸屏...");

        esp_lcd_touch_handle_t tp;
        esp_lcd_touch_config_t tp_cfg = {};
        tp_cfg.x_max = DISPLAY_WIDTH;
        tp_cfg.y_max = DISPLAY_HEIGHT;
        tp_cfg.rst_gpio_num = TOUCH_RST_PIN;
        tp_cfg.int_gpio_num = GPIO_NUM_NC;
        tp_cfg.levels.reset = 0;
        tp_cfg.levels.interrupt = 0;
        tp_cfg.flags.swap_xy = 0;
        tp_cfg.flags.mirror_x = 0;
        tp_cfg.flags.mirror_y = 0;

        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = {
            .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
            .control_phase_bytes = 1,
            .dc_bit_offset = 0,
            .lcd_cmd_bits = 16,
            .flags = {
                .disable_control_phase = 1,
            },
        };

        if (ESP_OK == i2c_device_probe(ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS)) {
            ESP_LOGI(TAG, "   检测到触摸屏 @ 0x%02X", ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS);
        } else if (ESP_OK == i2c_device_probe(ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP)) {
            ESP_LOGI(TAG, "   检测到触摸屏 @ 0x%02X", ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP);
            tp_io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
        } else {
            ESP_LOGW(TAG, "⚠️  未检测到GT911触摸屏");
            return;
        }

        tp_io_config.scl_speed_hz = 400 * 1000;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_touch_, &tp_io_config, &tp_io_handle));
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &tp));

        lvgl_port_touch_cfg_t touch_cfg = {};
        touch_cfg.disp = lv_display_get_default();
        touch_cfg.handle = tp;
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "✅ GT911触摸屏初始化完成");
    }
    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            app.ToggleChatState();
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
    FanFutureP4JD9165WiFi6TouchLcd7BBoard() :
        WifiBoard(),
        boot_button_(BOOT_BUTTON_GPIO) {
        InitializeCodecI2c();
        InitializeTouchI2c();
        InitializeAXP2101();
        ResetTouchGT911();
        InitializeLCD();
        InitializeSdForMjpeg();
        InitializeTouch();
        InitializeCamera();
        InitializeButtons();
        GetBacklight()->RestoreBrightness();
    }

    ~FanFutureP4JD9165WiFi6TouchLcd7BBoard() {
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
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display *GetDisplay() override {
        return display_;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

};

DECLARE_BOARD(FanFutureP4JD9165WiFi6TouchLcd7BBoard);
