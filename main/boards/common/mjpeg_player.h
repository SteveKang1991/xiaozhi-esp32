/**
 * @file mjpeg_player.h
 * @brief MJPEG 硬件解码播放（全屏 DPI / ROI / 可选 LVGL 画布）
 */
#ifndef MJPEG_PLAYER_H
#define MJPEG_PLAYER_H

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *file_path;
    esp_lcd_panel_handle_t panel;
    void *fb[2];
    uint16_t screen_width;
    uint16_t screen_height;
    uint8_t target_fps;
    bool loop;
    uint16_t fb_stride;
    uint32_t fb_size;
    /** 非 NULL：解码到 lv_canvas（需 LVGL）；NULL：走面板 */
    void *lv_video_canvas;
    /**
     * true：解码到小缓冲后送 (panel_roi_x, panel_roi_y)，尺寸 screen_width×screen_height。
     * 须 lv_video_canvas == NULL 且 panel 非空。建议先 bsp_display_lvgl_suspend(true)。
     */
    bool panel_blit_roi;
    uint16_t panel_roi_x;
    uint16_t panel_roi_y;
} mjpeg_player_cfg_t;

esp_err_t mjpeg_player_start(const mjpeg_player_cfg_t *cfg);
void mjpeg_player_stop(void);
bool mjpeg_player_is_running(void);

#ifdef __cplusplus
}
#endif

#endif
