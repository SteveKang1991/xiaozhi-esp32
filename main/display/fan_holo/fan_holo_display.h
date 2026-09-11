#ifndef FAN_HOLO_DISPLAY_H
#define FAN_HOLO_DISPLAY_H

#include "lcd_display.h"
#include "lvgl_theme.h"
#include "fan_holo_display_metrics.h"
#include "fan_holo_idle_page.h"
#include "fan_holo_chat_page.h"
#include "fan_holo_music_page.h"

#include <string>

/* 55B/5B 共用 Holo 显示协调器：LVGL/DSI、三页切换、MJPEG 硬件 blit。
 * idle / chat / music 各自拥有 screen 与控件，互切不共用控件树。 */
class FanHoloDisplay : public LcdDisplay {
public:
    enum class Page { Boot, Idle, Chat, Music };

    FanHoloDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                   int width, int height, int offset_x, int offset_y,
                   bool mirror_x, bool mirror_y, bool swap_xy,
                   const FanHoloMetrics& metrics);
    ~FanHoloDisplay() override;

    void SetEmotion(const char* emotion) override;
    void SetRoleAnimation(const char* state) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    void SetStatus(const char* status) override;
    void SetSystemReady() override;
    void PrepareForReboot() override;
    void SetMusicInfo(const char* song_name, const char* singer, int interval) override;
    void SetMusicProgress(int current_ms, const char* lyric, const char* lyric_next = nullptr) override;
    void ShowMusicCover(bool show, const std::string& picture_url = "") override;
    lv_obj_t* GetMusicCoverContainer() override;
    void SetTheme(Theme* theme) override;
    void UpdateStatusBar(bool update_all = false) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void SetIdleWeather(const IdleWeatherView& weather) override;

    void ShowIdlePage() { SetRoleAnimation("idle"); }
    void ShowChatPage(const char* state) { SetRoleAnimation(state); }
    void ShowMusicPage(const std::string& picture_url = "") { ShowMusicCover(true, picture_url); }

    void SwitchTo(Page page);
    void PreparePage(Page page);
    const FanHoloMetrics& metrics() const { return metrics_; }
    int screen_width() const { return width_; }
    int screen_height() const { return height_; }
    LvglTheme* GetLvglTheme() { return static_cast<LvglTheme*>(current_theme_); }
    bool SystemReady() const { return s_system_ready_; }
    bool StartMjpegEmotion(const char* full_path, bool idle_layout);
    void StopMjpegIfRunning();
    std::string FindRoleAnimation(const char* state);
    static const char* MapRoleStateToClip(const char* state);
    void NullBoundWidgets();
    void ApplyStatusBar(const FanHoloStatusBar& bar);
    void ApplyRole(const FanHoloRoleWidgets* role);
    void ApplyPreview(lv_obj_t* preview);
    void ApplyContainer(lv_obj_t* container);
    void ApplyChatStrip(lv_obj_t* strip);
    void ApplyVolumeBar(lv_obj_t* bar, lv_obj_t* overlay = nullptr, lv_obj_t* label = nullptr);

protected:
    void SetupUI() override;

private:
    friend class FanHoloIdlePage;
    friend class FanHoloChatPage;
    friend class FanHoloMusicPage;
    friend struct FanHoloStatusBar;
    friend struct FanHoloRoleWidgets;

    static bool FileExists(const std::string& path);
    void ShowVolumeSlider(int volume, int duration_ms);
    void HideVolumeSlider();
    void HideVolumeOverlay(lv_obj_t* overlay);
    void RaiseCurrentOverlays();
    void ApplyIdleBatteryIconColor();
    static void VolumeHideTimerCb(void* arg);

    lv_obj_t* volume_bar_ = nullptr;
    lv_obj_t* volume_overlay_ = nullptr;
    lv_obj_t* volume_label_ = nullptr;
    esp_timer_handle_t volume_hide_timer_ = nullptr;
    int last_volume_ = 0;
    bool volume_visible_ = false;
    bool low_battery_alert_ = false;

    Page current_page_ = Page::Boot;
    inline static bool s_system_ready_ = false;
    FanHoloMetrics metrics_;
    std::string current_mjpeg_path_;
    int roi_x_ = 0;
    int roi_y_ = 0;

    lv_obj_t* boot_screen_ = nullptr;
    lv_obj_t* boot_container_ = nullptr;
    FanHoloStatusBar boot_bar_;
    FanHoloRoleWidgets boot_role_;

    FanHoloIdlePage idle_page_;
    FanHoloChatPage chat_page_;
    FanHoloMusicPage music_page_;
};

#endif
