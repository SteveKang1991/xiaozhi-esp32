#include "fan_holo_idle_page.h"
#include "fan_holo_display.h"

#include <cstdio>

namespace {

constexpr uint32_t kOrange = 0xF5A623;
constexpr uint32_t kLunarOrange = 0xF07820;
constexpr uint32_t kCyan = 0x5EEAD4;
constexpr uint32_t kIconGreen = 0x22C55E;
constexpr uint32_t kLightGray = 0xC0C0C0;
constexpr uint32_t kWhite = 0xFFFFFF;

void NoScroll(lv_obj_t* obj) {
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
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
    const int left_w = static_cast<int>(metrics.idle_weather_w);

    weather_root_ = lv_obj_create(screen_);
    lv_obj_remove_style_all(weather_root_);
    lv_obj_add_flag(weather_root_, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(weather_root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(weather_root_, 0, clock_h - 26);
    lv_obj_set_size(weather_root_, host.screen_width(), host.screen_height() - clock_h + 26);
    lv_obj_set_style_bg_opa(weather_root_, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(weather_root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(weather_root_, 8, 0);
    lv_obj_set_style_pad_right(weather_root_, 8, 0);
    lv_obj_set_style_pad_top(weather_root_, 10, 0);
    lv_obj_set_style_pad_row(weather_root_, 12, 0);
    lv_obj_add_flag(weather_root_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    NoScroll(weather_root_);

    city_label_ = MakeLabel(weather_root_, font, kLunarOrange, "");
    lv_obj_add_flag(city_label_, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(city_label_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(city_label_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_pos(city_label_, 8, 0);
    lv_obj_set_height(city_label_, font->line_height);
    NoScroll(city_label_);

    today_box_ = lv_obj_create(weather_root_);
    lv_obj_remove_style_all(today_box_);
    lv_obj_set_size(today_box_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(today_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(today_box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(today_box_, 4, 0);
    lv_obj_set_style_pad_top(today_box_, font->line_height + 4, 0);
    lv_obj_set_style_translate_y(today_box_, -5, 0);
    lv_obj_add_flag(today_box_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    NoScroll(today_box_);

    lv_obj_t* mid = lv_obj_create(today_box_);
    lv_obj_remove_style_all(mid);
    lv_obj_set_size(mid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(mid, 12, 0);
    lv_obj_add_flag(mid, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
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

    today_icon_ = lv_obj_create(mid);
    lv_obj_remove_style_all(today_icon_);
    lv_obj_set_size(today_icon_, 44, 44);
    lv_obj_set_style_radius(today_icon_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(today_icon_, lv_color_hex(kOrange), 0);
    lv_obj_set_style_bg_opa(today_icon_, LV_OPA_COVER, 0);
    lv_obj_set_style_translate_x(today_icon_, 30, 0);
    lv_obj_set_style_translate_y(today_icon_, -20, 0);
    NoScroll(today_icon_);
    today_icon_mark_ = MakeLabel(today_icon_, font, 0x1A1A1A, "");
    lv_obj_center(today_icon_mark_);

    // Own layer next to the city name — not inside the scaled-temp row, or
    // translate_y pulls the line under the clock and it disappears.
    detail_label_ = MakeLabel(today_box_, font, kWhite, "");
    lv_obj_add_flag(detail_label_, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(detail_label_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_pos(detail_label_, host.screen_width()/3, 0);
    lv_obj_set_size(detail_label_, host.screen_width()/1.5, font->line_height);
    lv_obj_set_style_pad_right(detail_label_, 30, 0);
    lv_label_set_long_mode(detail_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_time(detail_label_, 18000, 0);

    forecast_box_ = lv_obj_create(weather_root_);
    lv_obj_remove_style_all(forecast_box_);
    lv_obj_set_size(forecast_box_, left_w, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(forecast_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_top(forecast_box_, 60, 0);
    lv_obj_set_flex_flow(forecast_box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(forecast_box_, 18, 0);
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

        day_icon_[i] = lv_obj_create(row);
        lv_obj_remove_style_all(day_icon_[i]);
        lv_obj_set_size(day_icon_[i], 36, 36);
        lv_obj_set_style_radius(day_icon_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(day_icon_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(day_icon_[i], lv_color_hex(kOrange), 0);
        lv_obj_set_style_translate_y(day_icon_[i], -60, 0);
        NoScroll(day_icon_[i]);
        day_icon_mark_[i] = MakeLabel(day_icon_[i], font, 0x1A1A1A, "");
        lv_obj_center(day_icon_mark_[i]);
    }
}

void FanHoloIdlePage::StyleWeatherIcon(lv_obj_t* icon, lv_obj_t* mark, int icon_id) {
    if (icon == nullptr || mark == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(icon, lv_color_hex(FanHoloWeatherIconColor(icon_id)), 0);
    lv_label_set_text(mark, FanHoloWeatherIconLabel(icon_id));
    lv_obj_center(mark);
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
    today_box_ = nullptr;
    forecast_box_ = nullptr;
    city_label_ = nullptr;
    temp_label_ = nullptr;
    text_label_ = nullptr;
    today_icon_ = nullptr;
    today_icon_mark_ = nullptr;
    detail_label_ = nullptr;
    for (int i = 0; i < 3; ++i) {
        day_title_[i] = nullptr;
        day_date_[i] = nullptr;
        day_icon_[i] = nullptr;
        day_icon_mark_[i] = nullptr;
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
    paint(today_icon_mark_, 0x1A1A1A);
    paint(detail_label_, kWhite);
    for (int i = 0; i < 3; ++i) {
        paint(day_title_[i], kLightGray);
        paint(day_date_[i], kOrange);
        paint(day_icon_mark_[i], 0x1A1A1A);
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
        return;
    }
    lv_obj_remove_flag(weather_root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(weather_root_);
    status_bar_.RaiseOverlays();

    lv_label_set_text(city_label_, weather.city);
    char buf[160];
    snprintf(buf, sizeof(buf), "%d", weather.temp);
    lv_label_set_text(temp_label_, buf);
    lv_label_set_text(text_label_, weather.text);
    StyleWeatherIcon(today_icon_, today_icon_mark_, weather.icon);

    snprintf(buf, sizeof(buf), "湿度：%d    降水：%d%%    风向：%s    风力：%s    风速：%s km/h    ",
             weather.humidity, weather.precip, weather.wind_dir, weather.wind_scale,
             weather.wind_speed);
    lv_label_set_text(detail_label_, buf);

    for (int i = 0; i < 3; ++i) {
        const auto& d = weather.days[i];
        if (day_title_[i]) {
            lv_label_set_text(day_title_[i], d.title);
        }
        if (day_date_[i]) {
            lv_label_set_text(day_date_[i], d.date);
        }
        StyleWeatherIcon(day_icon_[i], day_icon_mark_[i], d.icon);
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
