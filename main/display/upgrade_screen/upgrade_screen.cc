#include "upgrade_screen.h"
#include "assets/lang_config.h"

#include <cstdio>

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_lvgl_port.h>

#include "board.h"
#include "display.h"
#include "lvgl_theme.h"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

namespace {

constexpr const char* TAG = "UpgradeScreen";
constexpr int kPanelWidth = 480;
constexpr int kPanelHeight = 854;
constexpr int kBarWidth = 400;
constexpr int kBarHeight = 12;

// 调用方显式注入的字体 (可空)
const lv_font_t* s_explicit_font = nullptr;

const lv_font_t* ResolveFont() {
    if (s_explicit_font != nullptr) {
        return s_explicit_font;
    }
    // 尝试从当前主题取
    auto* board = &Board::GetInstance();
    Display* display = board->GetDisplay();
    if (display != nullptr) {
        Theme* theme = display->GetTheme();
        auto* lvgl_theme = static_cast<LvglTheme*>(theme);
        if (lvgl_theme != nullptr) {
            auto font_obj = lvgl_theme->text_font();
            if (font_obj != nullptr) {
                const lv_font_t* font = font_obj->font();
                if (font != nullptr) {
                    return font;
                }
            }
        }
    }
    // fallback: 编译期内置字体
    return &BUILTIN_TEXT_FONT;
}

struct UpgradeUi {
    lv_obj_t* screen = nullptr;
    lv_obj_t* bar = nullptr;
    lv_obj_t* percent_lbl = nullptr;
    lv_obj_t* bytes_lbl = nullptr;
    lv_obj_t* speed_lbl = nullptr;
    lv_obj_t* time_lbl = nullptr;
    lv_obj_t* status_lbl = nullptr;
    lv_obj_t* version_lbl = nullptr;
    bool active = false;
};

UpgradeUi s_ui;
int64_t s_start_time_us = 0;
// 保存 Show() 之前的当前 screen, Dismiss() 时还原
lv_obj_t* s_previous_screen = nullptr;

void FormatDuration(char* buf, size_t buf_size, int seconds) {
    if (seconds < 0) seconds = 0;
    const int hours = seconds / 3600;
    const int minutes = (seconds % 3600) / 60;
    const int secs = seconds % 60;
    if (hours > 0) {
        snprintf(buf, buf_size, "%d:%02d:%02d", hours, minutes, secs);
    } else {
        snprintf(buf, buf_size, "%02d:%02d", minutes, secs);
    }
}

void FormatDataSize(char* buf, size_t buf_size, size_t bytes) {
    if (bytes >= 1024 * 1024) {
        snprintf(buf, buf_size, "%.2f MB", bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        snprintf(buf, buf_size, "%.1f KB", bytes / 1024.0);
    } else {
        snprintf(buf, buf_size, "%u B", static_cast<unsigned>(bytes));
    }
}

void FormatSpeed(char* buf, size_t buf_size, size_t speed_bps) {
    if (speed_bps >= 1024 * 1024) {
        snprintf(buf, buf_size, "%.2f MB/s", speed_bps / (1024.0 * 1024.0));
    } else if (speed_bps >= 1024) {
        snprintf(buf, buf_size, "%.1f KB/s", speed_bps / 1024.0);
    } else {
        snprintf(buf, buf_size, "%u B/s", static_cast<unsigned>(speed_bps));
    }
}

// Strip default LVGL chrome (padding / margin / border / radius / scrollbar)
// from a generic container that we are using purely for layout.
void StripChrome(lv_obj_t* obj) {
    if (obj == nullptr) return;
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_margin_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

void OnScreenDeleted(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    if (lv_event_get_target_obj(e) == s_ui.screen) {
        s_ui = {};
        s_start_time_us = 0;
    }
}

lv_obj_t* CreateTouchBlocker(lv_obj_t* parent) {
    lv_obj_t* blocker = lv_obj_create(parent);
    StripChrome(blocker);
    lv_obj_set_size(blocker, kPanelWidth, kPanelHeight);
    lv_obj_align(blocker, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(blocker, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(blocker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(blocker, LV_OBJ_FLAG_SCROLLABLE);
    return blocker;
}

lv_obj_t* BuildScreen(const char* title, const char* version_text) {
    const lv_font_t* font = ResolveFont();

    lv_obj_t* screen = lv_obj_create(nullptr);
    StripChrome(screen);
    lv_obj_set_size(screen, kPanelWidth, kPanelHeight);
    // 浅冰蓝 (#73FBFD) — DSI 丢帧时显示这种颜色,跟正常 UI 一致,看不出闪烁。
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x73FBFD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t* title_lbl = lv_label_create(screen);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_text_font(title_lbl, font, LV_PART_MAIN);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 100);

    // Version label
    lv_obj_t* version_lbl = lv_label_create(screen);
    s_ui.version_lbl = version_lbl;
    lv_label_set_text(version_lbl, version_text && version_text[0] ? version_text : "");
    lv_obj_set_style_text_color(version_lbl, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_obj_set_style_text_font(version_lbl, font, LV_PART_MAIN);
    lv_obj_align(version_lbl, LV_ALIGN_TOP_MID, 0, 150);

    // Status label
    lv_obj_t* status_lbl = lv_label_create(screen);
    s_ui.status_lbl = status_lbl;
    lv_label_set_text(status_lbl, Lang::Strings::UPGRADING);
    lv_obj_set_style_text_color(status_lbl, lv_color_hex(0xC7CDD9), LV_PART_MAIN);
    lv_obj_set_style_text_font(status_lbl, font, LV_PART_MAIN);
    lv_obj_align(status_lbl, LV_ALIGN_TOP_MID, 0, 200);

    // Progress bar
    lv_obj_t* bar = lv_bar_create(screen);
    s_ui.bar = bar;
    lv_obj_set_size(bar, kBarWidth, kBarHeight);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, -10);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    // 槽位浅灰 (#D1D5DB) — 在浅冰蓝背景上对比明显
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xD1D5DB), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, kBarHeight / 2, LV_PART_MAIN);
    // indicator 黑色 — 跟屏幕丢帧色一致,DSI 出错也看不出闪烁
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x000000), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, kBarHeight / 2, LV_PART_INDICATOR);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    // Percent label
    lv_obj_t* percent_lbl = lv_label_create(screen);
    s_ui.percent_lbl = percent_lbl;
    lv_label_set_text(percent_lbl, "0%");
    lv_obj_set_style_text_color(percent_lbl, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_text_font(percent_lbl, font, LV_PART_MAIN);
    lv_obj_align(percent_lbl, LV_ALIGN_CENTER, 0, 40);

    // Bytes label
    lv_obj_t* bytes_lbl = lv_label_create(screen);
    s_ui.bytes_lbl = bytes_lbl;
    lv_label_set_text(bytes_lbl, "0 B / 0 B");
    lv_obj_set_style_text_color(bytes_lbl, lv_color_hex(0xC7CDD9), LV_PART_MAIN);
    lv_obj_set_style_text_font(bytes_lbl, font, LV_PART_MAIN);
    lv_obj_align(bytes_lbl, LV_ALIGN_CENTER, 0, 90);

    // Speed label
    lv_obj_t* speed_lbl = lv_label_create(screen);
    s_ui.speed_lbl = speed_lbl;
    lv_label_set_text(speed_lbl, "0 B/s");
    lv_obj_set_style_text_color(speed_lbl, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_obj_set_style_text_font(speed_lbl, font, LV_PART_MAIN);
    lv_obj_align(speed_lbl, LV_ALIGN_CENTER, 0, 130);

    // Time label
    lv_obj_t* time_lbl = lv_label_create(screen);
    s_ui.time_lbl = time_lbl;
    lv_label_set_text(time_lbl, "用时 00:00");
    lv_obj_set_style_text_color(time_lbl, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_obj_set_style_text_font(time_lbl, font, LV_PART_MAIN);
    lv_obj_align(time_lbl, LV_ALIGN_CENTER, 0, 170);

    // Hint
    lv_obj_t* hint = lv_label_create(screen);
    lv_label_set_text(hint, Lang::Strings::PLEASE_WAIT);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x6B7280), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, font, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -80);

    CreateTouchBlocker(screen);
    lv_obj_add_event_cb(screen, OnScreenDeleted, LV_EVENT_DELETE, nullptr);
    return screen;
}

void UpdateWidgets(int progress, size_t downloaded, size_t total, size_t speed_bps) {
    if (!s_ui.active || s_ui.bar == nullptr) return;

    if (progress < 0) progress = 0;
    else if (progress > 100) progress = 100;

    lv_bar_set_value(s_ui.bar, progress, LV_ANIM_OFF);

    char percent_buf[16];
    snprintf(percent_buf, sizeof(percent_buf), "%d%%", progress);
    lv_label_set_text(s_ui.percent_lbl, percent_buf);

    char downloaded_buf[24], total_buf[24];
    FormatDataSize(downloaded_buf, sizeof(downloaded_buf), downloaded);
    FormatDataSize(total_buf, sizeof(total_buf), total);
    char bytes_buf[64];
    snprintf(bytes_buf, sizeof(bytes_buf), "%s / %s", downloaded_buf, total_buf);
    lv_label_set_text(s_ui.bytes_lbl, bytes_buf);

    char speed_buf[32];
    FormatSpeed(speed_buf, sizeof(speed_buf), speed_bps);
    lv_label_set_text(s_ui.speed_lbl, speed_buf);

    const int64_t now_us = esp_timer_get_time();
    int elapsed_sec = 0;
    if (s_start_time_us > 0 && now_us > s_start_time_us) {
        elapsed_sec = static_cast<int>((now_us - s_start_time_us) / 1000000);
    }

    char elapsed_buf[16];
    FormatDuration(elapsed_buf, sizeof(elapsed_buf), elapsed_sec);
    char time_buf[32];
    snprintf(time_buf, sizeof(time_buf), "用时 %s", elapsed_buf);
    lv_label_set_text(s_ui.time_lbl, time_buf);
}

struct UpgradeProgressMsg {
    int progress;
    size_t downloaded;
    size_t total;
    size_t speed_bps;
};

void AsyncUpdateProgress(void* user_data) {
    auto* msg = static_cast<UpgradeProgressMsg*>(user_data);
    if (msg == nullptr) return;
    if (lvgl_port_lock(portMAX_DELAY)) {
        UpdateWidgets(msg->progress, msg->downloaded, msg->total, msg->speed_bps);
        lvgl_port_unlock();
    }
    delete msg;
}

}  // namespace

void UpgradeScreen::Show(const char* title, const char* version_text) {
    if (!lvgl_port_lock(portMAX_DELAY)) return;

    if (s_ui.active && s_ui.screen != nullptr) {
        lv_obj_del(s_ui.screen);
        s_ui = {};
    }

    s_previous_screen = lv_screen_active();
    s_ui.screen = BuildScreen(title, version_text);
    s_ui.active = true;
    s_start_time_us = esp_timer_get_time();
    lv_screen_load(s_ui.screen);
    UpdateWidgets(0, 0, 0, 0);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "Upgrade screen shown: %s", title);
}

void UpgradeScreen::Update(int progress, size_t downloaded, size_t total, size_t speed_bps) {
    if (!s_ui.active) return;
    auto* msg = new UpgradeProgressMsg{progress, downloaded, total, speed_bps};
    lv_async_call(AsyncUpdateProgress, msg);
}

void UpgradeScreen::SetStatusMessage(const char* message) {
    if (!s_ui.active || message == nullptr) return;
    if (!lvgl_port_lock(portMAX_DELAY)) return;
    if (s_ui.status_lbl != nullptr) {
        lv_label_set_text(s_ui.status_lbl, message);
    }
    if (s_ui.bar != nullptr) {
        lv_bar_set_value(s_ui.bar, 100, LV_ANIM_OFF);
        lv_label_set_text(s_ui.percent_lbl, "100%");
    }
    if (s_ui.time_lbl != nullptr && s_start_time_us > 0) {
        const int elapsed_sec = static_cast<int>((esp_timer_get_time() - s_start_time_us) / 1000000);
        char elapsed_buf[16];
        FormatDuration(elapsed_buf, sizeof(elapsed_buf), elapsed_sec);
        char time_buf[32];
        snprintf(time_buf, sizeof(time_buf), "用时 %s", elapsed_buf);
        lv_label_set_text(s_ui.time_lbl, time_buf);
    }
    lvgl_port_unlock();
}

void UpgradeScreen::Dismiss() {
    if (!lvgl_port_lock(portMAX_DELAY)) return;
    lv_obj_t* old_screen = s_ui.screen;
    lv_obj_t* restore_screen = s_previous_screen;
    s_ui = {};
    s_start_time_us = 0;
    s_previous_screen = nullptr;
    // 先把原来的 UI 加载回来, 再删除升级屏
    if (restore_screen != nullptr && restore_screen != old_screen) {
        lv_screen_load(restore_screen);
    }
    if (old_screen != nullptr) {
        lv_obj_delete(old_screen);
    }
    lvgl_port_unlock();
    ESP_LOGI(TAG, "Upgrade screen dismissed");
}

bool UpgradeScreen::IsActive() {
    return s_ui.active;
}

void UpgradeScreen::SetFont(const lv_font_t* font) {
    s_explicit_font = font;
    ESP_LOGI(TAG, "UpgradeScreen font %s", font ? "explicitly set" : "reset to auto-resolve");
}

const lv_font_t* UpgradeScreen::GetFont() {
    return ResolveFont();
}
