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

/**
 * 数据源选择：
 *   MJPEG_SRC_FILE      - 传统：file_path 路径文件 (fopen)
 *   MJPEG_SRC_PARTITION - 从 SPI flash partition 读取（esp_partition_read）
 *                        用 partition + partition_offset + partition_size
 */
typedef enum {
    MJPEG_SRC_FILE = 0,
    MJPEG_SRC_PARTITION = 1,
} mjpeg_player_src_t;

typedef struct {
    /* 数据源（默认 FILE 兼容老用法） */
    mjpeg_player_src_t src_type;
    const char *file_path;             /* src=FILE 时用 */
    /* src=PARTITION 时用：partition 由 esp_partition_find_first 返回的指针 */
    const void *partition;
    uint32_t partition_offset;         /* 相对分区起始 */
    uint32_t partition_size;           /* 文件字节数 */

    esp_lcd_panel_handle_t panel;
    void *fb[2];
    uint16_t screen_width;
    uint16_t screen_height;
    uint16_t panel_width;   /**< 实际 LCD 面板宽度 */
    uint16_t panel_height;  /**< 实际 LCD 面板高度 */
    uint8_t target_fps;
    bool loop;
    uint16_t fb_stride;
    uint32_t fb_size;
    /** 非 NULL：解码到 lv_canvas（需 LVGL）；NULL：走面板 */
    void *lv_video_canvas;
    /**
     * true：解码到紧密 RGB565 缓冲，经 esp_lcd_panel_draw_bitmap 仅刷 ROI。
     * 须 lv_video_canvas == NULL 且 panel 非空。若 ROI 与 LVGL 合成内容不重叠，可不与 LVGL 互斥
     *（本实现不取 lvgl_port_lock）；重叠或撕裂敏感时请改用画布模式或自管同步。
     */
    bool panel_blit_roi;
    uint16_t panel_roi_x;
    uint16_t panel_roi_y;
} mjpeg_player_cfg_t;

esp_err_t mjpeg_player_start(const mjpeg_player_cfg_t *cfg);
void mjpeg_player_stop(void);
/**
 * 异步停止：仅同步等待 read_task 退出（释放 256KB PSRAM read_buf + 32KB io_buf），
 * 旧 decode_task 由它在 s_running=false 后自行退出，资源回收推迟到下次 start。
 * 用于频繁 state-switch 场景（idle/listen/speak 间切换），可大幅缩短状态切换
 * 卡顿窗口（典型从 ~200ms 降到 ~50ms）。同步释放路径见 mjpeg_player_stop。
 */
void mjpeg_player_stop_async(void);
bool mjpeg_player_is_running(void);

#ifdef __cplusplus
}
#endif

#endif
