#ifndef FAN_HOLO_WEATHER_H
#define FAN_HOLO_WEATHER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <lvgl.h>

/* idle 天气：now 实时 + daily 今/明/后。 */
struct IdleWeatherDay {
    char title[16];
    char date[8];
    char text[24];
    int icon = 0;
    int temp_max = 0;
    int temp_min = 0;
    int humidity = 0;
    int precip = 0;
};

struct IdleWeatherView {
    char city[32];
    char text[16];
    char wind_dir[24];
    char wind_scale[8];
    char wind_speed[16];
    int temp = 0;
    int icon = 0;
    int humidity = 0;
    int precip = 0;
    IdleWeatherDay days[3]{};
    bool valid = false;
};

const char* FanHoloWeatherIconLabel(int icon_id);
unsigned int FanHoloWeatherIconColor(int icon_id);
void FanHoloWeatherIconSdPath(int icon_id, char* dst, size_t dst_len);
void FanHoloPrepareWeatherIcons(const IdleWeatherView& view);
const lv_image_dsc_t* FanHoloWeatherIconDsc(int slot);
bool FanHoloFetchWeather(const std::string& city, IdleWeatherView* out);

#endif
