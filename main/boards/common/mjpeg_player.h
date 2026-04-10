/**
 * @file mjpeg_player.h
 * @brief 🎬 MJPEG 播放器 - 零拷贝硬件解码，直接输出到 DPI 帧缓冲区
 */
#ifndef MJPEG_PLAYER_H
#define MJPEG_PLAYER_H

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MJPEG 播放器配置
 */
typedef struct {
    const char *file_path;              ///< MJPEG 文件路径
    esp_lcd_panel_handle_t panel;       ///< LCD 面板句柄
    void *fb[2];                        ///< DPI 面板双帧缓冲区指针（零拷贝）
    uint16_t screen_width;              ///< 屏幕宽度
    uint16_t screen_height;             ///< 屏幕高度
    uint8_t target_fps;                 ///< 目标帧率（0 = 不限速）
    bool loop;                          ///< 是否循环播放
} mjpeg_player_cfg_t;

/**
 * @brief 启动 MJPEG 播放（创建独立任务）
 */
esp_err_t mjpeg_player_start(const mjpeg_player_cfg_t *cfg);

/**
 * @brief 停止 MJPEG 播放
 */
void mjpeg_player_stop(void);

/**
 * @brief 检查播放器是否正在运行
 */
bool mjpeg_player_is_running(void);

#ifdef __cplusplus
}
#endif

#endif // MJPEG_PLAYER_H
