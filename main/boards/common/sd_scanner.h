#ifndef SD_SCANNER_H
#define SD_SCANNER_H

#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "soc/soc_caps.h"
#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 引脚定义 ====================
#define MOUNT_POINT "/sdcard"

#include "driver/gpio.h"

/** 1：使用板级 config.h 中 BOARD_SD_SPI_* 走 SDSPI；0：ESP32-P4 可走 SDMMC，其它芯片未配置则挂载失败 */
#ifndef BOARD_SD_USE_SPI
#define BOARD_SD_USE_SPI 0
#endif

// ==================== 类型定义 ====================

/**
 * @brief SD文件夹信息
 */
typedef struct {
    char name[64];
    int file_count;
    int type; // 0=其他, 1=图片, 2=视频, 3=音乐
} sd_folder_info_t;

/**
 * @brief SD扫描器结构体
 */
typedef struct {
    bool is_mounted;                                    ///< SD卡是否已挂载
    sdmmc_card_t *card;                                ///< SD卡句柄
    esp_vfs_fat_sdmmc_mount_config_t mount_config;     ///< 挂载配置
#if SOC_SDMMC_IO_POWER_EXTERNAL
    sd_pwr_ctrl_handle_t pwr_ctrl_handle;              ///< 片上LDO电源控制句柄
#endif
} sd_scanner_t;

/**
 * @brief SD扫描器句柄类型
 */
typedef sd_scanner_t* sd_scanner_handle_t;

// ==================== 函数声明 ====================

/**
 * @brief 初始化SD扫描器
 * 
 * @param handle SD扫描器句柄指针
 * @return 
 *     - ESP_OK: 成功
 *     - ESP_ERR_INVALID_ARG: 参数无效
 *     - ESP_ERR_NO_MEM: 内存不足
 */
esp_err_t sd_scanner_init(sd_scanner_handle_t *handle);

/**
 * @brief 挂载SD卡
 * 
 * @param handle SD扫描器句柄
 * @return 
 *     - ESP_OK: 成功
 *     - ESP_ERR_INVALID_ARG: 参数无效
 *     - ESP_ERR_INVALID_STATE: SD卡已挂载
 *     - 其他错误码: SD卡挂载失败
 */
esp_err_t sd_scanner_mount(sd_scanner_handle_t handle);

/**
 * @brief 扫描SD卡文件
 * 
 * @param handle SD扫描器句柄
 * @return 
 *     - ESP_OK: 成功
 *     - ESP_ERR_INVALID_ARG: 参数无效
 *     - ESP_ERR_INVALID_STATE: SD卡未挂载
 */
esp_err_t sd_scanner_scan(sd_scanner_handle_t handle);

/**
 * @brief 卸载SD卡
 * 
 * @param handle SD扫描器句柄
 * @return 
 *     - ESP_OK: 成功
 *     - ESP_ERR_INVALID_ARG: 参数无效
 */
esp_err_t sd_scanner_unmount(sd_scanner_handle_t handle);

/**
 * @brief 释放SD扫描器资源
 * 
 * @param handle SD扫描器句柄
 * @return 
 *     - ESP_OK: 成功
 *     - ESP_ERR_INVALID_ARG: 参数无效
 */
esp_err_t sd_scanner_deinit(sd_scanner_handle_t handle);

/**
 * @brief 获取扫描到的文件夹信息
 * @param folders 文件夹信息数组
 * @param max_folders 最大文件夹数量
 * @return 实际文件夹数量
 */
int sd_scanner_get_folder_info(sd_folder_info_t *folders, int max_folders);

/**
 * @brief 初始化并扫描SD卡（保持挂载状态）
 * 初始化SD卡，扫描文件夹信息，并保持挂载状态
 * @return 
 *     - ESP_OK: 成功
 *     - 其他错误码: 失败
 */
esp_err_t sd_scanner_init_and_scan(void);

/**
 * @brief 获取全局SD卡扫描器
 * @return 全局SD卡扫描器句柄，如果未初始化则返回NULL
 */
sd_scanner_handle_t sd_scanner_get_global(void);

/**
 * @brief 检查SD卡是否已挂载
 * @return 
 *     - true: SD卡已挂载
 *     - false: SD卡未挂载
 */
bool sd_scanner_is_mounted(void);

#ifdef __cplusplus
}
#endif

#endif // SD_SCANNER_H
