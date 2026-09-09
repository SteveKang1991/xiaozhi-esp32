#ifndef FAN_HOLO_IDLE_PAGE_H
#define FAN_HOLO_IDLE_PAGE_H

#include "fan_holo_status_bar.h"

class FanHoloDisplay;

/* 待命页：独立 LVGL screen。自建 statusbar + 角色 GIF/图标。
 * 时钟 / 天气 / 相册后续在本页扩展。
 * 与 chat、music 通过 lv_screen_load 互切，不共用控件树。 */
class FanHoloIdlePage {
public:
    void Create(FanHoloDisplay& host);
    void Destroy();
    void Show(FanHoloDisplay& host);
    void Bind(FanHoloDisplay& host) const;
    void HideRole();
    void ApplyTextFont(const lv_font_t* font, lv_color_t color);
    FanHoloRoleWidgets& role_widgets() { return role_; }

    lv_obj_t* screen() const { return screen_; }
    FanHoloStatusBar& status_bar() { return status_bar_; }
    const FanHoloStatusBar& status_bar() const { return status_bar_; }

private:
    lv_obj_t* screen_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    FanHoloStatusBar status_bar_;
    FanHoloRoleWidgets role_;
};

#endif
