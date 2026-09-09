#include "fan_holo_chat_page.h"
#include "fan_holo_display.h"

#include <esp_log.h>

void FanHoloChatPage::Create(FanHoloDisplay& host) {
    screen_ = FanHoloCreateScreen(host);
    container_ = FanHoloCreateFullBleedContainer(screen_, host);
    role_.Create(screen_, host);
    preview_image_ = FanHoloCreatePreviewImage(screen_, host);
    status_bar_.Create(screen_, host);

    const auto& metrics = host.metrics();
    auto* theme = host.GetLvglTheme();

    chat_container_ = lv_obj_create(screen_);
    lv_obj_set_scrollbar_mode(chat_container_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(chat_container_, LV_DIR_VER);
    lv_obj_set_size(chat_container_, (lv_coord_t)metrics.chat_strip_w, (lv_coord_t)(host.screen_height() - 300));
    lv_obj_set_style_bg_color(chat_container_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(chat_container_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chat_container_, 0, 0);
    lv_obj_set_style_pad_all(chat_container_, 0, 0);
    lv_obj_align(chat_container_, LV_ALIGN_LEFT_MID, 0, 0);

    chat_inner_label_ = lv_label_create(chat_container_);
    lv_label_set_text(chat_inner_label_, "");
    lv_obj_set_width(chat_inner_label_, (lv_coord_t)metrics.chat_strip_w);
    lv_obj_set_style_min_height(chat_inner_label_, (lv_coord_t)(host.screen_height() - 300), LV_PART_MAIN);
    lv_obj_set_style_bg_color(chat_inner_label_, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(chat_inner_label_, LV_OPA_COVER, LV_PART_MAIN);
    lv_label_set_long_mode(chat_inner_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(chat_inner_label_, theme->text_font()->font(), 0);
    lv_obj_set_style_text_align(chat_inner_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_inner_label_, theme->text_color(), 0);
}

void FanHoloChatPage::Destroy() {
    if (screen_ != nullptr) {
        lv_obj_del(screen_);
        screen_ = nullptr;
    }
    container_ = nullptr;
    preview_image_ = nullptr;
    chat_container_ = nullptr;
    chat_inner_label_ = nullptr;
    status_bar_ = {};
    role_ = {};
    last_chat_content_.clear();
}

void FanHoloChatPage::Bind(FanHoloDisplay& host) const {
    status_bar_.Bind(host);
    role_.Bind(host);
    host.ApplyPreview(preview_image_);
    host.ApplyContainer(container_);
    host.ApplyChatStrip(chat_container_);
}

void FanHoloChatPage::Show(FanHoloDisplay& host) {
    if (screen_ == nullptr) {
        return;
    }
    Bind(host);
    lv_screen_load(screen_);
}

void FanHoloChatPage::HideRole() {
    role_.Hide();
}

void FanHoloChatPage::ApplyTextFont(const lv_font_t* font, lv_color_t color) {
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
    if (chat_container_) {
        lv_obj_set_style_text_font(chat_container_, font, 0);
    }
    if (chat_inner_label_) {
        lv_obj_set_style_text_font(chat_inner_label_, font, 0);
        lv_obj_set_style_text_color(chat_inner_label_, color, 0);
    }
    status_bar_.ApplyTextFont(font, color);
}

void FanHoloChatPage::SetMessage(FanHoloDisplay& host, const char* role, const char* content) {
    if (!host.IsSetupUICalled()) {
        ESP_LOGW(host.metrics().tag, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!", role, content);
        return;
    }
    DisplayLockGuard lock(&host);
    if (chat_container_ == nullptr || chat_inner_label_ == nullptr) {
        ESP_LOGW(host.metrics().tag, "SetChatMessage('%s', '%s') failed: chat container not ready", role, content);
        return;
    }
    if (content == nullptr || content[0] == '\0') {
        lv_label_set_text(chat_inner_label_, "");
        static lv_anim_t s_scroll_anim;
        lv_anim_delete(&s_scroll_anim, nullptr);
        lv_obj_scroll_to_y(chat_container_, 0, LV_ANIM_OFF);
        last_chat_content_.clear();
        lv_obj_invalidate(chat_container_);
        lv_refr_now(nullptr);
        return;
    }
    if (last_chat_content_ == content) {
        return;
    }
    last_chat_content_ = content;

    {
        static lv_anim_t s_scroll_anim;
        lv_anim_delete(&s_scroll_anim, nullptr);
    }
    lv_obj_remove_flag(chat_inner_label_, LV_OBJ_FLAG_HIDDEN);

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
    lv_label_set_text(chat_inner_label_, out.c_str());

    lv_obj_update_layout(chat_inner_label_);
    lv_coord_t content_h = lv_obj_get_height(chat_inner_label_);
    lv_coord_t container_h = lv_obj_get_height(chat_container_);

    lv_obj_remove_flag(chat_inner_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(chat_inner_label_);

    if (content_h <= container_h) {
        lv_obj_scroll_to_y(chat_container_, 0, LV_ANIM_OFF);
        lv_obj_invalidate(chat_container_);
        lv_refr_now(nullptr);
        return;
    }

    int scroll_range_px = content_h - container_h;
    uint32_t duration_ms = (uint32_t)(scroll_range_px * 1000 / 40);
    if (duration_ms < 2000) duration_ms = 2000;
    if (duration_ms > 16000) duration_ms = 16000;

    lv_obj_scroll_to_y(chat_container_, 0, LV_ANIM_OFF);

    static lv_anim_t s_scroll_anim;
    lv_anim_delete(&s_scroll_anim, nullptr);
    lv_anim_init(&s_scroll_anim);
    lv_anim_set_var(&s_scroll_anim, chat_container_);
    lv_anim_set_values(&s_scroll_anim, 0, scroll_range_px);
    lv_anim_set_duration(&s_scroll_anim, duration_ms);
    lv_anim_set_path_cb(&s_scroll_anim, lv_anim_path_linear);
    lv_anim_set_exec_cb(&s_scroll_anim, [](void* var, int32_t v) {
        lv_obj_scroll_to_y((lv_obj_t*)var, v, LV_ANIM_OFF);
    });
    lv_anim_start(&s_scroll_anim);

    lv_obj_invalidate(chat_container_);
    lv_refr_now(nullptr);
}

void FanHoloChatPage::ClearMessages(FanHoloDisplay& host) {
    DisplayLockGuard lock(&host);
    if (chat_inner_label_ != nullptr) {
        lv_label_set_text(chat_inner_label_, "");
        lv_obj_update_layout(chat_inner_label_);
    }
    if (chat_container_ != nullptr) {
        static lv_anim_t s_scroll_anim;
        lv_anim_delete(&s_scroll_anim, nullptr);
        lv_obj_scroll_to_y(chat_container_, 0, LV_ANIM_OFF);
        lv_obj_invalidate(chat_container_);
    }
    last_chat_content_.clear();
    lv_refr_now(nullptr);
}
