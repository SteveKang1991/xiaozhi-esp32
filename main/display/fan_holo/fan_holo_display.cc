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
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <sys/stat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern "C" {
#include "mjpeg_player.h"
#include "sd_scanner.h"
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
    : LcdDisplay(panel_io, panel, width, height), metrics_(metrics) {

    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "dark");
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    ESP_LOGI(metrics_.tag, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(metrics_.tag, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_cfg);

    ESP_LOGI(metrics_.tag, "Adding LCD display");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 50),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = true,
            .sw_rotate = true,
        },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {
        .flags = {
            .avoid_tearing = false,
        }
    };
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(metrics_.tag, "Failed to add display");
        return;
    }

    ESP_LOGI(metrics_.tag, "LVGL native resolution: %dx%d",
             (int)lv_display_get_horizontal_resolution(display_),
             (int)lv_display_get_vertical_resolution(display_));

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
    teardownFft();
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
        lv_obj_align(volume_overlay_, LV_ALIGN_TOP_RIGHT, 0, lv_obj_get_y(volume_overlay_));
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
    lv_obj_t* parent = lv_obj_get_parent(overlay);
    lv_obj_invalidate(overlay);
    if (parent != nullptr) {
        lv_obj_invalidate(parent);
    }
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    if (parent != nullptr) {
        lv_obj_invalidate(parent);
    }
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
        const lv_coord_t y = lv_obj_get_y(volume_overlay_);
        lv_obj_align(volume_overlay_, LV_ALIGN_TOP_RIGHT, 0, y);
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
    LvglDisplay::ShowNotification(notification, duration_ms);
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
    lv_obj_t* scr = lv_screen_active();
    if (scr != nullptr) {
        lv_obj_invalidate(scr);
    }
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
    if (status == nullptr) {
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

void FanHoloDisplay::UpdateStatusBar(bool update_all) {
    const bool popup_was_hidden =
        (low_battery_popup_ == nullptr) || lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
    LvglDisplay::UpdateStatusBar(update_all);
    DisplayLockGuard lock(this);
    idle_page_.Tick();
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
        mjpeg_player_stop();
        current_mjpeg_path_.clear();
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
    ESP_LOGI(metrics_.tag, "SetRoleAnimation state=%s ready=%d sd_mounted=%d",
             state ? state : "<null>",
             (int)s_system_ready_, (int)sd_scanner_is_mounted());

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

    if (!s_system_ready_ || !sd_scanner_is_mounted()) {
        ESP_LOGW(metrics_.tag, "SetRoleAnimation skipped: system not ready or SD not mounted");
        return;
    }

    const char* clip = MapRoleStateToClip(state);
    const bool idle_clip = (strcmp(clip, "idle") == 0);
    const std::string& clip_path = FindRoleAnimation(clip);
    if (clip_path.empty()) {
        ESP_LOGI(metrics_.tag, "SetRoleAnimation: no available MJPEG for state=%s, skip", clip);
        return;
    }

    const Page target = idle_clip ? Page::Idle : Page::Chat;
    lv_obj_t* dest = idle_clip ? idle_page_.screen() : chat_page_.screen();
    if (current_page_ != target || lv_screen_active() != dest) {
        PreparePage(target);
    } else {
        StopMjpegIfRunning();
    }
    if (!StartMjpegEmotion(clip_path.c_str(), idle_clip)) {
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
    ESP_LOGI(metrics_.tag, "MJPEG ready: system is ready, SD card operations permitted");
    if (!mjpeg_player_is_running() && sd_scanner_is_mounted()) {
        SetRoleAnimation("idle");
    }
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

bool FanHoloDisplay::FileExists(const std::string& path) {
    struct stat st = {};
    return stat(path.c_str(), &st) == 0;
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

static bool ParseMjpegResFromPath(const char* path, uint16_t* w, uint16_t* h) {
    if (path == nullptr || w == nullptr || h == nullptr) {
        return false;
    }
    const char* dash = strrchr(path, '-');
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

std::string FanHoloDisplay::FindRoleAnimation(const char* state) {
    if (state == nullptr || state[0] == '\0') {
        state = "idle";
    }
    unsigned cw = 0;
    unsigned ch = 0;
    MjpegResForClip(state, &cw, &ch);
    char path[128];
    snprintf(path, sizeof(path), "/sdcard/Emotion/%s-%ux%u.mjpeg", state, cw, ch);
    if (FileExists(path)) {
        ESP_LOGI(metrics_.tag, "FindRoleAnimation: found %s", path);
        return path;
    }

    snprintf(path, sizeof(path), "/sdcard/Emotion/default-%s-%ux%u.mjpeg", state, cw, ch);
    if (FileExists(path)) {
        ESP_LOGI(metrics_.tag, "FindRoleAnimation: found default %s", path);
        return path;
    }

    ESP_LOGW(metrics_.tag, "FindRoleAnimation: no animation for state=%s", state);
    return "";
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

bool FanHoloDisplay::StartMjpegEmotion(const char* full_path, bool idle_layout) {
    if (!s_system_ready_) {
        ESP_LOGW(metrics_.tag, "MJPEG跳过：系统未就绪");
        return false;
    }
    if (!sd_scanner_is_mounted()) {
        ESP_LOGW(metrics_.tag, "MJPEG跳过：SD卡未挂载");
        return false;
    }
    if (full_path == nullptr || full_path[0] == '\0') {
        ESP_LOGW(metrics_.tag, "MJPEG跳过：路径为空");
        return false;
    }

    if (mjpeg_player_is_running()) {
        mjpeg_player_stop();
    }

    const int screen_w = width_;
    const int screen_h = height_;
    int rx = 0;
    int ry = 0;
    uint16_t src_x = 0;
    uint16_t src_y = 0;
    uint16_t src_block_w = 0;
    uint16_t src_block_h = 0;
    uint16_t out_w = 0;
    uint16_t out_h = 0;
    uint16_t protect_top = 0;
    uint8_t scale_n = 0;
    uint16_t vid_w = idle_layout
        ? (metrics_.mjpeg_idle_w ? metrics_.mjpeg_idle_w : metrics_.mjpeg_w)
        : metrics_.mjpeg_w;
    uint16_t vid_h = idle_layout
        ? (metrics_.mjpeg_idle_h ? metrics_.mjpeg_idle_h : metrics_.mjpeg_h)
        : metrics_.mjpeg_h;
    uint16_t file_w = 0;
    uint16_t file_h = 0;
    if (ParseMjpegResFromPath(full_path, &file_w, &file_h)) {
        vid_w = file_w;
        vid_h = file_h;
    }
    if (idle_layout) {
        rx = screen_w - static_cast<int>(vid_w);
        ry = screen_h - static_cast<int>(vid_h);
        if (ry > 0) {
            protect_top = static_cast<uint16_t>(ry);
        }
        ESP_LOGI(metrics_.tag, "idle MJPEG dest=(%d,%d) %ux%u protect_top=%u",
                 rx, ry, vid_w, vid_h, protect_top);
    } else {
        rx = (screen_w - static_cast<int>(vid_w)) / 2;
        ry = screen_h - static_cast<int>(vid_h);
        if (ry > 0) {
            protect_top = static_cast<uint16_t>(ry);
        }
        ESP_LOGI(metrics_.tag, "chat MJPEG dest=(%d,%d) %ux%u protect_top=%u",
                 rx, ry, vid_w, vid_h, protect_top);
    }
    if (rx < 0) {
        rx = 0;
    }
    if (ry < 0) {
        ry = 0;
    }
    roi_x_ = rx;
    roi_y_ = ry;

    current_mjpeg_path_ = full_path;
    mjpeg_player_cfg_t cfg = {};
    cfg.file_path = current_mjpeg_path_.c_str();
    cfg.panel = panel_;
    cfg.fb[0] = nullptr;
    cfg.fb[1] = nullptr;
    cfg.mjpeg_video_width = vid_w;
    cfg.mjpeg_video_height = vid_h;
    cfg.panel_width = static_cast<uint16_t>(screen_w);
    cfg.panel_height = static_cast<uint16_t>(screen_h);
    cfg.target_fps = metrics_.mjpeg_fps;
    cfg.loop = true;
    cfg.fb_stride = 0;
    cfg.fb_size = 0;
    cfg.lv_video_canvas = nullptr;
    cfg.panel_blit_roi = true;
    cfg.panel_roi_x = static_cast<uint16_t>(rx);
    cfg.panel_roi_y = static_cast<uint16_t>(ry);
    cfg.panel_roi_h = 0;
    cfg.panel_roi_w = 0;
    cfg.panel_roi_src_x = src_x;
    cfg.panel_roi_src_y = src_y;
    cfg.panel_src_w = src_block_w;
    cfg.panel_src_h = src_block_h;
    cfg.panel_out_w = out_w;
    cfg.panel_out_h = out_h;
    cfg.panel_scale_n = scale_n;
    cfg.panel_protect_top = protect_top;

    const esp_err_t ret = mjpeg_player_start(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(metrics_.tag, "MJPEG启动失败(%s): %s", current_mjpeg_path_.c_str(), esp_err_to_name(ret));
        current_mjpeg_path_.clear();
        return false;
    }
    return true;
}

void FanHoloDisplay::StopMjpegIfRunning() {
    if (mjpeg_player_is_running()) {
        mjpeg_player_stop();
    }
    current_mjpeg_path_.clear();
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
