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

#include "board.h"

extern "C" {
#include "mjpeg_player.h"
#include "sd_scanner.h"
#include "emotion_partition_storage.h"
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
        /* SetEmotion 用于系统开机阶段的表情（microchip_ai / download / circle_xmark 等）
         * 以及升级提示、错误告警等系统级状态。系统就绪（s_system_ready_=true）后，
         * 待机画面完全交给 SetRoleAnimation 接管，SetEmotion 直接 noop。
         * 这样状态机切换不会触发"先 stop MJPEG 再 start 同一路径"的诡异闪烁。 */
        if (s_system_ready_) {
            ESP_LOGD(TAG, "SetEmotion(%s) ignored: system ready, role animation owns the display", emotion ? emotion : "<null>");
            return;
        }

        // Stop any running GIF animation
        if (gif_controller_) {
            DisplayLockGuard lock(this);
            gif_controller_->Stop();
            gif_controller_.reset();
        }

        // 如果角色动画（MJPEG）正在播放，关闭它让位给 SetEmotion 的图标。
        // 注意：保留 current_clip_name_ 不清空，这样 SetRoleAnimation 进来时
        // 路径相同能直接 skip（表情包只显示几秒，下一次状态切换大概率还是同一文件）。
        // 异步 stop：避免长同步等待拖慢表情图标显示。
        if (mjpeg_player_is_running()) {
            mjpeg_player_stop_async();
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
                DisplayLockGuard lock(this);
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

    virtual void SetRoleAnimation(const char* state) override {
        if (emoji_image_ == nullptr) {
            return;
        }

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

        if (!s_system_ready_) {
            return;
        }

        const char* clip = MapRoleStateToClip(state);
        const ClipLoc clip_loc = FindRoleAnimation(clip);
        if (!clip_loc.valid()) {
            ESP_LOGI(TAG, "SetRoleAnimation: no available MJPEG for state=%s, skip", clip);
            return;
        }

        /* 单一真相源：文件名 + offset + size 联合比较。
         * 注意 current_clip_name_ 在 mjpeg_player_start 成功后才更新，确保永远反映"已播放"的状态。 */
        char cur_key[160] = {0};
        if (!current_clip_name_.empty()) {
            /* 简化：只比较 name（同一文件 offset/size 不会变） */
            snprintf(cur_key, sizeof(cur_key), "%s", current_clip_name_.c_str());
        }
        if (clip_loc.name == current_clip_name_) {
            if (mjpeg_player_is_running()) {
                ESP_LOGI(TAG, "SetRoleAnimation: same clip %s, skip (state=%s)", clip_loc.name.c_str(), state ? state : "null");
                return;
            }
            /* name 相同但 MJPEG 未运行（可能被 SetEmotion stop 了）→ 直接重启 */
            ESP_LOGI(TAG, "SetRoleAnimation: same clip %s but not running, restart (state=%s)",
                     clip_loc.name.c_str(), state ? state : "null");
        } else {
            ESP_LOGI(TAG, "SetRoleAnimation: state=%s clip=%s name=%s (differs from %s)",
                     state ? state : "null", clip, clip_loc.name.c_str(), current_clip_name_.c_str());
        }
        StartMjpegEmotion(clip_loc);
    }

    void SetSystemReady() override {
        s_system_ready_ = true;
        ESP_LOGI(TAG, "MJPEG ready: system is ready");
        /* 一次性扫描 assets 分区上的 Emotion 文件并缓存。 */
        ScanEmotionClips();
        /* 第一次进入 idle 时 s_system_ready_ 还没置位（SetSystemReady 在 SetDeviceState(idle) 之后调用），
         * SetRoleAnimation("idle") 会被上面的 ready 检查跳过，导致第一帧 idle 没启动。
         * 这里在就绪后再尝试一次，让待机的表情真正起得来。 */
        if (!mjpeg_player_is_running()) {
            SetRoleAnimation("idle");
        }
    }

private:
    inline static bool s_system_ready_ = false;

    static constexpr uint16_t kMjpegVideoWidth = 240;
    static constexpr uint16_t kMjpegVideoHeight = 290;
    /* S3 软解 esp_new_jpeg：30fps × 30ms/帧 几乎打满 CPU，与 Opus 解码抢 CPU 导致 TTS 卡顿。
     * 实测降到 20fps 后每帧 50ms 预算，软解 + Opus 不再争 CPU，TTS 流畅。 */
    static constexpr uint8_t kMjpegTargetFps = 24;
    std::string current_mjpeg_path_;
    std::string current_clip_name_;  /* 当前播放的 flash 文件名（part:offset:size 表示法） */

    /* flash 资产位置：文件表里的文件名 + offset + size
     * name 是资产名（如 "idle-240x290.mjpeg"），offset/size 是相对分区的偏移 */
    struct ClipLoc {
        std::string name;
        uint32_t offset = 0;
        uint32_t size = 0;
        bool valid() const { return !name.empty() && size > 0; }
    };

    /* 启动时（emotion_partition_storage 已 init）一次性扫描所有候选 flash 文件并缓存。
     * 后续 SetRoleAnimation 直接查此缓存，不再访问 SD 卡或 flash。
     * 文件命名约定：<name>-WxH.mjpeg（运行时 width/height）。
     *   - user 层：idle-240x290.mjpeg, listen-240x290.mjpeg, speak-240x290.mjpeg
     *   - default 层：default-idle-240x290.mjpeg
     */
    /* ClipCache 只存 user 层 3 个 slot + 唯一的 default-idle。
     * user 层有任意文件时 FindRoleAnimation 永不 fallback 到 default 层。 */
    struct ClipCache {
        bool scanned = false;
        ClipLoc idle;       /* user idle-240x290.mjpeg */
        ClipLoc listen;     /* user listen-240x290.mjpeg */
        ClipLoc speak;      /* user speak-240x290.mjpeg */
        ClipLoc default_clip;  /* default idle-240x290.mjpeg（只有 user 层全空时才用） */
    };
    static ClipCache s_clip_cache;

    /* 把 clip name + 分辨率组合成 flash 资产名。
     * 例如 ("idle") -> "idle-240x290.mjpeg"，("default-idle") -> "default-idle-240x290.mjpeg"。
     * 注意 out 缓冲必须 ≥ 48 字节。 */
    static void FillClipName(char* out, size_t out_size, const char* name) {
        snprintf(out, out_size, "%s-%ux%u.mjpeg", name,
                 (unsigned)kMjpegVideoWidth, (unsigned)kMjpegVideoHeight);
    }

    /* 启动时一次扫描 user 层 3 个 + default 层唯一的 default-idle。
     * user 层有任意文件时 FindRoleAnimation 永不 fallback 到 default 层。 */
    static void ScanEmotionClips() {
        if (s_clip_cache.scanned) return;
        s_clip_cache.scanned = true;

        if (emotion_partition_storage_get_partition() == NULL) {
            esp_err_t r = emotion_partition_storage_init();
            if (r != ESP_OK) {
                ESP_LOGW(TAG, "emotion_partition_storage_init 失败: %s", esp_err_to_name(r));
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

        /* user 层 */
        const char* user_names[3] = { "idle", "listen", "speak" };
        for (int i = 0; i < 3; i++) {
            char n[48];
            FillClipName(n, sizeof(n), user_names[i]);
            ClipLoc loc = try_find(n);
            if (loc.valid()) {
                if (i == 0) s_clip_cache.idle = loc;
                else if (i == 1) s_clip_cache.listen = loc;
                else s_clip_cache.speak = loc;
            }
        }

        /* default 层：只扫 default-idle */
        char dn[48];
        FillClipName(dn, sizeof(dn), "default-idle");
        ClipLoc dloc = try_find(dn);
        if (dloc.valid()) s_clip_cache.default_clip = dloc;

        ESP_LOGI(TAG, "ClipCache: idle=%s listen=%s speak=%s default_clip=%s",
                 s_clip_cache.idle.name.c_str(), s_clip_cache.listen.name.c_str(),
                 s_clip_cache.speak.name.c_str(), s_clip_cache.default_clip.name.c_str());
    }

    /* EmotionSync 下载/重试完成后调用：重新扫描并尝试启动 idle。
     * 注意：先清空已扫描标志，让 ScanEmotionClips 真正重扫。 */
    void RescanAndTryIdle() {
        s_clip_cache.scanned = false;
        s_clip_cache = ClipCache{};  /* 清空所有 entry */
        ScanEmotionClips();
        if (!mjpeg_player_is_running() && s_clip_cache.idle.valid()) {
            SetRoleAnimation("idle");
        }
    }

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

    /* 完全走缓存。
     * 规则：
     *   - user 层有任意文件 → 只在 user 层找，不 fallback 到 default
     *   - listen → user listen → user idle → 空
     *   - speak  → user speak → user idle → 空
     *   - idle   → user idle → 空
     *   - user 层全空 → fallback 到 default_idle
     */
    static ClipLoc FindRoleAnimation(const char* state) {
        if (!s_clip_cache.scanned) {
            ScanEmotionClips();
        }
        const bool user_has_any = s_clip_cache.idle.valid()
                                  || s_clip_cache.listen.valid()
                                  || s_clip_cache.speak.valid();

        if (user_has_any) {
            /* user 层有文件：只在 user 层查找 */
            if (state && strcmp(state, "listen") == 0) {
                if (s_clip_cache.listen.valid()) return s_clip_cache.listen;
                if (s_clip_cache.idle.valid())   return s_clip_cache.idle;
                return {};
            }
            if (state && strcmp(state, "speak") == 0) {
                if (s_clip_cache.speak.valid()) return s_clip_cache.speak;
                if (s_clip_cache.idle.valid())  return s_clip_cache.idle;
                return {};
            }
            /* idle */
            return s_clip_cache.idle;
        }

        /* user 层全空：fallback 到 default_idle */
        return s_clip_cache.default_clip;
    }

    bool StartMjpegEmotion(const ClipLoc& loc_in) {
        if (!s_system_ready_) {
            ESP_LOGW(TAG, "MJPEG跳过：系统未就绪");
            return false;
        }
        if (!loc_in.valid()) {
            ESP_LOGW(TAG, "MJPEG跳过：loc 无效");
            return false;
        }
        /* 拷贝成非 const 引用，避免后面 mjpeg_player_cfg_t 写入干扰逻辑读 */
        ClipLoc loc = loc_in;

        /* 同一文件已播放中？直接跳过。 */
        if (loc.name == current_clip_name_ && mjpeg_player_is_running()) {
            ESP_LOGI(TAG, "StartMjpegEmotion: same clip %s, skip (already running)", loc.name.c_str());
            return true;
        }
        if (loc.name != current_clip_name_ && mjpeg_player_is_running()) {
            ESP_LOGI(TAG, "StartMjpegEmotion: clip change %s -> %s, stop first (async)",
                     current_clip_name_.c_str(), loc.name.c_str());
            /* 用异步 stop：仅等 read_task 退出（释放大块 PSRAM），旧 decode_task 自行退出，
             * 状态切换卡顿从 ~200ms 降到 ~50ms，与 P4 项目相同的处理方式。 */
            mjpeg_player_stop_async();
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

        mjpeg_player_cfg_t cfg = {};
        cfg.src_type = MJPEG_SRC_PARTITION;
        cfg.partition = emotion_partition_storage_get_partition();
        cfg.partition_offset = loc.offset;
        cfg.partition_size = loc.size;
        cfg.file_path = nullptr;   /* PARTITION 模式不用 */
        cfg.panel = panel_;
        cfg.fb[0] = nullptr;
        cfg.fb[1] = nullptr;
        cfg.screen_width = kMjpegVideoWidth;
        cfg.screen_height = kMjpegVideoHeight;
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
            ESP_LOGW(TAG, "MJPEG启动失败（%s）：%s", loc.name.c_str(), esp_err_to_name(ret));
            return false;
        }
        /* start 成功才更新 current_xxx，保持单一真相源。 */
        current_clip_name_ = loc.name;
        current_mjpeg_path_ = loc.name;   /* 兼容旧代码读 current_mjpeg_path_ */

        ESP_LOGI(TAG, "MJPEG表情播放（flash 分区）：%s off=0x%x size=%u",
                 loc.name.c_str(), (unsigned)loc.offset, (unsigned)loc.size);
        return true;
    }

    void StopMjpegIfRunning() {
        /* 析构 / 关闭时同步等待，确保资源完全释放。普通状态切换走 StartMjpegEmotion
         * 里的 mjpeg_player_stop_async()，避免阻塞。 */
        mjpeg_player_stop();
        current_mjpeg_path_.clear();
        current_clip_name_.clear();
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

FanLcd778928Display::ClipCache FanLcd778928Display::s_clip_cache;

#endif // FAN_LCD778928_DISPLAY_H
