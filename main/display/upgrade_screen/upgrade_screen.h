#pragma once

#include <lvgl.h>
#include <cstddef>

class UpgradeScreen {
public:
    static void Show(const char* title, const char* version_text);
    static void Update(int progress, size_t downloaded, size_t total, size_t speed_bps);
    static void SetStatusMessage(const char* message);
    static void Dismiss();
    static bool IsActive();

    /**
     * 设置显示使用的字体。如果不设置, 会尝试从当前 LvglTheme 取;
     * 取不到时回退到编译期内置字体 BUILTIN_TEXT_FONT。
     * 传 nullptr 清除显式设置,恢复自动解析。
     */
    static void SetFont(const lv_font_t* font);

    /**
     * 获取当前生效的字体 (可能是显式设置, 也可能是自动解析)。
     */
    static const lv_font_t* GetFont();
};
