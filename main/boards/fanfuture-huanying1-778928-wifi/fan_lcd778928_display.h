#ifndef FAN_LCD778928_DISPLAY_H
#define FAN_LCD778928_DISPLAY_H

#include "lcd_display.h"
#include "gif/lvgl_gif.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "assets/lang_config.h"
#include "assets.h"

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
        port_cfg.task_priority = 5;
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

    ~FanLcd778928Display() override {
        StopMjpegIfRunning();

        DisplayLockGuard lock(this);
        if (music_cover_container_) {
            lv_obj_del(music_cover_container_);
            music_cover_container_ = nullptr;
        }
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

        /* 非 speak 状态时隐藏字幕 */
        if (state == nullptr || strcmp(state, "speak") != 0) {
            if (chat_message_label_ != nullptr) {
                lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
            }
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
        } /**else {
            ESP_LOGI(TAG, "SetRoleAnimation: state=%s clip=%s name=%s (differs from %s)",
                     state ? state : "null", clip, clip_loc.name.c_str(), current_clip_name_.c_str());
        }**/
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

    void SetChatMessage(const char* role, const char* content) override {
        DisplayLockGuard lock(this);
        bool has_content = (content != nullptr && content[0] != '\0');

        if (chat_message_label_ != nullptr) {
            lv_label_set_text(chat_message_label_, content ? content : "");
            if (has_content) {
                lv_obj_remove_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (notification_label_ != nullptr) {
            if (has_content) {
                lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (status_label_ != nullptr) {
            if (has_content) {
                lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    /* 完整歌名 / 歌手 / 总时长 元数据，子控件显示在 music_cover_container_ 内，
     * 不写到 chat_message_label_（那是 AI 聊天字幕专用）。
     * song_name/singer 为 nullptr/empty 表示对应字段清空；interval < 0 表示
     * 不更新总时长（保留上次值）。 */
    void SetMusicInfo(const char* song_name,
                      const char* singer,
                      int interval) override;

    /* 播放进度 + 歌词同步：
     *  - current_ms 推进进度条与 time_label
     *  - lyric    写到 lyric_label_；nullptr/empty 清空
     * 播放线程高频调用；这里只做 lv_label_set_text / lv_bar_set_value，
     * 实现内部有"相同值不重写"的去抖逻辑以避免无谓的 lvgl 重绘。 */
    void SetMusicProgress(int current_ms, const char* lyric, const char* lyric_next = nullptr) override;

    void ShowMusicCover(bool show, const std::string& picture_url = "") override;

private:
    inline static bool s_system_ready_ = false;

    static constexpr uint16_t kMjpegVideoWidth = 240;
    static constexpr uint16_t kMjpegVideoHeight = 290;
    /* S3 软解 esp_new_jpeg：30fps × 30ms/帧 几乎打满 CPU，与 Opus 解码抢 CPU 导致 TTS 卡顿。
     * 实测降到 20fps 后每帧 50ms 预算，软解 + Opus 不再争 CPU，TTS 流畅。 */
    static constexpr uint8_t kMjpegTargetFps = 24;
    std::string current_mjpeg_path_;
    std::string current_clip_name_;  /* 当前播放的 flash 文件名（part:offset:size 表示法） */

    /* 音乐封面相关：音乐播放时覆盖屏幕除状态栏外的整片区域，背景使用编译期内置的
     * music_bg.bin 图片，专辑图叠加在背景上方 1/4 附近；下方依次显示歌词 / 歌名 /
     * 歌手 / 进度条 / 当前时间 / 总时间。
     *
     * 容器尺寸 = 音乐背景图尺寸 = 屏幕宽度 × (屏幕高度 - 状态栏高度)，
     * 水平填满整个屏幕，垂直 bottom=0 紧贴屏底（顶部留给状态栏）。 */
    lv_obj_t* music_cover_container_ = nullptr;        // 音乐封面容器（480x816 / 720x1232）
    lv_obj_t* music_lyric_label_ = nullptr;            // 当前歌词（第 1 行，放大 1.1x）
    lv_obj_t* music_lyric_next_label_ = nullptr;       // 下一句歌词（第 2 行，原色 #9d9183）
    lv_obj_t* music_song_name_label_ = nullptr;        // 歌名
    lv_obj_t* music_singer_label_ = nullptr;           // 歌手
    lv_obj_t* music_progress_bar_ = nullptr;           // 进度条
    lv_obj_t* music_cur_time_label_ = nullptr;         // 当前时间 / 剩余时间（左对齐）
    lv_obj_t* music_total_time_label_ = nullptr;       // 总时长（右对齐）

    /* SetMusicProgress 去抖：避免每帧重复 lv_label_set_text / lv_bar_set_value。
     * last_progress_bar_update_ms_ 用于节流：进度条每 500ms 才刷一次。
     * last_progress_sec_ 用于时间标签：同一秒内不重复刷新。 */
    int last_progress_ms_ = -1;
    int last_progress_sec_ = -1;
    int64_t last_progress_bar_update_ms_ = -1;
    std::string last_lyric_;
    std::string last_lyric_next_;
    int music_total_interval_sec_ = 0;              // 当前歌曲总时长（秒）

    /** 容器尺寸：宽 = 屏宽，高 = 高度，bottom=0。 */
    static constexpr uint16_t kMusicCoverWidth  = 240;   
    static constexpr uint16_t kMusicCoverHeight = 290;   
    /** 音乐封面容器的顶部 y 偏移：留给状态栏。 */
    static constexpr uint16_t kMusicCoverTopOffset = 0;

    /** 在 SetupUI() 中创建 music_cover_container_ 与所有子控件（歌名/歌手/歌词/
     * 进度条/时间），容器初始隐藏。 */
    void SetupMusicCoverUI();

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

        //ESP_LOGI(TAG, "MJPEG表情播放（flash 分区）：%s off=0x%x size=%u",
                 //loc.name.c_str(), (unsigned)loc.offset, (unsigned)loc.size);
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

        // status_bar_ 放左下角，文字左对齐
        status_bar_ = lv_obj_create(screen);
        lv_obj_set_size(status_bar_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(status_bar_, 0, 0);
        lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(status_bar_, 0, 0);
        lv_obj_set_style_pad_all(status_bar_, 0, 0);
        lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_layout(status_bar_, LV_LAYOUT_FLEX, 0);
        lv_obj_set_flex_flow(status_bar_, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(status_bar_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_align(status_bar_, LV_ALIGN_BOTTOM_LEFT, lvgl_theme->spacing(1), -ui_bottom_inset);

        notification_label_ = lv_label_create(status_bar_);
        lv_obj_set_width(notification_label_, LV_SIZE_CONTENT);
        lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
        lv_obj_set_style_bg_color(notification_label_, lvgl_theme->background_color(), 0);
        lv_label_set_text(notification_label_, "");
        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

        status_label_ = lv_label_create(status_bar_);
        lv_obj_set_width(status_label_, LV_SIZE_CONTENT);
        lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
        lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);

        // chat_message_label_ 独立创建在 screen 上，覆盖 status_bar_ 区域
        chat_message_label_ = lv_label_create(screen);
        lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(38));
        lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
        lv_obj_set_style_bg_color(chat_message_label_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(chat_message_label_, LV_OPA_COVER, 0);
        lv_label_set_text(chat_message_label_, "");
        lv_obj_align(chat_message_label_, LV_ALIGN_BOTTOM_LEFT, lvgl_theme->spacing(1), -ui_bottom_inset);
        lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);

        // Right icons（网络 | 静音 | 电量，屏幕右下角）
        lv_obj_t* right_icons = lv_obj_create(screen);
        lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(right_icons, 0, 0);
        lv_obj_set_style_pad_all(right_icons, 0, 0);
        lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_align(right_icons, LV_ALIGN_BOTTOM_RIGHT, -lvgl_theme->spacing(1), -ui_bottom_inset);

        mute_label_ = lv_label_create(right_icons);
        lv_label_set_text(mute_label_, "");
        lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);
        lv_obj_set_style_margin_left(mute_label_, lvgl_theme->spacing(1), 0);

        network_label_ = lv_label_create(right_icons);
        lv_label_set_text(network_label_, "");
        lv_obj_set_style_text_font(network_label_, icon_font, 0);
        lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

        battery_label_ = lv_label_create(right_icons);
        lv_label_set_text(battery_label_, "");
        lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);

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

        low_battery_popup_ = lv_obj_create(screen);
        lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.5, text_font->line_height * 2);
        lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
        lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
        lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
        
        low_battery_label_ = lv_label_create(low_battery_popup_);
        lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
        lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
        lv_obj_center(low_battery_label_);
        lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

        /* 音乐播放 UI：歌名 / 歌手 / 歌词 / 进度条 / 时间，统一在
         * music_cover_container_ 下，初始化时容器隐藏，由 ShowMusicCover 控制显隐。 */
        SetupMusicCoverUI();
    }
};

/* 音乐封面：音乐播放时覆盖 MJPEG ROI 区域。
 *
 * 容器内部布局（在 SetupMusicCoverUI 中创建）：
 *   - music_cover_bg_img_ : 编译期内置的默认背景图（music_bg.bin）
 *   - music_cover_img_    : 运行时下载的专辑图，叠在背景上、距顶 98 居中
 *   - music_lyric_label_  : 当前歌词（第 1 行，距底 220，1.1x，白）
 *   - music_lyric_next_label_ : 下一句歌词（第 2 行，距底 200，1x，#9d9183）
 *   - music_song_name_label_ / music_singer_label_ : 歌名 / 歌手（距底 155 / 120，左对齐）
 *   - music_progress_bar_ : 进度条（左右各 45，距底 75）
 *   - music_cur_time_label_ / music_total_time_label_ : 当前时间（左对齐）/ 总时间（右对齐）
 *
 * `show=true` 停止 MJPEG 播放并显示容器；封面图未下载完前音乐背景图仍可见。
 * `show=false` 隐藏容器并恢复 MJPEG 角色动画。
 */
void FanLcd778928Display::ShowMusicCover(bool show, const std::string& picture_url) {
    DisplayLockGuard lock(this);

    if (music_cover_container_ == nullptr) {
        /* ShowMusicCover 在 SetupUI 之前调用了——理论上不会发生；保守起见不崩溃。 */
        ESP_LOGW(TAG, "ShowMusicCover before SetupUI; ignored");
        return;
    }

    if (show) {
        /* 停止 MJPEG 播放 */
        if (mjpeg_player_is_running()) {
            mjpeg_player_stop();
        }

        /* 显示容器（背景图立即可见，专辑图随后由下载线程设置） */
        lv_obj_remove_flag(music_cover_container_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(music_cover_container_, LV_OBJ_FLAG_HIDDEN);
    
        /* 重置进度去抖与缓存歌词，避免下次播放时残留旧值 */
        last_progress_ms_ = -1;
        last_progress_sec_ = -1;
        last_progress_bar_update_ms_ = -1;
        last_lyric_.clear();
        last_lyric_next_.clear();
        /* 不主动重置 music_total_interval_sec_ —— 下一首开始播放时
         * SetMusicInfo(..., interval) 会覆盖它。 */
    }
}

/* 在 SetupUI() 末尾调用一次。创建 music_cover_container_ 与全部子控件，
 * 容器初始隐藏，由 ShowMusicCover 控制显隐。
 *
 * "文字放大 2x / 1.5x" 用 lv_obj_set_style_transform_scale_* 实现：
 *   - 设 label layout 尺寸 = 视觉尺寸 / 倍数（如 2x 时 w=195）
 *   - 应用 transform_scale_x/y = 256 * 倍数（512 / 384）
 *   - 视觉尺寸 = layout * scale，文本按原字体绘制后视觉放大
 *
 * 所有"离底 N"用 LV_ALIGN_BOTTOM_LEFT/RIGHT + (-N) 的 y_off 实现。 */
void FanLcd778928Display::SetupMusicCoverUI() {
    auto screen = lv_screen_active();

    /* 容器：宽=屏宽，高=音乐背景图高，bottom=0，初始隐藏。 */
    music_cover_container_ = lv_obj_create(screen);
    lv_obj_set_pos(music_cover_container_, 0, (lv_coord_t)kMusicCoverTopOffset);
    lv_obj_set_size(music_cover_container_,
                    (lv_coord_t)kMusicCoverWidth,
                    (lv_coord_t)kMusicCoverHeight);
    lv_obj_set_style_radius(music_cover_container_, 0, 0);
    lv_obj_set_style_bg_color(music_cover_container_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(music_cover_container_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(music_cover_container_, 0, 0);
    lv_obj_set_style_pad_all(music_cover_container_, 0, 0);
    lv_obj_move_foreground(music_cover_container_);
    lv_obj_add_flag(music_cover_container_, LV_OBJ_FLAG_HIDDEN);

    /* 通用 label 创建工具：宽 = 容器宽 - 左右 20*2，留出左右内边距。
     * 歌词允许 2 行换行；歌名 / 歌手 / 时间固定单行高度（不够滚动）。 */
    const lv_coord_t side_pad = 20;                              // 离左 / 离右
    const lv_coord_t inner_w = (lv_coord_t)kMusicCoverWidth - side_pad * 2; 

    auto make_text_label = [&](lv_obj_t* parent, lv_coord_t width, lv_coord_t height) -> lv_obj_t* {
        lv_obj_t* lbl = lv_label_create(parent);
        lv_obj_set_size(lbl, width, height);
        lv_obj_set_style_bg_opa(lbl, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        /* 单行：超出长度截断不滚动（LV_LABEL_LONG_CLIP）。 */
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_label_set_text(lbl, "");
        return lbl;
    };

    /* 歌词：左对齐，距底 195，文字 1.1x（box=355x50, scale_x/y=281, pivot 0,50）。
     * NOTE: transform_scale_y 围绕 box 下边缘放大，视觉框比 layout box 高 ~5px。 */
    {
        const int scale_num = 281;  // 1.1x = 256 * 1.1 (四舍五入)
        const lv_coord_t box_w = inner_w * 256 / scale_num;     // 390 * 256 / 281 ≈ 355
        const lv_coord_t box_h = 50;
        music_lyric_label_ = make_text_label(music_cover_container_, box_w, box_h);
        lv_obj_set_style_text_align(music_lyric_label_, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_transform_scale_x(music_lyric_label_, scale_num, 0);
        lv_obj_set_style_transform_scale_y(music_lyric_label_, scale_num, 0);
        lv_obj_set_style_transform_pivot_x(music_lyric_label_, 0, 0);     // 左边缘
        lv_obj_set_style_transform_pivot_y(music_lyric_label_, box_h, 0); // 下边缘
        lv_obj_align(music_lyric_label_, LV_ALIGN_BOTTOM_LEFT, side_pad, -195);
    }

    /* 歌词下一行（第 2 行）：左对齐，距底 205，1x 不缩放。文字色 #9d9183（淡灰）。
    * 放在 lyric（第 1 行）下面，预先占位；新一句歌词触发时才由 SetMusicProgress 写入。 */
    music_lyric_next_label_ = make_text_label(music_cover_container_, inner_w, 30);
    lv_obj_set_style_text_align(music_lyric_next_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(music_lyric_next_label_, lv_color_hex(0x9d9183), 0);
    lv_obj_align(music_lyric_next_label_, LV_ALIGN_BOTTOM_LEFT, side_pad, -190);
 

    /* 歌名：左对齐，距底 120，文字 1.2x（box=325x32, scale_x/y=307, pivot 0,32）。
     * 视觉框 = 390x38.4，视觉底 = box 底 = 681；视觉顶 = 642.6。
     * 上方 lyric 视觉底 = 601 → 间距 41.6px。 */
    {
        const int scale_num = 307;  // 1.2x = 256 * 1.2 (四舍五入)
        const lv_coord_t box_w = inner_w * 256 / scale_num;     // 390 * 256 / 307 ≈ 325
        const lv_coord_t box_h = 32;
        music_song_name_label_ = make_text_label(music_cover_container_, box_w, box_h);
        lv_obj_set_size(music_song_name_label_, box_w, box_h);
        lv_obj_set_style_text_align(music_song_name_label_, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_transform_scale_x(music_song_name_label_, scale_num, 0);
        lv_obj_set_style_transform_scale_y(music_song_name_label_, scale_num, 0);
        lv_obj_set_style_transform_pivot_x(music_song_name_label_, 0, 0);     // 左边缘
        lv_obj_set_style_transform_pivot_y(music_song_name_label_, box_h, 0); // 下边缘
        lv_obj_align(music_song_name_label_, LV_ALIGN_BOTTOM_LEFT, side_pad, -120);
    }

    /* 歌手：左对齐，距底 100，高度 30，1x 不缩放。文字色 #dfd8d0（淡米白）。 */
    music_singer_label_ = make_text_label(music_cover_container_, inner_w, 30);
    lv_obj_set_style_text_align(music_singer_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(music_singer_label_, lv_color_hex(0xdfd8d0), 0);
    lv_obj_align(music_singer_label_, LV_ALIGN_BOTTOM_LEFT, side_pad, -100);

    /* 进度条：左右各 45，距底 75，高 6。 */
    music_progress_bar_ = lv_bar_create(music_cover_container_);
    lv_obj_set_size(music_progress_bar_, inner_w, 6);
    lv_obj_align(music_progress_bar_, LV_ALIGN_BOTTOM_LEFT, side_pad, -75);
    lv_bar_set_range(music_progress_bar_, 0, 100);  // 单位：百分比；由 SetMusicProgress 写入
    lv_bar_set_value(music_progress_bar_, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(music_progress_bar_, lv_color_make(0x40, 0x40, 0x40), LV_PART_MAIN);
    lv_obj_set_style_bg_color(music_progress_bar_, lv_color_white(), LV_PART_INDICATOR);

    /* 当前时间：左对齐，距底 45，高度 22（避开进度条；bar 底在 741）。 */
    music_cur_time_label_ = make_text_label(music_cover_container_, inner_w, 22);
    lv_obj_set_style_text_align(music_cur_time_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(music_cur_time_label_, LV_ALIGN_BOTTOM_LEFT, side_pad, -45);
    lv_label_set_text(music_cur_time_label_, "00:00");

    /* 总时间：右对齐，距右 45 / 距底 45。 */
    music_total_time_label_ = lv_label_create(music_cover_container_);
    lv_obj_set_size(music_total_time_label_, inner_w / 2 + 1, 22);  // +1 防边界像素
    lv_obj_set_style_bg_opa(music_total_time_label_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(music_total_time_label_, lv_color_white(), 0);
    lv_obj_set_style_text_align(music_total_time_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(music_total_time_label_, LV_LABEL_LONG_CLIP);
    lv_obj_align(music_total_time_label_, LV_ALIGN_BOTTOM_RIGHT, -side_pad, -45);
    lv_label_set_text(music_total_time_label_, "00:00");
}

/* 全量元数据接口（歌名/歌手/总时长）。chat_message_label_ 不参与。
 *
 * 总时长变化时立即刷新总时间标签 + 按当前进度刷新百分比，避免下次 SetMusicProgress
 * 调用前进度条看起来还停在旧基线上。 */
void FanLcd778928Display::SetMusicInfo(const char* song_name,
                                     const char* singer,
                                     int interval) {
    DisplayLockGuard lock(this);
    if (music_cover_container_ == nullptr) {
        return;
    }
    /* 歌名 */
    if (music_song_name_label_) {
        if (song_name != nullptr && song_name[0] != '\0') {
            lv_label_set_text(music_song_name_label_, song_name);
        } else {
            lv_label_set_text(music_song_name_label_, "");
        }
    }
    /* 歌手 */
    if (music_singer_label_) {
        if (singer != nullptr && singer[0] != '\0') {
            lv_label_set_text(music_singer_label_, singer);
        } else {
            lv_label_set_text(music_singer_label_, "");
        }
    }
    /* 总时长：负数表示不更新（保留上次值） */
    if (interval >= 0) {
        music_total_interval_sec_ = interval;
    }
    /* 总时间标签：立刻写一次，让 mm:ss 显示同步 */
    if (music_total_time_label_) {
        int total_sec = music_total_interval_sec_;
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d", total_sec / 60, total_sec % 60);
        lv_label_set_text(music_total_time_label_, buf);
    }
    /* 进度条也要重画一次（总时长变化会让百分比基线变化） */
    if (music_progress_bar_) {
        if (music_total_interval_sec_ > 0 && last_progress_ms_ > 0) {
            int pct = (int)((long long)last_progress_ms_ * 100 /
                            (music_total_interval_sec_ * 1000));
            if (pct > 100) pct = 100;
            lv_bar_set_value(music_progress_bar_, pct, LV_ANIM_OFF);
        } else {
            lv_bar_set_value(music_progress_bar_, 0, LV_ANIM_OFF);
        }
    }
}

/* 播放进度 + 当前/下一句歌词。同一 current_ms / lyric 不会重复刷 lvgl。
 *
 * lyric 语义：
 *   - non-null：新一句歌词，写入并去抖
 *   - null    ：歌词没换，**不要动歌词 label**，避免每帧 progress tick
 *               把已显示的歌词清成 ""（用户报过的 bug）
 *
 * lyric_next 语义：
 *   - non-null：写入第 2 行（接下来一句）；去抖避免无意义重绘
 *   - null    ：不要动第 2 行（保持当前显示） */
void FanLcd778928Display::SetMusicProgress(int current_ms, const char* lyric, const char* lyric_next) {
    DisplayLockGuard lock(this);
    if (music_cover_container_ == nullptr) {
        return;
    }
    if (current_ms < 0) current_ms = 0;

    int64_t now_ms = esp_log_timestamp();
    int cur_sec = current_ms / 1000;

    /* 时间标签：同一秒内不重复刷新 */
    if (cur_sec != last_progress_sec_) {
        last_progress_sec_ = cur_sec;
        if (music_cur_time_label_) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%02d:%02d", cur_sec / 60, cur_sec % 60);
            lv_label_set_text(music_cur_time_label_, buf);
        }
    }

    /* 进度条：每 500ms 刷新一次（歌词没变时不需要更频繁） */
    if (music_progress_bar_ && now_ms - last_progress_bar_update_ms_ > 500) {
        last_progress_ms_ = current_ms;
        last_progress_bar_update_ms_ = now_ms;
        int total_sec = music_total_interval_sec_;
        int pct = 0;
        if (total_sec > 0) {
            pct = (int)((long long)current_ms * 100 / (total_sec * 1000));
            if (pct > 100) pct = 100;
        }
        lv_bar_set_value(music_progress_bar_, pct, LV_ANIM_OFF);
    }

    /* 歌词（第 1 行，当前）：仅当 lyric 非空且文字变化时更新 */
    if (music_lyric_label_ && lyric != nullptr) {
        if (last_lyric_ != lyric) {
            last_lyric_ = lyric;
            lv_label_set_text(music_lyric_label_, lyric);
        }
    }

    /* 歌词（第 2 行，接下来）：仅当 lyric_next 非空且文字变化时更新 */
    if (music_lyric_next_label_ && lyric_next != nullptr) {
        if (last_lyric_next_ != lyric_next) {
            last_lyric_next_ = lyric_next;
            lv_label_set_text(music_lyric_next_label_, lyric_next);
        }
    }
}

FanLcd778928Display::ClipCache FanLcd778928Display::s_clip_cache;

#endif // FAN_LCD778928_DISPLAY_H
