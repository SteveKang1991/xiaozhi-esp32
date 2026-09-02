#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "lvgl_display.h"
#include "gif/lvgl_gif.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <font_emoji.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <memory>
#include <cmath>
#include <cstdint>

#define PREVIEW_IMAGE_DURATION_MS 5000

// FFT相关常量
#define FFT_SIZE 512


class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    
    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* bottom_bar_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    bool hide_subtitle_ = false;  // Control whether to hide chat messages/subtitles

    void InitializeLcdThemes();
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    // ========== FFT 相关成员变量 ==========
    // 一次性 arena：audio_data + fft_real/imag + hanning，避免 4 次 malloc 碎片
    uint8_t* fft_arena_ = nullptr;
    int16_t* audio_data_ = nullptr;
    float* fft_real_ = nullptr;
    float* fft_imag_ = nullptr;
    float* hanning_window_ = nullptr;

    // 频谱数据
    static float avg_power_spectrum_[FFT_SIZE / 2];
    static int bar_heights_[32];   // 柱子：快冲快落
    static int peak_heights_[32];  // 顶部小方块：慢回落

    static constexpr int FFT_BAR_COUNT = 24;
    static constexpr int FFT_CAP_H = 8;      // 峰值帽高度（原 4 的 2 倍）
    static constexpr int FFT_CAP_GAP = 2;
    static constexpr int FFT_BLOCK_STEP = 6;  // 柱子量化台阶，不随帽高变化
    lv_obj_t* fft_container_ = nullptr;
    int draw_bar_h_[FFT_BAR_COUNT] = {0};    // 持锁后快照，供 DRAW_MAIN 用
    int draw_peak_h_[FFT_BAR_COUNT] = {0};
    lv_color_t fft_cap_color_[FFT_BAR_COUNT];
    int last_draw_bar_h_[FFT_BAR_COUNT] = {0};
    int last_draw_peak_h_[FFT_BAR_COUNT] = {0};

    // FFT 显示状态
    bool fft_data_ready_ = false;
    bool fft_enabled_ = false;
    TaskHandle_t fft_task_handle_ = nullptr;
    std::atomic<bool> fft_task_should_stop_{false};

    // 频谱：单个容器自定义绘制，避免每帧 48 次 set_y 触发软件旋转。

    // 单条频谱条的样式属性（创建后缓存，避免每帧重新设置）
    int fft_bar_width_  = 0;                   // 单条宽度
    int fft_bar_gap_    = 2;                   // 条间距
    int fft_bar_max_h_  = 0;                   // 最大高度
    int fft_total_w_    = 0;                   // 频谱区总宽度
    int fft_origin_x_   = 0;                   // 起点 X
    int fft_origin_y_   = 0;                   // 基线 Y（底部）

    // ========== FFT 相关方法 ==========
    bool ensureFftBuffers();                     // 懒分配 FFT 计算内存 + hanning 窗（进程内只分配一次）
    void createFftBars(lv_obj_t* parent = nullptr);  // 只创建频谱容器（自定义绘制，避免几十个 lv_obj）
    void destroyFftBars();                            // 仅析构 / teardown 时销毁
    void setFftBarsVisible(bool visible);             // 进出音乐：隐藏而不是删 obj
    void teardownFft();                               // 真正释放任务 / obj / arena
    void readAudioData();
    void drawSpectrumIfReady();
    void drawSpectrum();
    static void fftDrawEventCb(lv_event_t* e);
    void drawFftLayer(lv_layer_t* layer, lv_obj_t* obj);
    lv_color_t getBarColor();
    lv_color_t getCapColor(int idx);
    void computeFft(float* real, float* imag, int n, bool forward);
    
    // FFT 任务
    void periodicUpdateTask();
    static void periodicUpdateTaskWrapper(void* arg);
    
    // 启用/禁用 FFT
public:
    virtual void EnableFft(bool enable) override;
    void StopFft() override;
    void ResetFftVisual() override;

protected:
    // Add protected constructor
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height);
    
public:
    ~LcdDisplay();
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    virtual void SetupUI() override;
    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;
    
    // Set whether to hide chat messages/subtitles
    void SetHideSubtitle(bool hide);
};

// SPI LCD display
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// RGB LCD display
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// MIPI LCD display
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                   int width, int height, int offset_x, int offset_y,
                   bool mirror_x, bool mirror_y, bool swap_xy);
};

#endif // LCD_DISPLAY_H
