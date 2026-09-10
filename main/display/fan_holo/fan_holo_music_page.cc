#include "fan_holo_music_page.h"
#include "fan_holo_display.h"
#include "board.h"
#include "assets/lang_config.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <stdexcept>
#include <cstdio>

extern "C" {
#include "sd_scanner.h"
#include "jpg/jpeg_to_image.h"
#include "mjpeg_player.h"
}

void FanHoloMusicPage::Create(FanHoloDisplay& host) {
    screen_ = FanHoloCreateScreen(host);
    root_container_ = FanHoloCreateFullBleedContainer(screen_, host);
    status_bar_.Create(screen_, host, FanHoloStatusBar::Kind::StatusLeft);
    status_bar_.SetStatusText(Lang::Strings::MUSIC_PLAYING);
    SetupCoverUI(host);
    LoadBackgroundImage(host);
}

void FanHoloMusicPage::Destroy() {
    music_cover_image_data_.reset();
    music_bg_image_.reset();
    if (screen_ != nullptr) {
        lv_obj_del(screen_);
        screen_ = nullptr;
    }
    root_container_ = nullptr;
    music_cover_container_ = nullptr;
    music_cover_bg_img_ = nullptr;
    music_cover_img_ = nullptr;
    music_lyric_label_ = nullptr;
    music_lyric_next_label_ = nullptr;
    music_song_name_label_ = nullptr;
    music_singer_label_ = nullptr;
    music_progress_bar_ = nullptr;
    music_cur_time_label_ = nullptr;
    music_total_time_label_ = nullptr;
    status_bar_ = {};
}

void FanHoloMusicPage::Bind(FanHoloDisplay& host) const {
    status_bar_.Bind(host);
    host.ApplyContainer(root_container_);
    host.ApplyPreview(nullptr);
    host.ApplyRole(nullptr);
    host.ApplyChatStrip(nullptr);
}

void FanHoloMusicPage::Show(FanHoloDisplay& host) {
    if (screen_ == nullptr) {
        return;
    }
    Bind(host);
    lv_screen_load(screen_);
}

void FanHoloMusicPage::ApplyTextFont(const lv_font_t* font, lv_color_t color) {
    if (font == nullptr) {
        return;
    }
    if (screen_) {
        lv_obj_set_style_text_font(screen_, font, 0);
        lv_obj_set_style_text_color(screen_, color, 0);
    }
    if (root_container_) {
        lv_obj_set_style_text_font(root_container_, font, 0);
    }
    if (music_cover_container_) {
        lv_obj_set_style_text_font(music_cover_container_, font, 0);
    }
    auto apply = [font](lv_obj_t* lbl) {
        if (lbl) {
            lv_obj_set_style_text_font(lbl, font, 0);
        }
    };
    apply(music_lyric_label_);
    apply(music_lyric_next_label_);
    apply(music_song_name_label_);
    apply(music_singer_label_);
    apply(music_cur_time_label_);
    apply(music_total_time_label_);
    status_bar_.ApplyTextFont(font, color);
}

void FanHoloMusicPage::ShowCover(FanHoloDisplay& host, bool show, const std::string& picture_url) {
    std::string fetch_url;
    {
        DisplayLockGuard lock(&host);
        if (music_cover_container_ == nullptr) {
            ESP_LOGW(host.metrics().tag, "ShowMusicCover before SetupUI; ignored");
            return;
        }
        if (show) {
            host.StopMjpegIfRunning();
            host.SwitchTo(FanHoloDisplay::Page::Music);
            lv_obj_remove_flag(music_cover_container_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(music_cover_container_);
            host.EnableFft(true);
            if (!picture_url.empty() && picture_url != current_music_picture_url_) {
                current_music_picture_url_ = picture_url;
                music_cover_image_data_.reset();
                if (music_cover_img_) {
                    lv_img_set_src(music_cover_img_, (const void*)nullptr);
                    lv_obj_add_flag(music_cover_img_, LV_OBJ_FLAG_HIDDEN);
                }
                fetch_url = picture_url;
            } else if (!picture_url.empty() && music_cover_image_data_ && music_cover_img_) {
                lv_img_set_src(music_cover_img_, music_cover_image_data_->image_dsc());
                lv_image_set_scale(music_cover_img_, music_cover_scale_);
                lv_obj_remove_flag(music_cover_img_, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            host.StopFft();
            current_music_picture_url_.clear();
            music_cover_image_data_.reset();
            if (music_cover_img_) {
                lv_img_set_src(music_cover_img_, (const void*)nullptr);
                lv_obj_add_flag(music_cover_img_, LV_OBJ_FLAG_HIDDEN);
            }
            last_progress_ms_ = -1;
            last_progress_sec_ = -1;
            last_progress_bar_update_ms_ = -1;
            last_lyric_.clear();
            last_lyric_next_.clear();
            host.SwitchTo(FanHoloDisplay::Page::Idle);
        }
    }
    if (!fetch_url.empty()) {
        FetchCoverSync(host, fetch_url);
    }
}

void FanHoloMusicPage::FetchCoverSync(FanHoloDisplay& host, const std::string& url) {
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(5);
    if (!http->Open("GET", url)) {
        ESP_LOGW(host.metrics().tag, "Music cover download failed, play without cover");
        return;
    }
    int status = http->GetStatusCode();
    size_t len = http->GetBodyLength();
    if (status != 200 || len == 0 || len > 256 * 1024) {
        ESP_LOGW(host.metrics().tag, "Music cover skip status=%d len=%u", status, (unsigned)len);
        http->Close();
        return;
    }
    uint8_t* data = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) {
        ESP_LOGE(host.metrics().tag, "OOM for music cover");
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
    if (total < 3 || data[0] != 0xFF || data[1] != 0xD8 || data[2] != 0xFF) {
        heap_caps_free(data);
        return;
    }
    uint8_t* decoded = nullptr;
    size_t decoded_len = 0, img_w = 0, img_h = 0, stride = 0;
    esp_err_t ret = jpeg_to_image_fit(data, total, &decoded, &decoded_len, &img_w, &img_h, &stride, 256);
    heap_caps_free(data);
    if (ret != ESP_OK || !decoded || img_w == 0 || img_h == 0) {
        ESP_LOGW(host.metrics().tag, "Music cover decode failed: %s", esp_err_to_name(ret));
        heap_caps_free(decoded);
        return;
    }
    ESP_LOGI(host.metrics().tag, "JPEG decoded: %ux%u", (unsigned)img_w, (unsigned)img_h);
    DisplayLockGuard lock(&host);
    music_cover_image_data_.reset();
    try {
        music_cover_image_data_ = std::make_unique<LvglAllocatedImage>(
            decoded, decoded_len, img_w, img_h, stride, LV_COLOR_FORMAT_RGB565);
        if (music_cover_img_ && music_cover_container_) {
            lv_img_set_src(music_cover_img_, music_cover_image_data_->image_dsc());
            const int32_t cover_box = host.metrics().cover_box;
            int32_t scale_w = 256 * cover_box / (int32_t)img_w;
            int32_t scale_h = 256 * cover_box / (int32_t)img_h;
            music_cover_scale_ = (scale_w < scale_h) ? scale_w : scale_h;
            if (music_cover_scale_ < 32) music_cover_scale_ = 32;
            if (music_cover_scale_ > 256) music_cover_scale_ = 256;
            lv_image_set_scale(music_cover_img_, music_cover_scale_);
            lv_obj_remove_flag(music_cover_img_, LV_OBJ_FLAG_HIDDEN);
        }
    } catch (const std::exception& e) {
        ESP_LOGE(host.metrics().tag, "Music cover apply failed: %s", e.what());
        heap_caps_free(decoded);
    }
}

void FanHoloMusicPage::SetupCoverUI(FanHoloDisplay& host) {
    const auto& metrics = host.metrics();
    music_cover_container_ = lv_obj_create(screen_);
    lv_obj_set_pos(music_cover_container_, 0, (lv_coord_t)metrics.music_cover_top);
    lv_obj_set_size(music_cover_container_,
                    (lv_coord_t)metrics.music_cover_w,
                    (lv_coord_t)metrics.music_cover_h);
    lv_obj_set_style_radius(music_cover_container_, 0, 0);
    lv_obj_set_style_bg_color(music_cover_container_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(music_cover_container_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(music_cover_container_, 0, 0);
    lv_obj_set_style_pad_all(music_cover_container_, 0, 0);

    music_cover_bg_img_ = lv_img_create(music_cover_container_);
    lv_obj_set_pos(music_cover_bg_img_, 0, 0);
    lv_obj_set_size(music_cover_bg_img_,
                    (lv_coord_t)metrics.music_cover_w,
                    (lv_coord_t)metrics.music_cover_h);
    lv_obj_set_style_bg_opa(music_cover_bg_img_, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(music_cover_bg_img_, LV_OBJ_FLAG_HIDDEN);

    {
        const lv_coord_t cover_w = metrics.cover_box;
        const lv_coord_t cover_h = metrics.cover_box;
        const lv_coord_t cover_x = ((lv_coord_t)metrics.music_cover_w - cover_w) / 2;
        const lv_coord_t cover_y = metrics.cover_y;
        music_cover_img_ = lv_img_create(music_cover_container_);
        lv_obj_set_pos(music_cover_img_, cover_x, cover_y);
        lv_obj_set_size(music_cover_img_, cover_w, cover_h);
        lv_image_set_inner_align(music_cover_img_, LV_IMAGE_ALIGN_COVER);
    }
    lv_obj_add_flag(music_cover_img_, LV_OBJ_FLAG_HIDDEN);
    music_cover_scale_ = 256;

    const lv_coord_t side_pad = metrics.side_pad;
    const lv_coord_t inner_w = (lv_coord_t)metrics.music_cover_w - side_pad * 2;

    auto make_text_label = [&](lv_obj_t* parent, lv_coord_t width, lv_coord_t height) -> lv_obj_t* {
        lv_obj_t* lbl = lv_label_create(parent);
        lv_obj_set_size(lbl, width, height);
        lv_obj_set_style_bg_opa(lbl, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_font(lbl, host.GetLvglTheme()->text_font()->font(), 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_label_set_text(lbl, "");
        return lbl;
    };

    {
        const int scale_num = 281;
        const lv_coord_t box_w = inner_w * 256 / scale_num;
        const lv_coord_t box_h = 50;
        music_lyric_label_ = make_text_label(music_cover_container_, box_w, box_h);
        lv_obj_set_style_text_align(music_lyric_label_, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_transform_scale_x(music_lyric_label_, scale_num, 0);
        lv_obj_set_style_transform_scale_y(music_lyric_label_, scale_num, 0);
        lv_obj_set_style_transform_pivot_x(music_lyric_label_, 0, 0);
        lv_obj_set_style_transform_pivot_y(music_lyric_label_, box_h, 0);
        lv_obj_align(music_lyric_label_, LV_ALIGN_BOTTOM_LEFT, side_pad, metrics.lyric_bottom);
    }

    music_lyric_next_label_ = make_text_label(music_cover_container_, inner_w, 30);
    lv_obj_set_style_text_align(music_lyric_next_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(music_lyric_next_label_, lv_color_hex(0x9d9183), 0);
    lv_obj_align(music_lyric_next_label_, LV_ALIGN_BOTTOM_LEFT, side_pad, metrics.lyric_next_bottom);

    {
        const int scale_num = 307;
        const lv_coord_t box_w = inner_w * 256 / scale_num;
        const lv_coord_t box_h = 32;
        music_song_name_label_ = make_text_label(music_cover_container_, box_w, box_h);
        lv_obj_set_size(music_song_name_label_, box_w, box_h);
        lv_obj_set_style_text_align(music_song_name_label_, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_transform_scale_x(music_song_name_label_, scale_num, 0);
        lv_obj_set_style_transform_scale_y(music_song_name_label_, scale_num, 0);
        lv_obj_set_style_transform_pivot_x(music_song_name_label_, 0, 0);
        lv_obj_set_style_transform_pivot_y(music_song_name_label_, 0, 0);
        lv_obj_align(music_song_name_label_, LV_ALIGN_TOP_LEFT, side_pad, metrics.song_top);
    }

    music_singer_label_ = make_text_label(music_cover_container_, inner_w, 30);
    lv_obj_set_style_text_align(music_singer_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(music_singer_label_, lv_color_hex(0xdfd8d0), 0);
    lv_obj_align(music_singer_label_, LV_ALIGN_TOP_LEFT, side_pad, metrics.singer_top);

    music_progress_bar_ = lv_bar_create(music_cover_container_);
    lv_obj_set_size(music_progress_bar_, inner_w, 6);
    lv_obj_align(music_progress_bar_, LV_ALIGN_BOTTOM_LEFT, side_pad, metrics.bar_bottom);
    lv_bar_set_range(music_progress_bar_, 0, 100);
    lv_bar_set_value(music_progress_bar_, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(music_progress_bar_, lv_color_hex(0x9d9183), LV_PART_MAIN);
    lv_obj_set_style_bg_color(music_progress_bar_, lv_color_white(), LV_PART_INDICATOR);

    music_cur_time_label_ = make_text_label(music_cover_container_, inner_w / 2 + 1, 30);
    lv_obj_set_style_text_align(music_cur_time_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(music_cur_time_label_, LV_ALIGN_BOTTOM_LEFT, side_pad, metrics.time_bottom);
    lv_label_set_text(music_cur_time_label_, "00:00");

    music_total_time_label_ = lv_label_create(music_cover_container_);
    lv_obj_set_size(music_total_time_label_, inner_w / 2 + 1, 30);
    lv_obj_set_style_bg_opa(music_total_time_label_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_font(music_total_time_label_, host.GetLvglTheme()->text_font()->font(), 0);
    lv_obj_set_style_text_color(music_total_time_label_, lv_color_white(), 0);
    lv_obj_set_style_text_align(music_total_time_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(music_total_time_label_, LV_LABEL_LONG_CLIP);
    lv_obj_align(music_total_time_label_, LV_ALIGN_BOTTOM_RIGHT, -side_pad, metrics.time_bottom);
    lv_label_set_text(music_total_time_label_, "00:00");
}

void FanHoloMusicPage::LoadBackgroundImage(FanHoloDisplay& host) {
    if (music_cover_bg_img_ == nullptr) {
        return;
    }
    if (!sd_scanner_is_mounted()) {
        ESP_LOGW(host.metrics().tag, "SD card not mounted; music cover will use plain black background");
        return;
    }

    const char* path = host.metrics().music_bg_path;
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        ESP_LOGW(host.metrics().tag, "music background not found (%s)", path);
        return;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return;
    }
    long size = ftell(f);
    if (size <= 0 || size > (long)(1024 * 1024 * 2)) {
        fclose(f);
        return;
    }
    rewind(f);

    uint8_t* buf = (uint8_t*)heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM);
    if (buf == nullptr) {
        buf = (uint8_t*)heap_caps_malloc((size_t)size, MALLOC_CAP_INTERNAL);
    }
    if (buf == nullptr) {
        fclose(f);
        return;
    }

    size_t read_bytes = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (read_bytes != (size_t)size) {
        heap_caps_free(buf);
        return;
    }

    struct CbinWithBuffer {
        uint8_t* buf;
        size_t size;
    };
    auto wrapper = std::make_shared<CbinWithBuffer>();
    wrapper->buf = buf;
    wrapper->size = (size_t)size;

    auto* cbin_image = new LvglCBinImage(buf);
    if (cbin_image->image_dsc() == nullptr) {
        delete cbin_image;
        heap_caps_free(buf);
        return;
    }

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
}

void FanHoloMusicPage::SetInfo(FanHoloDisplay& host, const char* song_name, const char* singer, int interval) {
    DisplayLockGuard lock(&host);
    if (music_cover_container_ == nullptr) {
        return;
    }
    if (music_song_name_label_) {
        lv_label_set_text(music_song_name_label_, (song_name != nullptr && song_name[0] != '\0') ? song_name : "");
    }
    if (music_singer_label_) {
        lv_label_set_text(music_singer_label_, (singer != nullptr && singer[0] != '\0') ? singer : "");
    }
    if (interval >= 0) {
        music_total_interval_sec_ = interval;
    }
    if (music_total_time_label_) {
        int total_sec = music_total_interval_sec_;
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d", total_sec / 60, total_sec % 60);
        lv_label_set_text(music_total_time_label_, buf);
    }
    if (music_progress_bar_) {
        if (music_total_interval_sec_ > 0 && last_progress_ms_ > 0) {
            int pct = (int)((long long)last_progress_ms_ * 100 / (music_total_interval_sec_ * 1000));
            if (pct > 100) pct = 100;
            lv_bar_set_value(music_progress_bar_, pct, LV_ANIM_OFF);
        } else {
            lv_bar_set_value(music_progress_bar_, 0, LV_ANIM_OFF);
        }
    }
}

void FanHoloMusicPage::SetProgress(FanHoloDisplay& host, int current_ms, const char* lyric, const char* lyric_next) {
    if (music_cover_container_ == nullptr) {
        return;
    }
    if (current_ms < 0) current_ms = 0;

    int64_t now_ms = esp_log_timestamp();
    int cur_sec = current_ms / 1000;
    bool need_time = (cur_sec != last_progress_sec_);
    bool need_bar = (music_progress_bar_ && now_ms - last_progress_bar_update_ms_ > 500);
    bool need_lyric = (lyric != nullptr);
    bool need_next = (lyric_next != nullptr);
    if (!need_time && !need_bar && !need_lyric && !need_next) {
        return;
    }

    DisplayLockGuard lock(&host);

    if (cur_sec != last_progress_sec_) {
        last_progress_sec_ = cur_sec;
        if (music_cur_time_label_) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%02d:%02d", cur_sec / 60, cur_sec % 60);
            lv_label_set_text(music_cur_time_label_, buf);
        }
    }

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

    if (music_lyric_label_ && lyric != nullptr) {
        if (last_lyric_ != lyric) {
            last_lyric_ = lyric;
            lv_label_set_text(music_lyric_label_, lyric);
        }
    }

    if (music_lyric_next_label_ && lyric_next != nullptr) {
        if (last_lyric_next_ != lyric_next) {
            last_lyric_next_ = lyric_next;
            lv_label_set_text(music_lyric_next_label_, lyric_next);
        }
    }
}
