#ifndef FAN_HOLO_CHAT_PAGE_H
#define FAN_HOLO_CHAT_PAGE_H

#include "fan_holo_status_bar.h"

#include <string>

class FanHoloDisplay;

/* 对话页（聆听 / 说话）：独立 screen。
 * 自建 statusbar、角色控件、纵向字幕。业务只在本文件处理。 */
class FanHoloChatPage {
public:
    void Create(FanHoloDisplay& host);
    void Destroy();
    void Show(FanHoloDisplay& host);
    void Bind(FanHoloDisplay& host) const;
    void HideRole();
    void SetMessage(FanHoloDisplay& host, const char* role, const char* content);
    void ClearMessages(FanHoloDisplay& host);
    void ApplyTextFont(const lv_font_t* font, lv_color_t color);

    lv_obj_t* screen() const { return screen_; }
    FanHoloStatusBar& status_bar() { return status_bar_; }
    const FanHoloStatusBar& status_bar() const { return status_bar_; }

private:
    lv_obj_t* screen_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* chat_container_ = nullptr;
    lv_obj_t* chat_inner_label_ = nullptr;
    FanHoloStatusBar status_bar_;
    FanHoloRoleWidgets role_;
    std::string last_chat_content_;
};

#endif
