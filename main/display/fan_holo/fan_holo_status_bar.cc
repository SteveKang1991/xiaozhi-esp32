#include "fan_holo_status_bar.h"
#include "fan_holo_display.h"
#include "assets/lang_config.h"

#include <font_awesome.h>

lv_obj_t* FanHoloCreateScreen(FanHoloDisplay& host) {
    auto* theme = host.GetLvglTheme();
    auto screen = lv_obj_create(nullptr);
    lv_obj_set_style_text_font(screen, theme->text_font()->font(), 0);
    lv_obj_set_style_text_color(screen, theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, theme->background_color(), 0);
    return screen;
}

lv_obj_t* FanHoloCreateFullBleedContainer(lv_obj_t* screen, FanHoloDisplay& host) {
    auto* theme = host.GetLvglTheme();
    lv_obj_t* container = lv_obj_create(screen);
    lv_obj_set_size(container, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_bg_color(container, theme->background_color(), 0);
    lv_obj_set_style_border_color(container, theme->border_color(), 0);
    return container;
}

lv_obj_t* FanHoloCreatePreviewImage(lv_obj_t* screen, FanHoloDisplay& host) {
    lv_obj_t* preview = lv_image_create(screen);
    lv_obj_set_size(preview, host.screen_width() / 2, host.screen_height() / 2);
    lv_obj_align(preview, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(preview, LV_OBJ_FLAG_HIDDEN);
    return preview;
}

void FanHoloRoleWidgets::Create(lv_obj_t* screen, FanHoloDisplay& host) {
    auto* theme = host.GetLvglTheme();
    auto large_icon_font = theme->large_icon_font()->font();

    box = lv_obj_create(screen);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);

    label = lv_label_create(box);
    lv_obj_set_style_text_font(label, large_icon_font, 0);
    lv_obj_set_style_text_color(label, theme->text_color(), 0);
    lv_label_set_text(label, FONT_AWESOME_MICROCHIP_AI);

    image = lv_img_create(box);
    lv_obj_center(image);
    lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
}

void FanHoloRoleWidgets::Hide() {
    if (image) {
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    }
    if (label) {
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    }
}

void FanHoloRoleWidgets::Bind(FanHoloDisplay& host) const {
    host.ApplyRole(this);
}

void FanHoloStatusBar::Create(lv_obj_t* screen, FanHoloDisplay& host) {
    auto* theme = host.GetLvglTheme();
    auto icon_font = theme->icon_font()->font();
    auto text_font = theme->text_font()->font();
    const auto& metrics = host.metrics();

    top_bar = lv_obj_create(screen);
    lv_obj_set_size(top_bar, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar, 0, 0);
    lv_obj_set_style_bg_opa(top_bar, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(top_bar, theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar, 0, 0);
    lv_obj_set_style_pad_all(top_bar, 0, 0);
    lv_obj_set_style_pad_top(top_bar, theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar, theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar, theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar, theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(top_bar, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t* left_status = lv_obj_create(top_bar);
    lv_obj_set_size(left_status, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(left_status, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left_status, 0, 0);
    lv_obj_set_style_pad_all(left_status, 0, 0);
    lv_obj_set_flex_flow(left_status, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_status, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_radius(left_status, 0, 0);
    lv_obj_set_scrollbar_mode(left_status, LV_SCROLLBAR_MODE_OFF);

    notification_label = lv_label_create(left_status);
    lv_obj_set_style_text_font(notification_label, text_font, 0);
    lv_obj_set_style_text_align(notification_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(notification_label, theme->text_color(), 0);
    lv_label_set_text(notification_label, "");
    lv_obj_add_flag(notification_label, LV_OBJ_FLAG_HIDDEN);

    status_label = lv_label_create(left_status);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(status_label, metrics.status_label_w);
    lv_obj_set_style_max_width(status_label, metrics.status_label_max_w, 0);
    lv_obj_set_style_text_font(status_label, text_font, 0);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(status_label, theme->text_color(), 0);
    lv_label_set_text(status_label, Lang::Strings::INITIALIZING);

    lv_obj_t* right_icons = lv_obj_create(top_bar);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    mute_label = lv_label_create(right_icons);
    lv_label_set_text(mute_label, "");
    lv_obj_set_style_text_font(mute_label, icon_font, 0);
    lv_obj_set_style_text_color(mute_label, theme->text_color(), 0);

    network_label = lv_label_create(right_icons);
    lv_label_set_text(network_label, "");
    lv_obj_set_style_text_font(network_label, icon_font, 0);
    lv_obj_set_style_text_color(network_label, theme->text_color(), 0);
    lv_obj_set_style_margin_left(network_label, theme->spacing(2), 0);

    battery_label = lv_label_create(right_icons);
    lv_label_set_text(battery_label, "");
    lv_obj_set_style_text_font(battery_label, icon_font, 0);
    lv_obj_set_style_text_color(battery_label, theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label, theme->spacing(2), 0);

    low_battery_popup = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup, LV_HOR_RES * 0.5, text_font->line_height * 2);
    lv_obj_align(low_battery_popup, LV_ALIGN_TOP_MID, 0, -theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup, theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup, theme->spacing(4), 0);

    low_battery_label = lv_label_create(low_battery_popup);
    lv_obj_set_style_text_font(low_battery_label, text_font, 0);
    lv_label_set_text(low_battery_label, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label, lv_color_white(), 0);
    lv_obj_center(low_battery_label);
    lv_obj_add_flag(low_battery_popup, LV_OBJ_FLAG_HIDDEN);
}

void FanHoloStatusBar::ApplyTextFont(const lv_font_t* font, lv_color_t color) {
    if (font == nullptr) {
        return;
    }
    if (top_bar) {
        lv_obj_set_style_text_font(top_bar, font, 0);
    }
    if (status_label) {
        lv_obj_set_style_text_font(status_label, font, 0);
        lv_obj_set_style_text_color(status_label, color, 0);
    }
    if (notification_label) {
        lv_obj_set_style_text_font(notification_label, font, 0);
        lv_obj_set_style_text_color(notification_label, color, 0);
    }
    if (low_battery_label) {
        lv_obj_set_style_text_font(low_battery_label, font, 0);
    }
}

void FanHoloStatusBar::Bind(FanHoloDisplay& host) const {
    host.ApplyStatusBar(*this);
}

void FanHoloStatusBar::CopyVisualFrom(const FanHoloStatusBar& other) {
    if (other.status_label && status_label) {
        lv_label_set_text(status_label, lv_label_get_text(other.status_label));
        if (lv_obj_has_flag(other.status_label, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(status_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (other.notification_label && notification_label) {
        lv_label_set_text(notification_label, lv_label_get_text(other.notification_label));
        if (lv_obj_has_flag(other.notification_label, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(notification_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(notification_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (other.mute_label && mute_label) {
        lv_label_set_text(mute_label, lv_label_get_text(other.mute_label));
    }
    if (other.network_label && network_label) {
        lv_label_set_text(network_label, lv_label_get_text(other.network_label));
    }
    if (other.battery_label && battery_label) {
        lv_label_set_text(battery_label, lv_label_get_text(other.battery_label));
    }
    if (other.low_battery_popup && low_battery_popup) {
        if (lv_obj_has_flag(other.low_battery_popup, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(low_battery_popup, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(low_battery_popup, LV_OBJ_FLAG_HIDDEN);
        }
    }
}
