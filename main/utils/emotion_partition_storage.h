/**
 * @file emotion_partition_storage.h
 * @brief 在 flash emotions 分区上提供简单的"文件名 → offset/size"映射表
 *
 * 设计目标：
 * - 用 ESP-IDF 自带的 esp_partition_read/write API 直接读写 SPI flash
 * - 不挂载文件系统（避开 spiffs/fatfs 的额外依赖与 erase 复杂度）
 * - 文件表 (header + N entries + 数据) 单线性布局，写新文件时：
 *     1. 追加到数据区（4KB 对齐）
 *     2. 把新 entry append 到 entry 区
 *     3. header.entry_count++ 并整体重写 header
 *   如果 space 不够，整分区擦除后重新写入（损失旧数据）。
 *
 * 适用：少量静态资源（每文件 500KB-2MB），极少写入，崩溃安全简单。
 * 不适用：频繁追加/删除大量小文件（每次擦 sector 寿命会缩短）。
 */
#ifndef EMOTION_PARTITION_STORAGE_H
#define EMOTION_PARTITION_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EMOTION_STORAGE_NAME_MAX     32     /* 含 '\0' */
#define EMOTION_STORAGE_MAX_ENTRIES  32
#define EMOTION_STORAGE_HEADER_MAGIC 0x56535041   /* 'APSV' (LE) - 与旧 asset_partition_storage 兼容 */

/* 4KB 对齐：与 SPI flash sector 大小一致，erase / write 友好 */
#define EMOTION_STORAGE_ALIGN        4096
#define EMOTION_STORAGE_DATA_RESERVE 4096   /* 数据区起始预留：1 个 sector */

/* entry flags */
#define EMOTION_FLAG_VALID   0
#define EMOTION_FLAG_DELETED 1

/* 触发碎片整理的删除次数阈值（deleted_count 达到此值时下次 upsert 自动整理） */
#define EMOTION_DEFRAG_THRESHOLD     8

/**
 * @brief 初始化存储模块，找到名为 "emotions" 的分区。
 *        调用一次；后续 read/upsert 隐式 init 后的状态。
 *
 * @return ESP_OK 找到并校验通过；ESP_ERR_NOT_FOUND 分区不存在；
 *         其他错误：分区格式错误时仍返回 OK（视为空）。
 */
esp_err_t emotion_partition_storage_init(void);

/**
 * @brief 写（或覆盖）一个文件。
 *
 * @param name    文件名（不含路径），最大 EMOTION_STORAGE_NAME_MAX-1 个字符
 * @param data    数据指针
 * @param size    数据字节数
 *
 * @return ESP_OK 成功；ESP_ERR_NO_MEM 空间不足；其他：flash 写入错误
 *
 * 注意：size 必须 > 0。空文件不存储。
 * 内部会将 size 上调到 4KB 对齐以保持 layout 整洁。
 */
esp_err_t emotion_partition_storage_upsert(const char* name, const void* data, size_t size);

/**
 * @brief 查找文件位置。
 *
 * @param name         文件名
 * @param out_offset   [out] 数据起始 offset（相对于分区）
 * @param out_size     [out] 数据实际大小（不含 padding）
 *
 * @return true 找到；false 不存在（含已被标记删除的）
 */
bool emotion_partition_storage_find(const char* name, uint32_t* out_offset, uint32_t* out_size);

/**
 * @brief 标记删除一个文件（不真正擦 flash，只标 entry flags=DELETED）。
 *        data 物理空间暂不回收；下次 upsert 触发 defrag 时整理。
 *
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 文件不在表中
 */
esp_err_t emotion_partition_storage_delete(const char* name);

/**
 * @brief 强制整理：把所有 VALID entries 的 data 重写到分区头部（4KB 对齐紧排），
 *        释放被删除 entry 占据的 data 空间。会擦整分区。
 *
 * @return ESP_OK 成功；其他：flash 错误
 */
esp_err_t emotion_partition_storage_defragment(void);

/**
 * @brief 直接读出文件数据到 buffer。
 *
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 不存在；ESP_ERR_INVALID_SIZE 缓冲不够
 */
esp_err_t emotion_partition_storage_read(const char* name, void* buf, size_t buf_size, size_t* out_size);

/**
 * @brief 枚举所有文件（debug）。
 */
typedef void (*emotion_storage_enum_cb)(const char* name, uint32_t offset, uint32_t size, void* user);
void emotion_partition_storage_enum(emotion_storage_enum_cb cb, void* user);

/**
 * @brief 全部擦除（清空分区，回到空白状态）。
 */
esp_err_t emotion_partition_storage_format(void);

/**
 * @brief 获取底层 esp_partition_t 指针（mjpeg_player 流式读时需要）。
 *        必须在 init 之后调用；返回 NULL 表示未 init。
 */
const void* emotion_partition_storage_get_partition(void);

#ifdef __cplusplus
}
#endif

#endif /* EMOTION_PARTITION_STORAGE_H */
