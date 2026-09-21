#ifndef FAN_HOLO_IDLE_PAGE_H
#define FAN_HOLO_IDLE_PAGE_H

#include "fan_holo_status_bar.h"
#include "fan_holo_weather.h"

class FanHoloDisplay;

/* 待命页：顶栏翻页时钟 + 左上天气短圆角框 + 右下角角色。 */
class FanHoloIdlePage {
public:
    void Create(FanHoloDisplay& host);
    void Destroy();
    void Show(FanHoloDisplay& host);
    void Bind(FanHoloDisplay& host) const;
    void HideRole();
    void Tick();
    void ApplyTextFont(const lv_font_t* font, lv_color_t color);
    void ApplyWeather(const IdleWeatherView& weather);
    void ApplyClockStyle(FanHoloDisplay& host);
    FanHoloRoleWidgets& role_widgets() { return role_; }

    lv_obj_t* screen() const { return screen_; }
    FanHoloStatusBar& status_bar() { return status_bar_; }
    const FanHoloStatusBar& status_bar() const { return status_bar_; }

private:
    void CreateWeather(FanHoloDisplay& host);
    void LayoutWeather(FanHoloDisplay& host);

    lv_obj_t* screen_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    FanHoloStatusBar status_bar_;
    FanHoloRoleWidgets role_;

    lv_obj_t* weather_root_ = nullptr;
    lv_obj_t* today_box_ = nullptr;   /* 预报右侧：城市名 + 实时温度 */
    lv_obj_t* city_label_ = nullptr;
    lv_obj_t* now_label_ = nullptr; /* 与城市同一行：温度 + 天气 */

    lv_obj_t* forecast_box_ = nullptr;
    lv_obj_t* day_title_[3]{};
    lv_obj_t* day_text_[3]{};
    lv_obj_t* day_temp_[3]{};
    lv_obj_t* day_hum_[3]{};
    lv_obj_t* day_precip_[3]{};

    IdleWeatherView last_weather_{};
    bool s6_weather_tune_ = false;
};

#endif
