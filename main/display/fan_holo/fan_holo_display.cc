#include "fan_holo_display.h"

#include "settings.h"
#include "gif/lvgl_gif.h"
#include "assets/lang_config.h"
#include "board.h"

#include <memory>

#include <font_awesome.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_timer.h>
#include <esp_psram.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <src/misc/cache/lv_cache.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern "C" {
#include "mjpeg_player.h"
#include "emotion_partition_storage.h"
}

namespace {

constexpr uint32_t kBatteryChargingColor = 0xF07820;
constexpr uint32_t kBatteryLowColor = 0xE11D2E;
constexpr uint32_t kBatteryNormalColor = 0x22C55E;

bool ParseHoloVolumeNotification(const char* notification, int* volume) {
    if (notification == nullptr || volume == nullptr) {
        return false;
    }
    if (strcmp(notification, Lang::Strings::MUTED) == 0) {
        *volume = 0;
        return true;
    }
    if (strcmp(notification, Lang::Strings::MAX_VOLUME) == 0) {
        *volume = 100;
        return true;
    }
    const char* prefix = Lang::Strings::VOLUME;
    const size_t prefix_len = strlen(prefix);
    if (strncmp(notification, prefix, prefix_len) != 0) {
        return false;
    }
    int value = atoi(notification + prefix_len);
    if (value < 0) {
        value = 0;
    } else if (value > 100) {
        value = 100;
    }
    *volume = value;
    return true;
}

}  // namespace

FanHoloDisplay::FanHoloDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                               int width, int height, int offset_x, int offset_y,
                               bool mirror_x, bool mirror_y, bool swap_xy,
                               const FanHoloMetrics& metrics)
    : LcdDisplay(panel_io, panel, width, height), metrics_(metrics),
      offset_x_(offset_x), offset_y_(offset_y) {

    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "dark");
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(metrics_.tag, "Turning display on");
    {
        esp_err_t err = esp_lcd_panel_disp_on_off(panel_, true);
        if (err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(metrics_.tag, "Panel does not support disp_on_off; assuming ON");
        } else {
            ESP_ERROR_CHECK(err);
        }
    }

    ESP_LOGI(metrics_.tag, "Initialize LVGL library");
    lv_init();

#if CONFIG_SPIRAM
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
        lv_image_cache_resize(2 * 1024 * 1024, true);
        ESP_LOGI(metrics_.tag, "Use 2MB of PSRAM for image cache");
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
        ESP_LOGI(metrics_.tag, "Use 512KB of PSRAM for image cache");
    }
#endif

    ESP_LOGI(metrics_.tag, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    /* 优先级勿高于 MJPEG decode(4)：稳定版 FanLcd 虽为 5，但其 UI 极轻。
     * FanHolo 顶栏更重，降至 1 减少与 ROI blit 抢 SPI。 */
    port_cfg.task_priority = 1;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(metrics_.tag, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 1,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(metrics_.tag, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }

    esp_timer_create_args_t volume_timer_args = {
        .callback = VolumeHideTimerCb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "holo_vol_hide",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&volume_timer_args, &volume_hide_timer_));

    SetupUI();
}

FanHoloDisplay::~FanHoloDisplay() {
    if (volume_hide_timer_ != nullptr) {
        esp_timer_stop(volume_hide_timer_);
        esp_timer_delete(volume_hide_timer_);
        volume_hide_timer_ = nullptr;
    }
    StopMjpegIfRunning();
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    DisplayLockGuard lock(this);
    NullBoundWidgets();
    if (boot_screen_ != nullptr) {
        lv_obj_del(boot_screen_);
        boot_screen_ = nullptr;
        boot_container_ = nullptr;
    }
    boot_bar_ = {};
    boot_role_ = {};
    idle_page_.Destroy();
    chat_page_.Destroy();
    music_page_.Destroy();
}

void FanHoloDisplay::ApplyStatusBar(const FanHoloStatusBar& bar) {
    top_bar_ = bar.top_bar;
    status_bar_ = nullptr;
    status_label_ = bar.status_label;
    notification_label_ = bar.notification_label;
    mute_label_ = bar.mute_label;
    network_label_ = bar.network_label;
    battery_label_ = bar.battery_label;
    low_battery_popup_ = bar.low_battery_popup;
    low_battery_label_ = bar.low_battery_label;
    ApplyVolumeBar(bar.volume_bar, bar.volume_overlay, bar.volume_label);
    last_battery_icon_color_ = 0;
    ApplyIdleBatteryIconColor();
    if (low_battery_popup_ != nullptr) {
        if (low_battery_alert_) {
            lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void FanHoloDisplay::ApplyRole(const FanHoloRoleWidgets* role) {
    if (role == nullptr) {
        emoji_box_ = nullptr;
        emoji_label_ = nullptr;
        emoji_image_ = nullptr;
        return;
    }
    emoji_box_ = role->box;
    emoji_label_ = role->label;
    emoji_image_ = role->image;
}

void FanHoloDisplay::ApplyPreview(lv_obj_t* preview) {
    preview_image_ = preview;
}

void FanHoloDisplay::ApplyContainer(lv_obj_t* container) {
    container_ = container;
}

void FanHoloDisplay::ApplyChatStrip(lv_obj_t* strip) {
    chat_message_label_ = strip;
    bottom_bar_ = nullptr;
}

void FanHoloDisplay::ApplyVolumeBar(lv_obj_t* bar, lv_obj_t* overlay, lv_obj_t* label) {
    if (volume_overlay_ != nullptr && volume_overlay_ != overlay) {
        HideVolumeOverlay(volume_overlay_);
    }
    volume_bar_ = bar;
    volume_overlay_ = overlay;
    volume_label_ = label;
    if (volume_overlay_ == nullptr) {
        return;
    }
    if (volume_visible_) {
        if (volume_label_ != nullptr) {
            lv_label_set_text_fmt(volume_label_, "%d", last_volume_);
        }
        if (volume_bar_ != nullptr) {
            lv_bar_set_value(volume_bar_, last_volume_, LV_ANIM_OFF);
        }
        lv_obj_align(volume_overlay_, LV_ALIGN_TOP_RIGHT, -4, 4);
        lv_obj_remove_flag(volume_overlay_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(volume_overlay_);
    } else {
        HideVolumeOverlay(volume_overlay_);
    }
}

void FanHoloDisplay::VolumeHideTimerCb(void* arg) {
    auto* display = static_cast<FanHoloDisplay*>(arg);
    DisplayLockGuard lock(display);
    display->HideVolumeSlider();
}

void FanHoloDisplay::HideVolumeOverlay(lv_obj_t* overlay) {
    if (overlay == nullptr) {
        return;
    }
    lv_obj_invalidate(overlay);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}

void FanHoloDisplay::HideVolumeSlider() {
    volume_visible_ = false;
    HideVolumeOverlay(boot_bar_.volume_overlay);
    HideVolumeOverlay(idle_page_.status_bar().volume_overlay);
    HideVolumeOverlay(chat_page_.status_bar().volume_overlay);
    HideVolumeOverlay(music_page_.status_bar().volume_overlay);
}

void FanHoloDisplay::ShowVolumeSlider(int volume, int duration_ms) {
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    last_volume_ = volume;
    volume_visible_ = true;
    if (volume_label_ != nullptr) {
        lv_label_set_text_fmt(volume_label_, "%d", volume);
    }
    if (volume_bar_ != nullptr) {
        lv_bar_set_value(volume_bar_, volume, LV_ANIM_OFF);
    }
    if (volume_overlay_ != nullptr) {
        lv_obj_align(volume_overlay_, LV_ALIGN_TOP_RIGHT, -4, 4);
        lv_obj_remove_flag(volume_overlay_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(volume_overlay_);
        lv_obj_invalidate(volume_overlay_);
    }
    if (volume_hide_timer_ != nullptr) {
        esp_timer_stop(volume_hide_timer_);
        if (duration_ms > 0) {
            esp_err_t err = esp_timer_start_once(volume_hide_timer_, (uint64_t)duration_ms * 1000);
            if (err != ESP_OK) {
                esp_timer_stop(volume_hide_timer_);
                err = esp_timer_start_once(volume_hide_timer_, (uint64_t)duration_ms * 1000);
            }
            if (err != ESP_OK) {
                ESP_LOGW(metrics_.tag, "volume hide timer start failed: %s", esp_err_to_name(err));
            }
        }
    }
}

void FanHoloDisplay::ShowNotification(const char* notification, int duration_ms) {
    int volume = 0;
    if (ParseHoloVolumeNotification(notification, &volume)) {
        DisplayLockGuard lock(this);
        ShowVolumeSlider(volume, duration_ms);
        return;
    }

    /* WiFi 扫描/连接等：与「正在初始化」同款左侧 SIZE_CONTENT 状态胶囊，不拉满屏宽。 */
    DisplayLockGuard lock(this);
    FanHoloStatusBar* bar = nullptr;
    switch (current_page_) {
        case Page::Boot:
            bar = &boot_bar_;
            break;
        case Page::Chat:
            bar = &chat_page_.status_bar();
            break;
        case Page::Music:
            bar = &music_page_.status_bar();
            break;
        case Page::Idle:
            break;
    }
    if (bar != nullptr && bar->status_pill != nullptr) {
        bar->SetStatusText(notification);
        if (notification_label_ != nullptr) {
            lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    LvglDisplay::ShowNotification(notification, duration_ms);
    if (notification_label_ != nullptr) {
        lv_obj_move_foreground(notification_label_);
    }
}

void FanHoloDisplay::ApplyIdleBatteryIconColor() {
    if (battery_label_ == nullptr) {
        return;
    }
    uint32_t color = kBatteryNormalColor;
    if (battery_icon_ != nullptr && strcmp(battery_icon_, FONT_AWESOME_BATTERY_BOLT) == 0) {
        color = kBatteryChargingColor;
    } else if (battery_icon_ != nullptr && strcmp(battery_icon_, FONT_AWESOME_BATTERY_EMPTY) == 0) {
        color = kBatteryLowColor;
    }
    if (color == last_battery_icon_color_) {
        return;
    }
    last_battery_icon_color_ = color;
    lv_obj_set_style_text_color(battery_label_, lv_color_hex(color), 0);
}

void FanHoloDisplay::NullBoundWidgets() {
    top_bar_ = nullptr;
    status_bar_ = nullptr;
    status_label_ = nullptr;
    notification_label_ = nullptr;
    mute_label_ = nullptr;
    network_label_ = nullptr;
    battery_label_ = nullptr;
    low_battery_popup_ = nullptr;
    low_battery_label_ = nullptr;
    volume_bar_ = nullptr;
    volume_overlay_ = nullptr;
    volume_label_ = nullptr;
    emoji_box_ = nullptr;
    emoji_label_ = nullptr;
    emoji_image_ = nullptr;
    preview_image_ = nullptr;
    container_ = nullptr;
    chat_message_label_ = nullptr;
    bottom_bar_ = nullptr;
    content_ = nullptr;
}

void FanHoloDisplay::PreparePage(Page page) {
    /* 只停 MJPEG + 切 LVGL 屏。禁止 draw_bitmap 铺黑/探 DSI：
     * 失败会刷屏打 log、把 CPU 卡住（AFE FEED 溢出），视觉上还会一行行刷黑。 */
    StopMjpegIfRunning();
    DisplayLockGuard lock(this);
    SwitchTo(page);
}

void FanHoloDisplay::SwitchTo(Page page) {
    lv_obj_t* want = nullptr;
    switch (page) {
        case Page::Boot: want = boot_screen_; break;
        case Page::Idle: want = idle_page_.screen(); break;
        case Page::Chat: want = chat_page_.screen(); break;
        case Page::Music: want = music_page_.screen(); break;
    }
    if (page == current_page_ && want != nullptr && lv_screen_active() == want) {
        return;
    }

    current_page_ = page;
    switch (page) {
        case Page::Boot:
            boot_bar_.Bind(*this);
            boot_role_.Bind(*this);
            ApplyContainer(boot_container_);
            ApplyPreview(nullptr);
            ApplyChatStrip(nullptr);
            if (boot_screen_ != nullptr) {
                lv_screen_load(boot_screen_);
            }
            break;
        case Page::Idle:
            idle_page_.Show(*this);
            break;
        case Page::Chat:
            chat_page_.Show(*this);
            break;
        case Page::Music:
            music_page_.Show(*this);
            break;
    }
}

void FanHoloDisplay::SetStatus(const char* status) {
    if (status == nullptr || status[0] == '\0') {
        return;
    }
    bool enter_chat = false;
    {
        DisplayLockGuard lock(this);
        const bool chat_status =
            (strcmp(status, Lang::Strings::CONNECTING) == 0) ||
            (strcmp(status, Lang::Strings::LISTENING) == 0) ||
            (strcmp(status, Lang::Strings::SPEAKING) == 0);
        const bool music_status = (strcmp(status, Lang::Strings::MUSIC_PLAYING) == 0);

        if (music_status) {
            music_page_.status_bar().SetStatusText(status);
            return;
        }
        if (chat_status) {
            chat_page_.status_bar().SetStatusText(status);
            /* 只重算字幕几何；不要 RaiseOverlays/move_foreground（会与 MJPEG 抢 SPI）。 */
            chat_page_.LayoutSubtitle(*this);
            enter_chat = (strcmp(status, Lang::Strings::CONNECTING) == 0);
            if (!enter_chat) {
                return;
            }
        } else if (current_page_ == Page::Boot) {
            boot_bar_.SetStatusText(status);
        } else {
            idle_page_.status_bar().SetStatusText(status);
        }
        last_status_update_time_ = std::chrono::system_clock::now();
    }
    if (enter_chat) {
        PreparePage(Page::Chat);
    }
}

void FanHoloDisplay::RaiseCurrentOverlays() {
    switch (current_page_) {
        case Page::Idle:
            idle_page_.status_bar().RaiseOverlays();
            break;
        case Page::Chat:
            /* 低电量等偶发抬层；勿在热路径反复 RaiseSubtitleThenOverlays。 */
            chat_page_.status_bar().RaiseOverlays();
            break;
        case Page::Music:
            music_page_.status_bar().RaiseOverlays();
            break;
        case Page::Boot:
            boot_bar_.RaiseOverlays();
            break;
    }
}

void FanHoloDisplay::SetIdleWeather(const IdleWeatherView& weather) {
    DisplayLockGuard lock(this);
    idle_page_.ApplyWeather(weather);
}

void FanHoloDisplay::SetIdleClockStyle(int style) {
    if (style != FanHoloMetrics::kIdleClockLed7Seg) {
        style = FanHoloMetrics::kIdleClockFlipPuhui;
    }
    if (idle_clock_style_ == style) {
        return;
    }
    idle_clock_style_ = style;
    DisplayLockGuard lock(this);
    idle_page_.ApplyClockStyle(*this);
}

void FanHoloDisplay::UpdateStatusBar(bool update_all) {
    const bool popup_was_hidden =
        (low_battery_popup_ == nullptr) || lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
    LvglDisplay::UpdateStatusBar(update_all);
    DisplayLockGuard lock(this);
    if (current_page_ == Page::Idle) {
        idle_page_.Tick();
    }
    ApplyIdleBatteryIconColor();
    if (low_battery_popup_ != nullptr) {
        low_battery_alert_ = !lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
    }
    /* 不要每秒 move_foreground：会把整屏标脏，和 MJPEG DSI blit 抢绘导致角色闪烁。 */
    if (popup_was_hidden && low_battery_alert_) {
        RaiseCurrentOverlays();
    }
}

void FanHoloDisplay::SetEmotion(const char* emotion) {
    if (s_system_ready_) {
        return;
    }
    if (emotion != nullptr && std::strcmp(emotion, "neutral") == 0 && mjpeg_player_is_running()) {
        return;
    }

    if (gif_controller_) {
        DisplayLockGuard lock(this);
        gif_controller_->Stop();
        gif_controller_.reset();
    }

    if (mjpeg_player_is_running()) {
        mjpeg_player_stop_async();
        current_mjpeg_path_.clear();
        current_clip_name_.clear();
    }

    auto& role = (current_page_ == Page::Boot) ? boot_role_ : idle_page_.role_widgets();
    if (role.image == nullptr) {
        return;
    }

    DisplayLockGuard lock(this);
    auto emoji_collection = GetLvglTheme()->emoji_collection();
    auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(emotion) : nullptr;
    if (image == nullptr) {
        image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage("neutral") : nullptr;
    }
    if (image == nullptr) {
        const char* utf8 = font_awesome_get_utf8(emotion);
        if (utf8 != nullptr && role.label != nullptr) {
            lv_label_set_text(role.label, utf8);
            lv_obj_add_flag(role.image, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(role.label, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if (image->IsGif()) {
        gif_controller_ = std::make_unique<LvglGif>(image->image_dsc());
        if (gif_controller_->IsLoaded()) {
            gif_controller_->SetFrameCallback([this]() {
                lv_obj_t* img = (current_page_ == Page::Boot) ? boot_role_.image : idle_page_.role_widgets().image;
                if (img != nullptr && gif_controller_) {
                    lv_image_set_src(img, gif_controller_->image_dsc());
                }
            });
            lv_image_set_src(role.image, gif_controller_->image_dsc());
            gif_controller_->Start();
            lv_obj_add_flag(role.label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(role.image, LV_OBJ_FLAG_HIDDEN);
        } else {
            ESP_LOGE(metrics_.tag, "Failed to load GIF for emotion: %s", emotion);
            gif_controller_.reset();
        }
    } else {
        lv_image_set_src(role.image, image->image_dsc());
        lv_obj_add_flag(role.label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(role.image, LV_OBJ_FLAG_HIDDEN);
    }
}

void FanHoloDisplay::SetRoleAnimation(const char* state) {
    ESP_LOGI(metrics_.tag, "SetRoleAnimation state=%s ready=%d",
             state ? state : "<null>", (int)s_system_ready_);

    if (gif_controller_) {
        DisplayLockGuard lock(this);
        gif_controller_->Stop();
        gif_controller_.reset();
    }

    {
        DisplayLockGuard lock(this);
        idle_page_.HideRole();
        chat_page_.HideRole();
    }

    if (!s_system_ready_) {
        ESP_LOGW(metrics_.tag, "SetRoleAnimation skipped: system not ready");
        return;
    }

    const char* clip = MapRoleStateToClip(state);
    const bool idle_clip = (strcmp(clip, "idle") == 0);
    const ClipLoc clip_loc = FindRoleAnimation(clip);
    if (!clip_loc.valid()) {
        ESP_LOGI(metrics_.tag, "SetRoleAnimation: no available MJPEG for state=%s, skip", clip);
        return;
    }

    const Page target = idle_clip ? Page::Idle : Page::Chat;
    lv_obj_t* dest = idle_clip ? idle_page_.screen() : chat_page_.screen();
    if (current_page_ != target || lv_screen_active() != dest) {
        PreparePage(target);
    }
    if (!StartMjpegEmotion(clip_loc, idle_clip)) {
        ESP_LOGW(metrics_.tag, "SetRoleAnimation: MJPEG start failed, keep role widget");
    }
}

void FanHoloDisplay::SetChatMessage(const char* role, const char* content) {
    chat_page_.SetMessage(*this, role, content);
}

void FanHoloDisplay::ClearChatMessages() {
    chat_page_.ClearMessages(*this);
}

void FanHoloDisplay::SetSystemReady() {
    s_system_ready_ = true;
    ESP_LOGI(metrics_.tag, "MJPEG ready: system is ready, partition clips permitted");
    ScanEmotionClips();
    /* 不在这里起 MJPEG：ActivationDone 里会先 SetSystemReady 再进 idle。
     * 若先占 2MB 预载池，随后 CustomWakeWord 的 mn_detect 任务可能建不起来，
     * 表现为「下载完自定义动画后唤醒失效，重启又好」。idle 状态机里会
     * EnableWakeWordDetection 之后再 SetRoleAnimation("idle")。 */
}

void FanHoloDisplay::PrepareForReboot() {
    StopMjpegIfRunning();
    LvglDisplay::PrepareForReboot();
}

void FanHoloDisplay::SetMusicInfo(const char* song_name, const char* singer, int interval) {
    music_page_.SetInfo(*this, song_name, singer, interval);
}

void FanHoloDisplay::SetMusicProgress(int current_ms, const char* lyric, const char* lyric_next) {
    music_page_.SetProgress(*this, current_ms, lyric, lyric_next);
}

void FanHoloDisplay::ShowMusicCover(bool show, const std::string& picture_url) {
    music_page_.ShowCover(*this, show, picture_url);
}

lv_obj_t* FanHoloDisplay::GetMusicCoverContainer() {
    return music_page_.container();
}

void FanHoloDisplay::SetTheme(::Theme* theme) {
    LcdDisplay::SetTheme(theme);
    auto* lvgl_theme = static_cast<LvglTheme*>(theme);
    if (lvgl_theme == nullptr || lvgl_theme->text_font() == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    auto text_font = lvgl_theme->text_font()->font();
    auto color = lvgl_theme->text_color();
    idle_page_.ApplyTextFont(text_font, color);
    chat_page_.ApplyTextFont(text_font, color);
    music_page_.ApplyTextFont(text_font, color);
    boot_bar_.ApplyTextFont(text_font, color);
}

void FanHoloDisplay::MjpegResForClip(const char* clip, unsigned* w, unsigned* h) const {
    const bool idle = (clip != nullptr && strcmp(clip, "idle") == 0);
    unsigned iw = metrics_.mjpeg_idle_w ? metrics_.mjpeg_idle_w : metrics_.mjpeg_w;
    unsigned ih = metrics_.mjpeg_idle_h ? metrics_.mjpeg_idle_h : metrics_.mjpeg_h;
    if (w) {
        *w = idle ? iw : metrics_.mjpeg_w;
    }
    if (h) {
        *h = idle ? ih : metrics_.mjpeg_h;
    }
}

void FanHoloDisplay::GetRoleMjpegSize(const char* type, int* width, int* height) const {
    unsigned w = 0;
    unsigned h = 0;
    MjpegResForClip(type, &w, &h);
    if (width) {
        *width = static_cast<int>(w);
    }
    if (height) {
        *height = static_cast<int>(h);
    }
}

void FanHoloDisplay::FillClipName(char* out, size_t out_size, const char* name) const {
    /* name 可为 "idle" / "listen" / "default-idle" 等；按状态取对应分辨率 */
    const char* state = name;
    if (name != nullptr && strncmp(name, "default-", 8) == 0) {
        state = name + 8;
    }
    unsigned cw = 0;
    unsigned ch = 0;
    MjpegResForClip(state, &cw, &ch);
    snprintf(out, out_size, "%s-%ux%u.mjpeg", name, cw, ch);
}

void FanHoloDisplay::ScanEmotionClips() {
    if (clip_cache_.scanned) {
        return;
    }
    clip_cache_.scanned = true;

    if (emotion_partition_storage_get_partition() == NULL) {
        esp_err_t r = emotion_partition_storage_init();
        if (r != ESP_OK) {
            ESP_LOGW(metrics_.tag, "emotion_partition_storage_init failed: %s", esp_err_to_name(r));
            return;
        }
    }

    auto try_find = [](const char* asset_name) -> ClipLoc {
        ClipLoc loc;
        if (emotion_partition_storage_find(asset_name, &loc.offset, &loc.size)) {
            loc.name = asset_name;
        }
        return loc;
    };

    const char* user_names[3] = { "idle", "listen", "speak" };
    for (int i = 0; i < 3; i++) {
        char n[48];
        FillClipName(n, sizeof(n), user_names[i]);
        ClipLoc loc = try_find(n);
        if (loc.valid()) {
            if (i == 0) {
                clip_cache_.idle = loc;
            } else if (i == 1) {
                clip_cache_.listen = loc;
            } else {
                clip_cache_.speak = loc;
            }
        }
    }

    /* 各状态独立 default-{state}-{WxH}，分辨率随状态变 */
    char dn[48];
    FillClipName(dn, sizeof(dn), "default-idle");
    clip_cache_.default_clip = try_find(dn);

    ESP_LOGI(metrics_.tag, "ClipCache: idle=%s listen=%s speak=%s default_idle=%s",
             clip_cache_.idle.name.c_str(), clip_cache_.listen.name.c_str(),
             clip_cache_.speak.name.c_str(), clip_cache_.default_clip.name.c_str());
}

void FanHoloDisplay::RescanAndTryIdle() {
    clip_cache_ = ClipCache{};
    ScanEmotionClips();
    if (!mjpeg_player_is_running()) {
        SetRoleAnimation("idle");
    }
}

void FanHoloDisplay::OnEmotionsUpdated() {
    clip_cache_ = ClipCache{};
    ScanEmotionClips();
    ESP_LOGI(metrics_.tag, "OnEmotionsUpdated: rescan done, idle=%s listen=%s speak=%s",
             clip_cache_.idle.name.c_str(), clip_cache_.listen.name.c_str(),
             clip_cache_.speak.name.c_str());
    /* 正在播的自定义被删掉时，强制按新查找结果重切（否则会继续播旧 clip） */
    if (s_system_ready_) {
        current_clip_name_.clear();
        current_mjpeg_path_.clear();
        if (mjpeg_player_is_running()) {
            mjpeg_player_stop_async();
        }
        SetRoleAnimation("idle");
    }
}

namespace {

struct FindByPrefixCtx {
    const char* prefix;      /* e.g. "listen-" */
    size_t prefix_len;
    const char* prefer_name; /* exact prefer if present among matches */
    char found[EMOTION_STORAGE_NAME_MAX];
    uint32_t offset = 0;
    uint32_t size = 0;
    bool hit = false;
};

void FindByPrefixCb(const char* name, uint32_t offset, uint32_t size, void* user) {
    auto* ctx = static_cast<FindByPrefixCtx*>(user);
    if (name == nullptr || strncmp(name, "default-", 8) == 0) {
        return;
    }
    if (strncmp(name, ctx->prefix, ctx->prefix_len) != 0) {
        return;
    }
    /* 优先精确名；否则取第一个匹配（兼容旧版错误分辨率命名） */
    if (ctx->prefer_name != nullptr && strcmp(name, ctx->prefer_name) == 0) {
        strncpy(ctx->found, name, sizeof(ctx->found) - 1);
        ctx->found[sizeof(ctx->found) - 1] = '\0';
        ctx->offset = offset;
        ctx->size = size;
        ctx->hit = true;
        return;
    }
    if (!ctx->hit) {
        strncpy(ctx->found, name, sizeof(ctx->found) - 1);
        ctx->found[sizeof(ctx->found) - 1] = '\0';
        ctx->offset = offset;
        ctx->size = size;
        ctx->hit = true;
    }
}

}  // namespace

FanHoloDisplay::ClipLoc FanHoloDisplay::FindRoleAnimation(const char* state) {
    /* 优先 {state}-{WxH}.mjpeg，再扫同前缀自定义文件，最后 default-{state}-{WxH} */
    if (state == nullptr || state[0] == '\0') {
        state = "idle";
    }
    if (emotion_partition_storage_get_partition() == NULL) {
        esp_err_t r = emotion_partition_storage_init();
        if (r != ESP_OK) {
            ESP_LOGW(metrics_.tag, "FindRoleAnimation: partition init failed: %s", esp_err_to_name(r));
            return {};
        }
    }

    unsigned cw = 0;
    unsigned ch = 0;
    MjpegResForClip(state, &cw, &ch);

    char path[64];
    snprintf(path, sizeof(path), "%s-%ux%u.mjpeg", state, cw, ch);
    ClipLoc loc;
    if (emotion_partition_storage_find(path, &loc.offset, &loc.size)) {
        loc.name = path;
        ESP_LOGI(metrics_.tag, "FindRoleAnimation: found %s", path);
        return loc;
    }

    /* 兼容：已下载但分辨率字段与板子期望不一致（如 listen-160x208） */
    char prefix[24];
    snprintf(prefix, sizeof(prefix), "%s-", state);
    FindByPrefixCtx ctx{};
    ctx.prefix = prefix;
    ctx.prefix_len = strlen(prefix);
    ctx.prefer_name = path;
    ctx.found[0] = '\0';
    emotion_partition_storage_enum(FindByPrefixCb, &ctx);
    if (ctx.hit) {
        loc.name = ctx.found;
        loc.offset = ctx.offset;
        loc.size = ctx.size;
        ESP_LOGW(metrics_.tag, "FindRoleAnimation: exact %s missing, using %s", path, ctx.found);
        return loc;
    }

    snprintf(path, sizeof(path), "default-%s-%ux%u.mjpeg", state, cw, ch);
    if (emotion_partition_storage_find(path, &loc.offset, &loc.size)) {
        loc.name = path;
        ESP_LOGI(metrics_.tag, "FindRoleAnimation: found default %s", path);
        return loc;
    }

    ESP_LOGW(metrics_.tag, "FindRoleAnimation: no animation for state=%s (%ux%u)", state, cw, ch);
    return {};
}

const char* FanHoloDisplay::MapRoleStateToClip(const char* state) {
    if (state == nullptr) {
        return "idle";
    }
    if (strcmp(state, "idle") == 0 ||
        strcmp(state, "listen") == 0 ||
        strcmp(state, "speak") == 0) {
        return state;
    }
    return "idle";
}

static bool ParseMjpegResFromName(const char* name, uint16_t* w, uint16_t* h) {
    if (name == nullptr || w == nullptr || h == nullptr) {
        return false;
    }
    const char* dash = strrchr(name, '-');
    if (dash == nullptr) {
        return false;
    }
    unsigned tw = 0;
    unsigned th = 0;
    if (sscanf(dash, "-%ux%u", &tw, &th) != 2 || tw == 0 || th == 0) {
        return false;
    }
    *w = static_cast<uint16_t>(tw);
    *h = static_cast<uint16_t>(th);
    return true;
}

bool FanHoloDisplay::StartMjpegEmotion(const ClipLoc& loc_in, bool idle_layout) {
    if (!s_system_ready_) {
        ESP_LOGW(metrics_.tag, "MJPEG跳过：系统未就绪");
        return false;
    }
    if (!loc_in.valid()) {
        ESP_LOGW(metrics_.tag, "MJPEG跳过：loc 无效");
        return false;
    }

    ClipLoc loc = loc_in;

    if (loc.name == current_clip_name_ && mjpeg_player_is_running()) {
        ESP_LOGI(metrics_.tag, "StartMjpegEmotion: same clip %s, skip", loc.name.c_str());
        return true;
    }
    if (loc.name != current_clip_name_ && mjpeg_player_is_running()) {
        ESP_LOGI(metrics_.tag, "StartMjpegEmotion: clip change %s -> %s, stop_async",
                 current_clip_name_.c_str(), loc.name.c_str());
        mjpeg_player_stop_async();
    }

    const int screen_w = width_;
    const int screen_h = height_;
    uint16_t vid_w = idle_layout
        ? (metrics_.mjpeg_idle_w ? metrics_.mjpeg_idle_w : metrics_.mjpeg_w)
        : metrics_.mjpeg_w;
    uint16_t vid_h = idle_layout
        ? (metrics_.mjpeg_idle_h ? metrics_.mjpeg_idle_h : metrics_.mjpeg_h)
        : metrics_.mjpeg_h;
    uint16_t file_w = 0;
    uint16_t file_h = 0;
    if (ParseMjpegResFromName(loc.name.c_str(), &file_w, &file_h)) {
        vid_w = file_w;
        vid_h = file_h;
    }

    /* idle：右下角原尺寸贴图，不缩放；chat：底部水平居中（位置/大小与原先一致）
     * rx/ry 为 LVGL 逻辑坐标；直刷 panel 须再加 offset（CONFIG_LCD_CUSTOM 的 OFFSET_X=23），
     * 否则 MJPEG 相对 LVGL 内容偏左，与预报栏重叠。 */
    int rx = 0;
    int ry = 0;
    if (idle_layout) {
        rx = screen_w - static_cast<int>(vid_w);
        ry = screen_h - static_cast<int>(vid_h);
    } else {
        rx = (screen_w - static_cast<int>(vid_w)) / 2;
        ry = screen_h - static_cast<int>(vid_h);
    }
    if (rx < 0) {
        rx = 0;
    }
    if (ry < 0) {
        ry = 0;
    }
    roi_x_ = rx;
    roi_y_ = ry;

    mjpeg_player_cfg_t cfg = {};
    cfg.src_type = MJPEG_SRC_PARTITION;
    cfg.partition = emotion_partition_storage_get_partition();
    cfg.partition_offset = loc.offset;
    cfg.partition_size = loc.size;
    cfg.file_path = nullptr;
    cfg.panel = panel_;
    cfg.fb[0] = nullptr;
    cfg.fb[1] = nullptr;
    cfg.screen_width = vid_w;
    cfg.screen_height = vid_h;
    cfg.panel_width = static_cast<uint16_t>(screen_w);
    cfg.panel_height = static_cast<uint16_t>(screen_h);
    cfg.target_fps = metrics_.mjpeg_fps;
    cfg.loop = true;
    cfg.fb_stride = 0;
    cfg.fb_size = 0;
    cfg.lv_video_canvas = nullptr;
    cfg.panel_blit_roi = true;
    cfg.panel_roi_x = static_cast<uint16_t>(rx + offset_x_);
    cfg.panel_roi_y = static_cast<uint16_t>(ry + offset_y_);

    const esp_err_t ret = mjpeg_player_start(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(metrics_.tag, "MJPEG启动失败(%s): %s", loc.name.c_str(), esp_err_to_name(ret));
        return false;
    }
    current_clip_name_ = loc.name;
    current_mjpeg_path_ = loc.name;
    ESP_LOGI(metrics_.tag, "MJPEG emotion (partition): %s dest=(%d,%d)+off(%d,%d) %ux%u idle=%d",
             loc.name.c_str(), rx, ry, offset_x_, offset_y_, vid_w, vid_h, (int)idle_layout);
    return true;
}

void FanHoloDisplay::StopRoleAnimation() {
    StopMjpegIfRunning();
}

void FanHoloDisplay::StopMjpegIfRunning() {
    /* 析构 / 关闭时同步等待，确保资源完全释放。普通状态切换走 StartMjpegEmotion
     * 里的 mjpeg_player_stop_async()，避免阻塞。 */
    mjpeg_player_stop();
    current_mjpeg_path_.clear();
    current_clip_name_.clear();
}

void FanHoloDisplay::SetupUI() {
    if (setup_ui_called_) {
        ESP_LOGW(metrics_.tag, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }
    Display::SetupUI();
    DisplayLockGuard lock(this);

    idle_page_.Create(*this);
    chat_page_.Create(*this);
    music_page_.Create(*this);

    boot_screen_ = FanHoloCreateScreen(*this);
    boot_container_ = FanHoloCreateFullBleedContainer(boot_screen_, *this);
    boot_role_.Create(boot_screen_, *this, false);
    boot_bar_.Create(boot_screen_, *this, FanHoloStatusBar::Kind::StatusLeft);
    boot_bar_.SetStatusText(Lang::Strings::INITIALIZING);
    SwitchTo(Page::Boot);
}
