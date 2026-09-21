#include "fan_holo_idle_page.h"
#include "fan_holo_display.h"
#include "icons/fan_holo_icon_temp.h"
#include "icons/fan_holo_icon_humidity.h"
#include "icons/fan_holo_icon_precip.h"

#include <cstdio>
#include <cstring>

namespace {

constexpr uint32_t kLunarOrange = 0xF07820;
constexpr uint32_t kCyan = 0x5EEAD4;
constexpr uint32_t kIconGreen = 0x22C55E;
constexpr uint32_t kLightGray = 0xC0C0C0;
constexpr uint32_t kWhite = 0xFFFFFF;
constexpr uint32_t kLedCardBg = 0x1A4854;
constexpr uint32_t kLedCardBorder = 0x3A6A78;
constexpr int kWeatherBelowClock = 8;
/* 预报框与右侧「城市/实时」间距 */
constexpr int kCityForecastGap = 6;
/* 预报框宽固定；文字略缩小便于窄框排字（LVGL 256=1.0） */
constexpr int kForecastBoxW = 78;
constexpr int kForecastPad = 3;
constexpr int32_t kForecastTextScale = 230; /* 温湿度数值 ≈90%；布局高度基准 */
constexpr int32_t kForecastTitleScale = 200; /* 标题布局高度基准 */
/* S6：视觉再放大一点，布局高度仍用上面基准，避免行距被撑开裁切底部 */
constexpr int32_t kForecastTextScaleS6Vis = 252;
constexpr int32_t kForecastTitleScaleS6Vis = 220;
constexpr int kForecastDayGap = 1; /* 天与天之间的行距 */
constexpr int kForecastLineGap = 0; /* 同一天内标题/指标行距 */
constexpr int kMetricIconDisp = 12; /* 布局占位不变 */
constexpr int32_t kMetricIconScale = 128; /* LVGL 256=1.0 */
constexpr int32_t kMetricIconScaleS6 = 152;
constexpr int kS6WeatherNudgeUp = 4;
constexpr int kHuanying1WeatherNudgeUp = 10;
constexpr int kS6ForecastPadLeftExtra = 4;

bool IsS6Board(const FanHoloDisplay& host) {
    return strcmp(host.metrics().tag, "FanLcd20Display") == 0;
}

bool IsHuanying1Board(const FanHoloDisplay& host) {
    return strcmp(host.metrics().tag, "FanLcd778928Display") == 0;
}

void NoScroll(lv_obj_t* obj) {
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
}

void StyleLedCard(lv_obj_t* obj, int radius) {
    lv_obj_set_style_bg_color(obj, lv_color_hex(kLedCardBg), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 2, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(kLedCardBorder), 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(obj, false, 0);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
}

lv_obj_t* MakeLabel(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_label_set_text(lbl, text);
    return lbl;
}

/* layout_scale 决定占位宽高；vis_scale 决定绘制大小。vis>layout 时字更大但不挤行距。 */
void StyleForecastScaledLabel(lv_obj_t* lbl, int line_h, int content_w, int32_t layout_scale,
                              int32_t vis_scale) {
    if (lbl == nullptr) {
        return;
    }
    const int layout_w = (content_w * 256 + layout_scale / 2) / layout_scale;
    lv_obj_set_width(lbl, layout_w > 8 ? layout_w : 8);
    lv_obj_set_height(lbl, (line_h * layout_scale + 255) / 256);
    lv_obj_set_style_transform_scale(lbl, vis_scale, 0);
    lv_obj_set_style_transform_pivot_x(lbl, 0, 0);
    lv_obj_set_style_transform_pivot_y(lbl, 0, 0);
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    NoScroll(lbl);
}

void StyleForecastLabel(lv_obj_t* lbl, int line_h, int content_w, bool s6) {
    const int32_t vis = s6 ? kForecastTextScaleS6Vis : kForecastTextScale;
    StyleForecastScaledLabel(lbl, line_h, content_w, kForecastTextScale, vis);
}

void StyleForecastTitle(lv_obj_t* lbl, int line_h, int content_w, bool s6) {
    const int32_t vis = s6 ? kForecastTitleScaleS6Vis : kForecastTitleScale;
    StyleForecastScaledLabel(lbl, line_h, content_w, kForecastTitleScale, vis);
}

/* 图标 + 数值一行，替代「温度/湿度/降水」文字前缀 */
lv_obj_t* MakeMetricRow(lv_obj_t* parent, const lv_image_dsc_t* icon, const lv_font_t* font,
                        uint32_t color, int line_h, int content_w, bool s6, lv_obj_t** out_label) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, content_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 2, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    NoScroll(row);

    lv_obj_t* img = lv_image_create(row);
    lv_image_set_src(img, icon);
    lv_obj_set_size(img, kMetricIconDisp, kMetricIconDisp);
    lv_image_set_scale(img, s6 ? kMetricIconScaleS6 : kMetricIconScale);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
    lv_obj_set_style_pad_all(img, 0, 0);
    lv_obj_set_style_bg_opa(img, LV_OPA_TRANSP, 0);
    NoScroll(img);

    const int label_w = content_w - kMetricIconDisp - 2;
    *out_label = MakeLabel(row, font, color, "");
    StyleForecastLabel(*out_label, line_h, label_w > 8 ? label_w : 8, s6);
    return row;
}

struct WeatherGeom {
    int x = 0;
    int y = 0;
    int max_w = 120;   /* 天气区最大宽 */
    int forecast_w = 78; /* 预报框宽；S6 = 屏宽 - idle MJPEG - 4 */
    int radius = 8;
    bool side_col = false;
};

/* S6 idle：预报框宽 = 有效屏宽(CONFIG_LCD_CUSTOM=297) - idle MJPEG - 4；其它板固定窄框 */
int ForecastBoxWidth(FanHoloDisplay& host) {
    const auto& m = host.metrics();
    if (strcmp(m.tag, "FanLcd20Display") == 0) {
        const int vid_w = m.mjpeg_idle_w ? static_cast<int>(m.mjpeg_idle_w)
                                         : static_cast<int>(m.mjpeg_w);
        /* screen_width_ = DISPLAY_WIDTH（CUSTOM 297，非物理 320） */
        int w = host.screen_width() - vid_w - 4;
        if (w < kForecastBoxW) {
            w = kForecastBoxW;
        }
        return w;
    }
    return kForecastBoxW;
}

WeatherGeom MakeWeatherGeom(FanHoloDisplay& host, int header_h) {
    const auto& m = host.metrics();
    const int sw = host.screen_width();
    const int sh = host.screen_height();
    const int vid_w = m.mjpeg_idle_w ? m.mjpeg_idle_w : m.mjpeg_w;
    const int vid_h = m.mjpeg_idle_h ? m.mjpeg_idle_h : m.mjpeg_h;

    int rx = sw - vid_w;
    int ry = sh - vid_h;
    if (ry < static_cast<int>(m.idle_mjpeg_h)) {
        ry = static_cast<int>(m.idle_mjpeg_h);
    }
    if (rx < 0) {
        rx = 0;
    }
    if (ry < 0) {
        ry = 0;
    }

    WeatherGeom g;
    g.y = header_h + kWeatherBelowClock;
    if (IsS6Board(host)) {
        g.y -= kS6WeatherNudgeUp; /* 仅天气上移，时钟/MJPEG 不动 */
    } else if (IsHuanying1Board(host)) {
        g.y -= kHuanying1WeatherNudgeUp; /* 预报/城市/实时整体上移 10，MJPEG 不动 */
    }
    if (g.y < 0) {
        g.y = 0;
    }
    g.x = 0;
    g.forecast_w = ForecastBoxWidth(host);
    g.side_col = (rx >= 72);
    /* 城市+实时可越过 MJPEG 上方；预报框右缘距 MJPEG 左缘 4 */
    g.max_w = sw - 4;
    if (g.max_w < g.forecast_w + kCityForecastGap + 64) {
        g.max_w = g.forecast_w + kCityForecastGap + 64;
    }
    g.radius = 8;
    return g;
}

/* 天气锚点：用固定 idle_clock_h，不随日期胶囊加高/时钟下移而改变，避免挤动预报与 MJPEG。 */
int WeatherAnchorY(FanHoloDisplay& host) {
    return static_cast<int>(host.metrics().idle_clock_h);
}

}  // namespace

void FanHoloIdlePage::CreateWeather(FanHoloDisplay& host) {
    auto* theme = host.GetLvglTheme();
    auto* font = theme->text_font()->font();
    const WeatherGeom geom = MakeWeatherGeom(host, WeatherAnchorY(host));
    const int line_h = font != nullptr ? font->line_height : 16;
    const bool s6 = IsS6Board(host);
    s6_weather_tune_ = s6;
    const int pad_left = kForecastPad + (s6 ? kS6ForecastPadLeftExtra : 0);
    const int pad_right = kForecastPad;

    /* 左预报、右城市+实时：同一顶边（原城市名所在 y） */
    weather_root_ = lv_obj_create(screen_);
    lv_obj_remove_style_all(weather_root_);
    lv_obj_add_flag(weather_root_, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(weather_root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(weather_root_, geom.x, geom.y);
    lv_obj_set_size(weather_root_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(weather_root_, geom.max_w, 0);
    lv_obj_set_style_bg_opa(weather_root_, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(weather_root_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(weather_root_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(weather_root_, 0, 0);
    lv_obj_set_style_pad_column(weather_root_, kCityForecastGap, 0);
    lv_obj_add_flag(weather_root_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    NoScroll(weather_root_);

    /* 预测：S6 框宽 = 屏宽 - idle MJPEG - 4，贴齐右下动画；其它板固定窄框 */
    const int forecast_w = geom.forecast_w;
    const int forecast_inner = forecast_w - pad_left - pad_right;
    forecast_box_ = lv_obj_create(weather_root_);
    lv_obj_remove_style_all(forecast_box_);
    lv_obj_set_size(forecast_box_, forecast_w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(forecast_box_, kForecastPad, 0);
    lv_obj_set_style_pad_bottom(forecast_box_, kForecastPad, 0);
    lv_obj_set_style_pad_left(forecast_box_, pad_left, 0);
    lv_obj_set_style_pad_right(forecast_box_, pad_right, 0);
    lv_obj_set_flex_flow(forecast_box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(forecast_box_, kForecastDayGap, 0);
    StyleLedCard(forecast_box_, 5);
    NoScroll(forecast_box_);

    for (int i = 0; i < 3; ++i) {
        lv_obj_t* col = lv_obj_create(forecast_box_);
        lv_obj_remove_style_all(col);
        lv_obj_set_size(col, forecast_inner, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(col, kForecastLineGap, 0);
        lv_obj_add_flag(col, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        NoScroll(col);

        day_title_[i] = MakeLabel(col, font, kLightGray, "");
        day_text_[i] = nullptr;
        StyleForecastTitle(day_title_[i], line_h, forecast_inner, s6);
        MakeMetricRow(col, &fan_holo_icon_temp, font, kLunarOrange, line_h, forecast_inner, s6,
                      &day_temp_[i]);
        MakeMetricRow(col, &fan_holo_icon_humidity, font, kCyan, line_h, forecast_inner, s6,
                      &day_hum_[i]);
        /* S6（FanLcd20Display）高度不够：永不创建降水行。 */
        const bool show_precip = host.metrics().idle_forecast_show_precip != 0 && !s6;
        if (show_precip) {
            MakeMetricRow(col, &fan_holo_icon_precip, font, kIconGreen, line_h, forecast_inner, s6,
                          &day_precip_[i]);
        } else {
            day_precip_[i] = nullptr;
        }
    }

    /* 右侧：城市名 + 实时温度同一行，按内容宽度往右撑开，不裁剪 */
    today_box_ = lv_obj_create(weather_root_);
    lv_obj_remove_style_all(today_box_);
    lv_obj_set_size(today_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(today_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(today_box_, 0, 0);
    lv_obj_set_style_pad_all(today_box_, 0, 0);
    lv_obj_set_flex_flow(today_box_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(today_box_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(today_box_, 4, 0);
    lv_obj_add_flag(today_box_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    NoScroll(today_box_);

    city_label_ = MakeLabel(today_box_, font, kLunarOrange, "");
    lv_label_set_long_mode(city_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(city_label_, LV_SIZE_CONTENT);
    lv_obj_add_flag(city_label_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    NoScroll(city_label_);

    now_label_ = MakeLabel(today_box_, font, kWhite, "--°");
    lv_label_set_long_mode(now_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(now_label_, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(now_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_add_flag(now_label_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    NoScroll(now_label_);
}

void FanHoloIdlePage::LayoutWeather(FanHoloDisplay& host) {
    if (weather_root_ == nullptr) {
        return;
    }
    const WeatherGeom geom = MakeWeatherGeom(host, WeatherAnchorY(host));
    const bool s6 = IsS6Board(host);
    s6_weather_tune_ = s6;
    const int pad_left = kForecastPad + (s6 ? kS6ForecastPadLeftExtra : 0);
    const int pad_right = kForecastPad;

    lv_obj_set_pos(weather_root_, geom.x, geom.y);
    lv_obj_set_size(weather_root_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(weather_root_, geom.max_w, 0);
    if (forecast_box_) {
        const int forecast_w = geom.forecast_w;
        const int forecast_inner = forecast_w - pad_left - pad_right;
        const int label_w = forecast_inner - kMetricIconDisp - 2;
        lv_obj_set_size(forecast_box_, forecast_w, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_left(forecast_box_, pad_left, 0);
        lv_obj_set_style_pad_right(forecast_box_, pad_right, 0);
        auto* theme = host.GetLvglTheme();
        auto* font = theme != nullptr ? theme->text_font()->font() : nullptr;
        const int line_h = font != nullptr ? font->line_height : 16;
        for (int i = 0; i < 3; ++i) {
            if (day_title_[i] != nullptr) {
                if (lv_obj_t* col = lv_obj_get_parent(day_title_[i])) {
                    lv_obj_set_size(col, forecast_inner, LV_SIZE_CONTENT);
                }
                StyleForecastTitle(day_title_[i], line_h, forecast_inner, s6);
            }
            auto resize_metric = [forecast_inner, label_w, line_h, s6](lv_obj_t* lbl) {
                if (lbl == nullptr) {
                    return;
                }
                if (lv_obj_t* row = lv_obj_get_parent(lbl)) {
                    lv_obj_set_size(row, forecast_inner, LV_SIZE_CONTENT);
                    uint32_t n = lv_obj_get_child_cnt(row);
                    for (uint32_t ci = 0; ci < n; ++ci) {
                        lv_obj_t* child = lv_obj_get_child(row, ci);
                        if (lv_obj_check_type(child, &lv_image_class)) {
                            lv_image_set_scale(child, s6 ? kMetricIconScaleS6 : kMetricIconScale);
                        }
                    }
                }
                StyleForecastLabel(lbl, line_h, label_w > 8 ? label_w : 8, s6);
            };
            resize_metric(day_temp_[i]);
            resize_metric(day_hum_[i]);
            resize_metric(day_precip_[i]);
        }
    }
    lv_obj_update_layout(weather_root_);
    lv_obj_move_foreground(weather_root_);
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
    LayoutWeather(host);
    status_bar_.RaiseOverlays();
}

void FanHoloIdlePage::Destroy() {
    for (int i = 0; i < 6; ++i) {
        lv_anim_delete(&status_bar_.digits[i], nullptr);
        status_bar_.digits[i] = {};
    }
    if (screen_ != nullptr) {
        lv_obj_del(screen_);
        screen_ = nullptr;
    }
    container_ = nullptr;
    preview_image_ = nullptr;
    weather_root_ = nullptr;
    today_box_ = nullptr;
    city_label_ = nullptr;
    now_label_ = nullptr;
    forecast_box_ = nullptr;
    for (int i = 0; i < 3; ++i) {
        day_title_[i] = nullptr;
        day_text_[i] = nullptr;
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
    LayoutWeather(host);
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
    paint(now_label_, kWhite);
    auto* theme_font = font;
    const int line_h = theme_font != nullptr ? theme_font->line_height : 16;
    const bool s6 = s6_weather_tune_;
    const int pad_left = kForecastPad + (s6 ? kS6ForecastPadLeftExtra : 0);
    const int pad_right = kForecastPad;
    const int forecast_w = (forecast_box_ != nullptr) ? static_cast<int>(lv_obj_get_width(forecast_box_))
                                                      : kForecastBoxW;
    const int forecast_inner = forecast_w - pad_left - pad_right;
    const int label_w = forecast_inner - kMetricIconDisp - 2;
    for (int i = 0; i < 3; ++i) {
        paint(day_title_[i], kLightGray);
        paint(day_temp_[i], kLunarOrange);
        paint(day_hum_[i], kCyan);
        paint(day_precip_[i], kIconGreen);
        StyleForecastTitle(day_title_[i], line_h, forecast_inner, s6);
        StyleForecastLabel(day_temp_[i], line_h, label_w > 8 ? label_w : 8, s6);
        StyleForecastLabel(day_hum_[i], line_h, label_w > 8 ? label_w : 8, s6);
        StyleForecastLabel(day_precip_[i], line_h, label_w > 8 ? label_w : 8, s6);
    }
}

void FanHoloIdlePage::ApplyClockStyle(FanHoloDisplay& host) {
    if (screen_ == nullptr) {
        return;
    }
    status_bar_.RecreateIdleClock(screen_, host);
    LayoutWeather(host);
    status_bar_.RaiseOverlays();
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
    const bool was_hidden = lv_obj_has_flag(weather_root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(weather_root_, LV_OBJ_FLAG_HIDDEN);
    if (was_hidden) {
        lv_obj_move_foreground(weather_root_);
        status_bar_.RaiseOverlays();
    }

    lv_label_set_text(city_label_, weather.city[0] != '\0' ? weather.city : "--");

    /* 温度 + 天气紧跟城市名同一行，例如「25° 晴转多云」；不截断长天气名 */
    char now_buf[64];
    snprintf(now_buf, sizeof(now_buf), "%d° %s", weather.temp,
             weather.text[0] != '\0' ? weather.text : "--");
    lv_label_set_text(now_label_, now_buf);

    char buf[48];
    for (int i = 0; i < 3; ++i) {
        const auto& d = weather.days[i];
        if (day_title_[i]) {
            /* S6：「今天  雷阵雨」隔两空格；其它板一空格 */
            if (s6_weather_tune_) {
                snprintf(buf, sizeof(buf), "%s  %s", d.title,
                         d.text[0] != '\0' ? d.text : "--");
            } else {
                snprintf(buf, sizeof(buf), "%s %s", d.title,
                         d.text[0] != '\0' ? d.text : "--");
            }
            lv_label_set_text(day_title_[i], buf);
        }
        if (day_temp_[i]) {
            snprintf(buf, sizeof(buf), " %d-%d", d.temp_min, d.temp_max);
            lv_label_set_text(day_temp_[i], buf);
        }
        if (day_hum_[i]) {
            snprintf(buf, sizeof(buf), " %d", d.humidity);
            lv_label_set_text(day_hum_[i], buf);
        }
        if (day_precip_[i]) {
            snprintf(buf, sizeof(buf), " %d%%", d.precip);
            lv_label_set_text(day_precip_[i], buf);
        }
    }
}
