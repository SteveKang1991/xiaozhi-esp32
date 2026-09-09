#include "fan_holo_idle_page.h"
#include "fan_holo_display.h"

void FanHoloIdlePage::Create(FanHoloDisplay& host) {
    screen_ = FanHoloCreateScreen(host);
    container_ = FanHoloCreateFullBleedContainer(screen_, host);
    role_.Create(screen_, host);
    preview_image_ = FanHoloCreatePreviewImage(screen_, host);
    status_bar_.Create(screen_, host);
    /* 时钟 / 天气 / 相册控件后续在本页 Create，不要放到 chat/music。 */
}

void FanHoloIdlePage::Destroy() {
    if (screen_ != nullptr) {
        lv_obj_del(screen_);
        screen_ = nullptr;
    }
    container_ = nullptr;
    preview_image_ = nullptr;
    status_bar_ = {};
    role_ = {};
}

void FanHoloIdlePage::Bind(FanHoloDisplay& host) const {
    status_bar_.Bind(host);
    role_.Bind(host);
    host.ApplyPreview(preview_image_);
    host.ApplyContainer(container_);
    host.ApplyChatStrip(nullptr);
}

void FanHoloIdlePage::Show(FanHoloDisplay& host) {
    if (screen_ == nullptr) {
        return;
    }
    Bind(host);
    lv_screen_load(screen_);
}

void FanHoloIdlePage::HideRole() {
    role_.Hide();
}

void FanHoloIdlePage::ApplyTextFont(const lv_font_t* font, lv_color_t color) {
    if (font == nullptr) {
        return;
    }
    if (screen_) {
        lv_obj_set_style_text_font(screen_, font, 0);
        lv_obj_set_style_text_color(screen_, color, 0);
    }
    if (container_) {
        lv_obj_set_style_text_font(container_, font, 0);
    }
    status_bar_.ApplyTextFont(font, color);
}
