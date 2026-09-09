#ifndef FAN_HOLO_MUSIC_PAGE_H
#define FAN_HOLO_MUSIC_PAGE_H

#include "fan_holo_status_bar.h"
#include "lvgl_display/lvgl_image.h"

#include <memory>
#include <string>

class FanHoloDisplay;

/* 音乐页：独立 screen。自建 statusbar、封面/歌词/进度。FFT 仍挂在封面容器上。 */
class FanHoloMusicPage {
public:
    void Create(FanHoloDisplay& host);
    void Destroy();
    void Show(FanHoloDisplay& host);
    void Bind(FanHoloDisplay& host) const;
    void ShowCover(FanHoloDisplay& host, bool show, const std::string& picture_url);
    void SetInfo(FanHoloDisplay& host, const char* song_name, const char* singer, int interval);
    void SetProgress(FanHoloDisplay& host, int current_ms, const char* lyric, const char* lyric_next);
    void ApplyTextFont(const lv_font_t* font, lv_color_t color);

    lv_obj_t* screen() const { return screen_; }
    lv_obj_t* container() const { return music_cover_container_; }
    FanHoloStatusBar& status_bar() { return status_bar_; }
    const FanHoloStatusBar& status_bar() const { return status_bar_; }

private:
    void SetupCoverUI(FanHoloDisplay& host);
    void LoadBackgroundImage(FanHoloDisplay& host);
    void FetchCoverSync(FanHoloDisplay& host, const std::string& url);

    lv_obj_t* screen_ = nullptr;
    lv_obj_t* root_container_ = nullptr;
    FanHoloStatusBar status_bar_;

    lv_obj_t* music_cover_container_ = nullptr;
    lv_obj_t* music_cover_bg_img_ = nullptr;
    lv_obj_t* music_cover_img_ = nullptr;
    lv_obj_t* music_lyric_label_ = nullptr;
    lv_obj_t* music_lyric_next_label_ = nullptr;
    lv_obj_t* music_song_name_label_ = nullptr;
    lv_obj_t* music_singer_label_ = nullptr;
    lv_obj_t* music_progress_bar_ = nullptr;
    lv_obj_t* music_cur_time_label_ = nullptr;
    lv_obj_t* music_total_time_label_ = nullptr;

    std::shared_ptr<LvglCBinImage> music_bg_image_;
    std::unique_ptr<LvglAllocatedImage> music_cover_image_data_;
    std::string current_music_picture_url_;
    int32_t music_cover_scale_ = 256;
    int last_progress_ms_ = -1;
    int last_progress_sec_ = -1;
    int64_t last_progress_bar_update_ms_ = -1;
    std::string last_lyric_;
    std::string last_lyric_next_;
    int music_total_interval_sec_ = 0;
};

#endif
