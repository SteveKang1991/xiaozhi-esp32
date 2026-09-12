#ifndef FAN_HOLO_STATUS_BAR_H
#define FAN_HOLO_STATUS_BAR_H

#include <lvgl.h>

class FanHoloDisplay;

/* 方案2：七段数码管单字。 */
struct FanHoloSegDigit {
    lv_obj_t* box = nullptr;
    lv_obj_t* seg[7]{};
    char current = 0;
};

/* 方案1：白底挂历翻页卡 + 普惠数字。 */
struct FanHoloFlipDigit {
    lv_obj_t* card = nullptr;
    lv_obj_t* top_clip = nullptr;
    lv_obj_t* top_lbl = nullptr;
    lv_obj_t* bot_clip = nullptr;
    lv_obj_t* bot_lbl = nullptr;
    lv_obj_t* flap = nullptr;
    lv_obj_t* flap_lbl = nullptr;
    lv_obj_t* hinge = nullptr;
    lv_obj_t* shade = nullptr;
    int32_t label_ofs_y = 0;
    int32_t digit_w = 0;
    int32_t digit_h = 0;
    int32_t digit_half = 0;
    int32_t card_pad = 0;
    int32_t font_scale = 256;
    char current = '0';
    char target = 0;
    char pending = 0;
    char old_ch = '0';
    bool flap_lower = false;
    bool animating = false;
};

/* Idle：日期胶囊 + 时钟（方案1翻页 / 方案2数码管）。Chat / Music：左侧状态。 */
struct FanHoloStatusBar {
    enum class Kind { IdleFull, StatusLeft };

    lv_obj_t* top_bar = nullptr;
    lv_obj_t* status_label = nullptr;
    lv_obj_t* status_pill = nullptr;
    lv_obj_t* notification_label = nullptr;
    lv_obj_t* mute_label = nullptr;
    lv_obj_t* network_label = nullptr;
    lv_obj_t* battery_label = nullptr;
    lv_obj_t* low_battery_popup = nullptr;
    lv_obj_t* low_battery_label = nullptr;
    lv_obj_t* volume_overlay = nullptr;
    lv_obj_t* volume_bar = nullptr;
    lv_obj_t* volume_label = nullptr;

    lv_obj_t* year_label = nullptr;
    lv_obj_t* month_label = nullptr;
    lv_obj_t* mday_label = nullptr;
    lv_obj_t* weekday_label = nullptr;
    lv_obj_t* lunar_year_label = nullptr;
    lv_obj_t* lunar_month_label = nullptr;
    lv_obj_t* lunar_label = nullptr;

    FanHoloFlipDigit digits[6]{};
    lv_obj_t* clock_card = nullptr;
    FanHoloSegDigit hm_digits[4]{};
    FanHoloSegDigit sec_digits[2]{};
    int last_hm = -1;
    int last_sec = -1;
    bool clock_primed = false;
    int last_yday = -1;
    int status_scroll_w = 176;

    void Create(lv_obj_t* screen, FanHoloDisplay& host, Kind kind);
    void Bind(FanHoloDisplay& host) const;
    void RaiseOverlays() const;
    void SetStatusText(const char* status);
    void ApplyTextFont(const lv_font_t* font, lv_color_t color);
    void Tick();
};

struct FanHoloRoleWidgets {
    lv_obj_t* box = nullptr;
    lv_obj_t* label = nullptr;
    lv_obj_t* image = nullptr;

    void Create(lv_obj_t* screen, FanHoloDisplay& host, bool idle_corner = false);
    void Hide();
    void Bind(FanHoloDisplay& host) const;
};

lv_obj_t* FanHoloCreateScreen(FanHoloDisplay& host);
lv_obj_t* FanHoloCreateFullBleedContainer(lv_obj_t* screen, FanHoloDisplay& host);
lv_obj_t* FanHoloCreatePreviewImage(lv_obj_t* screen, FanHoloDisplay& host);

#endif
