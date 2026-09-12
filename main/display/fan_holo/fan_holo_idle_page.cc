#include "fan_holo_idle_page.h"
#include "fan_holo_display.h"

#include <cstdio>
#include <esp_timer.h>

namespace {

constexpr uint32_t kOrange = 0xF5A623;
constexpr uint32_t kLunarOrange = 0xF07820;
constexpr uint32_t kCyan = 0x5EEAD4;
constexpr uint32_t kIconGreen = 0x22C55E;
constexpr uint32_t kLightGray = 0xC0C0C0;
constexpr uint32_t kWhite = 0xFFFFFF;
constexpr uint32_t kLedCardBg = 0x1A4854;
constexpr uint32_t kLedCardBorder = 0x3A6A78;

void NoScroll(lv_obj_t* obj) {
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
}

void StyleLedCard(lv_obj_t* obj) {
    lv_obj_set_style_bg_color(obj, lv_color_hex(kLedCardBg), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 12, 0);
    lv_obj_set_style_border_width(obj, 2, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(kLedCardBorder), 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
}

lv_obj_t* MakeLabel(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_label_set_text(lbl, text);
    return lbl;
}

}  // namespace

void FanHoloIdlePage::CreateWeather(FanHoloDisplay& host) {
    auto* theme = host.GetLvglTheme();
    auto* font = theme->text_font()->font();
    const auto& metrics = host.metrics();
    const int clock_h = static_cast<int>(metrics.idle_clock_h);
    int card_w = host.screen_width() - static_cast<int>(metrics.mjpeg_idle_w);
    if (card_w < 1) {
        card_w = static_cast<int>(metrics.idle_weather_w);
    }

    weather_root_ = lv_obj_create(screen_);
    lv_obj_remove_style_all(weather_root_);
    lv_obj_add_flag(weather_root_, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(weather_root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(weather_root_, 0, clock_h - 55);
    lv_obj_set_size(weather_root_, card_w, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(weather_root_, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(weather_root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(weather_root_, 0, 0);
    lv_obj_set_style_pad_right(weather_root_, 8, 0);
    lv_obj_set_style_pad_top(weather_root_, 0, 0);
    lv_obj_set_style_pad_bottom(weather_root_, 12, 0);
    lv_obj_set_style_pad_row(weather_root_, 12, 0);
    NoScroll(weather_root_);

    city_label_ = MakeLabel(weather_root_, font, kLunarOrange, "");
    lv_obj_set_size(city_label_, card_w, font->line_height);
    lv_obj_set_style_pad_left(city_label_, 8, 0);
    lv_obj_set_style_bg_opa(city_label_, LV_OPA_TRANSP, 0);
    NoScroll(city_label_);

    today_box_ = lv_obj_create(weather_root_);
    lv_obj_remove_style_all(today_box_);
    lv_obj_set_size(today_box_, card_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(today_box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(today_box_, 4, 0);
    lv_obj_set_style_pad_top(today_box_, 4, 0);
    lv_obj_set_style_pad_bottom(today_box_, 4, 0);
    lv_obj_set_style_pad_left(today_box_, 8, 0);
    StyleLedCard(today_box_);
    NoScroll(today_box_);

    lv_obj_t* mid = lv_obj_create(today_box_);
    lv_obj_remove_style_all(mid);
    lv_obj_set_size(mid, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(mid, 12, 0);
    NoScroll(mid);

    lv_obj_t* temp_col = lv_obj_create(mid);
    lv_obj_remove_style_all(temp_col);
    lv_obj_set_size(temp_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(temp_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(temp_col, 0, 0);
    lv_obj_add_flag(temp_col, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    NoScroll(temp_col);

    lv_obj_t* temp_row = lv_obj_create(temp_col);
    lv_obj_remove_style_all(temp_row);
    lv_obj_set_size(temp_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(temp_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(temp_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(temp_row, 2, 0);
    lv_obj_add_flag(temp_row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    NoScroll(temp_row);

    temp_label_ = MakeLabel(temp_row, font, kWhite, "--");
    lv_obj_set_style_transform_scale(temp_label_, 480, 0);
    lv_obj_set_style_transform_pivot_x(temp_label_, 0, 0);
    lv_obj_set_style_transform_pivot_y(temp_label_, 0, 0);
    lv_obj_set_width(temp_label_, 72);
    lv_obj_set_height(temp_label_, font->line_height * 2);
    lv_obj_add_flag(temp_label_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_t* deg = MakeLabel(temp_row, font, kWhite, "。");
    lv_obj_set_style_translate_x(deg, -5, 0);
    lv_obj_set_style_translate_y(deg, -4, 0);
    text_label_ = MakeLabel(temp_col, font, kWhite, "");
    lv_obj_set_style_translate_x(text_label_, 20, 0);

    today_icon_ = lv_image_create(mid);
    lv_obj_set_size(today_icon_, 1, 1);
    lv_image_set_inner_align(today_icon_, LV_IMAGE_ALIGN_CENTER);
    lv_obj_set_style_pad_all(today_icon_, 0, 0);
    lv_obj_set_style_bg_opa(today_icon_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(today_icon_, LV_OPA_COVER, 0);
    lv_obj_set_style_image_opa(today_icon_, LV_OPA_COVER, 0);
    lv_obj_set_style_translate_x(today_icon_, -10, 0);
    lv_obj_set_style_translate_y(today_icon_, -10, 0);
    NoScroll(today_icon_);

    detail_card_ = lv_obj_create(screen_);
    lv_obj_remove_style_all(detail_card_);
    lv_obj_add_flag(detail_card_, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(detail_card_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(detail_card_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_flag(detail_card_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(detail_card_, card_w, clock_h - 55 + font->line_height + 12);
    lv_obj_set_size(detail_card_, host.screen_width() - card_w, font->line_height);
    StyleLedCard(detail_card_);
    lv_obj_set_style_pad_left(detail_card_, 18, 0);
    NoScroll(detail_card_);

    detail_label_ = MakeLabel(detail_card_, font, kWhite, "");
    lv_obj_set_size(detail_label_, LV_PCT(100), LV_PCT(100));
    lv_label_set_long_mode(detail_label_, LV_LABEL_LONG_CLIP);

    forecast_box_ = lv_obj_create(weather_root_);
    lv_obj_remove_style_all(forecast_box_);
    lv_obj_set_size(forecast_box_, card_w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(forecast_box_, 28, 0);
    lv_obj_set_style_pad_bottom(forecast_box_, 4, 0);
    lv_obj_set_style_pad_left(forecast_box_, 8, 0);
    lv_obj_set_style_pad_right(forecast_box_, 8, 0);
    lv_obj_set_flex_flow(forecast_box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(forecast_box_, 18, 0);
    lv_obj_set_style_translate_y(forecast_box_, 5, 0);
    StyleLedCard(forecast_box_);
    NoScroll(forecast_box_);

    for (int i = 0; i < 3; ++i) {
        lv_obj_t* row = lv_obj_create(forecast_box_);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        if (i == 0) {
            lv_obj_set_style_translate_y(row, -20, 0);
        } else if (i == 1) {
            lv_obj_set_style_translate_y(row, -10, 0);
        }
        NoScroll(row);

        lv_obj_t* info = lv_obj_create(row);
        lv_obj_remove_style_all(info);
        lv_obj_set_size(info, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(info, 2, 0);
        NoScroll(info);
        day_title_[i] = MakeLabel(info, font, kLightGray, "");
        day_date_[i] = MakeLabel(info, font, kOrange, "");
        day_temp_[i] = MakeLabel(info, font, kLunarOrange, "");
        day_hum_[i] = MakeLabel(info, font, kCyan, "");
        day_precip_[i] = MakeLabel(info, font, kIconGreen, "");

        lv_obj_t* icon_pack = lv_obj_create(row);
        lv_obj_remove_style_all(icon_pack);
        lv_obj_set_size(icon_pack, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(icon_pack, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(icon_pack, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(icon_pack, 0, 0);
        const int icon_nudge_x = -2;
        lv_obj_set_style_translate_x(icon_pack, icon_nudge_x, 0);
        lv_obj_set_style_translate_y(icon_pack, -60, 0);
        NoScroll(icon_pack);

        day_text_[i] = MakeLabel(icon_pack, font, kWhite, "");
        lv_obj_set_style_transform_scale(day_text_[i], 210, 0);
        lv_obj_set_style_transform_pivot_x(day_text_[i], 0, 0);
        lv_obj_set_style_transform_pivot_y(day_text_[i], font->line_height / 2, 0);
        lv_obj_set_style_translate_x(day_text_[i], 5 + icon_nudge_x, 0);
        lv_obj_set_style_translate_y(day_text_[i], -5, 0);
        NoScroll(day_text_[i]);

        day_icon_[i] = lv_image_create(icon_pack);
        lv_obj_set_size(day_icon_[i], 1, 1);
        lv_image_set_inner_align(day_icon_[i], LV_IMAGE_ALIGN_CENTER);
        lv_obj_set_style_pad_all(day_icon_[i], 0, 0);
        lv_obj_set_style_bg_opa(day_icon_[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_opa(day_icon_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_image_opa(day_icon_[i], LV_OPA_COVER, 0);
        NoScroll(day_icon_[i]);
    }
}

void FanHoloIdlePage::StyleWeatherIcon(lv_obj_t* icon, int slot) {
    if (icon == nullptr) {
        return;
    }
    const lv_image_dsc_t* dsc = FanHoloWeatherIconDsc(slot);
    if (dsc == nullptr || dsc->data == nullptr) {
        return;
    }
    if (lv_image_get_src(icon) != dsc) {
        lv_image_set_src(icon, dsc);
    }
    lv_obj_set_size(icon, dsc->header.w, dsc->header.h);
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
}

void FanHoloIdlePage::Create(FanHoloDisplay& host) {
    screen_ = FanHoloCreateScreen(host);
    container_ = FanHoloCreateFullBleedContainer(screen_, host);
    lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(container_, lv_color_black(), 0);
    role_.Create(screen_, host, true);
    preview_image_ = FanHoloCreatePreviewImage(screen_, host);

    status_bar_.Create(screen_, host, FanHoloStatusBar::Kind::IdleFull);
    lv_obj_set_pos(status_bar_.top_bar, 0, 0);
    CreateWeather(host);
    if (last_weather_.valid) {
        ApplyWeather(last_weather_);
    }
    if (weather_root_) {
        lv_obj_move_foreground(weather_root_);
    }
    status_bar_.RaiseOverlays();
}

void FanHoloIdlePage::Destroy() {
    if (screen_ != nullptr) {
        lv_obj_del(screen_);
        screen_ = nullptr;
    }
    container_ = nullptr;
    preview_image_ = nullptr;
    weather_root_ = nullptr;
    detail_card_ = nullptr;
    today_box_ = nullptr;
    forecast_box_ = nullptr;
    city_label_ = nullptr;
    temp_label_ = nullptr;
    text_label_ = nullptr;
    today_icon_ = nullptr;
    detail_label_ = nullptr;
    detail_line0_[0] = '\0';
    detail_line1_[0] = '\0';
    detail_line_ = 0;
    for (int i = 0; i < 3; ++i) {
        day_title_[i] = nullptr;
        day_date_[i] = nullptr;
        day_text_[i] = nullptr;
        day_icon_[i] = nullptr;
        day_temp_[i] = nullptr;
        day_hum_[i] = nullptr;
        day_precip_[i] = nullptr;
    }
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
    lv_obj_set_pos(status_bar_.top_bar, 0, 0);
    lv_screen_load(screen_);
    if (weather_root_) {
        lv_obj_move_foreground(weather_root_);
    }
    status_bar_.RaiseOverlays();
    status_bar_.Tick();
}

void FanHoloIdlePage::HideRole() {
    role_.Hide();
}

void FanHoloIdlePage::Tick() {
    status_bar_.Tick();
    if (detail_label_ == nullptr || detail_line0_[0] == '\0') {
        return;
    }
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - detail_swap_ms_ < 3500) {
        return;
    }
    detail_swap_ms_ = now_ms;
    detail_line_ ^= 1;
    lv_label_set_text(detail_label_, detail_line_ ? detail_line1_ : detail_line0_);
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
    auto paint = [font](lv_obj_t* lbl, uint32_t hex) {
        if (lbl == nullptr) {
            return;
        }
        lv_obj_set_style_text_font(lbl, font, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(hex), 0);
    };
    paint(city_label_, kLunarOrange);
    paint(temp_label_, kWhite);
    paint(text_label_, kWhite);
    paint(detail_label_, kWhite);
    for (int i = 0; i < 3; ++i) {
        paint(day_title_[i], kLightGray);
        paint(day_date_[i], kOrange);
        paint(day_text_[i], kWhite);
        paint(day_temp_[i], kLunarOrange);
        paint(day_hum_[i], kCyan);
        paint(day_precip_[i], kIconGreen);
    }
}

void FanHoloIdlePage::ApplyWeather(const IdleWeatherView& weather) {
    last_weather_ = weather;
    if (weather_root_ == nullptr) {
        return;
    }
    if (!weather.valid) {
        lv_obj_add_flag(weather_root_, LV_OBJ_FLAG_HIDDEN);
        if (detail_card_) {
            lv_obj_add_flag(detail_card_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    const bool was_hidden = lv_obj_has_flag(weather_root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(weather_root_, LV_OBJ_FLAG_HIDDEN);
    if (detail_card_) {
        lv_obj_remove_flag(detail_card_, LV_OBJ_FLAG_HIDDEN);
    }
    if (was_hidden) {
        lv_obj_move_foreground(weather_root_);
        if (detail_card_) {
            lv_obj_move_foreground(detail_card_);
        }
        status_bar_.RaiseOverlays();
    }

    lv_label_set_text(city_label_, weather.city);
    char buf[160];
    snprintf(buf, sizeof(buf), "%d", weather.temp);
    lv_label_set_text(temp_label_, buf);
    lv_label_set_text(text_label_, weather.text);
    StyleWeatherIcon(today_icon_, 0);

    snprintf(detail_line0_, sizeof(detail_line0_), "湿度:%d     降水:%d%%     风力:%s",
             weather.humidity, weather.precip, weather.wind_scale);
    snprintf(detail_line1_, sizeof(detail_line1_), "风向:%s   风速:%s km/h",
             weather.wind_dir, weather.wind_speed);
    detail_line_ = 0;
    detail_swap_ms_ = esp_timer_get_time() / 1000;
    lv_label_set_text(detail_label_, detail_line0_);

    for (int i = 0; i < 3; ++i) {
        const auto& d = weather.days[i];
        if (day_title_[i]) {
            lv_label_set_text(day_title_[i], d.title);
        }
        if (day_date_[i]) {
            lv_label_set_text(day_date_[i], d.date);
        }
        if (day_text_[i]) {
            lv_label_set_text(day_text_[i], d.text);
        }
        StyleWeatherIcon(day_icon_[i], i + 1);
        if (day_temp_[i]) {
            snprintf(buf, sizeof(buf), "温度：%d-%d", d.temp_min, d.temp_max);
            lv_label_set_text(day_temp_[i], buf);
        }
        if (day_hum_[i]) {
            snprintf(buf, sizeof(buf), "湿度：%d", d.humidity);
            lv_label_set_text(day_hum_[i], buf);
        }
        if (day_precip_[i]) {
            snprintf(buf, sizeof(buf), "降水：%d%%", d.precip);
            lv_label_set_text(day_precip_[i], buf);
        }
    }
}
