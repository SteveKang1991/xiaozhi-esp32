#include "fan_holo_chat_page.h"
#include "fan_holo_display.h"

#include <esp_log.h>

namespace {

constexpr uint32_t kChatSubtitleColor = 0xC9FF20;
constexpr int kSubtitleGap = 8;
constexpr int kSubtitleRightPad = 4;

}  // namespace

void FanHoloChatPage::LayoutSubtitle(FanHoloDisplay& host) {
    if (chat_inner_label_ == nullptr || status_bar_.status_pill == nullptr) {
        return;
    }
    if (status_bar_.top_bar != nullptr) {
        lv_obj_update_layout(status_bar_.top_bar);
    }
    lv_obj_update_layout(status_bar_.status_pill);

    lv_area_t pill{};
    lv_obj_get_coords(status_bar_.status_pill, &pill);
    const int x = (int)pill.x2 + 1 + kSubtitleGap;
    const int y = (int)pill.y1;
    const int h = (int)pill.y2 - (int)pill.y1 + 1;
    const int w = host.screen_width() - x - kSubtitleRightPad;
    if (w < 24 || h < 8) {
        lv_obj_add_flag(chat_inner_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_set_pos(chat_inner_label_, (lv_coord_t)x, (lv_coord_t)y);
    lv_obj_set_size(chat_inner_label_, (lv_coord_t)w, (lv_coord_t)h);
    if (last_chat_content_.empty()) {
        lv_obj_add_flag(chat_inner_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(chat_inner_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

void FanHoloChatPage::Create(FanHoloDisplay& host) {
    screen_ = FanHoloCreateScreen(host);
    container_ = FanHoloCreateFullBleedContainer(screen_, host);
    role_.Create(screen_, host);
    preview_image_ = FanHoloCreatePreviewImage(screen_, host);
    status_bar_.Create(screen_, host, FanHoloStatusBar::Kind::StatusLeft);

    auto* theme = host.GetLvglTheme();

    /* 顶部字幕：紧贴「聆听中/说话中」右侧，横向至屏右；SCROLL_CIRCULAR 从右往左。
     * 动画时长拉长，减少 SPI 脏刷新频率（仍用 LVGL 自带 scroll，不用自定义 anim）。 */
    chat_inner_label_ = lv_label_create(screen_);
    lv_obj_remove_style_all(chat_inner_label_);
    lv_obj_add_flag(chat_inner_label_, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(chat_inner_label_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(chat_inner_label_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(chat_inner_label_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(chat_inner_label_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(chat_inner_label_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chat_inner_label_, 0, LV_PART_MAIN);
    lv_label_set_long_mode(chat_inner_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(chat_inner_label_, lv_anim_speed_clamped(40, 300, 60000), LV_PART_MAIN);
    lv_obj_set_style_text_font(chat_inner_label_, theme->text_font()->font(), 0);
    lv_obj_set_style_text_align(chat_inner_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(chat_inner_label_, lv_color_hex(kChatSubtitleColor), 0);
    lv_label_set_text(chat_inner_label_, "");
    lv_obj_add_flag(chat_inner_label_, LV_OBJ_FLAG_HIDDEN);
    chat_container_ = chat_inner_label_;
    LayoutSubtitle(host);
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
    host.ApplyChatStrip(chat_inner_label_);
}

void FanHoloChatPage::RaiseSubtitleThenOverlays(FanHoloDisplay& host) {
    LayoutSubtitle(host);
    /* 层级：顶栏 → 字幕 → AEC 提示 → 音量 → 低电量 */
    if (status_bar_.top_bar != nullptr) {
        lv_obj_move_foreground(status_bar_.top_bar);
    }
    if (chat_inner_label_ != nullptr && !last_chat_content_.empty()) {
        lv_obj_move_foreground(chat_inner_label_);
    }
    if (status_bar_.notification_label != nullptr) {
        lv_obj_move_foreground(status_bar_.notification_label);
    }
    if (status_bar_.volume_overlay != nullptr) {
        lv_obj_move_foreground(status_bar_.volume_overlay);
    }
    if (status_bar_.low_battery_popup != nullptr) {
        lv_obj_move_foreground(status_bar_.low_battery_popup);
    }
}

void FanHoloChatPage::Show(FanHoloDisplay& host) {
    if (screen_ == nullptr) {
        return;
    }
    Bind(host);
    lv_screen_load(screen_);
    RaiseSubtitleThenOverlays(host);
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
    if (chat_inner_label_) {
        lv_obj_set_style_text_font(chat_inner_label_, font, 0);
        lv_obj_set_style_text_color(chat_inner_label_, lv_color_hex(kChatSubtitleColor), 0);
    }
    status_bar_.ApplyTextFont(font, color);
}

void FanHoloChatPage::SetMessage(FanHoloDisplay& host, const char* role, const char* content) {
    if (!host.IsSetupUICalled()) {
        ESP_LOGW(host.metrics().tag, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!", role, content);
        return;
    }
    DisplayLockGuard lock(&host);
    if (chat_inner_label_ == nullptr) {
        ESP_LOGW(host.metrics().tag, "SetChatMessage('%s', '%s') failed: chat subtitle not ready", role, content);
        return;
    }
    /* 与稳定版 FanLcd 一致：流式字幕只改文字，禁止 move_foreground。
     * 每次 RaiseOverlays 会整屏标脏，和 MJPEG ROI 抢 lvgl_port_lock(5ms) → 帧率塌到 6fps。 */
    if (content == nullptr || content[0] == '\0') {
        lv_label_set_text(chat_inner_label_, "");
        last_chat_content_.clear();
        lv_obj_add_flag(chat_inner_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (last_chat_content_ == content) {
        return;
    }
    const bool was_empty = last_chat_content_.empty();
    last_chat_content_ = content;
    lv_label_set_text(chat_inner_label_, content);
    if (was_empty) {
        LayoutSubtitle(host);
    } else {
        lv_obj_remove_flag(chat_inner_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

void FanHoloChatPage::ClearMessages(FanHoloDisplay& host) {
    DisplayLockGuard lock(&host);
    if (chat_inner_label_ != nullptr) {
        lv_label_set_text(chat_inner_label_, "");
        lv_obj_add_flag(chat_inner_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(chat_inner_label_);
    }
    last_chat_content_.clear();
}
