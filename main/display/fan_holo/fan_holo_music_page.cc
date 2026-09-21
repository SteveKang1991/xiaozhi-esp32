#include "fan_holo_music_page.h"
#include "fan_holo_display.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <cstdio>
#include <cstring>

extern "C" {
#include "mjpeg_player.h"
}

void FanHoloMusicPage::Create(FanHoloDisplay& host) {
    screen_ = FanHoloCreateScreen(host);
    root_container_ = FanHoloCreateFullBleedContainer(screen_, host);
    status_bar_.Create(screen_, host, FanHoloStatusBar::Kind::StatusLeft);
    status_bar_.SetStatusText(Lang::Strings::MUSIC_PLAYING);
    SetupCoverUI(host);
}

void FanHoloMusicPage::Destroy() {
    if (screen_ != nullptr) {
        lv_obj_del(screen_);
        screen_ = nullptr;
    }
    root_container_ = nullptr;
    music_cover_container_ = nullptr;
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
    status_bar_.SetStatusText(Lang::Strings::MUSIC_PLAYING);
    /* 封面容器可能盖住整屏，状态栏必须再抬到最前。 */
    if (music_cover_container_ != nullptr) {
        lv_obj_move_foreground(music_cover_container_);
    }
    status_bar_.RaiseOverlays();
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
    (void)picture_url;
    bool go_music = false;
    bool go_chat = false;
    {
        DisplayLockGuard lock(&host);
        if (music_cover_container_ == nullptr) {
            ESP_LOGW(host.metrics().tag, "ShowMusicCover before SetupUI; ignored");
            return;
        }
        if (show) {
            go_music = true;
            if (music_cover_img_) {
                lv_img_set_src(music_cover_img_, (const void*)nullptr);
                lv_obj_add_flag(music_cover_img_, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            if (music_cover_img_) {
                lv_img_set_src(music_cover_img_, (const void*)nullptr);
                lv_obj_add_flag(music_cover_img_, LV_OBJ_FLAG_HIDDEN);
            }
            last_progress_ms_ = -1;
            last_progress_sec_ = -1;
            last_progress_bar_update_ms_ = -1;
            last_lyric_.clear();
            last_lyric_next_.clear();
            go_chat = (host.current_page_ == FanHoloDisplay::Page::Music);
        }
    }
    if (go_music) {
        host.PreparePage(FanHoloDisplay::Page::Music);
        DisplayLockGuard lock(&host);
        lv_obj_remove_flag(music_cover_container_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(music_cover_container_);
        if (music_cover_img_) {
            lv_obj_add_flag(music_cover_img_, LV_OBJ_FLAG_HIDDEN);
        }
        status_bar_.SetStatusText(Lang::Strings::MUSIC_PLAYING);
        status_bar_.RaiseOverlays();
    } else if (go_chat) {
        host.PreparePage(FanHoloDisplay::Page::Chat);
    }
}

void FanHoloMusicPage::SetupCoverUI(FanHoloDisplay& host) {
    const auto& metrics = host.metrics();
    const bool s6 = strcmp(metrics.tag, "FanLcd20Display") == 0;
    const bool center_text =
        s6 || strcmp(metrics.tag, "FanLcd778928Display") == 0; /* S6 / 幻影1：歌名歌手歌词居中 */
    /* S6 有 297/320 两种宽，按实际屏宽高布局，避免 metrics 写死 320 溢出。 */
    const lv_coord_t cover_w = static_cast<lv_coord_t>(host.screen_width());
    const lv_coord_t cover_h = static_cast<lv_coord_t>(
        host.screen_height() > metrics.music_cover_top
            ? host.screen_height() - metrics.music_cover_top
            : host.screen_height());
    music_cover_container_ = lv_obj_create(screen_);
    lv_obj_set_pos(music_cover_container_, 0, (lv_coord_t)metrics.music_cover_top);
    lv_obj_set_size(music_cover_container_, cover_w, cover_h);
    lv_obj_set_style_radius(music_cover_container_, 0, 0);
    lv_obj_set_style_bg_color(music_cover_container_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(music_cover_container_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(music_cover_container_, 0, 0);
    lv_obj_set_style_pad_all(music_cover_container_, 0, 0);
    lv_obj_remove_flag(music_cover_container_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(music_cover_container_, LV_SCROLLBAR_MODE_OFF);

    {
        const lv_coord_t img_w = metrics.cover_box;
        const lv_coord_t img_h = metrics.cover_box;
        const lv_coord_t cover_x = (cover_w - img_w) / 2;
        const lv_coord_t cover_y = metrics.cover_y;
        music_cover_img_ = lv_img_create(music_cover_container_);
        lv_obj_set_pos(music_cover_img_, cover_x, cover_y);
        lv_obj_set_size(music_cover_img_, img_w, img_h);
        lv_image_set_inner_align(music_cover_img_, LV_IMAGE_ALIGN_COVER);
    }
    lv_obj_add_flag(music_cover_img_, LV_OBJ_FLAG_HIDDEN);

    const lv_coord_t side_pad = metrics.side_pad;
    const lv_coord_t inner_w = cover_w - side_pad * 2;

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
        lv_obj_set_style_transform_scale_x(music_lyric_label_, scale_num, 0);
        lv_obj_set_style_transform_scale_y(music_lyric_label_, scale_num, 0);
        if (center_text) {
            lv_obj_set_style_text_align(music_lyric_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_transform_pivot_x(music_lyric_label_, box_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(music_lyric_label_, box_h, 0);
            lv_obj_align(music_lyric_label_, LV_ALIGN_BOTTOM_MID, 0, metrics.lyric_bottom);
        } else {
            lv_obj_set_style_text_align(music_lyric_label_, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_set_style_transform_pivot_x(music_lyric_label_, 0, 0);
            lv_obj_set_style_transform_pivot_y(music_lyric_label_, box_h, 0);
            lv_obj_align(music_lyric_label_, LV_ALIGN_BOTTOM_LEFT, side_pad, metrics.lyric_bottom);
        }
    }

    music_lyric_next_label_ = make_text_label(music_cover_container_, inner_w, 30);
    lv_obj_set_style_text_color(music_lyric_next_label_, lv_color_hex(0x9d9183), 0);
    if (center_text) {
        lv_obj_set_style_text_align(music_lyric_next_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(music_lyric_next_label_, LV_ALIGN_BOTTOM_MID, 0, metrics.lyric_next_bottom);
    } else {
        lv_obj_set_style_text_align(music_lyric_next_label_, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(music_lyric_next_label_, LV_ALIGN_BOTTOM_LEFT, side_pad, metrics.lyric_next_bottom);
    }

    {
        const int scale_num = 307;
        const lv_coord_t box_w = inner_w * 256 / scale_num;
        const lv_coord_t box_h = 32;
        music_song_name_label_ = make_text_label(music_cover_container_, box_w, box_h);
        lv_obj_set_size(music_song_name_label_, box_w, box_h);
        lv_obj_set_style_transform_scale_x(music_song_name_label_, scale_num, 0);
        lv_obj_set_style_transform_scale_y(music_song_name_label_, scale_num, 0);
        if (center_text) {
            lv_obj_set_style_text_align(music_song_name_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_transform_pivot_x(music_song_name_label_, box_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(music_song_name_label_, 0, 0);
            lv_obj_align(music_song_name_label_, LV_ALIGN_TOP_MID, 0, metrics.song_top);
        } else {
            lv_obj_set_style_text_align(music_song_name_label_, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_set_style_transform_pivot_x(music_song_name_label_, 0, 0);
            lv_obj_set_style_transform_pivot_y(music_song_name_label_, 0, 0);
            lv_obj_align(music_song_name_label_, LV_ALIGN_TOP_LEFT, side_pad, metrics.song_top);
        }
    }

    music_singer_label_ = make_text_label(music_cover_container_, inner_w, 30);
    lv_obj_set_style_text_color(music_singer_label_, lv_color_hex(0xdfd8d0), 0);
    if (center_text) {
        lv_obj_set_style_text_align(music_singer_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(music_singer_label_, LV_ALIGN_TOP_MID, 0, metrics.singer_top);
    } else {
        lv_obj_set_style_text_align(music_singer_label_, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(music_singer_label_, LV_ALIGN_TOP_LEFT, side_pad, metrics.singer_top);
    }

    music_progress_bar_ = lv_bar_create(music_cover_container_);
    /* 进度条略收进，两端时间同步往中间靠 */
    const lv_coord_t bar_pad = side_pad + 12;
    const lv_coord_t bar_w = cover_w - bar_pad * 2;
    lv_obj_set_size(music_progress_bar_, bar_w, 6);
    lv_obj_align(music_progress_bar_, LV_ALIGN_BOTTOM_LEFT, bar_pad, metrics.bar_bottom);
    lv_bar_set_range(music_progress_bar_, 0, 100);
    lv_bar_set_value(music_progress_bar_, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(music_progress_bar_, lv_color_hex(0x9d9183), LV_PART_MAIN);
    lv_obj_set_style_bg_color(music_progress_bar_, lv_color_white(), LV_PART_INDICATOR);

    music_cur_time_label_ = make_text_label(music_cover_container_, bar_w / 2 + 1, 30);
    lv_obj_set_style_text_align(music_cur_time_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(music_cur_time_label_, LV_ALIGN_BOTTOM_LEFT, bar_pad, metrics.time_bottom);
    lv_label_set_text(music_cur_time_label_, "00:00");

    music_total_time_label_ = lv_label_create(music_cover_container_);
    lv_obj_set_size(music_total_time_label_, bar_w / 2 + 1, 30);
    lv_obj_set_style_bg_opa(music_total_time_label_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_font(music_total_time_label_, host.GetLvglTheme()->text_font()->font(), 0);
    lv_obj_set_style_text_color(music_total_time_label_, lv_color_white(), 0);
    lv_obj_set_style_text_align(music_total_time_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(music_total_time_label_, LV_LABEL_LONG_CLIP);
    lv_obj_align(music_total_time_label_, LV_ALIGN_BOTTOM_RIGHT, -bar_pad, metrics.time_bottom);
    lv_label_set_text(music_total_time_label_, "00:00");
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
