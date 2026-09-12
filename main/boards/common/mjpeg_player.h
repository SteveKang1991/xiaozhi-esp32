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
    uint16_t mjpeg_video_width;
    uint16_t mjpeg_video_height;
    uint16_t panel_width;   // 面板物理分辨率宽度
    uint16_t panel_height;   // 面板物理分辨率高度
    uint8_t target_fps;
    bool loop;
    uint16_t fb_stride;
    uint32_t fb_size;
    /** 非 NULL：解码到 lv_canvas（需 LVGL）；NULL：走面板 */
    void *lv_video_canvas;
    /**
     * true：解码到紧密 RGB565 缓冲，经 esp_lcd_panel_draw_bitmap 仅刷 ROI。
     * 须 lv_video_canvas == NULL 且 panel 非空。DSI 绘制由
     * mjpeg_attach_panel_draw_guard 与 LVGL flush 串行，不取 lvgl_port_lock。
     */
    bool panel_blit_roi;
    uint16_t panel_roi_x;
    uint16_t panel_roi_y;
    /** 0：按 mjpeg_video_height 全高 blit。非 0：只画 N 行。 */
    uint16_t panel_roi_h;
    /** 0：按 mjpeg_video_width 全宽 blit。非 0：目标宽度（须配合 src_x，按行拷贝，避免 stride 花屏）。 */
    uint16_t panel_roi_w;
    /** 解码缓冲里水平起始列，配合 panel_roi_w 取画面。 */
    uint16_t panel_roi_src_x;
    /** 解码缓冲里垂直起始行，0 表示从顶部取（idle 保头部、裁底部）。 */
    uint16_t panel_roi_src_y;
    /** PPA 输入块宽高；0 表示用整帧。 */
    uint16_t panel_src_w;
    uint16_t panel_src_h;
    /** 非 0：解码后仅走 PPA 缩放到该尺寸再 blit，失败则启动失败。 */
    uint16_t panel_out_w;
    uint16_t panel_out_h;
    /** 非 0：PPA 缩放为 n/16（硬件步进），须与 panel_out 配套。 */
    uint8_t panel_scale_n;
    /** letterbox 顶边保护：不刷 y < 此值（idle 时钟栏）。 */
    uint16_t panel_protect_top;
} mjpeg_player_cfg_t;

esp_err_t mjpeg_player_start(const mjpeg_player_cfg_t *cfg);
void mjpeg_player_stop(void);
bool mjpeg_player_is_running(void);

/**
 * DPI 同时只能有一笔 draw_bitmap。LVGL flush 与 MJPEG blit 并发会
 * previous draw not finished，UI 花屏/闪、天气只剩一条黑带。
 * 包装 panel->draw_bitmap：互斥 + 忙重试，所有调用方共用。
 */
void mjpeg_attach_panel_draw_guard(esp_lcd_panel_handle_t panel);

/** 播放中把落入 MJPEG ROI 的 LVGL 脏区裁掉，避免全宽/全屏 flush 盖住视频。 */
void mjpeg_attach_lvgl_inv_guard(void *lv_display);

#ifdef __cplusplus
}
#endif

#endif
