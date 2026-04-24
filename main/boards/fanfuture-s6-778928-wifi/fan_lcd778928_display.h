#ifndef FAN_LCD778928_DISPLAY_H
#define FAN_LCD778928_DISPLAY_H

#include "lcd_display.h"
#include "gif/lvgl_gif.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "assets/lang_config.h"

#include <vector>
#include <algorithm>
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <cstring>
#include <cstdio>
#include <src/misc/cache/lv_cache.h>
#include <sys/stat.h>

#include "board.h"

extern "C" {
#include "mjpeg_player.h"
#include "sd_scanner.h"
}

#define TAG "FanLcd778928Display"

// FAN LCD 2.8寸显示器
class FanLcd778928Display : public LcdDisplay {
public:
    FanLcd778928Display(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

        // Load theme from settings
        Settings settings("display", false);
        std::string theme_name = settings.GetString("theme", "dark");
        current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

        // draw white
        std::vector<uint16_t> buffer(width_, 0xFFFF);
        for (int y = 0; y < height_; y++) {
            esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
        }

        // Set the display to on
        ESP_LOGI(TAG, "Turning display on");
        {
            esp_err_t __err = esp_lcd_panel_disp_on_off(panel_, true);
            if (__err == ESP_ERR_NOT_SUPPORTED) {
                ESP_LOGW(TAG, "Panel does not support disp_on_off; assuming ON");
            } else {
                ESP_ERROR_CHECK(__err);
            }
        }

        ESP_LOGI(TAG, "Initialize LVGL library");
        lv_init();

    #if CONFIG_SPIRAM
        // lv image cache, currently only PNG is supported
        size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
        if (psram_size_mb >= 8) {
            lv_image_cache_resize(2 * 1024 * 1024, true);
            ESP_LOGI(TAG, "Use 2MB of PSRAM for image cache");
        } else if (psram_size_mb >= 2) {
            lv_image_cache_resize(512 * 1024, true);
            ESP_LOGI(TAG, "Use 512KB of PSRAM for image cache");
        }
    #endif

        ESP_LOGI(TAG, "Initialize LVGL port");
        lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
        port_cfg.task_priority = 2;
    #if CONFIG_SOC_CPU_CORES_NUM > 1
        port_cfg.task_affinity = 1;
    #endif
        lvgl_port_init(&port_cfg);

        ESP_LOGI(TAG, "Adding LCD display");
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
            ESP_LOGE(TAG, "Failed to add display");
            return;
        }

        if (offset_x != 0 || offset_y != 0) {
            lv_display_set_offset(display_, offset_x, offset_y);
        }

        SetupUI();
    }

    ~FanLcd778928Display() {
        StopMjpegIfRunning();
    }

    virtual void SetEmotion(const char* emotion) override {
        // Stop any running GIF animation
        if (gif_controller_) {
            DisplayLockGuard lock(this);
            gif_controller_->Stop();
            gif_controller_.reset();
        }
        
        if (emoji_image_ == nullptr) {
            return;
        }

        if (StartMjpegEmotion(emotion)) {
            DisplayLockGuard lock(this);
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            return;
        }

        /* 说话/LLM 会频繁 SetEmotion；若 MJPEG 已在播而本次又未能启动新 clip，切勿走主题 GIF 分支里的
         * StopMjpegIfRunning()，否则会把正播的视频整段停掉。 */
        if (mjpeg_player_is_running()) {
            DisplayLockGuard lock(this);
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            return;
        }

        auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
        auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(emotion) : nullptr;
        if (image == nullptr) {
            image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage("neutral") : nullptr;
        }
        if (image == nullptr) {
            StopMjpegIfRunning();
            const char* utf8 = font_awesome_get_utf8(emotion);
            if (utf8 != nullptr && emoji_label_ != nullptr) {
                DisplayLockGuard lock(this);
                lv_label_set_text(emoji_label_, utf8);
                lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            }
            return;
        }

        StopMjpegIfRunning();
        DisplayLockGuard lock(this);
        if (image->IsGif()) {
            // Create new GIF controller
            gif_controller_ = std::make_unique<LvglGif>(image->image_dsc());
            
            if (gif_controller_->IsLoaded()) {
                // Set up frame update callback
                gif_controller_->SetFrameCallback([this]() {
                    lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
                });
                
                // Set initial frame and start animation
                lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
                gif_controller_->Start();
                
                // Show GIF, hide others
                lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            } else {
                ESP_LOGE(TAG, "Failed to load GIF for emotion: %s", emotion);
                gif_controller_.reset();
            }
        } else {
            lv_image_set_src(emoji_image_, image->image_dsc());
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        }
    }

private:
    static constexpr uint16_t kMjpegVideoWidth = 240;
    static constexpr uint16_t kMjpegVideoHeight = 290;
    static constexpr uint8_t kMjpegTargetFps = 30;
    std::string current_mjpeg_path_;

    static bool FileExists(const std::string& path) {
        struct stat st = {};
        return stat(path.c_str(), &st) == 0;
    }

    static std::string BuildMjpegPath(const char* name) {
        char suffix[48];
        snprintf(suffix, sizeof(suffix), "-%ux%u.mjpeg", (unsigned)kMjpegVideoWidth, (unsigned)kMjpegVideoHeight);
        return std::string("/sdcard/Emotion/") + name + suffix;
    }

    static const char* MapEmotionToClip(const char* emotion) {
        if (emotion == nullptr || std::strlen(emotion) == 0) {
            return "idle";
        }
        if (strcmp(emotion, "neutral") == 0) {
            return "idle";
        }
        return emotion;
    }

    bool StartMjpegEmotion(const char* emotion) {
        if (!sd_scanner_is_mounted()) {
            ESP_LOGW(TAG, "MJPEG跳过：SD卡未挂载");
            return false;
        }

        const char* clip_name = MapEmotionToClip(emotion);
        std::string clip_path = BuildMjpegPath(clip_name);
        if (!FileExists(clip_path)) {
            ESP_LOGW(TAG, "未找到情绪视频文件 '%s'：%s", clip_name, clip_path.c_str());
            clip_path = BuildMjpegPath("idle");
            if (!FileExists(clip_path)) {
                ESP_LOGW(TAG, "未找到回退视频文件：%s", clip_path.c_str());
                ESP_LOGW(TAG, "请放置文件：/sdcard/Emotion/<emotion>-%ux%u.mjpeg（当前目标分辨率 %ux%u）",
                         (unsigned)kMjpegVideoWidth, (unsigned)kMjpegVideoHeight,
                         (unsigned)kMjpegVideoWidth, (unsigned)kMjpegVideoHeight);
                return false;
            }
            ESP_LOGI(TAG, "使用回退视频文件：%s", clip_path.c_str());
        }

        if (mjpeg_player_is_running()) {
            if (clip_path == current_mjpeg_path_) {
                return true;
            }
            mjpeg_player_stop();
        }

        int rx = (width_ - static_cast<int>(kMjpegVideoWidth)) / 2;
        /* 顶端对齐：视频从屏幕 y=0 开始显示 */
        int ry = 0;
        if (rx < 0) {
            rx = 0;
        }
        if (ry < 0) {
            ry = 0;
        }

        current_mjpeg_path_ = clip_path;
        mjpeg_player_cfg_t cfg = {};
        cfg.file_path = current_mjpeg_path_.c_str();
        cfg.panel = panel_;
        cfg.fb[0] = nullptr;
        cfg.fb[1] = nullptr;
        cfg.screen_width = kMjpegVideoWidth;
        cfg.screen_height = kMjpegVideoHeight;
        cfg.target_fps = kMjpegTargetFps;
        cfg.loop = true;
        cfg.fb_stride = 0;
        cfg.fb_size = 0;
        cfg.lv_video_canvas = nullptr;
        cfg.panel_blit_roi = true;
        cfg.panel_roi_x = static_cast<uint16_t>(rx);
        cfg.panel_roi_y = static_cast<uint16_t>(ry);

        const esp_err_t ret = mjpeg_player_start(&cfg);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "MJPEG启动失败（%s）：%s", current_mjpeg_path_.c_str(), esp_err_to_name(ret));
            current_mjpeg_path_.clear();
            return false;
        }

        ESP_LOGI(TAG, "MJPEG表情播放：%s", current_mjpeg_path_.c_str());
        return true;
    }

    void StopMjpegIfRunning() {
        if (mjpeg_player_is_running()) {
            mjpeg_player_stop();
        }
        current_mjpeg_path_.clear();
    }

    void SetupUI() {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetupUI() 被重复调用，跳过");
            return;
        }
        Display::SetupUI();
        DisplayLockGuard lock(this);
        LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        auto text_font = lvgl_theme->text_font()->font();
        auto icon_font = lvgl_theme->icon_font()->font();
        auto large_icon_font = lvgl_theme->large_icon_font()->font();

        /* 底部一排：网络 | 状态文案 | 电量等；高度用于字幕与低电量条避让 */
        const int ui_bottom_inset = lvgl_theme->spacing(2);

        auto screen = lv_screen_active();
        lv_obj_set_style_text_font(screen, text_font, 0);
        lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
        lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

        /* Container - used as background */
        container_ = lv_obj_create(screen);
        lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_radius(container_, 0, 0);
        lv_obj_set_style_pad_all(container_, 0, 0);
        lv_obj_set_style_border_width(container_, 0, 0);
        lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
        lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

        /* Bottom layer: emoji_box_ - centered display */
        emoji_box_ = lv_obj_create(screen);
        lv_obj_set_size(emoji_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(emoji_box_, 0, 0);
        lv_obj_set_style_border_width(emoji_box_, 0, 0);
        lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, 0);

        emoji_label_ = lv_label_create(emoji_box_);
        lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
        lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
        lv_label_set_text(emoji_label_, FONT_AWESOME_MICROCHIP_AI);

        emoji_image_ = lv_img_create(emoji_box_);
        lv_obj_center(emoji_image_);
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

        /* Middle layer: preview_image_ - centered display */
        preview_image_ = lv_image_create(screen);
        lv_obj_set_size(preview_image_, width_ / 2, height_ / 2);
        lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

        /* Layer 1: Top bar - for status icons */
        /**top_bar_ = lv_obj_create(screen);
        lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(top_bar_, 0, 0);
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
        lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
        lv_obj_set_style_border_width(top_bar_, 0, 0);
        lv_obj_set_style_pad_all(top_bar_, 0, 0);
        lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
        lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
        lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
        lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
        lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);

        // Left icon
        network_label_ = lv_label_create(top_bar_);
        lv_label_set_text(network_label_, "");
        lv_obj_set_style_text_font(network_label_, icon_font, 0);
        lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

        // Right icons container
        lv_obj_t* right_icons = lv_obj_create(top_bar_);
        lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(right_icons, 0, 0);
        lv_obj_set_style_pad_all(right_icons, 0, 0);
        lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);**/

        // Left icon（屏幕最下方一排左侧）
        network_label_ = lv_label_create(screen);
        lv_label_set_text(network_label_, "");
        lv_obj_set_style_text_font(network_label_, icon_font, 0);
        lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);
        lv_obj_align(network_label_, LV_ALIGN_BOTTOM_LEFT, lvgl_theme->spacing(4), -ui_bottom_inset);

        // Right icons container（屏幕最下方一排右侧）
        lv_obj_t* right_icons = lv_obj_create(screen);
        lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(right_icons, 0, 0);
        lv_obj_set_style_pad_all(right_icons, 0, 0);
        lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_align(right_icons, LV_ALIGN_BOTTOM_RIGHT, -lvgl_theme->spacing(4), -ui_bottom_inset);

        mute_label_ = lv_label_create(right_icons);
        lv_label_set_text(mute_label_, "");
        lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

        battery_label_ = lv_label_create(right_icons);
        lv_label_set_text(battery_label_, "");
        lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
        lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

        /* 状态栏：与网络/电量同一排，居中（屏幕最下方） */
        status_bar_ = lv_obj_create(screen);
        lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(status_bar_, 0, 0);
        lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
        lv_obj_set_style_border_width(status_bar_, 0, 0);
        lv_obj_set_style_pad_all(status_bar_, 0, 0);
        //lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
        //lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
        lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
        lv_obj_align(status_bar_, LV_ALIGN_BOTTOM_MID, 0, -ui_bottom_inset);
        //lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);

        notification_label_ = lv_label_create(status_bar_);
        lv_obj_set_width(notification_label_, LV_HOR_RES * 0.75);
        lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
        lv_label_set_text(notification_label_, "");
        lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

        status_label_ = lv_label_create(status_bar_);
        lv_obj_set_width(status_label_, LV_HOR_RES * 0.75);
        lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
        lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
        lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

        /* Top layer: Bottom bar - fixed at bottom, minimum height 48, height can be adaptive */
        /**bottom_bar_ = lv_obj_create(screen);
        lv_obj_set_width(bottom_bar_, LV_HOR_RES);
        lv_obj_set_height(bottom_bar_, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(bottom_bar_, 48, 0); // Set minimum height 48
        lv_obj_set_style_radius(bottom_bar_, 0, 0);
        lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
        lv_obj_set_style_text_color(bottom_bar_, lvgl_theme->text_color(), 0);
        lv_obj_set_style_pad_top(bottom_bar_, lvgl_theme->spacing(2), 0);
        lv_obj_set_style_pad_bottom(bottom_bar_, lvgl_theme->spacing(2), 0);
        lv_obj_set_style_pad_left(bottom_bar_, lvgl_theme->spacing(4), 0);
        lv_obj_set_style_pad_right(bottom_bar_, lvgl_theme->spacing(4), 0);
        lv_obj_set_style_border_width(bottom_bar_, 0, 0);
        lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);**/

        /* chat_message_label_ placed in bottom_bar_ and vertically centered */
        chat_message_label_ = lv_label_create(screen);
        lv_label_set_text(chat_message_label_, "");
        lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8)); // Subtract left and right padding
        //lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP); // Auto wrap mode
        lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);  // 文字超出会滚动
        lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0); // Center text alignment
        lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
        //lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0); // Vertically and horizontally centered in bottom_bar_
        lv_obj_align(chat_message_label_, LV_ALIGN_BOTTOM_MID, 0, -35);
        lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);

        low_battery_popup_ = lv_obj_create(screen);
        lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
        lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
        lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
        lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
        
        low_battery_label_ = lv_label_create(low_battery_popup_);
        lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
        lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
        lv_obj_center(low_battery_label_);
        lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
    }
};

#endif // FAN_LCD778928_DISPLAY_H
