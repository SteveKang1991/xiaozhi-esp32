#ifndef FAN_HOLO_DISPLAY_METRICS_H
#define FAN_HOLO_DISPLAY_METRICS_H

#include <cstdint>
#include <lvgl.h>

/* 55B / 5B 板级差异：屏驱、分辨率、MJPEG、音乐封面布局。业务逻辑不放这里。 */
struct FanHoloMetrics {
    const char* tag;

    uint16_t mjpeg_w;
    uint16_t mjpeg_h;
    uint16_t mjpeg_idle_w;
    uint16_t mjpeg_idle_h;
    uint8_t mjpeg_fps;

    uint16_t music_cover_w;
    uint16_t music_cover_h;
    uint16_t music_cover_top;
    const char* music_bg_path;

    uint16_t status_label_w;
    uint16_t status_label_max_w;
    uint16_t chat_strip_w;

    lv_coord_t cover_box;
    lv_coord_t cover_y;
    lv_coord_t side_pad;
    lv_coord_t lyric_bottom;
    lv_coord_t lyric_next_bottom;
    lv_coord_t song_top;
    lv_coord_t singer_top;
    lv_coord_t bar_bottom;
    lv_coord_t time_bottom;

    uint16_t idle_clock_h;
    uint16_t idle_weather_w;
    uint16_t idle_mjpeg_h; /* idle 角色顶部再留空，避免盖住时钟；贴图仍贴右下角 */
    uint16_t idle_digit_w;
    uint16_t idle_digit_h;
    uint16_t idle_digit_radius;
    uint16_t idle_pill_h;
    uint16_t idle_role_scale; /* 256=1.0，仅 idle 角色 GIF */
    uint16_t idle_date_scale; /* 日期栏整体缩放，256=1.0 */
    uint8_t idle_date_pad_hor;
    uint8_t idle_date_col_gap;
    uint8_t idle_date_bar_pad;
    uint8_t idle_flip_pad; /* 翻页卡片单边留边 */
    uint16_t idle_flip_font_scale; /* 翻页数字缩放，256=1.0，不改变卡片 */
    uint8_t weather_icon_today;
    uint8_t weather_icon_day;
    int8_t weather_icon_nudge_y;
    int8_t weather_icon_nudge_x;
    uint8_t weather_detail_pad_left;
    /* 1=翻页普惠字体  2=圆角框七段数码管（DesktopClock Font7） */
    uint8_t idle_clock_style;

    static constexpr uint8_t kIdleClockFlipPuhui = 1;
    static constexpr uint8_t kIdleClockLed7Seg = 2;

    static constexpr FanHoloMetrics For55B() {
        return FanHoloMetrics{
            "FanMIPI55Display",
            656, 1232, 480, 896, 24,
            720, 1232, 48, "/sdcard/Music/musicbg-720x1232.bin",
            160, 200, 30,
            486, 224, 110, -420, -385, 90, 150, -310, -265,
            300, 240, 56, 90, 136, 10, 36, 56,
            256, 10, 6, 8, 0, 256, 96, 62, -60, -2, 18,
            kIdleClockFlipPuhui,
        };
    }

    static constexpr FanHoloMetrics For50B() {
        return FanHoloMetrics{
            "FanMIPI50Display",
            416, 816, 288, 560, 24,
            480, 816, 38, "/sdcard/Music/musicbg-480x816.bin",
            100, 140, 24,
            330, 140, 70, -260, -250, 45, 75, -185, -150,
            236, 192, 44, 52, 84, 7, 28, 56,
            256, 5, 3, 4, 3, 200, 64, 44, -24, -2, 9,
            kIdleClockFlipPuhui,
        };
    }
};

#endif
