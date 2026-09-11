#include "fan_holo_status_bar.h"
#include "fan_holo_display.h"
#include "fan_holo_lunar.h"
#include "assets/lang_config.h"
#include "settings.h"

#include <font_awesome.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

LV_FONT_DECLARE(font_puhui_number_120_4);

extern "C" {
extern const uint32_t kFanHoloFont7Digit[10][48];
}

namespace {

constexpr uint32_t kYearBg = 0x2B7DE9;
constexpr uint32_t kWeekBg = 0x2EAE4F;
constexpr uint32_t kLunarBg = 0xF07820;
constexpr uint32_t kLunarMonthFg = 0x8B3D9E;
constexpr uint32_t kMonthBg = 0xE11D2E;
constexpr uint32_t kMonthFg = 0xFFFFFF;
constexpr uint32_t kDayBg = 0xFFFFFF;
constexpr uint32_t kDayFg = 0x111111;
constexpr uint32_t kGanzhiBg = 0xF5C400;
constexpr uint32_t kGanzhiFg = 0x111111;
constexpr uint32_t kIconFg = 0x22C55E;
constexpr uint32_t kIconPillBg = 0xFFFFFF;
constexpr uint32_t kBarBg = 0x000000;
constexpr uint32_t kCardBg = 0xFFFFFF;
constexpr uint32_t kCardBgTop = 0xF4F4F6;
constexpr uint32_t kHingeColor = 0xC8C8CC;
constexpr uint32_t kDigitColor = 0x111111;
constexpr uint32_t kColonColor = 0xD8D8D8;
constexpr uint32_t kFlipDurationMs = 480;
constexpr int kDigitCount = 6;
constexpr int kPairGap = 6;
constexpr int kDateRowY = 40;
constexpr int kClockBelowDate = 50;
/* 图1：卡片深灰可见；时分亮冰青；秒青色。 */
constexpr uint32_t kLedCardBg = 0x1A4854;
constexpr uint32_t kLedCardBorder = 0x3A6A78;
constexpr uint32_t kLedTimeOn = 0xC8F6FF;
constexpr uint32_t kLedSecOn = 0x00E8F0;

constexpr uint32_t kVolumeTrack = 0xE6E6E6;
constexpr uint32_t kVolumeFill = 0xF07820;
constexpr int kVolumeBarW = 10;
constexpr int kVolumeLabelGap = 4;

void CreateLowBatteryPopup(FanHoloStatusBar* bar, lv_obj_t* screen, FanHoloDisplay& host) {
    auto* theme = host.GetLvglTheme();
    auto* text_font = theme->text_font()->font();

    bar->low_battery_popup = lv_obj_create(screen);
    lv_obj_remove_style_all(bar->low_battery_popup);
    lv_obj_remove_flag(bar->low_battery_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bar->low_battery_popup, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(bar->low_battery_popup, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_scrollbar_mode(bar->low_battery_popup, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(bar->low_battery_popup, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(bar->low_battery_popup, LV_HOR_RES * 38 / 100, 0);
    lv_obj_set_style_bg_color(bar->low_battery_popup, theme->low_battery_color(), 0);
    lv_obj_set_style_bg_opa(bar->low_battery_popup, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar->low_battery_popup, 8, 0);
    lv_obj_set_style_pad_hor(bar->low_battery_popup, 8, 0);
    lv_obj_set_style_pad_ver(bar->low_battery_popup, 2, 0);
    lv_obj_set_style_border_width(bar->low_battery_popup, 0, 0);
    lv_obj_align(bar->low_battery_popup, LV_ALIGN_TOP_MID, 0, 0);
    bar->low_battery_label = lv_label_create(bar->low_battery_popup);
    lv_obj_set_style_text_font(bar->low_battery_label, text_font, 0);
    lv_label_set_long_mode(bar->low_battery_label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(bar->low_battery_label, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(bar->low_battery_label, lv_color_white(), 0);
    lv_obj_center(bar->low_battery_label);
    lv_obj_add_flag(bar->low_battery_popup, LV_OBJ_FLAG_HIDDEN);
}

void CreateVolumeOverlay(FanHoloStatusBar* bar, lv_obj_t* screen, FanHoloDisplay& host) {
    auto* theme = host.GetLvglTheme();
    auto* font = theme->text_font()->font();
    const auto& metrics = host.metrics();
    const int clock_y = kDateRowY + metrics.idle_pill_h + kClockBelowDate;
    const int label_h = font->line_height;

    lv_obj_t* box = lv_obj_create(screen);
    lv_obj_remove_style_all(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(box, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(box, kVolumeLabelGap, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_obj_t* label = lv_label_create(box);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(label, "100");

    lv_obj_t* slider = lv_bar_create(box);
    lv_obj_remove_flag(slider, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(slider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(slider, kVolumeBarW, metrics.idle_digit_h);
    lv_bar_set_range(slider, 0, 100);
    lv_bar_set_value(slider, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(slider, kVolumeBarW / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(kVolumeTrack), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(slider, 1, LV_PART_MAIN);
    lv_obj_set_style_border_width(slider, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, (kVolumeBarW - 2) / 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(kVolumeFill), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);

    lv_obj_align(box, LV_ALIGN_TOP_RIGHT, 0, clock_y - label_h - kVolumeLabelGap);
    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);

    bar->volume_overlay = box;
    bar->volume_label = label;
    bar->volume_bar = slider;
}

constexpr uint32_t kStatusListenBg = 0xE11D2E;
constexpr uint32_t kStatusSpeakBg = 0x2EAE4F;
constexpr uint32_t kStatusMusicBg = 0xF07820;
constexpr uint32_t kStatusOtherBg = 0x3A3A3A;

void StyleStatusPill(FanHoloStatusBar* bar, const char* status) {
    if (bar->status_pill == nullptr || bar->status_label == nullptr) {
        return;
    }
    uint32_t bg = kStatusOtherBg;
    bool scroll = false;
    if (strcmp(status, Lang::Strings::LISTENING) == 0) {
        bg = kStatusListenBg;
    } else if (strcmp(status, Lang::Strings::SPEAKING) == 0) {
        bg = kStatusSpeakBg;
    } else if (strcmp(status, Lang::Strings::MUSIC_PLAYING) == 0) {
        bg = kStatusMusicBg;
        scroll = true;
    }
    lv_obj_set_style_bg_color(bar->status_pill, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar->status_pill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(bar->status_label, lv_color_white(), 0);
    if (scroll) {
        const int inner_w = bar->status_scroll_w;
        lv_obj_set_width(bar->status_pill, inner_w);
        lv_obj_set_width(bar->status_label, inner_w - 20);
        lv_label_set_long_mode(bar->status_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_align(bar->status_label, LV_TEXT_ALIGN_LEFT, 0);
    } else {
        lv_obj_set_width(bar->status_pill, LV_SIZE_CONTENT);
        lv_obj_set_width(bar->status_label, LV_SIZE_CONTENT);
        lv_label_set_long_mode(bar->status_label, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(bar->status_label, LV_TEXT_ALIGN_CENTER, 0);
    }
}

void StyleNoScroll(lv_obj_t* obj) {
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t* CreatePill(lv_obj_t* parent, uint32_t bg, int radius, int height, int pad_hor) {
    lv_obj_t* pill = lv_obj_create(parent);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, height);
    lv_obj_set_style_min_height(pill, height, LV_PART_MAIN);
    lv_obj_set_style_bg_color(pill, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(pill, radius, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(pill, pad_hor, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(pill, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    StyleNoScroll(pill);
    return pill;
}

lv_obj_t* CreatePillLabel(lv_obj_t* pill, const lv_font_t* font, uint32_t fg, const char* text) {
    lv_obj_t* lbl = lv_label_create(pill);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(fg), 0);
    lv_label_set_text(lbl, text);
    return lbl;
}

constexpr int kFont7W = 32;
constexpr int kFont7H = 48;

void OnFont7DigitDraw(lv_event_t* e) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    if (obj == nullptr || layer == nullptr) {
        return;
    }
    const intptr_t d = reinterpret_cast<intptr_t>(lv_obj_get_user_data(obj));
    if (d < 0 || d > 9) {
        return;
    }
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const int32_t dw = lv_area_get_width(&a);
    const int32_t dh = lv_area_get_height(&a);
    if (dw < 2 || dh < 2) {
        return;
    }
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_obj_get_style_text_color(obj, LV_PART_MAIN);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = 0;
    dsc.border_width = 0;

    int sy = 0;
    while (sy < kFont7H) {
        const uint32_t row = kFanHoloFont7Digit[d][sy];
        int sy2 = sy + 1;
        while (sy2 < kFont7H && kFanHoloFont7Digit[d][sy2] == row) {
            sy2++;
        }
        const int32_t y0 = a.y1 + sy * dh / kFont7H;
        int32_t y1 = a.y1 + sy2 * dh / kFont7H - 1;
        if (y1 < y0) {
            y1 = y0;
        }
        int run = -1;
        for (int sx = 0; sx <= kFont7W; ++sx) {
            const bool on = (sx < kFont7W) && (((row >> (31 - sx)) & 1u) != 0);
            if (on) {
                if (run < 0) {
                    run = sx;
                }
            } else if (run >= 0) {
                const int32_t x0 = a.x1 + run * dw / kFont7W;
                int32_t x1 = a.x1 + sx * dw / kFont7W - 1;
                if (x1 < x0) {
                    x1 = x0;
                }
                lv_area_t pix = {x0, y0, x1, y1};
                lv_draw_rect(layer, &dsc, &pix);
                run = -1;
            }
        }
        sy = sy2;
    }
}

void CreateLedDigit(lv_obj_t* parent, FanHoloSegDigit* d, int w, int h, uint32_t color) {
    d->box = lv_obj_create(parent);
    lv_obj_remove_style_all(d->box);
    lv_obj_set_size(d->box, w, h);
    lv_obj_set_style_bg_color(d->box, lv_color_hex(kLedCardBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(d->box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(d->box, 0, LV_PART_MAIN);
    lv_obj_set_style_text_color(d->box, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_user_data(d->box, reinterpret_cast<void*>(static_cast<intptr_t>(-1)));
    lv_obj_add_event_cb(d->box, OnFont7DigitDraw, LV_EVENT_DRAW_POST, nullptr);
    StyleNoScroll(d->box);
    d->current = 0;
}

void SegDigitSet(FanHoloSegDigit* g, char ch) {
    if (g == nullptr || g->box == nullptr || ch < '0' || ch > '9') {
        return;
    }
    if (ch != g->current) {
        g->current = ch;
        lv_obj_set_user_data(g->box, reinterpret_cast<void*>(static_cast<intptr_t>(ch - '0')));
    }
    lv_obj_invalidate(g->box);
}

void FlipDigitSetChar(lv_obj_t* lbl, char ch) {
    if (lbl == nullptr) {
        return;
    }
    char buf[2] = {ch, '\0'};
    lv_label_set_text(lbl, buf);
}

void FlipDigitRaiseDecor(FanHoloFlipDigit* d) {
    if (d == nullptr) {
        return;
    }
    if (d->shade != nullptr) {
        lv_obj_move_foreground(d->shade);
    }
    if (d->hinge != nullptr) {
        lv_obj_move_foreground(d->hinge);
    }
}

void FlipDigitApplyProgress(FanHoloFlipDigit* d, int32_t progress) {
    if (d == nullptr || d->flap == nullptr) {
        return;
    }
    const int32_t half = d->digit_half;
    const int32_t max_p = half * 2;
    if (progress < 0) {
        progress = 0;
    }
    if (progress > max_p) {
        progress = max_p;
    }

    const bool lower = progress > half;
    if (lower != d->flap_lower) {
        d->flap_lower = lower;
        if (lower) {
            FlipDigitSetChar(d->flap_lbl, d->target);
            lv_obj_set_style_bg_color(d->flap, lv_color_hex(kCardBg), LV_PART_MAIN);
        } else {
            FlipDigitSetChar(d->flap_lbl, d->old_ch);
            lv_obj_set_style_bg_color(d->flap, lv_color_hex(kCardBgTop), LV_PART_MAIN);
        }
    }

    int32_t h;
    int32_t y;
    int32_t lbl_y;
    if (!lower) {
        h = half - progress;
        if (h < 1) {
            h = 1;
        }
        y = half - h;
        lbl_y = d->label_ofs_y - y;
    } else {
        h = progress - half;
        if (h < 1) {
            h = 1;
        }
        y = half;
        lbl_y = d->label_ofs_y - half;
    }

    lv_obj_set_pos(d->flap, 0, y);
    lv_obj_set_height(d->flap, h);
    lv_obj_set_y(d->flap_lbl, lbl_y);

    const int32_t dist = lower ? (max_p - progress) : progress;
    const lv_opa_t shade = static_cast<lv_opa_t>(dist * 160 / half);
    if (d->shade != nullptr) {
        lv_obj_set_pos(d->shade, 0, y);
        lv_obj_set_height(d->shade, h);
        lv_obj_set_style_bg_opa(d->shade, shade, LV_PART_MAIN);
        lv_obj_remove_flag(d->shade, LV_OBJ_FLAG_HIDDEN);
    }
}

void FlipDigitStartFlip(FanHoloFlipDigit* d, char next);

void FlipDigitFinish(FanHoloFlipDigit* d, char ch) {
    if (d == nullptr) {
        return;
    }
    FlipDigitSetChar(d->top_lbl, ch);
    FlipDigitSetChar(d->bot_lbl, ch);
    d->current = ch;
    d->target = 0;
    d->flap_lower = false;
    if (d->flap != nullptr) {
        lv_obj_add_flag(d->flap, LV_OBJ_FLAG_HIDDEN);
    }
    if (d->shade != nullptr) {
        lv_obj_add_flag(d->shade, LV_OBJ_FLAG_HIDDEN);
    }
    d->animating = false;

    const char queued = d->pending;
    d->pending = 0;
    if (queued != 0 && queued != d->current) {
        FlipDigitStartFlip(d, queued);
    }
}

void FlipDigitStartFlip(FanHoloFlipDigit* d, char next) {
    if (d == nullptr || d->card == nullptr || d->flap == nullptr) {
        return;
    }
    if (next == d->current && !d->animating) {
        return;
    }
    if (d->animating) {
        d->pending = next;
        return;
    }

    d->old_ch = d->current;
    d->target = next;
    d->animating = true;
    d->flap_lower = false;

    FlipDigitSetChar(d->top_lbl, next);
    FlipDigitSetChar(d->bot_lbl, d->old_ch);
    FlipDigitSetChar(d->flap_lbl, d->old_ch);
    lv_obj_set_style_bg_color(d->flap, lv_color_hex(kCardBgTop), LV_PART_MAIN);

    lv_obj_remove_flag(d->flap, LV_OBJ_FLAG_HIDDEN);
    FlipDigitApplyProgress(d, 0);
    FlipDigitRaiseDecor(d);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, d);
    lv_anim_set_values(&a, 0, d->digit_half * 2);
    lv_anim_set_duration(&a, kFlipDurationMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_user_data(&a, d);
    lv_anim_set_exec_cb(&a, [](void* var, int32_t v) {
        FlipDigitApplyProgress(static_cast<FanHoloFlipDigit*>(var), v);
    });
    lv_anim_set_completed_cb(&a, [](lv_anim_t* anim) {
        FanHoloFlipDigit* digit = static_cast<FanHoloFlipDigit*>(lv_anim_get_user_data(anim));
        if (digit == nullptr) {
            return;
        }
        FlipDigitFinish(digit, digit->target);
    });
    lv_anim_start(&a);
}

void FlipDigitSet(FanHoloFlipDigit* d, char ch, bool animate) {
    if (d == nullptr || ch < '0' || ch > '9') {
        return;
    }
    if (!animate || d->card == nullptr) {
        lv_anim_delete(d, nullptr);
        d->pending = 0;
        d->target = 0;
        d->animating = false;
        d->flap_lower = false;
        FlipDigitSetChar(d->top_lbl, ch);
        FlipDigitSetChar(d->bot_lbl, ch);
        d->current = ch;
        if (d->flap != nullptr) {
            lv_obj_add_flag(d->flap, LV_OBJ_FLAG_HIDDEN);
        }
        if (d->shade != nullptr) {
            lv_obj_add_flag(d->shade, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (ch == d->current && !d->animating) {
        return;
    }
    FlipDigitStartFlip(d, ch);
}

lv_obj_t* CreateHalfClip(lv_obj_t* parent, int y, int w, int h, uint32_t bg) {
    lv_obj_t* clip = lv_obj_create(parent);
    lv_obj_remove_style_all(clip);
    lv_obj_set_size(clip, w, h);
    lv_obj_set_pos(clip, 0, y);
    lv_obj_set_style_bg_color(clip, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(clip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(clip, false, LV_PART_MAIN);
    lv_obj_set_style_radius(clip, 0, LV_PART_MAIN);
    lv_obj_remove_flag(clip, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    StyleNoScroll(clip);
    return clip;
}

lv_obj_t* CreateDigitLabel(lv_obj_t* parent, FanHoloFlipDigit* d, int32_t y, const lv_font_t* font) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, "0");
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kDigitColor), LV_PART_MAIN);
    lv_obj_set_style_text_opa(lbl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(lbl, d->digit_w);
    lv_obj_set_pos(lbl, 0, y);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

void CreateFlipDigit(lv_obj_t* parent, FanHoloFlipDigit* d, int w, int h, int radius, const lv_font_t* font) {
    d->digit_w = w;
    d->digit_h = h;
    d->digit_half = h / 2;
    d->font_scale = 256;
    const int32_t line_h = (font != nullptr && font->line_height > 0) ? font->line_height : 86;
    d->label_ofs_y = (h - line_h) / 2;

    d->card = lv_obj_create(parent);
    lv_obj_remove_style_all(d->card);
    lv_obj_set_size(d->card, w, h);
    lv_obj_set_style_radius(d->card, radius, LV_PART_MAIN);
    lv_obj_set_style_bg_color(d->card, lv_color_hex(kCardBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(d->card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(d->card, true, LV_PART_MAIN);
    StyleNoScroll(d->card);

    d->top_clip = CreateHalfClip(d->card, 0, w, d->digit_half, kCardBgTop);
    d->top_lbl = CreateDigitLabel(d->top_clip, d, d->label_ofs_y, font);

    d->bot_clip = CreateHalfClip(d->card, d->digit_half, w, d->digit_half, kCardBg);
    d->bot_lbl = CreateDigitLabel(d->bot_clip, d, d->label_ofs_y - d->digit_half, font);

    d->flap = CreateHalfClip(d->card, 0, w, d->digit_half, kCardBgTop);
    d->flap_lbl = CreateDigitLabel(d->flap, d, d->label_ofs_y, font);
    lv_obj_add_flag(d->flap, LV_OBJ_FLAG_HIDDEN);

    d->shade = lv_obj_create(d->card);
    lv_obj_remove_style_all(d->shade);
    lv_obj_set_size(d->shade, w, d->digit_half);
    lv_obj_set_style_bg_color(d->shade, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(d->shade, LV_OPA_TRANSP, LV_PART_MAIN);
    StyleNoScroll(d->shade);
    lv_obj_add_flag(d->shade, LV_OBJ_FLAG_HIDDEN);

    d->hinge = lv_obj_create(d->card);
    lv_obj_remove_style_all(d->hinge);
    lv_obj_set_size(d->hinge, w, 3);
    lv_obj_set_pos(d->hinge, 0, d->digit_half - 1);
    lv_obj_set_style_bg_color(d->hinge, lv_color_hex(kHingeColor), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(d->hinge, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(d->hinge, LV_OBJ_FLAG_CLICKABLE);

    d->current = '0';
    d->target = 0;
    d->pending = 0;
    d->old_ch = '0';
    d->flap_lower = false;
    d->animating = false;
}

lv_obj_t* CreateColon(lv_obj_t* parent, int group_gap, int digit_h) {
    lv_obj_t* col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, group_gap, digit_h);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, LV_PART_MAIN);
    StyleNoScroll(col);

    const int dot = (digit_h >= 120) ? 22 : 18;
    const int gap = digit_h / 5;
    lv_obj_t* top = lv_obj_create(col);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, dot, dot);
    lv_obj_set_style_radius(top, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(top, lv_color_hex(kColonColor), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(top, LV_ALIGN_CENTER, 0, -gap);
    StyleNoScroll(top);

    lv_obj_t* bot = lv_obj_create(col);
    lv_obj_remove_style_all(bot);
    lv_obj_set_size(bot, dot, dot);
    lv_obj_set_style_radius(bot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bot, lv_color_hex(kColonColor), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(bot, LV_ALIGN_CENTER, 0, gap);
    StyleNoScroll(bot);
    return col;
}

lv_obj_t* CreateLedColon(lv_obj_t* parent, int width, int digit_h, uint32_t color) {
    lv_obj_t* col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, width, digit_h);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(col, 0, LV_PART_MAIN);
    StyleNoScroll(col);

    int dot = digit_h / 8;
    if (dot < 6) {
        dot = 6;
    }
    const int gap = digit_h / 6;
    lv_obj_t* top = lv_obj_create(col);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, dot, dot);
    lv_obj_set_style_radius(top, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(top, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(top, LV_ALIGN_CENTER, 0, -gap);
    StyleNoScroll(top);

    lv_obj_t* bot = lv_obj_create(col);
    lv_obj_remove_style_all(bot);
    lv_obj_set_size(bot, dot, dot);
    lv_obj_set_style_radius(bot, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bot, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(bot, LV_ALIGN_CENTER, 0, gap);
    StyleNoScroll(bot);
    return col;
}

void CreateIdleLedClock(FanHoloStatusBar* bar, int pill_h, int dw, int dh) {
    const int group_gap = (dw >= 80) ? 30 : 24;
    const int card_w = dw * 6 + group_gap * 2 + kPairGap * 2;
    const int hm_w = dw * 88 / 100;
    const int hm_h = dh * 88 / 100;
    const int card_h = hm_h + 32;
    const int hm_pair_gap = 12;
    const int sec_h = hm_h * 18 / 40;
    const int sec_w = hm_w * 22 / 40;
    const int sec_colon_w = 16;
    const int sec_digit_gap = 8;
    const int side_pad = (card_w >= 400) ? 36 : 22;
    const int clock_y = kDateRowY + pill_h + kClockBelowDate;

    bar->clock_card = lv_obj_create(bar->top_bar);
    lv_obj_remove_style_all(bar->clock_card);
    lv_obj_set_size(bar->clock_card, card_w, card_h);
    lv_obj_set_style_layout(bar->clock_card, LV_LAYOUT_NONE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar->clock_card, lv_color_hex(kLedCardBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar->clock_card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar->clock_card, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar->clock_card, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar->clock_card, lv_color_hex(kLedCardBorder), LV_PART_MAIN);
    lv_obj_set_style_border_opa(bar->clock_card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar->clock_card, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(bar->clock_card, true, LV_PART_MAIN);
    StyleNoScroll(bar->clock_card);
    lv_obj_align(bar->clock_card, LV_ALIGN_TOP_MID, 0, clock_y);

    int inner_w = card_w - 2 * side_pad;
    const int digit_span = hm_w * 4 + sec_w * 2 + hm_pair_gap * 2 + sec_digit_gap + sec_colon_w;
    int rest = inner_w - digit_span;
    if (rest < 48) {
        rest = 48;
        inner_w = digit_span + rest;
    }
    const int hm_colon_w = rest * 22 / 40;
    const int min_sec_gap = rest - hm_colon_w;
    lv_obj_t* pack = lv_obj_create(bar->clock_card);
    lv_obj_remove_style_all(pack);
    lv_obj_set_size(pack, inner_w, hm_h);
    lv_obj_set_style_bg_opa(pack, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(pack, 0, LV_PART_MAIN);
    lv_obj_add_flag(pack, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    StyleNoScroll(pack);
    lv_obj_align(pack, LV_ALIGN_CENTER, 0, 0);

    const int time_y = 0;
    const int sec_y = (hm_h - sec_h) / 2;
    int x = 0;

    CreateLedDigit(pack, &bar->hm_digits[0], hm_w, hm_h, kLedTimeOn);
    lv_obj_set_pos(bar->hm_digits[0].box, x, time_y);
    x += hm_w + hm_pair_gap;
    CreateLedDigit(pack, &bar->hm_digits[1], hm_w, hm_h, kLedTimeOn);
    lv_obj_set_pos(bar->hm_digits[1].box, x, time_y);
    x += hm_w;
    lv_obj_t* hm_colon = CreateLedColon(pack, hm_colon_w, hm_h, kLedTimeOn);
    lv_obj_set_pos(hm_colon, x, time_y);
    x += hm_colon_w;
    CreateLedDigit(pack, &bar->hm_digits[2], hm_w, hm_h, kLedTimeOn);
    lv_obj_set_pos(bar->hm_digits[2].box, x, time_y);
    x += hm_w + hm_pair_gap;
    CreateLedDigit(pack, &bar->hm_digits[3], hm_w, hm_h, kLedTimeOn);
    lv_obj_set_pos(bar->hm_digits[3].box, x, time_y);
    x += hm_w + min_sec_gap;

    lv_obj_t* sec_colon = CreateLedColon(pack, sec_colon_w, sec_h, kLedSecOn);
    lv_obj_set_pos(sec_colon, x, sec_y);
    x += sec_colon_w;
    CreateLedDigit(pack, &bar->sec_digits[0], sec_w, sec_h, kLedSecOn);
    lv_obj_set_pos(bar->sec_digits[0].box, x, sec_y);
    x += sec_w + sec_digit_gap;
    CreateLedDigit(pack, &bar->sec_digits[1], sec_w, sec_h, kLedSecOn);
    lv_obj_set_pos(bar->sec_digits[1].box, x, sec_y);
    lv_obj_update_layout(bar->clock_card);
}

void CreateIdleClockBar(FanHoloStatusBar* bar, lv_obj_t* screen, FanHoloDisplay& host) {
    auto* theme = host.GetLvglTheme();
    auto icon_font = theme->icon_font()->font();
    auto text_font = theme->text_font()->font();
    const auto& metrics = host.metrics();
    const int dw = metrics.idle_digit_w;
    const int dh = metrics.idle_digit_h;
    const int radius = metrics.idle_digit_radius;
    const int pill_h = metrics.idle_pill_h;
    const int pill_r = (pill_h >= 36) ? 10 : 8;

    const int clock_y = kDateRowY + pill_h + kClockBelowDate;
    /* idle_clock_h 含时钟下方大块留白；占位层用实际时钟下沿，避免盖住城市名。 */
    int header_h = clock_y + dh;

    bar->top_bar = lv_obj_create(screen);
    lv_obj_remove_style_all(bar->top_bar);
    lv_obj_add_flag(bar->top_bar, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(bar->top_bar, 0, 0);
    lv_obj_set_size(bar->top_bar, LV_HOR_RES, header_h);
    lv_obj_set_style_layout(bar->top_bar, LV_LAYOUT_NONE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar->top_bar, lv_color_hex(kBarBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar->top_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar->top_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(bar->top_bar, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_right(bar->top_bar, 8, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(bar->top_bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(bar->top_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* date_row = lv_obj_create(bar->top_bar);
    lv_obj_remove_style_all(date_row);
    lv_obj_set_pos(date_row, 0, kDateRowY);
    lv_obj_set_size(date_row, LV_PCT(100), pill_h);
    lv_obj_set_flex_flow(date_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(date_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(date_row, 6, LV_PART_MAIN);
    StyleNoScroll(date_row);

    lv_obj_t* year_pill = CreatePill(date_row, kYearBg, pill_r, pill_h, 10);
    bar->year_label = CreatePillLabel(year_pill, text_font, 0xFFFFFF, "----");

    lv_obj_t* md = lv_obj_create(date_row);
    lv_obj_remove_style_all(md);
    lv_obj_set_size(md, LV_SIZE_CONTENT, pill_h);
    lv_obj_set_style_radius(md, pill_r, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(md, true, LV_PART_MAIN);
    lv_obj_set_flex_flow(md, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(md, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(md, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(md, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(md, LV_OPA_TRANSP, LV_PART_MAIN);
    StyleNoScroll(md);

    lv_obj_t* month_half = CreatePill(md, kMonthBg, 0, pill_h, 10);
    lv_obj_set_style_pad_left(month_half, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_right(month_half, 6, LV_PART_MAIN);
    bar->month_label = CreatePillLabel(month_half, text_font, kMonthFg, "--");

    lv_obj_t* day_half = CreatePill(md, kDayBg, 0, pill_h, 10);
    lv_obj_set_style_pad_left(day_half, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_right(day_half, 10, LV_PART_MAIN);
    bar->mday_label = CreatePillLabel(day_half, text_font, kDayFg, "--");

    lv_obj_t* week_pill = CreatePill(date_row, kWeekBg, pill_r, pill_h, 10);
    bar->weekday_label = CreatePillLabel(week_pill, text_font, 0xFFFFFF, "星期--");

    lv_obj_t* ganzhi_pill = CreatePill(date_row, kGanzhiBg, pill_r, pill_h, 10);
    bar->lunar_year_label = CreatePillLabel(ganzhi_pill, text_font, kGanzhiFg, "----年");

    lv_obj_t* lunar_pill = CreatePill(date_row, kLunarBg, pill_r, pill_h, 10);
    lv_obj_set_style_pad_column(lunar_pill, 2, LV_PART_MAIN);
    bar->lunar_month_label = CreatePillLabel(lunar_pill, text_font, kLunarMonthFg, "----");
    bar->lunar_label = CreatePillLabel(lunar_pill, text_font, 0xFFFFFF, "----");

    lv_obj_t* spacer = lv_obj_create(date_row);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, LV_PART_MAIN);
    StyleNoScroll(spacer);

    lv_obj_t* net_pill = CreatePill(date_row, kIconPillBg, pill_r, pill_h, 8);
    bar->network_label = lv_label_create(net_pill);
    lv_label_set_text(bar->network_label, "");
    lv_obj_set_style_text_font(bar->network_label, icon_font, 0);
    lv_obj_set_style_text_color(bar->network_label, lv_color_hex(kIconFg), 0);

    lv_obj_t* bat_pill = CreatePill(date_row, kIconPillBg, pill_r, pill_h, 8);
    bar->battery_label = lv_label_create(bat_pill);
    lv_label_set_text(bar->battery_label, "");
    lv_obj_set_style_text_font(bar->battery_label, icon_font, 0);
    lv_obj_set_style_text_color(bar->battery_label, lv_color_hex(kIconFg), 0);

    int clock_style = metrics.idle_clock_style;
    {
        Settings display_settings("display", false);
        clock_style = display_settings.GetInt("idle_clock_style", clock_style);
    }
    if (clock_style == FanHoloMetrics::kIdleClockLed7Seg) {
        CreateIdleLedClock(bar, pill_h, dw, dh);
        const int hm_h = dh * 88 / 100;
        const int card_h = hm_h + 32;
        lv_obj_set_height(bar->top_bar, clock_y + card_h);
    } else {
        lv_obj_t* clock_row = lv_obj_create(bar->top_bar);
        lv_obj_remove_style_all(clock_row);
        lv_obj_set_size(clock_row, LV_SIZE_CONTENT, dh);
        lv_obj_set_style_min_height(clock_row, dh, LV_PART_MAIN);
        lv_obj_set_flex_flow(clock_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(clock_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(clock_row, 0, LV_PART_MAIN);
        StyleNoScroll(clock_row);
        lv_obj_align(clock_row, LV_ALIGN_TOP_MID, 0, clock_y);

        const int group_gap = (dw >= 80) ? 30 : 24;
        for (int i = 0; i < kDigitCount; ++i) {
            if (i > 0) {
                if (i % 2 == 0) {
                    CreateColon(clock_row, group_gap, dh);
                } else {
                    lv_obj_t* gap = lv_obj_create(clock_row);
                    lv_obj_remove_style_all(gap);
                    lv_obj_set_size(gap, kPairGap, 1);
                    lv_obj_set_style_bg_opa(gap, LV_OPA_TRANSP, LV_PART_MAIN);
                    StyleNoScroll(gap);
                }
            }
            CreateFlipDigit(clock_row, &bar->digits[i], dw, dh, radius, &font_puhui_number_120_4);
        }
        lv_obj_align(clock_row, LV_ALIGN_TOP_MID, 0, clock_y);
    }

    bar->notification_label = lv_label_create(screen);
    lv_obj_set_style_text_font(bar->notification_label, text_font, 0);
    lv_obj_set_style_text_color(bar->notification_label, lv_color_white(), 0);
    lv_label_set_text(bar->notification_label, "");
    lv_obj_set_pos(bar->notification_label, -1000, -1000);
    lv_obj_add_flag(bar->notification_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(bar->notification_label, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(bar->notification_label, LV_OBJ_FLAG_FLOATING);

    bar->status_label = lv_label_create(screen);
    lv_label_set_text(bar->status_label, "");
    lv_obj_set_pos(bar->status_label, -1000, -1000);
    lv_obj_add_flag(bar->status_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(bar->status_label, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(bar->status_label, LV_OBJ_FLAG_FLOATING);

    bar->mute_label = lv_label_create(screen);
    lv_label_set_text(bar->mute_label, "");
    lv_obj_set_pos(bar->mute_label, -1000, -1000);
    lv_obj_add_flag(bar->mute_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(bar->mute_label, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(bar->mute_label, LV_OBJ_FLAG_FLOATING);

    CreateLowBatteryPopup(bar, screen, host);

    CreateVolumeOverlay(bar, screen, host);

    bar->clock_primed = false;
    bar->last_yday = -1;
    bar->last_hm = -1;
    bar->last_sec = -1;
}

}  // namespace

lv_obj_t* FanHoloCreateScreen(FanHoloDisplay& host) {
    auto* theme = host.GetLvglTheme();
    auto screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(screen, theme->background_color(), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_margin_all(screen, 0, 0);
    lv_obj_set_style_text_font(screen, theme->text_font()->font(), 0);
    lv_obj_set_style_text_color(screen, theme->text_color(), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_layout(screen, LV_LAYOUT_NONE, 0);
    return screen;
}

lv_obj_t* FanHoloCreateFullBleedContainer(lv_obj_t* screen, FanHoloDisplay& host) {
    auto* theme = host.GetLvglTheme();
    lv_obj_t* container = lv_obj_create(screen);
    lv_obj_remove_style_all(container);
    lv_obj_add_flag(container, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_size(container, host.screen_width(), host.screen_height());
    lv_obj_set_style_radius(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(container, theme->background_color(), 0);
    lv_obj_move_background(container);
    return container;
}

lv_obj_t* FanHoloCreatePreviewImage(lv_obj_t* screen, FanHoloDisplay& host) {
    lv_obj_t* preview = lv_image_create(screen);
    lv_obj_set_size(preview, host.screen_width() / 2, host.screen_height() / 2);
    lv_obj_align(preview, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(preview, LV_OBJ_FLAG_HIDDEN);
    return preview;
}

void FanHoloRoleWidgets::Create(lv_obj_t* screen, FanHoloDisplay& host, bool idle_corner) {
    auto* theme = host.GetLvglTheme();
    auto large_icon_font = theme->large_icon_font()->font();

    box = lv_obj_create(screen);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    if (idle_corner) {
        lv_obj_align(box, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
        lv_obj_set_style_transform_pivot_x(box, LV_PCT(100), 0);
        lv_obj_set_style_transform_pivot_y(box, LV_PCT(100), 0);
        lv_obj_set_style_transform_scale_x(box, host.metrics().idle_role_scale, 0);
        lv_obj_set_style_transform_scale_y(box, host.metrics().idle_role_scale, 0);
    } else {
        lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    }

    label = lv_label_create(box);
    lv_obj_set_style_text_font(label, large_icon_font, 0);
    lv_obj_set_style_text_color(label, theme->text_color(), 0);
    lv_label_set_text(label, FONT_AWESOME_MICROCHIP_AI);

    image = lv_img_create(box);
    lv_obj_center(image);
    lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
}

void FanHoloRoleWidgets::Hide() {
    if (image) {
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    }
    if (label) {
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    }
}

void FanHoloRoleWidgets::Bind(FanHoloDisplay& host) const {
    host.ApplyRole(this);
}

void FanHoloStatusBar::Create(lv_obj_t* screen, FanHoloDisplay& host, Kind kind) {
    auto* theme = host.GetLvglTheme();
    auto icon_font = theme->icon_font()->font();
    auto text_font = theme->text_font()->font();
    const auto& metrics = host.metrics();
    const bool idle_full = (kind == Kind::IdleFull);

    year_label = nullptr;
    month_label = nullptr;
    mday_label = nullptr;
    weekday_label = nullptr;
    lunar_year_label = nullptr;
    lunar_month_label = nullptr;
    lunar_label = nullptr;
    clock_card = nullptr;
    clock_primed = false;
    last_yday = -1;
    last_hm = -1;
    last_sec = -1;
    status_pill = nullptr;
    status_scroll_w = (metrics.status_label_w >= 140) ? 176 : 128;
    volume_overlay = nullptr;
    volume_bar = nullptr;
    volume_label = nullptr;

    if (idle_full) {
        CreateIdleClockBar(this, screen, host);
        return;
    }

    top_bar = lv_obj_create(screen);
    lv_obj_set_size(top_bar, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar, 0, 0);
    lv_obj_set_style_bg_opa(top_bar, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(top_bar, theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar, 0, 0);
    lv_obj_set_style_pad_all(top_bar, 0, 0);
    lv_obj_set_style_pad_top(top_bar, 4, 0);
    lv_obj_set_style_pad_bottom(top_bar, 4, 0);
    lv_obj_set_style_pad_left(top_bar, theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar, theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(top_bar, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t* left_status = lv_obj_create(top_bar);
    lv_obj_set_size(left_status, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(left_status, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left_status, 0, 0);
    lv_obj_set_style_pad_all(left_status, 0, 0);
    lv_obj_set_flex_flow(left_status, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_status, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_radius(left_status, 0, 0);
    lv_obj_set_scrollbar_mode(left_status, LV_SCROLLBAR_MODE_OFF);

    notification_label = lv_label_create(left_status);
    lv_obj_set_style_text_font(notification_label, text_font, 0);
    lv_obj_set_style_text_align(notification_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(notification_label, theme->text_color(), 0);
    lv_label_set_text(notification_label, "");
    lv_obj_add_flag(notification_label, LV_OBJ_FLAG_HIDDEN);

    status_pill = lv_obj_create(left_status);
    lv_obj_remove_style_all(status_pill);
    lv_obj_set_size(status_pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_pill, 6, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(status_pill, false, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(status_pill, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(status_pill, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(status_pill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(status_pill, lv_color_hex(kStatusOtherBg), LV_PART_MAIN);
    lv_obj_set_style_border_width(status_pill, 0, LV_PART_MAIN);
    StyleNoScroll(status_pill);

    status_label = lv_label_create(status_pill);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(status_label, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(status_label, text_font, 0);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label, lv_color_white(), 0);
    lv_label_set_text(status_label, "");

    mute_label = lv_label_create(screen);
    lv_label_set_text(mute_label, "");
    lv_obj_set_pos(mute_label, -1000, -1000);
    lv_obj_add_flag(mute_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(mute_label, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(mute_label, LV_OBJ_FLAG_FLOATING);
    network_label = nullptr;
    battery_label = nullptr;
    CreateLowBatteryPopup(this, screen, host);
    CreateVolumeOverlay(this, screen, host);
    (void)icon_font;
}

void FanHoloStatusBar::SetStatusText(const char* status) {
    if (status_label == nullptr || status == nullptr) {
        return;
    }
    if (year_label != nullptr) {
        /* idle 时钟页不把 STANDBY / HH:MM 写到顶栏 */
        if (strcmp(status, Lang::Strings::STANDBY) == 0) {
            return;
        }
        if (strlen(status) == 5 && status[2] == ':') {
            return;
        }
        return;
    }
    const char* current = lv_label_get_text(status_label);
    if (current != nullptr && strcmp(current, status) == 0) {
        return;
    }
    lv_label_set_text(status_label, status);
    StyleStatusPill(this, status);
    lv_obj_remove_flag(status_label, LV_OBJ_FLAG_HIDDEN);
    if (status_pill != nullptr) {
        lv_obj_remove_flag(status_pill, LV_OBJ_FLAG_HIDDEN);
    }
    if (notification_label != nullptr) {
        lv_obj_add_flag(notification_label, LV_OBJ_FLAG_HIDDEN);
    }
}

void FanHoloStatusBar::Tick() {
    if (year_label == nullptr) {
        return;
    }
    time_t now = time(nullptr);
    struct tm* tm = localtime(&now);
    if (tm == nullptr || tm->tm_year < 2025 - 1900) {
        return;
    }

    if (tm->tm_yday != last_yday) {
        last_yday = tm->tm_yday;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", tm->tm_year + 1900);
        lv_label_set_text(year_label, buf);
        snprintf(buf, sizeof(buf), "%02d", tm->tm_mon + 1);
        lv_label_set_text(month_label, buf);
        snprintf(buf, sizeof(buf), "%02d", tm->tm_mday);
        lv_label_set_text(mday_label, buf);
        lv_label_set_text(weekday_label, FanHoloWeekdayCn(tm->tm_wday));
        char ganzhi[16];
        FanHoloFormatLunarYear(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                               ganzhi, sizeof(ganzhi));
        if (lunar_year_label) {
            lv_label_set_text(lunar_year_label, ganzhi);
        }
        char lunar_m[16];
        char lunar_d[16];
        FanHoloFormatLunarParts(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                                lunar_m, sizeof(lunar_m), lunar_d, sizeof(lunar_d));
        if (lunar_month_label) {
            lv_label_set_text(lunar_month_label, lunar_m);
        }
        lv_label_set_text(lunar_label, lunar_d);
    }

    if (clock_card != nullptr && hm_digits[0].box != nullptr) {
        if (!clock_primed) {
            lv_obj_update_layout(clock_card);
            last_hm = -1;
            last_sec = -1;
        }
        const int hm = tm->tm_hour * 100 + tm->tm_min;
        if (hm != last_hm) {
            char hhmm[8];
            snprintf(hhmm, sizeof(hhmm), "%02d%02d", tm->tm_hour, tm->tm_min);
            for (int i = 0; i < 4; ++i) {
                SegDigitSet(&hm_digits[i], hhmm[i]);
            }
            last_hm = hm;
        }
        char ss[8];
        snprintf(ss, sizeof(ss), "%02d", tm->tm_sec);
        SegDigitSet(&sec_digits[0], ss[0]);
        SegDigitSet(&sec_digits[1], ss[1]);
        last_sec = tm->tm_sec;
        clock_primed = true;
        return;
    }

    if (digits[0].card == nullptr) {
        return;
    }
    char hhmmss[8];
    snprintf(hhmmss, sizeof(hhmmss), "%02d%02d%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
    const bool animate = clock_primed;
    for (int i = 0; i < kDigitCount; ++i) {
        FlipDigitSet(&digits[i], hhmmss[i], animate);
    }
    clock_primed = true;
}

void FanHoloStatusBar::ApplyTextFont(const lv_font_t* font, lv_color_t color) {
    if (font == nullptr) {
        return;
    }
    if (year_label) {
        lv_obj_set_style_text_font(year_label, font, 0);
        lv_obj_set_style_text_color(year_label, lv_color_white(), 0);
    }
    if (month_label) {
        lv_obj_set_style_text_font(month_label, font, 0);
        lv_obj_set_style_text_color(month_label, lv_color_hex(0xFFFFFF), 0);
    }
    if (mday_label) {
        lv_obj_set_style_text_font(mday_label, font, 0);
        lv_obj_set_style_text_color(mday_label, lv_color_hex(0x111111), 0);
    }
    if (weekday_label) {
        lv_obj_set_style_text_font(weekday_label, font, 0);
        lv_obj_set_style_text_color(weekday_label, lv_color_white(), 0);
    }
    if (lunar_year_label) {
        lv_obj_set_style_text_font(lunar_year_label, font, 0);
        lv_obj_set_style_text_color(lunar_year_label, lv_color_hex(0x111111), 0);
    }
    if (lunar_month_label) {
        lv_obj_set_style_text_font(lunar_month_label, font, 0);
        lv_obj_set_style_text_color(lunar_month_label, lv_color_hex(0x8B3D9E), 0);
    }
    if (lunar_label) {
        lv_obj_set_style_text_font(lunar_label, font, 0);
        lv_obj_set_style_text_color(lunar_label, lv_color_white(), 0);
    }
    if (status_label && year_label == nullptr) {
        lv_obj_set_style_text_font(status_label, font, 0);
        lv_obj_set_style_text_color(status_label, lv_color_white(), 0);
    }
    if (notification_label) {
        lv_obj_set_style_text_font(notification_label, font, 0);
        lv_obj_set_style_text_color(notification_label, color, 0);
    }
    if (low_battery_label) {
        lv_obj_set_style_text_font(low_battery_label, font, 0);
    }
    if (network_label && year_label) {
        lv_obj_set_style_text_color(network_label, lv_color_hex(0x22C55E), 0);
    }
    for (int i = 0; i < kDigitCount; ++i) {
        auto paint = [](lv_obj_t* lbl) {
            if (lbl == nullptr) {
                return;
            }
            lv_obj_set_style_text_font(lbl, &font_puhui_number_120_4, LV_PART_MAIN);
            lv_obj_set_style_text_color(lbl, lv_color_hex(kDigitColor), LV_PART_MAIN);
            lv_obj_set_style_text_opa(lbl, LV_OPA_COVER, LV_PART_MAIN);
        };
        paint(digits[i].top_lbl);
        paint(digits[i].bot_lbl);
        paint(digits[i].flap_lbl);
    }
}

void FanHoloStatusBar::Bind(FanHoloDisplay& host) const {
    host.ApplyStatusBar(*this);
}

void FanHoloStatusBar::RaiseOverlays() const {
    if (top_bar != nullptr) {
        lv_obj_move_foreground(top_bar);
    }
    if (volume_overlay != nullptr) {
        lv_obj_move_foreground(volume_overlay);
    }
    if (low_battery_popup != nullptr) {
        lv_obj_move_foreground(low_battery_popup);
    }
}
