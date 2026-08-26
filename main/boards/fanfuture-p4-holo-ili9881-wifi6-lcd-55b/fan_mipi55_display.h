#ifndef FAN_MIPI55_DISPLAY_H
#define FAN_MIPI55_DISPLAY_H

#include "lcd_display.h"
#include "gif/lvgl_gif.h"
#include "lvgl_display/lvgl_image.h"
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
#include <lvgl.h>
#include <esp_psram.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <cstdio>
#include <sys/stat.h>

#include "board.h"

#define FAN_MIPI55_DISPLAY_TAG "FanMIPI55Display"

extern "C" {
#include "mjpeg_player.h"
#include "sd_scanner.h"
#include "jpg/jpeg_to_image.h"
}

// FAN MIPI 5.5寸显示器
class FanMIPI55Display : public LcdDisplay {
public:

    FanMIPI55Display(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

        // Load theme from settings
        Settings settings("display", false);
        std::string theme_name = settings.GetString("theme", "dark");
        current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

        ESP_LOGI(FAN_MIPI55_DISPLAY_TAG, "Initialize LVGL library");
        lv_init();

        ESP_LOGI(FAN_MIPI55_DISPLAY_TAG, "Initialize LVGL port");
        lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
        lvgl_port_init(&port_cfg);

        ESP_LOGI(FAN_MIPI55_DISPLAY_TAG, "Adding LCD display");
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
            ESP_LOGE(FAN_MIPI55_DISPLAY_TAG, "Failed to add display");
            return;
        }

        // 物理屏为 480x854 竖屏，无需旋转
        ESP_LOGI(FAN_MIPI55_DISPLAY_TAG, "LVGL native resolution: %dx%d",
                 (int)lv_display_get_horizontal_resolution(display_),
                 (int)lv_display_get_vertical_resolution(display_));

        if (offset_x != 0 || offset_y != 0) {
            lv_display_set_offset(display_, offset_x, offset_y);
        }

        SetupUI();
    }

    ~FanMIPI55Display() override {
        StopMjpegIfRunning();
        if (music_cover_download_thread_.joinable()) {
            music_cover_download_thread_.join();
        }
        music_cover_image_data_.reset();
        DisplayLockGuard lock(this);
        if (music_cover_container_) {
            lv_obj_del(music_cover_container_);
            music_cover_container_ = nullptr;
        }
    }

    virtual void SetEmotion(const char* emotion) override {
        /* SetEmotion 用于系统开机阶段的表情（microchip_ai / download / circle_xmark 等）
         * 以及升级提示、错误告警等系统级状态。所有这些场景都走主题 GIF / 内置图标，
         * 不再走 MJPEG 路径——MJPEG 仅由 SetRoleAnimation 接管。 */
        if (s_system_ready_) {
            //ESP_LOGW(FAN_MIPI55_DISPLAY_TAG, "No SetEmotions_，system_ready_");
            return;
        }

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
                ESP_LOGE(FAN_MIPI55_DISPLAY_TAG, "Failed to load GIF for emotion: %s", emotion);
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
     *
     * 播放优先级：
     * 1. 用户角色动画：idle-{WIDTH}x{HEIGHT}.mjpeg / listen-* / speak-*
     *    - 三个文件至少有一个存在时，启用用户角色动画
     *    - listen 缺失 → 播放 idle；speak 缺失 → 播放 idle
     * 2. 系统默认动画：default-idle-{WIDTH}x{HEIGHT}.mjpeg / default-listen-* / default-speak-*
     *    - 仅在用户角色动画完全不存在时启用
     *    - listen 缺失 → 播放 default-idle；speak 缺失 → 播放 default-idle
     * 3. 均不存在：不播放 MJPEG
     */
    virtual void SetRoleAnimation(const char* state) override {
        ESP_LOGI(FAN_MIPI55_DISPLAY_TAG, "SetRoleAnimation state=%s ready=%d sd_mounted=%d",
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
            ESP_LOGW(FAN_MIPI55_DISPLAY_TAG, "SetRoleAnimation skipped: system not ready or SD not mounted");
            return;
        }

        const char* clip = MapRoleStateToClip(state);
        const std::string& clip_path = FindRoleAnimation(clip);
        if (clip_path.empty()) {
            ESP_LOGI(FAN_MIPI55_DISPLAY_TAG, "SetRoleAnimation: no available MJPEG for state=%s, skip", clip);
            return;
        }
        StartMjpegEmotion(clip_path.c_str());
    }

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

    /** 进入待命状态 系统就绪 */
    void SetSystemReady() override {
        s_system_ready_ = true;
        ESP_LOGI(FAN_MIPI55_DISPLAY_TAG, "MJPEG ready: system is ready, SD card operations permitted");
        /* 第一次进入 idle 时 s_system_ready_ 还没置位（SetSystemReady 在 SetDeviceState(idle) 之后调用），
         * SetRoleAnimation("idle") 会被上面的 ready 检查跳过，导致第一帧 idle 没启动。
         * 这里在就绪后再尝试一次，让待机的表情真正起得来。 */
        if (!mjpeg_player_is_running() && sd_scanner_is_mounted()) {
            SetRoleAnimation("idle");
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

    /* 音乐封面相关：音乐播放时覆盖屏幕除状态栏外的整片区域，背景使用编译期内置的
     * musicbg-720x1232.bin 图片，专辑图叠加在背景上方 1/4 附近；下方依次显示歌词 / 歌名 /
     * 歌手 / 进度条 / 当前时间 / 总时间。
     *
     * 容器尺寸 = 音乐背景图尺寸 = 屏幕宽度 × (屏幕高度 - 状态栏高度)，
     * 水平填满整个屏幕，垂直 bottom=0 紧贴屏底（顶部留给状态栏）。 */
    lv_obj_t* music_cover_container_ = nullptr;        // 音乐封面容器（720x1232）
    lv_obj_t* music_cover_bg_img_ = nullptr;           // 背景图（musicbg-720x1232.bin）
    lv_obj_t* music_cover_img_ = nullptr;              // 专辑图（HTTP 下载）
    lv_obj_t* music_lyric_label_ = nullptr;            // 当前歌词（第 1 行）
    lv_obj_t* music_lyric_next_label_ = nullptr;       // 下一句歌词（第 2 行，#9d9183）
    lv_obj_t* music_song_name_label_ = nullptr;        // 歌名
    lv_obj_t* music_singer_label_ = nullptr;           // 歌手
    lv_obj_t* music_progress_bar_ = nullptr;           // 进度条
    lv_obj_t* music_cur_time_label_ = nullptr;         // 当前时间 / 剩余时间（左对齐）
    lv_obj_t* music_total_time_label_ = nullptr;       // 总时长（右对齐）
    /* musicbg-720x1232.bin 持有者。背景图 bin 由 LoadMusicBackgroundImage() 从 SD 卡
     * /sdcard/Music/musicbg-720x1232.bin 加载到 PSRAM heap，包成 LvglCBinImage。
     * shared_ptr 的自定义删除器在销毁时同时 free 像素 buffer。 */
    std::shared_ptr<LvglCBinImage> music_bg_image_;
    std::unique_ptr<LvglAllocatedImage> music_cover_image_data_;
    std::string current_music_picture_url_;
    std::thread music_cover_download_thread_;
    /* 下载线程只负责 HTTP → raw JPEG buffer，解码线程负责 jpeg_to_image。
     * raw_jpeg_cover_data_ 保护 download 和 decode 之间的交接：download 持有 unique_ptr，
     * decode thread 取走所有权后 unique_ptr 置空、decode 线程自己负责 free。 */
    std::unique_ptr<uint8_t[]> raw_jpeg_cover_data_;
    size_t raw_jpeg_cover_size_ = 0;
    std::mutex raw_jpeg_cover_mutex_;
    int32_t music_cover_scale_ = 256;               // 缩放因子 (256 = 1.0x)
    /* SetMusicProgress 去抖：避免每帧重复 lv_label_set_text / lv_bar_set_value。
     * last_progress_bar_update_ms_ 用于节流：进度条每 500ms 才刷一次。
     * last_progress_sec_ 用于时间标签：同一秒内不重复刷新。 */
    int last_progress_ms_ = -1;
    int last_progress_sec_ = -1;
    int64_t last_progress_bar_update_ms_ = -1;
    std::string last_lyric_;
    std::string last_lyric_next_;
    int music_total_interval_sec_ = 0;              // 当前歌曲总时长（秒）
    int roi_x_ = 0;                                 // MJPEG ROI x 偏移（保留备用）
    int roi_y_ = 0;                                 // MJPEG ROI y 偏移（保留备用）

    /** 容器尺寸：宽 = 屏宽，高 = musicbg-720x1232.bin 高度，bottom=0。 */
    static constexpr uint16_t kMusicCoverWidth  = 720;    // 55B：屏宽 720
    static constexpr uint16_t kMusicCoverHeight = 1232;   // 55B：music_bg-720x1232
    /** 音乐封面容器的顶部 y 偏移：留给状态栏。
     * 55B 屏高 1280 - bg 高 1232 = 48。 */
    static constexpr uint16_t kMusicCoverTopOffset = 48;

    /** 加载 /sdcard/Music/musicbg-720x1232.bin（开发者手动放到 SD 卡的 /Music 目录）到 music_bg_image_。
     * 必须在 SetupUI() 之后、ShowMusicCover(true) 调用前调用一次。
     * 失败则保持黑色背景。 */
    void LoadMusicBackgroundImage();

    /** 在 SetupUI() 中创建 music_cover_container_ 与所有子控件（歌名/歌手/歌词/
     * 进度条/时间），容器初始隐藏。 */
    void SetupMusicCoverUI();

    static bool FileExists(const std::string& path) {
        struct stat st = {};
        return stat(path.c_str(), &st) == 0;
    }

    /**
     * 查找角色动画 MJPEG 路径，两级回退：
     * 1. 用户角色动画 {prefix}{state}-{W}x{H}.mjpeg（prefix=""）
     * 2. 系统默认动画 default-{prefix}{state}-{W}x{H}.mjpeg（prefix="default-"）
     *
     * listen → 缺失时回退 idle；speak → 缺失时回退 idle。
     * 查找角色动画 MJPEG 文件。
     * 查找顺序：先在用户层（无前缀）找所有状态（orig -> idle 回退），找不到才去 default 层。
     * 均不存在返回空字符串。
     */
    static std::string FindRoleAnimation(const char* state) {
        // 先尝试在用户层（无前缀）查找
        const char* clips[2] = { state, "idle" };
        for (int i = 0; i < 2; i++) {
            char path[128];
            snprintf(path, sizeof(path), "/sdcard/Emotion/%s-%ux%u.mjpeg", clips[i],
                     (unsigned)kMjpegVideoWidth, (unsigned)kMjpegVideoHeight);
            if (FileExists(path)) {
                ESP_LOGI(FAN_MIPI55_DISPLAY_TAG, "FindRoleAnimation: found %s", path);
                return path;
            }
        }
        
        // 用户层找不到，检查是否有任何用户角色动画存在
        bool user_has_any = false;
        const char* all_clips[3] = { "idle", "listen", "speak" };
        for (int i = 0; i < 3; i++) {
            char test_path[128];
            snprintf(test_path, sizeof(test_path), "/sdcard/Emotion/%s-%ux%u.mjpeg",
                     all_clips[i], (unsigned)kMjpegVideoWidth, (unsigned)kMjpegVideoHeight);
            if (FileExists(test_path)) {
                user_has_any = true;
                break;
            }
        }
        
        // 如果用户层有角色动画但不包含请求的 state/idle，不再降级到 default 层
        if (user_has_any) {
            ESP_LOGW(FAN_MIPI55_DISPLAY_TAG, "FindRoleAnimation: user has role animation but no %s, skip", state);
            return "";
        }
        
        // 用户层完全没有角色动画，尝试 default 层
        for (int i = 0; i < 2; i++) {
            char path[128];
            snprintf(path, sizeof(path), "/sdcard/Emotion/default-%s-%ux%u.mjpeg", clips[i],
                     (unsigned)kMjpegVideoWidth, (unsigned)kMjpegVideoHeight);
            if (FileExists(path)) {
                ESP_LOGI(FAN_MIPI55_DISPLAY_TAG, "FindRoleAnimation: found default %s", path);
                return path;
            }
        }
        
        ESP_LOGW(FAN_MIPI55_DISPLAY_TAG, "FindRoleAnimation: no animation for state=%s", state);
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

    bool StartMjpegEmotion(const char* full_path) {
        if (!s_system_ready_) {
            ESP_LOGW(FAN_MIPI55_DISPLAY_TAG, "MJPEG跳过：系统未就绪");
            return false;
        }
        if (!sd_scanner_is_mounted()) {
            ESP_LOGW(FAN_MIPI55_DISPLAY_TAG, "MJPEG跳过：SD卡未挂载");
            return false;
        }
        if (full_path == nullptr || full_path[0] == '\0') {
            ESP_LOGW(FAN_MIPI55_DISPLAY_TAG, "MJPEG跳过：路径为空");
            return false;
        }

        if (mjpeg_player_is_running()) {
            if (strcmp(full_path, current_mjpeg_path_.c_str()) == 0) {
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
        roi_x_ = rx;
        roi_y_ = ry;

        current_mjpeg_path_ = full_path;
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
            ESP_LOGW(FAN_MIPI55_DISPLAY_TAG, "MJPEG启动失败(%s): %s", current_mjpeg_path_.c_str(), esp_err_to_name(ret));
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
            ESP_LOGW(FAN_MIPI55_DISPLAY_TAG, "SetupUI() called multiple times, skipping duplicate call");
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
        /* 根因修复：container 设不透明黑色背景。
         * 之前 LV_OPA_TRANSP 让旧文字像素直接画到 MJPEG 视频帧上,
         * 文字内容变化时只有被新字符覆盖的区域重绘,旧字符残留。
         * 不透明背景保证 container 每帧重绘时整个矩形区域被清屏,旧字符全部消失。
         * 视觉效果:字幕区是窄黑条,字浮在黑条上(类似卡拉OK字幕)。 */
        lv_obj_set_style_bg_color(chat_message_label_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(chat_message_label_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(chat_message_label_, 0, 0);
        lv_obj_set_style_pad_all(chat_message_label_, 0, 0);
        /* LV_ALIGN_LEFT_MID: 左边缘对齐到屏左 x=0，垂直居中。 */
        lv_obj_align(chat_message_label_, LV_ALIGN_LEFT_MID, 0, 0);

        chat_message_inner_label_ = lv_label_create(chat_message_label_);
        lv_label_set_text(chat_message_inner_label_, "");
        /* 宽 30 (字宽 = font_puhui_basic_30_4 字宽 20)，高度跟随内容。 */
        lv_obj_set_width(chat_message_inner_label_, 30);
        /* inner label 透明即可 — 外层 container 已经是不透明背景负责清屏。 */
        lv_obj_set_style_bg_opa(chat_message_inner_label_, LV_OPA_TRANSP, 0);
        lv_label_set_long_mode(chat_message_inner_label_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(chat_message_inner_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(chat_message_inner_label_, lvgl_theme->text_color(), 0);

        low_battery_popup_ = lv_obj_create(screen);
        lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.5, text_font->line_height * 2);
        lv_obj_align(low_battery_popup_, LV_ALIGN_TOP_MID, 0, -lvgl_theme->spacing(4));
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

        /* 加载 /sdcard/Music/musicbg-720x1232.bin 的默认音乐背景图。失败也不致命：容器仍能
         * 显示纯黑背景 + 专辑图。 */
        LoadMusicBackgroundImage();
    }
};

/* 纵向字幕：把 content 按 UTF-8 字符强制拆行（每字符一行），写到 chat_message_inner_label_。
 * container chat_message_label_ (24×(height-300)) 设 LV_DIR_VER scroll + clip，超长文本
 * 通过 lv_anim 把 scroll_y 从 0 一次性动画到 content_h - container_h，到底后停在末尾。 */
inline void FanMIPI55Display::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(FAN_MIPI55_DISPLAY_TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!", role, content);
    }
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr || chat_message_inner_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(FAN_MIPI55_DISPLAY_TAG, "SetChatMessage('%s', '%s') failed: chat container not ready", role, content);
        }
        return;
    }
    if (content == nullptr || content[0] == '\0') {
        lv_label_set_text(chat_message_inner_label_, "");
        static lv_anim_t s_scroll_anim;
        lv_anim_delete(&s_scroll_anim, nullptr);
        lv_obj_scroll_to_y(chat_message_label_, 0, LV_ANIM_OFF);
        last_chat_content_.clear();
        if (bottom_bar_ != nullptr) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (last_chat_content_ == content) {
        if (bottom_bar_ != nullptr && !hide_subtitle_) {
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    last_chat_content_ = content;

    {
        static lv_anim_t s_scroll_anim;
        lv_anim_delete(&s_scroll_anim, nullptr);
    }
    lv_obj_remove_flag(chat_message_inner_label_, LV_OBJ_FLAG_HIDDEN);

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

    /* 恢复显示 inner_label */
    lv_obj_remove_flag(chat_message_inner_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(chat_message_inner_label_);

    if (content_h <= container_h) {
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
    /* 总时长根据像素距离线性计算,20px/s 的速度。
     * 之前 400ms/字符导致单次滚动最多 60s,期间每帧都触发 layout。
     * 用像素距离算时长使得时长正比于实际滚动距离,减少动画总帧数。 */
    uint32_t duration_ms = (uint32_t)(scroll_range_px * 1000 / 20);  // 20px/s
    if (duration_ms < 2000) duration_ms = 2000;
    if (duration_ms > 30000) duration_ms = 30000;

    /* 先归零：若之前在滚动,确保从 0 开始。 */
    lv_obj_scroll_to_y(chat_message_label_, 0, LV_ANIM_OFF);

    /* exec_cb: 每帧直接把 v 写到 container 的 scroll_y（线性、单次、动画到底后停）。
     * 用 lv_obj_scroll_to_y(..., LV_ANIM_OFF) 替代带动画版本 — 每帧直接跳到 v 位置，
     * 不启动新动画，CPU 开销小一个数量级。 */
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
 * 但 chat_message_label_ 实际上是 container obj，重写后改清 inner label。
 * 进入 idle 状态时调用此函数，确保旧字幕完全清除不留残影。 */
inline void FanMIPI55Display::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (chat_message_inner_label_ != nullptr) {
        lv_label_set_text(chat_message_inner_label_, "");
        lv_obj_update_layout(chat_message_inner_label_);
    }
    if (chat_message_label_ != nullptr) {
        static lv_anim_t s_scroll_anim;
        lv_anim_delete(&s_scroll_anim, nullptr);
        lv_obj_scroll_to_y(chat_message_label_, 0, LV_ANIM_OFF);
    }
    last_chat_content_.clear();
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    /* 强制立即 flush 一次 — MJPEG 持锁期间 LVGL 常规 timer 任务被饿死,
     * 单纯改 LVGL 内部对象状态不会立即画到 LCD,需要在这里主动 push 一帧。 */
    lv_refr_now(nullptr);
}

/* 音乐封面：音乐播放时覆盖 MJPEG ROI 区域。
 *
 * 容器内部布局（在 SetupMusicCoverUI 中创建）：
 *   - music_cover_bg_img_ : 从 SD 卡 /sdcard/Music/musicbg-720x1232.bin 加载的默认背景图
 *   - music_cover_img_    : 运行时下载的专辑图 486x486，叠在背景上、距顶 224 居中
 *   - music_lyric_label_  : 歌词第 1 行（离左 80、距底 380，1.1x，左对齐）
 *   - music_lyric_next_label_ : 歌词第 2 行（离左 80、距底 355，1x，左对齐，淡灰 #9d9183）
 *   - music_song_name_label_ / music_singer_label_ : 歌名（距底 250，1.2x）/ 歌手（距底 210，淡米白 #dfd8d0，左对齐）
 *   - music_progress_bar_ : 进度条（左右各 80，距底 140）
 *   - music_cur_time_label_ / music_total_time_label_ : 剩余时间（左对齐）/ 总时间（右对齐，距底 100）
 *
 * `show=true` 停止 MJPEG 播放并显示容器；封面图未下载完前音乐背景图仍可见。
 * `show=false` 隐藏容器并恢复 MJPEG 角色动画。
 */
void FanMIPI55Display::ShowMusicCover(bool show, const std::string& picture_url) {
    DisplayLockGuard lock(this);

    if (music_cover_container_ == nullptr) {
        ESP_LOGW(FAN_MIPI55_DISPLAY_TAG, "ShowMusicCover before SetupUI; ignored");
        return;
    }

    if (show) {
        if (mjpeg_player_is_running()) {
            mjpeg_player_stop();
        }

        lv_obj_remove_flag(music_cover_container_, LV_OBJ_FLAG_HIDDEN);
        /* 强制置顶，避免 Setup 后新创建的控件（如 toast/通知/状态栏）盖在时间区上面。 */
        lv_obj_move_foreground(music_cover_container_);

        if (!picture_url.empty() && picture_url != current_music_picture_url_) {
            current_music_picture_url_ = picture_url;
            std::string url_copy = picture_url;

            if (music_cover_download_thread_.joinable()) {
                music_cover_download_thread_.join();
            }
            music_cover_download_thread_ = std::thread([this, url_copy]() {
                auto network = Board::GetInstance().GetNetwork();
                auto http = network->CreateHttp(5);
                if (!http->Open("GET", url_copy)) {
                    ESP_LOGE(FAN_MIPI55_DISPLAY_TAG, "Failed to open picture URL for music cover");
                    return;
                }
                int status = http->GetStatusCode();
                if (status != 200) {
                    ESP_LOGE(FAN_MIPI55_DISPLAY_TAG, "Music cover HTTP status: %d", status);
                    http->Close();
                    return;
                }
                size_t len = http->GetBodyLength();
                if (len == 0) {
                    ESP_LOGW(FAN_MIPI55_DISPLAY_TAG, "Music cover empty response (0 bytes)");
                    http->Close();
                    return;
                }
                uint8_t* data = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_8BIT);
                if (!data) {
                    ESP_LOGE(FAN_MIPI55_DISPLAY_TAG, "OOM for music cover picture (%d bytes)", len);
                    http->Close();
                    return;
                }
                size_t total = 0;
                while (total < len) {
                    int r = http->Read((char*)data + total, len - total);
                    if (r <= 0) break;
                    total += r;
                }
                http->Close();

                ESP_LOGI(FAN_MIPI55_DISPLAY_TAG, "Music cover downloaded: %d bytes, spawning decode thread...", len);

                if (len < 3 || data[0] != 0xFF || data[1] != 0xD8 || data[2] != 0xFF) {
                    heap_caps_free(data);
                    return;
                }

                /* 把 raw JPEG 移交给 decode 线程，然后立即退出本线程。
                 * download 线程不再阻塞在 decode 上，play 线程 / AFE 不会被打断。 */
                {
                    std::lock_guard<std::mutex> lock(raw_jpeg_cover_mutex_);
                    raw_jpeg_cover_data_.reset(data);
                    raw_jpeg_cover_size_ = len;
                }

                std::thread decode_thread([this]() {
                    uint8_t* data_to_decode = nullptr;
                    size_t data_len = 0;
                    {
                        std::lock_guard<std::mutex> lock(raw_jpeg_cover_mutex_);
                        if (raw_jpeg_cover_data_) {
                            data_to_decode = raw_jpeg_cover_data_.release();
                            data_len = raw_jpeg_cover_size_;
                            raw_jpeg_cover_size_ = 0;
                        }
                    }

                    if (!data_to_decode) {
                        ESP_LOGW(FAN_MIPI55_DISPLAY_TAG, "No JPEG data to decode");
                        return;
                    }

                    uint8_t* decoded = nullptr;
                    size_t decoded_len = 0, img_w = 0, img_h = 0, stride = 0;
                    esp_err_t ret = jpeg_to_image(data_to_decode, data_len, &decoded, &decoded_len, &img_w, &img_h, &stride);
                    heap_caps_free(data_to_decode);

                    if (ret == ESP_OK && decoded && img_w > 0 && img_h > 0) {
                        ESP_LOGI(FAN_MIPI55_DISPLAY_TAG, "JPEG decoded: %ux%u", img_w, img_h);

                        DisplayLockGuard lock_inner(this);

                        music_cover_image_data_.reset();

                        try {
                            music_cover_image_data_ = std::make_unique<LvglAllocatedImage>(
                                decoded, decoded_len, img_w, img_h, stride, LV_COLOR_FORMAT_RGB565);

                            if (music_cover_img_ && music_cover_container_) {
                                lv_img_set_src(music_cover_img_, music_cover_image_data_->image_dsc());
                                /* 计算缩放因子，让图片等比填满 486x486 框（55B 实际显示尺寸）。
                                 * 取 width/height 缩放比的较小者（contain），保证图片完全可见，
                                 * lv_image_set_inner_align(LV_IMAGE_ALIGN_COVER) 让其填满并裁剪超出。 */
                                const int32_t cover_box = 486;
                                int32_t scale_w = 256 * cover_box / (int32_t)img_w;
                                int32_t scale_h = 256 * cover_box / (int32_t)img_h;
                                music_cover_scale_ = (scale_w < scale_h) ? scale_w : scale_h;
                                if (music_cover_scale_ < 32) music_cover_scale_ = 32;
                                if (music_cover_scale_ > 256) music_cover_scale_ = 256;
                                lv_image_set_scale(music_cover_img_, music_cover_scale_);
                                lv_obj_remove_flag(music_cover_img_, LV_OBJ_FLAG_HIDDEN);
                            }
                        } catch (const std::exception& e) {
                            ESP_LOGE(FAN_MIPI55_DISPLAY_TAG, "Music cover decode failed: %s", e.what());
                            heap_caps_free(decoded);
                        }
                    } else {
                        ESP_LOGE(FAN_MIPI55_DISPLAY_TAG, "JPEG decode failed: %s", esp_err_to_name(ret));
                        heap_caps_free(decoded);  // 释放 jpeg_to_image 分配的缓冲区
                    }
                });
                decode_thread.detach();
            });
        } else if (!picture_url.empty() && music_cover_image_data_) {
            if (music_cover_img_) {
                lv_img_set_src(music_cover_img_, music_cover_image_data_->image_dsc());
                lv_image_set_scale(music_cover_img_, music_cover_scale_);
                lv_obj_remove_flag(music_cover_img_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else {
        lv_obj_add_flag(music_cover_container_, LV_OBJ_FLAG_HIDDEN);
        if (music_cover_download_thread_.joinable()) {
            music_cover_download_thread_.join();
        }
        current_music_picture_url_.clear();
        music_cover_image_data_.reset();
        if (music_cover_img_) {
            lv_img_set_src(music_cover_img_, (const void*)nullptr);
            lv_obj_add_flag(music_cover_img_, LV_OBJ_FLAG_HIDDEN);
        }
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
 * 布局（针对 5.5" ILI9881 LCD 720x1280 + music_bg 720x1232）：
 *
 *   容器坐标系（container coord），container 本身在屏幕 y=48..1280：
 *
 *   ┌─────────────────────────────────┐ y=0    (= screen y=48)
 *   │ bg image (720x1232, 填满容器)   │
 *   │                                 │
 *   │   ┌──────────────┐              │ y=140  cover top
 *   │   │  cover_img   │ 645x645 居中 │
 *   │   │   x=37       │              │
 *   │   └──────────────┘              │ y=785  cover bottom
 *   │                                 │
 *   │  lyric 第1行 1.1x  bottom=380   │ y=797..852  visual (h=55, box h=50)
 *   │  lyricNext 第2行 1x bottom=340  │ y=862..892  (#9d9183, h=30)
 *   │  song 1.2x  bottom=250          │ y=912..950  visual (h=38, box h=32)
 *   │  singer 1x #dfd8d0 bottom=210   │ y=950..980  (h=30)
 *   │  ─────────────────────────────  │ y=1086 bar bottom    (h=6, bottom=140)
 *   │  cur_time  左对齐 bottom=100    │ y=1110 top, y=1132 bottom (h=22)
 *   │            total_time 右对齐    │
 *   └─────────────────────────────────┘ y=1232 (= screen y=1280)
 *
 * 所有"离底 N"用 LV_ALIGN_BOTTOM_LEFT/RIGHT + (-N) 的 y_off 实现。 */
void FanMIPI55Display::SetupMusicCoverUI() {
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

    /* 背景图：1:1 填满容器，src 在 LoadMusicBackgroundImage() 中设置。 */
    music_cover_bg_img_ = lv_img_create(music_cover_container_);
    lv_obj_set_pos(music_cover_bg_img_, 0, 0);
    lv_obj_set_size(music_cover_bg_img_,
                    (lv_coord_t)kMusicCoverWidth,
                    (lv_coord_t)kMusicCoverHeight);
    lv_obj_set_style_bg_opa(music_cover_bg_img_, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(music_cover_bg_img_, LV_OBJ_FLAG_HIDDEN);

    /* 专辑图：486x486，水平居中（(720-486)/2 = 117），距容器顶 224。
     * 项目使用 LVGL 9，lv_image_set_inner_align 让源图在 obj 内居中显示。 */
    {
        const lv_coord_t cover_w = 486;
        const lv_coord_t cover_h = 486;
        const lv_coord_t cover_x = ((lv_coord_t)kMusicCoverWidth - cover_w) / 2;   // 117
        const lv_coord_t cover_y = 224;
        music_cover_img_ = lv_img_create(music_cover_container_);
        lv_obj_set_pos(music_cover_img_, cover_x, cover_y);
        lv_obj_set_size(music_cover_img_, cover_w, cover_h);
        lv_image_set_inner_align(music_cover_img_, LV_IMAGE_ALIGN_COVER);
    }
    lv_obj_add_flag(music_cover_img_, LV_OBJ_FLAG_HIDDEN);
    music_cover_scale_ = 256;

    /* 通用 label 创建工具：宽 = 容器宽 - 左右 110*2 = 500。
     * 歌词允许 2 行换行；歌名 / 歌手 / 时间固定单行高度。 */
    const lv_coord_t side_pad = 110;
    const lv_coord_t inner_w = (lv_coord_t)kMusicCoverWidth - side_pad * 2;  // 720 - 220 = 500

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

    /* 歌词（第 1 行，当前唱的）：左对齐，离左 110、距底 380，文字 1.1x（box≈455x50, scale_x/y=281, pivot 0,50）。
     * 视觉框 = 500x55，视觉底 = box 底 = 852；视觉顶 = 797。 */
    {
        const int scale_num = 281;  // 1.1x = 256 * 1.1 (四舍五入)
        const lv_coord_t box_w = inner_w * 256 / scale_num;     // 500 * 256 / 281 ≈ 455
        const lv_coord_t box_h = 50;
        music_lyric_label_ = make_text_label(music_cover_container_, box_w, box_h);
        lv_obj_set_style_text_align(music_lyric_label_, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_transform_scale_x(music_lyric_label_, scale_num, 0);
        lv_obj_set_style_transform_scale_y(music_lyric_label_, scale_num, 0);
        lv_obj_set_style_transform_pivot_x(music_lyric_label_, 0, 0);     // 左边缘
        lv_obj_set_style_transform_pivot_y(music_lyric_label_, box_h, 0); // 下边缘
        lv_obj_align(music_lyric_label_, LV_ALIGN_BOTTOM_LEFT, side_pad, -380);
    }

    /* 歌词下一行（第 2 行）：左对齐，离左 110、距底 355，1x。文字色 #9d9183（淡灰）。
     * 放在 lyric（第 1 行）下面，预先占位；新一句歌词触发时才由 SetMusicProgress 写入。 */
     music_lyric_next_label_ = make_text_label(music_cover_container_, inner_w, 30);
     lv_obj_set_style_text_align(music_lyric_next_label_, LV_TEXT_ALIGN_LEFT, 0);
     lv_obj_set_style_text_color(music_lyric_next_label_, lv_color_hex(0x9d9183), 0);
     lv_obj_align(music_lyric_next_label_, LV_ALIGN_BOTTOM_LEFT, side_pad, -355);

    /* 歌名：左对齐，距底 250，文字 1.2x（box≈417x32, scale_x/y=307, pivot 0,32）。
     * 视觉框 = 500x38.4，视觉底 = box 底 = 950；视觉顶 = 911.6。 */
    {
        const int scale_num = 307;  // 1.2x = 256 * 1.2 (四舍五入)
        const lv_coord_t box_w = inner_w * 256 / scale_num;     // 500 * 256 / 307 ≈ 417
        const lv_coord_t box_h = 32;
        music_song_name_label_ = make_text_label(music_cover_container_, box_w, box_h);
        lv_obj_set_size(music_song_name_label_, box_w, box_h);
        lv_obj_set_style_text_align(music_song_name_label_, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_transform_scale_x(music_song_name_label_, scale_num, 0);
        lv_obj_set_style_transform_scale_y(music_song_name_label_, scale_num, 0);
        lv_obj_set_style_transform_pivot_x(music_song_name_label_, 0, 0);     // 左边缘
        lv_obj_set_style_transform_pivot_y(music_song_name_label_, box_h, 0); // 下边缘
        lv_obj_align(music_song_name_label_, LV_ALIGN_BOTTOM_LEFT, side_pad, -250);
    }

    /* 歌手：左对齐，距底 210，高度 30，1x 不缩放。文字色 #dfd8d0（淡米白）。 */
    music_singer_label_ = make_text_label(music_cover_container_, inner_w, 30);
    lv_obj_set_style_text_align(music_singer_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(music_singer_label_, lv_color_hex(0xdfd8d0), 0);
    lv_obj_align(music_singer_label_, LV_ALIGN_BOTTOM_LEFT, side_pad, -210);

    /* 进度条：左右各 110，距底 145，高 6。 */
    music_progress_bar_ = lv_bar_create(music_cover_container_);
    lv_obj_set_size(music_progress_bar_, inner_w, 6);
    lv_obj_align(music_progress_bar_, LV_ALIGN_BOTTOM_LEFT, side_pad, -145);
    lv_bar_set_range(music_progress_bar_, 0, 100);
    lv_bar_set_value(music_progress_bar_, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(music_progress_bar_, lv_color_hex(0x9d9183), LV_PART_MAIN);
    lv_obj_set_style_bg_color(music_progress_bar_, lv_color_white(), LV_PART_INDICATOR);

    /* 当前时间：左对齐，距底 100，高度 30（避开进度条；22 太小时单行字下半被 obj mask 裁切）。 */
    music_cur_time_label_ = make_text_label(music_cover_container_, inner_w / 2 + 1, 30);
    lv_obj_set_style_text_align(music_cur_time_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(music_cur_time_label_, LV_ALIGN_BOTTOM_LEFT, side_pad, -100);
    lv_label_set_text(music_cur_time_label_, "00:00");

    /* 总时间：右对齐，距右 110 / 距底 100。高度 30 避免行高大于 box 时半截显示。 */
    music_total_time_label_ = lv_label_create(music_cover_container_);
    lv_obj_set_size(music_total_time_label_, inner_w / 2 + 1, 30);
    lv_obj_set_style_bg_opa(music_total_time_label_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(music_total_time_label_, lv_color_white(), 0);
    lv_obj_set_style_text_align(music_total_time_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(music_total_time_label_, LV_LABEL_LONG_CLIP);
    lv_obj_align(music_total_time_label_, LV_ALIGN_BOTTOM_RIGHT, -side_pad, -100);
    lv_label_set_text(music_total_time_label_, "00:00");
}

/* 从 SD 卡加载默认音乐背景图（/sdcard/Music/musicbg-720x1232.bin，由
 * scripts/build_music_bg.py 离线生成，开发者手动拷贝到 SD 卡的 /Music/ 子目录）。
 *
 * 为什么不放进 assets.bin：
 * 1. assets.bin 走 spi_flash_mmap，把整张图 mmapped 到地址空间，720x1232x2 ≈ 1.77MB
 *    内存在 PSRAM 紧张时容易爆破；mp3 解码 + 屏幕帧缓冲 + lvgl 缓存已经把 PSRAM 啃
 *    大半，再多 1.7MB 静态映射几乎一定会触发 OOM。
 * 2. 音乐背景是装饰性资源，不需要长期驻留，按需 fopen/fread 到内部 SRAM / PSRAM
 *    heap（即用即释放），用完 ~3 秒后 LVGL 解码缓存占的内存可被回收。
 *
 * 加载流程：
 *   fopen("/sdcard/Music/musicbg-720x1232.bin") → fseek 到末尾取 size → malloc(PSRAM) →
 *   fread 完整 bin → fclose → LvglCBinImage(ptr) 顶层负责 free。 */
#if HAVE_LVGL
void FanMIPI55Display::LoadMusicBackgroundImage() {
    if (music_cover_bg_img_ == nullptr) {
        return;
    }
    if (!sd_scanner_is_mounted()) {
        ESP_LOGW(FAN_MIPI55_DISPLAY_TAG,
                 "SD card not mounted; music cover will use plain black background");
        return;
    }

    const char* path = "/sdcard/Music/musicbg-720x1232.bin";
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        ESP_LOGW(FAN_MIPI55_DISPLAY_TAG,
                 "musicbg-720x1232.bin not found on SD card (%s); "
                 "music cover will use plain black background", path);
        return;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        ESP_LOGE(FAN_MIPI55_DISPLAY_TAG, "fseek(end) failed on %s", path);
        fclose(f);
        return;
    }
    long size = ftell(f);
    if (size <= 0 || size > (long)(1024 * 1024 * 2)) {
        ESP_LOGE(FAN_MIPI55_DISPLAY_TAG,
                 "musicbg-720x1232.bin size %ld looks invalid (must be 0 < size <= 2MB)", size);
        fclose(f);
        return;
    }
    rewind(f);

    /* 优先放 PSRAM（720x1232 RGB565 ≈ 1.77MB）。fallback 到内部 SRAM。 */
    uint8_t* buf = (uint8_t*)heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM);
    if (buf == nullptr) {
        buf = (uint8_t*)heap_caps_malloc((size_t)size, MALLOC_CAP_INTERNAL);
    }
    if (buf == nullptr) {
        ESP_LOGE(FAN_MIPI55_DISPLAY_TAG,
                 "Failed to allocate %ld bytes for musicbg-720x1232.bin", size);
        fclose(f);
        return;
    }

    size_t read_bytes = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (read_bytes != (size_t)size) {
        ESP_LOGE(FAN_MIPI55_DISPLAY_TAG,
                 "Short read on musicbg-720x1232.bin: got %u of %ld bytes",
                 read_bytes, size);
        heap_caps_free(buf);
        return;
    }

    /* 把 buf 交给 LvglCBinImage。注意：cbin_img_dsc_create 内部只是把
     * img_dsc->data 重新指向像素起始点（不分配新内存），析构时 free 的是
     * 它自己 heap-alloc 的 dsc，外层 buf 由 LvglCBinImage 的析构外
     * 由我们自己管理 —— 这里我们直接转交 buf 的所有权给 LvglCBinImage。 */
    struct CbinWithBuffer {
        uint8_t* buf;
        size_t size;
    };
    auto wrapper = std::make_shared<CbinWithBuffer>();
    wrapper->buf = buf;
    wrapper->size = (size_t)size;

    auto* cbin_image = new LvglCBinImage(buf);
    if (cbin_image->image_dsc() == nullptr) {
        ESP_LOGE(FAN_MIPI55_DISPLAY_TAG,
                 "Failed to construct LvglCBinImage from musicbg-720x1232.bin");
        delete cbin_image;
        heap_caps_free(buf);
        return;
    }

    /* 用 shared_ptr 的自定义删除器，让 LvglCBinImage 析构完后 free buf。 */
    music_bg_image_ = std::shared_ptr<LvglCBinImage>(
        cbin_image,
        [wrapper](LvglCBinImage* p) {
            if (p) {
                delete p;
            }
            if (wrapper->buf) {
                heap_caps_free(wrapper->buf);
                wrapper->buf = nullptr;
            }
        });

    lv_img_set_src(music_cover_bg_img_, music_bg_image_->image_dsc());
    lv_obj_remove_flag(music_cover_bg_img_, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(FAN_MIPI55_DISPLAY_TAG,
             "Loaded musicbg-720x1232.bin from SD card (%zu bytes)", wrapper->size);
}
#endif  // HAVE_LVGL

/* 全量元数据接口（歌名/歌手/总时长）。chat_message_label_ 不参与。
 *
 * 总时长变化时立即刷新总时间标签 + 按当前进度刷新百分比，避免下次 SetMusicProgress
 * 调用前进度条看起来还停在旧基线上。 */
void FanMIPI55Display::SetMusicInfo(const char* song_name,
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
    /* 总时间标签：立刻写一次 */
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
void FanMIPI55Display::SetMusicProgress(int current_ms, const char* lyric, const char* lyric_next) {
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

#endif // FAN_MIPI55_DISPLAY_H
