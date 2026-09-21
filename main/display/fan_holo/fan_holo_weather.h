#ifndef FAN_HOLO_WEATHER_H
#define FAN_HOLO_WEATHER_H

#include <string>

/* idle 天气：now 实时 + daily 今/明/后（仅文字，无图标）。 */
struct IdleWeatherDay {
    char title[16];
    char date[8];
    char text[24];
    int temp_max = 0;
    int temp_min = 0;
    int humidity = 0;
    int precip = 0;
};

struct IdleWeatherView {
    char city[32];
    char text[16];
    int temp = 0;
    int humidity = 0;
    int precip = 0;
    IdleWeatherDay days[3]{};
    bool valid = false;
};

bool FanHoloFetchWeather(const std::string& city, IdleWeatherView* out);

#endif
