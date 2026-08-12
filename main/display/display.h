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
    virtual void SetChatMessage(const char* role, const char* content);
    virtual void ClearChatMessages();
    virtual void SetTheme(Theme* theme);
    virtual Theme* GetTheme() { return current_theme_; }
    virtual void UpdateStatusBar(bool update_all = false);
    virtual void SetPowerSaveMode(bool on);
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
     * Called continuously from the playback thread (~every MP3 frame) while
     * music is playing. Implementations update the progress bar position
     * and (if non-null) the on-screen lyric text.
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
     * 角色动画：进入待命状态后由 application 根据对话阶段调用。
     * - 默认值 "idle"（待命），播放 SD 卡 /sdcard/Emotion/idle-*.mjpeg
     * - "listen"（聆听中），播放 listen-*.mjpeg
     * - "speak"（说话中），播放 speak-*.mjpeg
     * 基础类默认实现退化为 SetEmotion(state)，由具体 display 重写为 MJPEG 播放器。
     */
    virtual void SetRoleAnimation(const char* state) { SetEmotion(state); }

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
    DisplayLockGuard(Display *display) : display_(display), locked_(false) {
        locked_ = display_->Lock(30000);
        if (!locked_) {
            ESP_LOGE("Display", "Failed to lock display");
        }
    }
    ~DisplayLockGuard() {
        if (locked_) {
            display_->Unlock();
        }
    }

private:
    Display *display_;
    bool locked_;
};

class NoDisplay : public Display {
private:
    virtual bool Lock(int timeout_ms = 0) override {
        return true;
    }
    virtual void Unlock() override {}
};

#endif
