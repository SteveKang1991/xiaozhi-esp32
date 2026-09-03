#ifndef DISPLAY_H
#define DISPLAY_H

#include "emoji_collection.h"

#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
#define HAVE_LVGL 1
#include <lvgl.h>
#endif

#include <esp_timer.h>
#include <esp_log.h>
#include <esp_pm.h>

#include <string>
#include <chrono>

class Theme {
public:
    Theme(const std::string& name) : name_(name) {}
    virtual ~Theme() = default;

    inline std::string name() const { return name_; }
private:
    std::string name_;
};

class Display {
public:
    Display();
    virtual ~Display();

    virtual void SetStatus(const char* status);
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string &notification, int duration_ms = 3000);
    virtual void SetEmotion(const char* emotion);
    /**
     * 角色动画：进入待命状态后由 application 根据对话阶段调用。
     * - 默认值 "idle"（待命），播放 SD 卡 /sdcard/Emotion/idle-*.mjpeg
     * - "listen"（聆听中），播放 listen-*.mjpeg
     * - "speak"（说话中），播放 speak-*.mjpeg
     * 基础类默认实现退化为 SetEmotion(state)，由具体 display 重写为 MJPEG 播放器。
     */
    virtual void SetRoleAnimation(const char* state) { SetEmotion(state); }
    virtual void SetChatMessage(const char* role, const char* content);
    virtual void ClearChatMessages();
    virtual void SetTheme(Theme* theme);
    virtual Theme* GetTheme() { return current_theme_; }
    virtual void UpdateStatusBar(bool update_all = false);
    virtual void SetPowerSaveMode(bool on);
    /**
     * 重启前：铺满黑屏，避免 MIPI 残留画面在复位时显示成蓝屏。
     * 子类可先停 MJPEG 等叠加层，再调用基类。
     */
    virtual void PrepareForReboot() {}
    virtual void SetupUI() { 
        setup_ui_called_ = true;
    }
    virtual void SetSystemReady();
    /**
     * Music playback metadata: full song metadata shown in the music UI.
     *
     * @param song_name   歌名 (UTF-8, may be nullptr/empty to clear).
     * @param singer      歌手 (UTF-8, may be nullptr/empty).
     * @param interval    Total duration in seconds (>=0). Used by the music
     *                    UI's progress bar; pass 0 if unknown.
     *
     * Subclasses that have a dedicated music UI should override this. The
     * default implementation is a thin wrapper that discards `singer` and
     * `interval` and only forwards `song_name` to the legacy 1-arg variant
     * so existing displays keep working.
     */
    virtual void SetMusicInfo(const char* song_name,
                              const char* singer,
                              int interval);
    /**
     * Music playback progress / lyric update.
     *
     * Called from the FFT display task (~10Hz) while music is playing.
     * play_thread must not call this — grabbing the LVGL lock from the audio
     * path causes I2S underruns.
     *
     * @param current_ms   Current playback time in milliseconds.
     * @param lyric        UTF-8 lyric line for `current_ms`, or nullptr/empty
     *                     to clear.
     * @param lyric_next   UTF-8 lyric line that follows the current one
     *                     (for 2-line preview). nullptr = don't change the
     *                     "next lyric" label. Default: nullptr.
     */
    virtual void SetMusicProgress(int current_ms, const char* lyric, const char* lyric_next = nullptr);
    /**
     * 音乐封面显示：进入 music_playing 状态时调用 ShowMusicCover(true, picture_url) 弹出封面；
     * 退出音乐播放时调用 ShowMusicCover(false, "") 隐藏并回到 MJPEG 角色动画。
     * 默认实现为空，由具体 display 子类重写。 */
    virtual void ShowMusicCover(bool show, const std::string& picture_url = "") {}

    /**
     * FFT：启用/停用频谱。停用只隐藏条并暂停计算，缓冲区与任务跨歌曲复用。
     * 真正释放在 Display 析构。 */
    virtual void EnableFft(bool enable) {}

    /**
     * 停止 FFT 显示（隐藏并暂停；不释放 arena / 不删 24 个 bar）。
     * 默认实现为空，由具体 display 子类重写。 */
    virtual void StopFft() {}

    /**
     * 把频谱柱/帽打回初始高度并清计算缓存。切歌打断或一曲结束时调用，
     * 避免下一曲开头仍画着上一曲最后一帧。 */
    virtual void ResetFftVisual() {}

    /**
     * 获取音乐封面容器对象，用于 FFT canvas 的父对象。
     * 返回 nullptr 表示音乐封面未激活。
     * 默认实现为空。 */
    virtual lv_obj_t* GetMusicCoverContainer() { return nullptr; }

    inline int width() const { return width_; }
    inline int height() const { return height_; }
    inline bool IsSetupUICalled() const { return setup_ui_called_; }

protected:
    int width_ = 0;
    int height_ = 0;
    bool setup_ui_called_ = false;  // Track if SetupUI() has been called

    Theme* current_theme_ = nullptr;

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};


class DisplayLockGuard {
public:
    DisplayLockGuard(Display *display) : display_(display) {
        if (!display_->Lock(30000)) {
            ESP_LOGE("Display", "Failed to lock display");
        }
    }
    ~DisplayLockGuard() {
        display_->Unlock();
    }

private:
    Display *display_;
};

class NoDisplay : public Display {
private:
    virtual bool Lock(int timeout_ms = 0) override {
        return true;
    }
    virtual void Unlock() override {}
};

#endif
