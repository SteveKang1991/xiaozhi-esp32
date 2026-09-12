#ifndef FAN_HOLO_IDLE_PAGE_H
#define FAN_HOLO_IDLE_PAGE_H

#include "fan_holo_status_bar.h"
#include "fan_holo_weather.h"

class FanHoloDisplay;

/* 待命页：顶栏翻页时钟 + 时钟下今日天气 + 左侧后三天 + 右下角缩小角色。 */
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
    FanHoloRoleWidgets& role_widgets() { return role_; }

    lv_obj_t* screen() const { return screen_; }
    FanHoloStatusBar& status_bar() { return status_bar_; }
    const FanHoloStatusBar& status_bar() const { return status_bar_; }

private:
    void CreateWeather(FanHoloDisplay& host);
    void StyleWeatherIcon(lv_obj_t* icon, int slot);

    lv_obj_t* screen_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    FanHoloStatusBar status_bar_;
    FanHoloRoleWidgets role_;

    lv_obj_t* weather_root_ = nullptr;
    lv_obj_t* detail_card_ = nullptr;
    lv_obj_t* today_box_ = nullptr;
    lv_obj_t* city_label_ = nullptr;
    lv_obj_t* temp_label_ = nullptr;
    lv_obj_t* text_label_ = nullptr;
    lv_obj_t* today_icon_ = nullptr;
    lv_obj_t* detail_label_ = nullptr;
    char detail_line0_[96]{};
    char detail_line1_[80]{};
    uint8_t detail_line_ = 0;
    int64_t detail_swap_ms_ = 0;

    lv_obj_t* forecast_box_ = nullptr;
    lv_obj_t* day_title_[3]{};
    lv_obj_t* day_date_[3]{};
    lv_obj_t* day_text_[3]{};
    lv_obj_t* day_icon_[3]{};
    lv_obj_t* day_temp_[3]{};
    lv_obj_t* day_hum_[3]{};
    lv_obj_t* day_precip_[3]{};

    IdleWeatherView last_weather_{};
};

#endif
