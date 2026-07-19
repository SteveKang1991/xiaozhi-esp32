#ifndef FAN_MIPI55_DISPLAY_H
#define FAN_MIPI55_DISPLAY_H

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
#include <lvgl.h>
#include <esp_psram.h>
#include <cstring>
#include <sys/stat.h>

#include "board.h"

#define TAG "FanMIPI55Display"

extern "C" {
#include "mjpeg_player.h"
#include "sd_scanner.h"
}

// FAN MIPI 5.5寸显示器
class FanMIPI55Display : public LcdDisplay {
public:
    /* 重写 SetChatMessage：纵向字幕要求文本按字符换行（每字符一行），
     * 而基类 SetChatMessage 直接把 content 写入 chat_message_label_。
     * LVGL label 在 LONG_WRAP 模式下：英文按词 wrap（一词装不下就换行，导致一行只显示 2-3 个字母），
     * 中文按字符 wrap（因为中文没有空格 word boundary，所以反而一字一行正好）。
     * 这里在写入 label 之前把 UTF-8 字符串按字符插入 '\n'，强制每字符一行，
     * 无论中英文/标点都整齐竖排。 */
    void SetChatMessage(const char* role, const char* content) override;
    /* 重写 ClearChatMessages：基类会调 lv_label_set_text(chat_message_label_, "")，
     * 但 chat_message_label_ 现在是 container obj，重写后清掉 inner label 文本即可。 */
    void ClearChatMessages() override;

    FanMIPI55Display(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

        // Load theme from settings
        Settings settings("display", false);
        std::string theme_name = settings.GetString("theme", "dark");
        current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

        ESP_LOGI(TAG, "Initialize LVGL library");
        lv_init();

        ESP_LOGI(TAG, "Initialize LVGL port");
        lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
        lvgl_port_init(&port_cfg);

        ESP_LOGI(TAG, "Adding LCD display");
        const lvgl_port_display_cfg_t disp_cfg = {
            .io_handle = panel_io,
            .panel_handle = panel,
            .control_handle = nullptr,
            .buffer_size = static_cast<uint32_t>(width_ * 50),
            /* 双缓冲：降低连续 flush/拷贝与 DSI 提交冲突概率 */
            .double_buffer = true,
            .hres = static_cast<uint32_t>(width_),
            .vres = static_cast<uint32_t>(height_),
            .monochrome = false,
            /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
            .rotation = {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
            .flags = {
                .buff_dma = true,
                /* 大分辨率的 LVGL 中间缓冲放 PSRAM，避免和 WiFi/编解码/DSI 强抢内部SRAM */
                .buff_spiram = true,
                .sw_rotate = true,
            },
        };

        const lvgl_port_display_dsi_cfg_t dpi_cfg = {
            .flags = {
                /* 开启 software rotate 时，此处通常需保持 false，否则与 esp_lvgl_port 内部 buffer 管理冲突 */
                .avoid_tearing = false,
            }
        };
        display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
        if (display_ == nullptr) {
            ESP_LOGE(TAG, "Failed to add display");
            return;
        }

        // 物理屏为 480x854 竖屏，无需旋转
        ESP_LOGI(TAG, "LVGL native resolution: %dx%d",
                 (int)lv_display_get_horizontal_resolution(display_),
                 (int)lv_display_get_vertical_resolution(display_));

        if (offset_x != 0 || offset_y != 0) {
            lv_display_set_offset(display_, offset_x, offset_y);
        }

        SetupUI();
    }

    ~FanMIPI55Display() override {
        StopMjpegIfRunning();
    }

    virtual void SetEmotion(const char* emotion) override {
        /* SetEmotion 用于系统开机阶段的表情（microchip_ai / download / circle_xmark 等）
         * 以及升级提示、错误告警等系统级状态。所有这些场景都走主题 GIF / 内置图标，
         * 不再走 MJPEG 路径——MJPEG 仅由 SetRoleAnimation 接管。 */

        /* 进入待机后，connecting/llm 等过渡状态会下发 "neutral"。
         * 这种过渡态 emotion 不应打断角色动画（idle/listen/speak 的 MJPEG）。
         * 真正需要覆盖动画的系统级表情（microchip_ai/download/circle_xmark 等）继续走原逻辑。 */
        if (emotion != nullptr && std::strcmp(emotion, "neutral") == 0 && mjpeg_player_is_running()) {
            return;
        }

        // Stop any running GIF animation
        if (gif_controller_) {
            DisplayLockGuard lock(this);
            gif_controller_->Stop();
            gif_controller_.reset();
        }

        // 如果角色动画（MJPEG）正在播放，关闭它让位给 SetEmotion 的图标
        if (mjpeg_player_is_running()) {
            mjpeg_player_stop();
            current_mjpeg_path_.clear();
        }

        if (emoji_image_ == nullptr) {
            return;
        }

        DisplayLockGuard lock(this);
        auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
        auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(emotion) : nullptr;
        if (image == nullptr) {
            image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage("neutral") : nullptr;
        }
        if (image == nullptr) {
            const char* utf8 = font_awesome_get_utf8(emotion);
            if (utf8 != nullptr && emoji_label_ != nullptr) {
                lv_label_set_text(emoji_label_, utf8);
                lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            }
            return;
        }

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

    /**
     * 角色动画：进入待命状态后由 application 根据对话阶段调用。
     * state ∈ {"idle", "listen", "speak"}，播放 SD 卡对应 clip。
     */
    virtual void SetRoleAnimation(const char* state) override {
        ESP_LOGI(TAG, "SetRoleAnimation state=%s ready=%d sd_mounted=%d",
                 state ? state : "<null>",
                 (int)s_system_ready_, (int)sd_scanner_is_mounted());
        if (emoji_image_ == nullptr) {
            return;
        }

        // 停掉主题 GIF / 内置图标——角色动画接管表情区域
        if (gif_controller_) {
            DisplayLockGuard lock(this);
            gif_controller_->Stop();
            gif_controller_.reset();
        }

        {
            DisplayLockGuard lock(this);
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }

        if (!s_system_ready_ || !sd_scanner_is_mounted()) {
            ESP_LOGW(TAG, "SetRoleAnimation skipped: system not ready or SD not mounted");
            return;
        }

        const char* clip = MapRoleStateToClip(state);
        ESP_LOGI(TAG, "SetRoleAnimation clip=%s", clip);
        StartMjpegEmotion(clip);
    }

    void SetSystemReady() override {
        s_system_ready_ = true;
        ESP_LOGI(TAG, "MJPEG ready: system is ready, SD card operations permitted");
        /* 第一次进入 idle 时 s_system_ready_ 还没置位（SetSystemReady 在 SetDeviceState(idle) 之后调用），
         * SetRoleAnimation("idle") 会被上面的 ready 检查跳过，导致第一帧 idle 没启动。
         * 这里在就绪后再尝试一次，让待机的表情真正起得来。 */
        if (!mjpeg_player_is_running() && sd_scanner_is_mounted()) {
            SetRoleAnimation("idle");
        }
    }

private:
    inline static bool s_system_ready_ = false;

    /* MJPEG 视频分辨率：板子 fanfuture-p4-holo-ili9881-wifi6-lcd-55b 专用 656×1232。 */
    static constexpr uint16_t kMjpegVideoWidth = 656;
    static constexpr uint16_t kMjpegVideoHeight = 1232;
    static constexpr uint8_t kMjpegTargetFps = 24;
    std::string current_mjpeg_path_;

    /* 纵向字幕子标签：作为 chat_message_label_ (container) 的子节点，
     * 长 = 字宽 20, 高 = 内容 (LV_SIZE_CONTENT, 由 LONG_WRAP 自动 wrap)。
     * 完全透明背景，避免黑底重叠视频画面。 */
    lv_obj_t* chat_message_inner_label_ = nullptr;
    /* 上次设置的 content：连续相同 content 时不重启滚动动画，避免闪烁/重启开销。 */
    std::string last_chat_content_;

    static bool FileExists(const std::string& path) {
        struct stat st = {};
        return stat(path.c_str(), &st) == 0;
    }

    /**
     * 查找指定 clip 名的 MJPEG 文件
     * 优先查找 {clip}-{width}x{height}.mjpeg，不存在则回退到默认 idle-{width}x{height}.mjpeg
     * 参数 clip_name 必须是已映射过、合法的 clip 名（如 "idle"/"listen"/"speak"）。
     */
    static std::string FindMjpegPath(const char* clip_name) {
        char default_path[128];
        snprintf(default_path, sizeof(default_path),
                 "/sdcard/Emotion/%s-%ux%u.mjpeg", clip_name,
                 (unsigned)kMjpegVideoWidth, (unsigned)kMjpegVideoHeight);
        if (FileExists(default_path)) {
            return default_path;
        }
        ESP_LOGW(TAG, "FindMjpegPath: %s 不存在，回退检查 idle", default_path);
        // 回退到 idle
        if (strcmp(clip_name, "idle") != 0) {
            snprintf(default_path, sizeof(default_path),
                     "/sdcard/Emotion/idle-%ux%u.mjpeg",
                     (unsigned)kMjpegVideoWidth, (unsigned)kMjpegVideoHeight);
            if (FileExists(default_path)) {
                return default_path;
            }
            ESP_LOGE(TAG, "FindMjpegPath: 回退 idle 也找不到: %s", default_path);
        }
        return "";
    }

    /**
     * 角色动画状态到 MJPEG clip 名的映射。
     * 仅允许 idle / listen / speak 三种，其他（含 nullptr）一律回退到 idle。
     */
    static const char* MapRoleStateToClip(const char* state) {
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

    bool StartMjpegEmotion(const char* emotion) {
        if (!s_system_ready_) {
            ESP_LOGW(TAG, "MJPEG跳过：系统未就绪");
            return false;
        }
        if (!sd_scanner_is_mounted()) {
            ESP_LOGW(TAG, "MJPEG跳过：SD卡未挂载");
            return false;
        }

        std::string clip_path = FindMjpegPath(emotion);
        if (clip_path.empty()) {
            ESP_LOGE(TAG, "StartMjpegEmotion: clip=%s 无可用文件，跳过", emotion);
            return false;
        }

        if (mjpeg_player_is_running()) {
            if (clip_path == current_mjpeg_path_) {
                return true;
            }
            mjpeg_player_stop();
        }

        int rx = (width_ - static_cast<int>(kMjpegVideoWidth)) / 2;
        /* 视频区从状态栏下方开始，与屏底对齐：ry = height - video_height */
        int ry = ((int)height_ - (int)kMjpegVideoHeight);
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
        cfg.mjpeg_video_width = kMjpegVideoWidth;
        cfg.mjpeg_video_height = kMjpegVideoHeight;
        cfg.panel_width = static_cast<uint16_t>(width_);
        cfg.panel_height = static_cast<uint16_t>(height_);
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
            ESP_LOGW(TAG, "MJPEG启动失败(%s): %s", current_mjpeg_path_.c_str(), esp_err_to_name(ret));
            current_mjpeg_path_.clear();
            return false;
        }

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
            ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
            return;
        }
        Display::SetupUI();
        DisplayLockGuard lock(this);
        LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        auto text_font = lvgl_theme->text_font()->font();
        auto icon_font = lvgl_theme->icon_font()->font();
        auto large_icon_font = lvgl_theme->large_icon_font()->font();

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
        top_bar_ = lv_obj_create(screen);
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
        //lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);

        // Left side: status label container (replaces former "network_label_" position).
        // Wraps notification_label_ + status_label_ so the original notification overlay logic still works.
        lv_obj_t* left_status = lv_obj_create(top_bar_);
        lv_obj_set_size(left_status, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(left_status, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(left_status, 0, 0);
        lv_obj_set_style_pad_all(left_status, 0, 0);
        lv_obj_set_flex_flow(left_status, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(left_status, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_radius(left_status, 0, 0);
        lv_obj_set_scrollbar_mode(left_status, LV_SCROLLBAR_MODE_OFF);

        notification_label_ = lv_label_create(left_status);
        lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
        lv_label_set_text(notification_label_, "");
        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

        status_label_ = lv_label_create(left_status);
        lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(status_label_, 160);  // 左侧固定宽度（屏宽 720，够显示 ~12 汉字，超长循环滚动）
        lv_obj_set_style_max_width(status_label_, 200, 0);
        lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
        lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);

        // Right icons container
        lv_obj_t* right_icons = lv_obj_create(top_bar_);
        lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(right_icons, 0, 0);
        lv_obj_set_style_pad_all(right_icons, 0, 0);
        lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        mute_label_ = lv_label_create(right_icons);
        lv_label_set_text(mute_label_, "");
        lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

        // Network icon goes to the right side, just before battery (next to it).
        network_label_ = lv_label_create(right_icons);
        lv_label_set_text(network_label_, "");
        lv_obj_set_style_text_font(network_label_, icon_font, 0);
        lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);
        lv_obj_set_style_margin_left(network_label_, lvgl_theme->spacing(2), 0);

        battery_label_ = lv_label_create(right_icons);
        lv_label_set_text(battery_label_, "");
        lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
        lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

        /* 旧的 status_bar_ 全屏宽透明覆盖层已废弃：状态文本直接放到 top_bar_ 左侧
         * (left_status 容器)。这里显式置 nullptr，确保任何外部"if (status_bar_)"检查走
         * 未初始化/无效路径，避免误操作 (基类 LcdDisplay 析构里有 nullptr 守卫)。 */
        status_bar_ = nullptr;

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
        lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);**/

        /* chat_message_label_ 屏左侧纵向字幕（竖条字幕）。
         *
         * 实现：lvgl_container (24×(height-300)) + 内部 label (宽 24, LV_SIZE_CONTENT, LONG_WRAP)，
         * 内部 label 高度 = 文本高度（按字符强制换行后单字符一行）。
         * container 设 LV_DIR_VER scroll + clip，超长文本通过 lv_anim 把 scroll_y 从 0 一次性
         * 动画到 content_h - container_h,到底后停在末尾。文本短（≤ container_h）时不滚动。 */
        chat_message_label_ = lv_obj_create(screen);
        lv_obj_set_scrollbar_mode(chat_message_label_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_scroll_dir(chat_message_label_, LV_DIR_VER);
        lv_obj_set_size(chat_message_label_, 30, (lv_coord_t)(height_ - 300));
        /* container 必须透明，不能设 bg_color — 否则整块黑底会重叠视频画面。 */
        lv_obj_set_style_bg_opa(chat_message_label_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(chat_message_label_, 0, 0);
        lv_obj_set_style_pad_all(chat_message_label_, 0, 0);
        /* LV_ALIGN_LEFT_MID: 左边缘对齐到屏左 x=0，垂直居中。 */
        lv_obj_align(chat_message_label_, LV_ALIGN_LEFT_MID, 0, 0);

        chat_message_inner_label_ = lv_label_create(chat_message_label_);
        lv_label_set_text(chat_message_inner_label_, "");
        /* 宽 30 (字宽 = font_puhui_basic_30_4 字宽 20)，高度跟随内容。 */
        lv_obj_set_width(chat_message_inner_label_, 30);
        lv_obj_set_style_bg_opa(chat_message_inner_label_, LV_OPA_TRANSP, 0);
        lv_label_set_long_mode(chat_message_inner_label_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(chat_message_inner_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(chat_message_inner_label_, lvgl_theme->text_color(), 0);

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

/* 纵向字幕：把 content 按 UTF-8 字符强制拆行（每字符一行），写到 chat_message_inner_label_。
 * container chat_message_label_ (24×(height-300)) 设 LV_DIR_VER scroll + clip，超长文本
 * 通过 lv_anim 把 scroll_y 从 0 一次性动画到 content_h - container_h，到底后停在末尾。 */
inline void FanMIPI55Display::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!", role, content);
    }
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr || chat_message_inner_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetChatMessage('%s', '%s') failed: chat container not ready", role, content);
        }
        return;
    }
    if (content == nullptr || content[0] == '\0') {
        lv_label_set_text(chat_message_inner_label_, "");
        last_chat_content_.clear();
        /* 立即停止任何进行中的滚动动画（防止 SetChatMessage("") 后还在继续滚动） */
        lv_obj_scroll_to_y(chat_message_label_, 0, LV_ANIM_OFF);
        if (bottom_bar_ != nullptr) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    /* 内容去重：连续相同 content 不重启动画，避免闪烁。 */
    if (last_chat_content_ == content) {
        if (bottom_bar_ != nullptr && !hide_subtitle_) {
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    last_chat_content_ = content;

    /* 按 UTF-8 字符拆分：每个字符后插一个 '\n'。 */
    std::string in(content);
    std::string out;
    out.reserve(in.size() * 2);
    for (size_t i = 0; i < in.size(); ) {
        unsigned char c = (unsigned char)in[i];
        size_t step = 1;
        if ((c & 0x80) == 0) {
            step = 1;
        } else if ((c & 0xE0) == 0xC0) {
            step = 2;
        } else if ((c & 0xF0) == 0xE0) {
            step = 3;
        } else if ((c & 0xF8) == 0xF0) {
            step = 4;
        }
        if (i + step > in.size()) step = in.size() - i;
        out.append(in, i, step);
        out.push_back('\n');
        i += step;
    }
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
    }
    lv_label_set_text(chat_message_inner_label_, out.c_str());

    /* 强制 layout 计算内容高度。 */
    lv_obj_update_layout(chat_message_inner_label_);
    lv_coord_t content_h = lv_obj_get_height(chat_message_inner_label_);
    lv_coord_t container_h = lv_obj_get_height(chat_message_label_);

    if (content_h <= container_h) {
        /* 文本短，不滚动，定位到顶（content 顶部对齐 container 顶部）。 */
        lv_obj_scroll_to_y(chat_message_label_, 0, LV_ANIM_OFF);
        if (bottom_bar_ != nullptr && !hide_subtitle_) {
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    /* 文本超长：单次"向上滚动到底后静止"。
     * lv_anim 默认 repeat_count = 1（不循环），linear 路径从 scroll_y=0 一次性动画到 scroll_range_px，
     * 到底后停在末尾，不重头滚、不反向。 */
    int scroll_range_px = content_h - container_h;
    int chars_per_screen = container_h / 20;
    if (chars_per_screen < 1) chars_per_screen = 1;
    int total_chars = content_h / 20;
    int extra_chars = total_chars - chars_per_screen;
    if (extra_chars < 1) extra_chars = 1;
    uint32_t duration_ms = (uint32_t)(extra_chars * 400);  // 400ms/字符
    if (duration_ms < 2000) duration_ms = 2000;
    if (duration_ms > 60000) duration_ms = 60000;

    /* 先归零：若之前在滚动，确保从 0 开始。 */
    lv_obj_scroll_to_y(chat_message_label_, 0, LV_ANIM_OFF);

    /* exec_cb: 每帧直接把 v 写到 container 的 scroll_y（线性、单次、动画到底后停）。 */
    static lv_anim_t s_scroll_anim;
    lv_anim_delete(&s_scroll_anim, nullptr);
    lv_anim_init(&s_scroll_anim);
    lv_anim_set_var(&s_scroll_anim, chat_message_label_);
    lv_anim_set_values(&s_scroll_anim, 0, scroll_range_px);
    lv_anim_set_duration(&s_scroll_anim, duration_ms);
    /* 不调用 set_repeat_count = INFINITE → 默认 1 次走完到底。 */
    lv_anim_set_path_cb(&s_scroll_anim, lv_anim_path_linear);
    lv_anim_set_exec_cb(&s_scroll_anim, [](void* var, int32_t v) {
        lv_obj_scroll_to_y((lv_obj_t*)var, v, LV_ANIM_OFF);
    });
    lv_anim_start(&s_scroll_anim);

    if (bottom_bar_ != nullptr && !hide_subtitle_) {
        lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 清空纵向字幕：基类 ClearChatMessages 默认调 lv_label_set_text(chat_message_label_, "")，
 * 但 chat_message_label_ 实际上是 container obj，重写后改清 inner label。 */
inline void FanMIPI55Display::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (chat_message_inner_label_ != nullptr) {
        lv_label_set_text(chat_message_inner_label_, "");
    }
    if (chat_message_label_ != nullptr) {
        lv_obj_scroll_to_y(chat_message_label_, 0, LV_ANIM_OFF);
    }
    last_chat_content_.clear();
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}

#endif // FAN_MIPI55_DISPLAY_H
