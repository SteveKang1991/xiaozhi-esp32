#include "fan_holo_lunar.h"

#include <cstdio>

namespace {

/* 标准农历表 1900-2049（与常见 calendar 库一致） */
static const unsigned int kLunarInfo[] = {
    0x04bd8, 0x04ae0, 0x0a570, 0x054d5, 0x0d260, 0x0d950, 0x16554, 0x056a0, 0x09ad0, 0x055d2,
    0x04ae0, 0x0a5b6, 0x0a4d0, 0x0d250, 0x1d255, 0x0b540, 0x0d6a0, 0x0ada2, 0x095b0, 0x14977,
    0x04970, 0x0a4b0, 0x0b4b5, 0x06a50, 0x06d40, 0x1ab54, 0x02b60, 0x09570, 0x052f2, 0x04970,
    0x06566, 0x0d4a0, 0x0ea50, 0x06e95, 0x05ad0, 0x02b60, 0x186e3, 0x092e0, 0x1c8d7, 0x0c950,
    0x0d4a0, 0x1d8a6, 0x0b550, 0x056a0, 0x1a5b4, 0x025d0, 0x092d0, 0x0d2b2, 0x0a950, 0x0b557,
    0x06ca0, 0x0b550, 0x15355, 0x04da0, 0x0a5d0, 0x14573, 0x052d0, 0x0a9a8, 0x0e950, 0x06aa0,
    0x0aea6, 0x0ab50, 0x04b60, 0x0aae4, 0x0a570, 0x05260, 0x0f263, 0x0d950, 0x05b57, 0x056a0,
    0x096d0, 0x04dd5, 0x04ad0, 0x0a4d0, 0x0d4d4, 0x0d250, 0x0d558, 0x0b540, 0x0b5a0, 0x195a6,
    0x095b0, 0x049b0, 0x0a974, 0x0a4b0, 0x0b27a, 0x06a50, 0x06d40, 0x0af46, 0x0ab60, 0x09570,
    0x04af5, 0x04970, 0x064b0, 0x074a3, 0x0ea50, 0x06b58, 0x055c0, 0x0ab60, 0x096d5, 0x092e0,
    0x0c960, 0x0d954, 0x0d4a0, 0x0da50, 0x07552, 0x056a0, 0x0abb7, 0x025d0, 0x092d0, 0x0cab5,
    0x0a950, 0x0b4a0, 0x0baa4, 0x0ad50, 0x055d9, 0x04ba0, 0x0a5b0, 0x15176, 0x052b0, 0x0a930,
    0x07954, 0x06aa0, 0x0ad50, 0x05b52, 0x04b60, 0x0a6e6, 0x0a4e0, 0x0d260, 0x0ea65, 0x0d530,
    0x05aa0, 0x076a3, 0x096d0, 0x04bd7, 0x04ad0, 0x0a4d0, 0x1d0b6, 0x0d250, 0x0d520, 0x0dd45,
    0x0b5a0, 0x056d0, 0x055b2, 0x049b0, 0x0a577, 0x0a4b0, 0x0aa50, 0x1b255, 0x06d20, 0x0ada0};

static const char* kLunarMonths[] = {"正", "二", "三", "四", "五", "六", "七", "八", "九", "十", "冬", "腊"};
static const char* kLunarDays[] = {
    "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
    "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
    "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十"};
static const char* kTianGan[] = {"甲", "乙", "丙", "丁", "戊", "己", "庚", "辛", "壬", "癸"};
static const char* kDiZhi[] = {"子", "丑", "寅", "卯", "辰", "巳", "午", "未", "申", "酉", "戌", "亥"};
static const char* kWeekdays[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};

bool GregorianLeap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int LeapMonth(int year) {
    return kLunarInfo[year - 1900] & 0xf;
}

int LeapDays(int year) {
    if (LeapMonth(year)) {
        return (kLunarInfo[year - 1900] & 0x10000) ? 30 : 29;
    }
    return 0;
}

/* m: 1-12 */
int MonthDays(int year, int month) {
    return (kLunarInfo[year - 1900] & (0x10000 >> month)) ? 30 : 29;
}

int LunarYearDays(int year) {
    int sum = 348;
    for (int i = 0x8000; i > 0x8; i >>= 1) {
        if (kLunarInfo[year - 1900] & i) {
            sum++;
        }
    }
    return sum + LeapDays(year);
}

/* 距 1900-01-31（农历 1900 正月初一）的天数 */
int DaysFromEpoch(int year, int month, int day) {
    int days = 0;
    for (int y = 1900; y < year; y++) {
        days += GregorianLeap(y) ? 366 : 365;
    }
    static const int kMd[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (int m = 1; m < month; m++) {
        days += kMd[m];
    }
    if (month > 2 && GregorianLeap(year)) {
        days++;
    }
    days += day;
    return days - 31;
}

}  // namespace

FanHoloLunarDate FanHoloSolarToLunar(int year, int month, int day) {
    FanHoloLunarDate result = {year, 1, 1, false};
    if (year < 1900 || year > 2049 || month < 1 || month > 12) {
        return result;
    }

    int offset = DaysFromEpoch(year, month, day);
    if (offset < 0) {
        return result;
    }

    int lunar_year = 1900;
    for (; lunar_year < 2050; lunar_year++) {
        int yd = LunarYearDays(lunar_year);
        if (offset < yd) {
            break;
        }
        offset -= yd;
    }

    const int leap_month = LeapMonth(lunar_year);
    bool is_leap = false;
    int i = 1;
    int temp = 0;
    for (i = 1; i < 13 && offset > 0; i++) {
        if (leap_month > 0 && i == (leap_month + 1) && !is_leap) {
            --i;
            is_leap = true;
            temp = LeapDays(lunar_year);
        } else {
            temp = MonthDays(lunar_year, i);
        }
        offset -= temp;
        if (is_leap && i == (leap_month + 1)) {
            is_leap = false;
        }
    }
    if (offset == 0 && leap_month > 0 && i == leap_month + 1) {
        if (is_leap) {
            is_leap = false;
        } else {
            is_leap = true;
            --i;
        }
    }
    if (offset < 0) {
        offset += temp;
        --i;
    }

    result.year = lunar_year;
    result.month = i;
    result.day = offset + 1;
    result.is_leap = is_leap;
    if (result.month < 1) {
        result.month = 1;
    }
    if (result.month > 12) {
        result.month = 12;
    }
    if (result.day < 1) {
        result.day = 1;
    }
    if (result.day > 30) {
        result.day = 30;
    }
    return result;
}

void FanHoloFormatLunarYear(int year, int month, int day, char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    out[0] = '\0';
    FanHoloLunarDate lunar = FanHoloSolarToLunar(year, month, day);
    if (lunar.year < 1900) {
        return;
    }
    const int gan = (lunar.year - 4) % 10;
    const int zhi = (lunar.year - 4) % 12;
    snprintf(out, out_len, "%s%s年", kTianGan[gan], kDiZhi[zhi]);
}

void FanHoloFormatLunarParts(int year, int month, int day, char* month_out, size_t month_len, char* day_out, size_t day_len) {
    if (month_out && month_len) {
        month_out[0] = '\0';
    }
    if (day_out && day_len) {
        day_out[0] = '\0';
    }
    FanHoloLunarDate lunar = FanHoloSolarToLunar(year, month, day);
    if (lunar.month < 1 || lunar.month > 12 || lunar.day < 1 || lunar.day > 30) {
        return;
    }
    if (month_out && month_len) {
        snprintf(month_out, month_len, "%s%s月", lunar.is_leap ? "闰" : "", kLunarMonths[lunar.month - 1]);
    }
    if (day_out && day_len) {
        snprintf(day_out, day_len, "%s", kLunarDays[lunar.day - 1]);
    }
}

const char* FanHoloWeekdayCn(int tm_wday) {
    if (tm_wday < 0 || tm_wday > 6) {
        return "星期日";
    }
    return kWeekdays[tm_wday];
}
