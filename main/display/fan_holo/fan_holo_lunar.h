#ifndef FAN_HOLO_LUNAR_H
#define FAN_HOLO_LUNAR_H

#include <cstddef>

/* 公历转农历（1900-2049），按 1900-01-31=正月初一 的标准日差算法。 */
struct FanHoloLunarDate {
    int year = 0;
    int month = 1; /* 1-12 */
    int day = 1;   /* 1-30 */
    bool is_leap = false;
};

FanHoloLunarDate FanHoloSolarToLunar(int year, int month, int day);
void FanHoloFormatLunarYear(int year, int month, int day, char* out, size_t out_len);
void FanHoloFormatLunarParts(int year, int month, int day, char* month_out, size_t month_len, char* day_out, size_t day_len);
const char* FanHoloWeekdayCn(int tm_wday);

#endif
