#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "esp32_camera.h"

#include "power_save_timer.h"
#include "led/single_led.h"
#include "led/circular_strip.h"
#include "assets/lang_config.h"
#include "power_manager.h"
#include "lightam_controller.h"
#include "fan_lcdili948835_display.h"
#include "custom_audio_codec.h"

#include <wifi_station.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include "esp_lcd_ili9488.h"
#include <esp_lcd_panel_commands.h>
#include <driver/spi_common.h>

#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_ota_ops.h> 
 
#define TAG "FanFutureS6ILI948835WiFiBoard"

/*
 * ILI9488 V090：B1 须 2 参数、B6 须 3 参数；NL=0x3B → 480 行。
 * 残影/紫边：恢复 CMI 的 C5/B6(0x22)；B4 试列反转 0x00；B1 略抬帧率(RTN↓)；SPI 见 config.h。
 */
namespace {

static const uint8_t ili9488_cmi35_e0[] = {0x00, 0x06, 0x0C, 0x07, 0x15, 0x0A, 0x3C, 0x89, 0x47, 0x08, 0x10, 0x0E, 0x1E, 0x22, 0x0F};
static const uint8_t ili9488_cmi35_e1[] = {0x00, 0x1F, 0x23, 0x07, 0x12, 0x07, 0x32, 0x34, 0x47, 0x04, 0x0D, 0x0B, 0x34, 0x39, 0x0F};
static const uint8_t ili9488_cmi35_c0[] = {0x13, 0x13};
static const uint8_t ili9488_cmi35_c1[] = {0x41};
static const uint8_t ili9488_cmi35_c5[] = {0x00, 0x2E, 0x80};
/* 0x36：与 DISPLAY_RGB_ORDER 一致；需 BGR 子像素时再改 bit3 */
static const uint8_t ili9488_cmi35_madctl[] = {0x00};
/* SPI 18bpp（0x66）；本模组用 16bpp+0x55 易全白/无图 */
static const uint8_t ili9488_cmi35_colmod[] = {0x66};
static const uint8_t ili9488_cmi35_b0[] = {0x00};
/* B1：第二字节 RTNA 略小→帧率略高，减轻液晶保持；异常则改回 {0xA0,0x11} */
static const uint8_t ili9488_cmi35_b1[] = {0xA0, 0x06};
/* B4：列反转(DINV=0x00)；紫边仍重再试 0x02(2-dot) 或 0x03 */
static const uint8_t ili9488_cmi35_b4[] = {0x00};
static const uint8_t ili9488_cmi35_b6[] = {0x02, 0x22, 0x3B};
static const uint8_t ili9488_cmi35_b7[] = {0xC6};
static const uint8_t ili9488_cmi35_be[] = {0x00, 0x04};
static const uint8_t ili9488_cmi35_e9[] = {0x00};
static const uint8_t ili9488_cmi35_f7[] = {0xA9, 0x51, 0x2C, 0x82};

static const ili9488_lcd_init_cmd_t ili9488_cmi35_init_cmds[] = {
    {0xE0, ili9488_cmi35_e0, sizeof(ili9488_cmi35_e0), 0},
    {0xE1, ili9488_cmi35_e1, sizeof(ili9488_cmi35_e1), 0},
    {0xC0, ili9488_cmi35_c0, sizeof(ili9488_cmi35_c0), 0},
    {0xC1, ili9488_cmi35_c1, sizeof(ili9488_cmi35_c1), 0},
    {0xC5, ili9488_cmi35_c5, sizeof(ili9488_cmi35_c5), 0},
    {LCD_CMD_MADCTL, ili9488_cmi35_madctl, sizeof(ili9488_cmi35_madctl), 0},
    {LCD_CMD_COLMOD, ili9488_cmi35_colmod, sizeof(ili9488_cmi35_colmod), 0},
    {0xB0, ili9488_cmi35_b0, sizeof(ili9488_cmi35_b0), 0},
    {0xB1, ili9488_cmi35_b1, sizeof(ili9488_cmi35_b1), 0},
    {0xB4, ili9488_cmi35_b4, sizeof(ili9488_cmi35_b4), 0},
    {0xB6, ili9488_cmi35_b6, sizeof(ili9488_cmi35_b6), 0},
    {0xB7, ili9488_cmi35_b7, sizeof(ili9488_cmi35_b7), 0},
    {0xBE, ili9488_cmi35_be, sizeof(ili9488_cmi35_be), 0},
    {0xE9, ili9488_cmi35_e9, sizeof(ili9488_cmi35_e9), 0},
    {0xF7, ili9488_cmi35_f7, sizeof(ili9488_cmi35_f7), 0},
    {LCD_CMD_SLPOUT, nullptr, 0, 120},
    {LCD_CMD_DISPON, nullptr, 0, 100},
};

static const ili9488_vendor_config_t ili9488_cmi35_vendor_config = {
    ili9488_cmi35_init_cmds,
    static_cast<uint16_t>(sizeof(ili9488_cmi35_init_cmds) / sizeof(ili9488_cmi35_init_cmds[0])),
};

}  // namespace

class FanFutureS6ILI948835WiFiBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Button mode_button_;
    Display* display_;
    PowerSaveTimer* power_save_timer_;
    PowerManager* power_manager_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Pca9557* pca9557_;
    Esp32Camera* camera_;
    bool aec_device = false;

    void InitializePowerManager() {
        power_manager_ = new PowerManager(GPIO_NUM_11);
        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
            } else {
                power_save_timer_->SetEnabled(true);
            }
        });
    }

    void InitializePowerSaveTimer() {
        //rtc_gpio_init(GPIO_NUM_37);
        //rtc_gpio_set_direction(GPIO_NUM_37, RTC_GPIO_MODE_OUTPUT_ONLY);
        //rtc_gpio_set_level(GPIO_NUM_37, 1);

        power_save_timer_ = new PowerSaveTimer(-1, 300, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "Enabling sleep mode");
            display_->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(1);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            ESP_LOGI(TAG, "Exit sleep mode");
            display_->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            ESP_LOGI(TAG, "Shutting down");
            //rtc_gpio_set_level(GPIO_NUM_37, 0);
            // 启用保持功能，确保睡眠期间电平不变
            //rtc_gpio_hold_en(GPIO_NUM_37);
            //esp_lcd_panel_disp_on_off(panel_, false); //关闭显示
            //esp_deep_sleep_start();
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
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
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        //vTaskDelay(1000 / portTICK_PERIOD_MS);

        // Scan for I2C devices
        /**uint8_t address;
        ESP_LOGI(TAG, "Scanning I2C bus...");
        for (address = 1; address < 127; address++) {
            i2c_master_bus_handle_t cmd = i2c_bus_;
            esp_err_t ret;
            ret = i2c_master_probe(cmd, address, -1);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Found I2C device at address 0x%02x", address);
            }
        }**/

        // Initialize PCA9557
        pca9557_ = new Pca9557(i2c_bus_, 0x19);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_NC;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = DISPLAY_SPI_PCLK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io_));

        /* 面板：CMI 3.5 ILI9488；初始化表见本文件 ili9488_cmi35_init_cmds + vendor_config */
        ESP_LOGD(TAG, "Install LCD driver (ILI9488 CMI3.5, local esp_lcd_ili9488)");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.vendor_config = (void *)&ili9488_cmi35_vendor_config;
        /* 18bpp：驱动内 RGB565→24bit 写屏；缓冲须 ≥ LVGL 单次刷新行数 */
        panel_config.bits_per_pixel = 18;
        const size_t ili9488_spi_buf_pixels = (size_t)DISPLAY_WIDTH * 28;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9488(panel_io_, &panel_config, ili9488_spi_buf_pixels, &panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, DISPLAY_INVERT_COLOR));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));

        display_ = new FanLcdILI948835Display(panel_io_, panel_,
                                  DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                  DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }
 
    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto& app = Application::GetInstance();
            app.ToggleChatState();
        });

        mode_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        mode_button_.OnLongPress([this]() {
            #if CONFIG_USE_DEVICE_AEC
            power_save_timer_->WakeUp();
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
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        //static LightAMController lamp(LIGHT_AM_GPIO);
    }

    void InitializeCamera() {
        // Open camera power
        pca9557_->SetOutputState(2, 0);

        camera_config_t config = {};
        config.ledc_channel = LEDC_CHANNEL_2;
        config.ledc_timer = LEDC_TIMER_2;
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
        config.pin_sccb_sda = -1;
        config.pin_sccb_scl = CAMERA_PIN_SIOC;
        config.sccb_i2c_port = 1;
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.xclk_freq_hz = XCLK_FREQ_HZ;
        config.pixel_format = PIXFORMAT_RGB565;
        config.frame_size = FRAMESIZE_VGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

        camera_ = new Esp32Camera(config);
        camera_->SetVFlip(true);
    }

public:
    FanFutureS6ILI948835WiFiBoard() :
        boot_button_(BOOT_BUTTON_GPIO),
        mode_button_(MODE_BUTTON_GPIO) {
        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeI2c();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializeIot();
        InitializeCamera();
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            GetBacklight()->RestoreBrightness();
        }  

        #if CONFIG_USE_DEVICE_AEC
        Settings settings("aecMode", false);
        aec_device = settings.GetBool("aec_device", aec_device);
        auto& app = Application::GetInstance();
        app.SetAecMode(aec_device ? kAecOnDeviceSide : kAecOff);
        #endif
    }

    /**virtual Led* GetLed() override {
        static CircularStrip led(BUILTIN_LED_GPIO, 4);
        return &led;
    }**/

    virtual AudioCodec* GetAudioCodec() override {
        static CustomAudioCodec audio_codec(
            i2c_bus_, 
            pca9557_);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
            return &backlight;
        }
        return nullptr;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        level = power_manager_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }
};

DECLARE_BOARD(FanFutureS6ILI948835WiFiBoard);
