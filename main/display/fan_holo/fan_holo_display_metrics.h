#ifndef FAN_HOLO_DISPLAY_METRICS_H
#define FAN_HOLO_DISPLAY_METRICS_H

#include <cstdint>
#include <lvgl.h>

/* 55B / 5B 板级差异：屏驱、分辨率、MJPEG、音乐封面布局。业务逻辑不放这里。 */
struct FanHoloMetrics {
    const char* tag;

    uint16_t mjpeg_w;
    uint16_t mjpeg_h;
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
    /* 1=翻页普惠字体  2=圆角框七段数码管（DesktopClock Font7） */
    uint8_t idle_clock_style;

    static constexpr uint8_t kIdleClockFlipPuhui = 1;
    static constexpr uint8_t kIdleClockLed7Seg = 2;

    static constexpr FanHoloMetrics For55B() {
        return FanHoloMetrics{
            "FanMIPI55Display",
            656, 1232, 24,
            720, 1232, 48, "/sdcard/Music/musicbg-720x1232.bin",
            160, 200, 30,
            486, 224, 110, -420, -385, 90, 150, -310, -265,
            300, 200, 56, 90, 136, 10, 36, 56,
            kIdleClockFlipPuhui,
        };
    }

    static constexpr FanHoloMetrics For50B() {
        return FanHoloMetrics{
            "FanMIPI50Display",
            416, 816, 24,
            480, 816, 38, "/sdcard/Music/musicbg-480x816.bin",
            100, 140, 24,
            330, 140, 70, -260, -250, 30, 55, -110, -80,
            220, 140, 44, 58, 94, 7, 28, 56,
            kIdleClockFlipPuhui,
        };
    }
};

#endif
