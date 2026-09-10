#ifndef FAN_HOLO_STATUS_BAR_H
#define FAN_HOLO_STATUS_BAR_H

#include <lvgl.h>

class FanHoloDisplay;

/* Idle：完整顶栏（状态 + 静音/网络/电池）。
 * Chat / Music：只有左侧状态文字，不要右侧图标。 */
struct FanHoloStatusBar {
    enum class Kind { IdleFull, StatusLeft };

    lv_obj_t* top_bar = nullptr;
    lv_obj_t* status_label = nullptr;
    lv_obj_t* notification_label = nullptr;
    lv_obj_t* mute_label = nullptr;
    lv_obj_t* network_label = nullptr;
    lv_obj_t* battery_label = nullptr;
    lv_obj_t* low_battery_popup = nullptr;
    lv_obj_t* low_battery_label = nullptr;

    void Create(lv_obj_t* screen, FanHoloDisplay& host, Kind kind);
    void Bind(FanHoloDisplay& host) const;
    void SetStatusText(const char* status);
    void ApplyTextFont(const lv_font_t* font, lv_color_t color);
};

struct FanHoloRoleWidgets {
    lv_obj_t* box = nullptr;
    lv_obj_t* label = nullptr;
    lv_obj_t* image = nullptr;

    void Create(lv_obj_t* screen, FanHoloDisplay& host);
    void Hide();
    void Bind(FanHoloDisplay& host) const;
};

lv_obj_t* FanHoloCreateScreen(FanHoloDisplay& host);
lv_obj_t* FanHoloCreateFullBleedContainer(lv_obj_t* screen, FanHoloDisplay& host);
lv_obj_t* FanHoloCreatePreviewImage(lv_obj_t* screen, FanHoloDisplay& host);

#endif
