#ifndef FAN_HOLO_STATUS_BAR_H
#define FAN_HOLO_STATUS_BAR_H

#include <lvgl.h>

class FanHoloDisplay;

/* 三页各自一份相同样式的顶栏。切页时 Bind 到 LvglDisplay 指针，
 * 让 SetStatus / UpdateStatusBar 继续写当前页。 */
struct FanHoloStatusBar {
    lv_obj_t* top_bar = nullptr;
    lv_obj_t* status_label = nullptr;
    lv_obj_t* notification_label = nullptr;
    lv_obj_t* mute_label = nullptr;
    lv_obj_t* network_label = nullptr;
    lv_obj_t* battery_label = nullptr;
    lv_obj_t* low_battery_popup = nullptr;
    lv_obj_t* low_battery_label = nullptr;

    void Create(lv_obj_t* screen, FanHoloDisplay& host);
    void Bind(FanHoloDisplay& host) const;
    void CopyVisualFrom(const FanHoloStatusBar& other);
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
