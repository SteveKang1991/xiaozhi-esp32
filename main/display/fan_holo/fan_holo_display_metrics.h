#ifndef FAN_HOLO_DISPLAY_METRICS_H
#define FAN_HOLO_DISPLAY_METRICS_H

#include <cstdint>
#include <lvgl.h>

/* 板级差异：屏驱、分辨率、MJPEG、音乐封面布局。业务逻辑不放这里。 */
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
    uint16_t idle_mjpeg_h; /* idle 角色顶部再留空；贴图仍贴右下角 */
    uint16_t idle_digit_w;
    uint16_t idle_digit_h;
    uint16_t idle_digit_radius;
    uint16_t idle_pill_h;
    uint16_t idle_role_scale;
    uint16_t idle_date_scale;
    uint8_t idle_date_pad_hor;
    uint8_t idle_date_col_gap;
    uint8_t idle_date_bar_pad;
    uint8_t idle_date_nudge_y;
    uint8_t idle_flip_pad;
    uint16_t idle_flip_font_scale;
    uint8_t idle_clock_style;
    /* 1=预报含降水行；S6 高度不够必须为 0 */
    uint8_t idle_forecast_show_precip;

    static constexpr uint8_t kIdleClockFlipPuhui = 1;
    static constexpr uint8_t kIdleClockLed7Seg = 2;

    static constexpr FanHoloMetrics For55B() {
        return FanHoloMetrics{
            .tag = "FanMIPI55Display",
            .mjpeg_w = 656,
            .mjpeg_h = 1232,
            .mjpeg_idle_w = 480,
            .mjpeg_idle_h = 896,
            .mjpeg_fps = 24,
            .music_cover_w = 720,
            .music_cover_h = 1232,
            .music_cover_top = 48,
            .music_bg_path = "/sdcard/Music/musicbg-720x1232.bin",
            .status_label_w = 160,
            .status_label_max_w = 200,
            .chat_strip_w = 30,
            .cover_box = 486,
            .cover_y = 224,
            .side_pad = 110,
            .lyric_bottom = -420,
            .lyric_next_bottom = -385,
            .song_top = 90,
            .singer_top = 150,
            .bar_bottom = -310,
            .time_bottom = -265,
            .idle_clock_h = 300,
            .idle_weather_w = 240,
            .idle_mjpeg_h = 56,
            .idle_digit_w = 90,
            .idle_digit_h = 136,
            .idle_digit_radius = 10,
            .idle_pill_h = 36,
            .idle_role_scale = 56,
            .idle_date_scale = 256,
            .idle_date_pad_hor = 10,
            .idle_date_col_gap = 6,
            .idle_date_bar_pad = 8,
            .idle_date_nudge_y = 10,
            .idle_flip_pad = 0,
            .idle_flip_font_scale = 256,
            .idle_clock_style = kIdleClockFlipPuhui,
            .idle_forecast_show_precip = 1,
        };
    }

    static constexpr FanHoloMetrics For50B() {
        return FanHoloMetrics{
            .tag = "FanMIPI50Display",
            .mjpeg_w = 416,
            .mjpeg_h = 816,
            .mjpeg_idle_w = 288,
            .mjpeg_idle_h = 560,
            .mjpeg_fps = 24,
            .music_cover_w = 480,
            .music_cover_h = 816,
            .music_cover_top = 38,
            .music_bg_path = "/sdcard/Music/musicbg-480x816.bin",
            .status_label_w = 100,
            .status_label_max_w = 140,
            .chat_strip_w = 24,
            .cover_box = 330,
            .cover_y = 140,
            .side_pad = 70,
            .lyric_bottom = -260,
            .lyric_next_bottom = -250,
            .song_top = 45,
            .singer_top = 75,
            .bar_bottom = -185,
            .time_bottom = -150,
            .idle_clock_h = 236,
            .idle_weather_w = 192,
            .idle_mjpeg_h = 44,
            .idle_digit_w = 52,
            .idle_digit_h = 84,
            .idle_digit_radius = 7,
            .idle_pill_h = 28,
            .idle_role_scale = 56,
            .idle_date_scale = 256,
            .idle_date_pad_hor = 5,
            .idle_date_col_gap = 3,
            .idle_date_bar_pad = 4,
            .idle_date_nudge_y = 0,
            .idle_flip_pad = 3,
            .idle_flip_font_scale = 200,
            .idle_clock_style = kIdleClockFlipPuhui,
            .idle_forecast_show_precip = 1,
        };
    }

    /* 幻影1：无农历；日期胶囊 pad 加宽；预报保留降水 */
    static constexpr FanHoloMetrics ForHuanying1() {
        return FanHoloMetrics{
            .tag = "FanLcd778928Display",
            .mjpeg_w = 240,
            .mjpeg_h = 290,
            .mjpeg_idle_w = 160,
            .mjpeg_idle_h = 208,
            .mjpeg_fps = 24,
            .music_cover_w = 240,
            .music_cover_h = 290,
            .music_cover_top = 0,
            .music_bg_path = nullptr,
            .status_label_w = 96,
            .status_label_max_w = 120,
            .chat_strip_w = 18,
            .cover_box = 120,
            .cover_y = 36,
            .side_pad = 12,
            .lyric_bottom = -115,
            .lyric_next_bottom = -115,
            .song_top = 58,
            .singer_top = 81,
            .bar_bottom = -70,
            .time_bottom = -33,
            .idle_clock_h = 80,
            .idle_weather_w = 120,
            .idle_mjpeg_h = 100,
            .idle_digit_w = 32,
            .idle_digit_h = 48,
            .idle_digit_radius = 5,
            .idle_pill_h = 18,
            .idle_role_scale = 180,
            .idle_date_scale = 256,
            .idle_date_pad_hor = 6,
            .idle_date_col_gap = 2,
            .idle_date_bar_pad = 2,
            .idle_date_nudge_y = 0,
            .idle_flip_pad = 1,
            .idle_flip_font_scale = 90,
            .idle_clock_style = kIdleClockFlipPuhui,
            .idle_forecast_show_precip = 1,
        };
    }

    /* S6：无农历；日期胶囊 pad 与幻影1 同步加宽；预报去掉降水 */
    static constexpr FanHoloMetrics ForS6() {
        return FanHoloMetrics{
            .tag = "FanLcd20Display",
            .mjpeg_w = 288,
            .mjpeg_h = 208,
            .mjpeg_idle_w = 192,
            .mjpeg_idle_h = 144,
            .mjpeg_fps = 24,
            .music_cover_w = 297, /* CONFIG_LCD_CUSTOM 有效宽，非物理 320 */
            .music_cover_h = 240,
            .music_cover_top = 0,
            .music_bg_path = nullptr,
            .status_label_w = 80,
            .status_label_max_w = 100,
            .chat_strip_w = 16,
            .cover_box = 100,
            .cover_y = 16,
            .side_pad = 10,
            .lyric_bottom = -75,       /* 原 -45，上移 30 */
            .lyric_next_bottom = -73,  /* 原 -53，上移 20 */
            .song_top = 51,            /* 原 46，下移 5 */
            .singer_top = 75,          /* 原 65，下移 10 */
            .bar_bottom = -42,         /* 原 -12，上移 30 */
            .time_bottom = -1,         /* 原 19，上移 20 */
            .idle_clock_h = 72,
            .idle_weather_w = 110,
            .idle_mjpeg_h = 70,
            .idle_digit_w = 34,
            .idle_digit_h = 42,
            .idle_digit_radius = 4,
            .idle_pill_h = 18,
            .idle_role_scale = 160,
            .idle_date_scale = 256,
            .idle_date_pad_hor = 6,
            .idle_date_col_gap = 2,
            .idle_date_bar_pad = 2,
            .idle_date_nudge_y = 0,
            .idle_flip_pad = 1,
            .idle_flip_font_scale = 80,
            .idle_clock_style = kIdleClockFlipPuhui,
            .idle_forecast_show_precip = 0,
        };
    }
};

#endif
