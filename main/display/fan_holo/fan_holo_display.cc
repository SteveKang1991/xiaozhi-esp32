#include "fan_holo_display.h"

#include "settings.h"
#include "gif/lvgl_gif.h"
#include "assets/lang_config.h"
#include "board.h"

#include <memory>

#include <font_awesome.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <cstring>
#include <cstdio>
#include <sys/stat.h>

extern "C" {
#include "mjpeg_player.h"
#include "sd_scanner.h"
}

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

    SetupUI();
}

FanHoloDisplay::~FanHoloDisplay() {
    StopMjpegIfRunning();
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    teardownFft();
    DisplayLockGuard lock(this);
    NullBoundWidgets();
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
    emoji_box_ = nullptr;
    emoji_label_ = nullptr;
    emoji_image_ = nullptr;
    preview_image_ = nullptr;
    container_ = nullptr;
    chat_message_label_ = nullptr;
    bottom_bar_ = nullptr;
    content_ = nullptr;
}

void FanHoloDisplay::SwitchTo(Page page) {
    FanHoloStatusBar* prev = nullptr;
    switch (current_page_) {
        case Page::Idle: prev = &idle_page_.status_bar(); break;
        case Page::Chat: prev = &chat_page_.status_bar(); break;
        case Page::Music: prev = &music_page_.status_bar(); break;
    }

    current_page_ = page;
    FanHoloStatusBar* now = nullptr;
    switch (page) {
        case Page::Idle:
            idle_page_.Show(*this);
            now = &idle_page_.status_bar();
            break;
        case Page::Chat:
            chat_page_.Show(*this);
            now = &chat_page_.status_bar();
            break;
        case Page::Music:
            music_page_.Show(*this);
            now = &music_page_.status_bar();
            break;
    }
    if (prev && now && prev != now) {
        now->CopyVisualFrom(*prev);
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

    auto& role = idle_page_.role_widgets();
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
                lv_image_set_src(idle_page_.role_widgets().image, gif_controller_->image_dsc());
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
    const std::string& clip_path = FindRoleAnimation(clip);
    if (clip_path.empty()) {
        ESP_LOGI(metrics_.tag, "SetRoleAnimation: no available MJPEG for state=%s, skip", clip);
        return;
    }

    {
        DisplayLockGuard lock(this);
        SwitchTo((strcmp(clip, "idle") == 0) ? Page::Idle : Page::Chat);
    }
    StartMjpegEmotion(clip_path.c_str());
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
}

bool FanHoloDisplay::FileExists(const std::string& path) {
    struct stat st = {};
    return stat(path.c_str(), &st) == 0;
}

std::string FanHoloDisplay::FindRoleAnimation(const char* state) {
    const char* clips[2] = { state, "idle" };
    for (int i = 0; i < 2; i++) {
        char path[128];
        snprintf(path, sizeof(path), "/sdcard/Emotion/%s-%ux%u.mjpeg", clips[i],
                 (unsigned)metrics_.mjpeg_w, (unsigned)metrics_.mjpeg_h);
        if (FileExists(path)) {
            ESP_LOGI(metrics_.tag, "FindRoleAnimation: found %s", path);
            return path;
        }
    }

    bool user_has_any = false;
    const char* all_clips[3] = { "idle", "listen", "speak" };
    for (int i = 0; i < 3; i++) {
        char test_path[128];
        snprintf(test_path, sizeof(test_path), "/sdcard/Emotion/%s-%ux%u.mjpeg",
                 all_clips[i], (unsigned)metrics_.mjpeg_w, (unsigned)metrics_.mjpeg_h);
        if (FileExists(test_path)) {
            user_has_any = true;
            break;
        }
    }

    if (user_has_any) {
        ESP_LOGW(metrics_.tag, "FindRoleAnimation: user has role animation but no %s, skip", state);
        return "";
    }

    for (int i = 0; i < 2; i++) {
        char path[128];
        snprintf(path, sizeof(path), "/sdcard/Emotion/default-%s-%ux%u.mjpeg", clips[i],
                 (unsigned)metrics_.mjpeg_w, (unsigned)metrics_.mjpeg_h);
        if (FileExists(path)) {
            ESP_LOGI(metrics_.tag, "FindRoleAnimation: found default %s", path);
            return path;
        }
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

bool FanHoloDisplay::StartMjpegEmotion(const char* full_path) {
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
        if (strcmp(full_path, current_mjpeg_path_.c_str()) == 0) {
            return true;
        }
        mjpeg_player_stop();
    }

    int rx = (width_ - static_cast<int>(metrics_.mjpeg_w)) / 2;
    int ry = ((int)height_ - (int)metrics_.mjpeg_h);
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
    cfg.mjpeg_video_width = metrics_.mjpeg_w;
    cfg.mjpeg_video_height = metrics_.mjpeg_h;
    cfg.panel_width = static_cast<uint16_t>(width_);
    cfg.panel_height = static_cast<uint16_t>(height_);
    cfg.target_fps = metrics_.mjpeg_fps;
    cfg.loop = true;
    cfg.fb_stride = 0;
    cfg.fb_size = 0;
    cfg.lv_video_canvas = nullptr;
    cfg.panel_blit_roi = true;
    cfg.panel_roi_x = static_cast<uint16_t>(rx);
    cfg.panel_roi_y = static_cast<uint16_t>(ry);

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
    SwitchTo(Page::Idle);
}
