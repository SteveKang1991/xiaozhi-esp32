/**
 * @file emotion_partition_storage.c
 *
 * 文件表 layout (in emotions partition):
 *
 *   [Header  64B] @ offset 0
 *     magic     uint32 = 'APSV'
 *     version   uint8  = 1
 *     entry_count uint8
 *     reserved  (50 bytes)
 *
 *   [Entry  64B] @ 64 + i*64, i=0..N-1
 *     name[32]
 *     offset     uint32   // 数据起始（相对分区）
 *     size       uint32   // 实际大小（不含 padding）
 *     crc32      uint32   // data crc32
 *     reserved[16]
 *
 *   [Data ...] @ EMOTION_STORAGE_DATA_RESERVE 起，按 4KB 对齐追加
 *
 * 写入策略：
 *   - upsert: 找到 name 相同 entry → 数据原地写 (size 不能变)；
 *             否则追加 entry + 数据
 *   - entry_count == MAX → 整分区擦除，重写所有文件（把 data 紧排在 header+entries 后）
 *
 * 兼容性：保留 magic='APSV'（0x56535041），使本模块能识别旧版本
 *        asset_partition_storage 打出的 bin 镜像（仅 magic + layout 兼容）。
 */

#include "emotion_partition_storage.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_flash.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* TAG = "EmotionStore";

/* 文件表自身的占位大小 (header + max entries) */
#define HEADER_SIZE            64
#define ENTRY_SIZE             64
#define ENTRY_TOTAL_SIZE       (HEADER_SIZE + ENTRY_SIZE * EMOTION_STORAGE_MAX_ENTRIES)

/* 单文件的最大允许大小（防止单文件撑爆分区） */
#define MAX_SINGLE_FILE_SIZE   (8 * 1024 * 1024)   /* 8MB */

typedef struct __attribute__((packed)) {
    char     name[EMOTION_STORAGE_NAME_MAX];
    uint32_t offset;
    uint32_t size;
    uint32_t crc32;
    uint8_t  flags;           /* 0=valid, 1=deleted */
    uint8_t  reserved[ENTRY_SIZE - EMOTION_STORAGE_NAME_MAX - 4 * 3 - 1];
} emotion_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  entry_count;     /* 总槽位数（valid + deleted），不含 reserved */
    uint8_t  reserved0;
    uint8_t  reserved1;
    uint32_t deleted_count;   /* 累计删除次数，用于触发 defrag */
    uint8_t  reserved[HEADER_SIZE - 12];
} emotion_header_t;

_Static_assert(sizeof(emotion_header_t) == HEADER_SIZE, "header size mismatch");
_Static_assert(sizeof(emotion_entry_t) == ENTRY_SIZE, "entry size mismatch");

static esp_partition_t* s_part = NULL;
static emotion_header_t s_header;
static emotion_entry_t  s_entries[EMOTION_STORAGE_MAX_ENTRIES];
static SemaphoreHandle_t s_mutex = NULL;

static uint32_t align_up(uint32_t v, uint32_t a) {
    return (v + (a - 1)) & ~(a - 1);
}

/* 全文件 flash 写入。必须 4KB 对齐且 size 是 4KB 的整数倍。
 * 注意 ESP-IDF esp_partition_write 支持非对齐 offset，但是只支持 4KB 整数 size。 */
static esp_err_t flash_write_aligned(uint32_t offset, const void* data, uint32_t size) {
    if (offset % 4096 != 0 || size % 4096 != 0) {
        ESP_LOGE(TAG, "flash_write_aligned: offset/size 必须 4KB 对齐 (off=%u size=%u)", offset, size);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = esp_partition_erase_range(s_part, offset, size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "erase_range(off=%u size=%u) failed: %s", offset, size, esp_err_to_name(ret));
        return ret;
    }
    ret = esp_partition_write(s_part, offset, data, size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "partition_write failed: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

/* 把单个文件 (可能非 4KB 对齐大小) 写入 flash，padding 0xFF */
static esp_err_t flash_write_padded(uint32_t offset, const void* data, uint32_t size) {
    uint32_t pad_size = align_up(size, EMOTION_STORAGE_ALIGN);
    if (offset % EMOTION_STORAGE_ALIGN != 0) {
        ESP_LOGE(TAG, "flash_write_padded: offset 必须 4KB 对齐 (off=%u)", offset);
        return ESP_ERR_INVALID_ARG;
    }
    if (pad_size == size) {
        return flash_write_aligned(offset, data, pad_size);
    }
    /* 分配临时缓冲 */
    uint8_t* tmp = (uint8_t*)heap_caps_malloc(pad_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tmp) {
        tmp = (uint8_t*)malloc(pad_size);
    }
    if (!tmp) {
        ESP_LOGE(TAG, "无法分配 %u 字节缓冲", pad_size);
        return ESP_ERR_NO_MEM;
    }
    memcpy(tmp, data, size);
    memset(tmp + size, 0xFF, pad_size - size);
    esp_err_t ret = flash_write_aligned(offset, tmp, pad_size);
    free(tmp);
    return ret;
}

/* 重写 header + 全部 entries 到分区头部 */
static esp_err_t rewrite_header_and_entries(void) {
    /* 整体作为一个 sector (4KB) 写：方便 erase */
    uint8_t buf[EMOTION_STORAGE_ALIGN];
    memset(buf, 0xFF, sizeof(buf));
    memcpy(buf, &s_header, sizeof(s_header));
    memcpy(buf + HEADER_SIZE, s_entries, sizeof(emotion_entry_t) * s_header.entry_count);
    /* 即使有未使用 entry，把它们的字节也写进去保持 layout 一致 */
    return flash_write_aligned(0, buf, EMOTION_STORAGE_ALIGN);
}

/* 计算"实际使用"的数据末尾（考虑 DELETED entry 的 data 已死，VALID 的必须保留） */
static uint32_t compute_data_end(void) {
    uint32_t max_end = EMOTION_STORAGE_DATA_RESERVE;
    for (uint8_t i = 0; i < s_header.entry_count; i++) {
        if (s_entries[i].flags == EMOTION_FLAG_VALID) {
            uint32_t end = s_entries[i].offset + align_up(s_entries[i].size, EMOTION_STORAGE_ALIGN);
            if (end > max_end) max_end = end;
        }
    }
    return max_end;
}

/* 找到第一个空闲 entry 槽位（deleted 或超出当前 count） */
static int find_free_entry_slot(void) {
    /* 先找 deleted 的复用 */
    for (uint8_t i = 0; i < s_header.entry_count; i++) {
        if (s_entries[i].flags == EMOTION_FLAG_DELETED) {
            return (int)i;
        }
    }
    /* 否则 append 到末尾 */
    if (s_header.entry_count < EMOTION_STORAGE_MAX_ENTRIES) {
        return (int)(s_header.entry_count++);
    }
    return -1;
}

/* 校验分区大小 */
static bool partition_has_room(uint32_t data_offset, uint32_t data_size) {
    if (data_offset + data_size > s_part->size) {
        return false;
    }
    return true;
}

esp_err_t emotion_partition_storage_init(void) {
    if (s_part && s_mutex) return ESP_OK;

    s_part = (esp_partition_t*)esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                        ESP_PARTITION_SUBTYPE_ANY,
                                                        "emotions");
    if (!s_part) {
        ESP_LOGE(TAG, "找不到名为 'emotions' 的 DATA 分区");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "找到 emotions 分区: offset=0x%lx size=%lu KB",
             (unsigned long)s_part->address, (unsigned long)(s_part->size / 1024));

    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) return ESP_ERR_NO_MEM;
    }

    /* 读 header */
    esp_err_t ret = esp_partition_read(s_part, 0, &s_header, sizeof(s_header));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "读 header 失败: %s", esp_err_to_name(ret));
        return ret;
    }
    if (s_header.magic != EMOTION_STORAGE_HEADER_MAGIC || s_header.version != 1) {
        ESP_LOGW(TAG, "header 无效 (magic=0x%08lx ver=%u)，格式化",
                 (unsigned long)s_header.magic, s_header.version);
        memset(&s_header, 0, sizeof(s_header));
        s_header.magic = EMOTION_STORAGE_HEADER_MAGIC;
        s_header.version = 1;
        s_header.entry_count = 0;
        s_header.deleted_count = 0;
        memset(s_entries, 0, sizeof(s_entries));
        ret = rewrite_header_and_entries();
        if (ret != ESP_OK) return ret;
    } else {
        if (s_header.entry_count > EMOTION_STORAGE_MAX_ENTRIES) {
            ESP_LOGE(TAG, "entry_count=%u 超过 MAX=%d，重置", s_header.entry_count, EMOTION_STORAGE_MAX_ENTRIES);
            s_header.entry_count = 0;
        }
        /* 读 entries */
        ret = esp_partition_read(s_part, HEADER_SIZE, s_entries,
                                 sizeof(emotion_entry_t) * s_header.entry_count);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "读 entries 失败: %s", esp_err_to_name(ret));
            return ret;
        }
        /* 防御：name[32] 必须以 '\0' 结尾，否则后续 strcmp/printf 会越过 name 数组。
         * 历史镜像（旧版 emotion_bin_packer / 老镜像）可能没显式 \0，这里强制补 \0。 */
        for (uint8_t i = 0; i < s_header.entry_count; i++) {
            size_t name_len = strnlen(s_entries[i].name, EMOTION_STORAGE_NAME_MAX);
            if (name_len >= EMOTION_STORAGE_NAME_MAX) {
                ESP_LOGW(TAG, "entry[%u] name 无 \\0 终止，强制截断", i);
                s_entries[i].name[EMOTION_STORAGE_NAME_MAX - 1] = '\0';
            }
        }
    }

    uint8_t valid_count = 0;
    for (uint8_t i = 0; i < s_header.entry_count; i++) {
        if (s_entries[i].flags == EMOTION_FLAG_VALID) {
            ESP_LOGI(TAG, "  [%u] %s off=0x%x size=%u", i, s_entries[i].name,
                     (unsigned)s_entries[i].offset, (unsigned)s_entries[i].size);
            valid_count++;
        }
    }
    ESP_LOGI(TAG, "init OK: %u valid / %u total slots (deleted_count=%u)",
             valid_count, s_header.entry_count, (unsigned)s_header.deleted_count);
    return ESP_OK;
}

esp_err_t emotion_partition_storage_format(void) {
    if (!s_part) {
        esp_err_t r = emotion_partition_storage_init();
        if (r != ESP_OK) return r;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    ESP_LOGW(TAG, "擦除整个 emotions 分区 (size=%lu)...", (unsigned long)s_part->size);
    esp_err_t ret = esp_partition_erase_range(s_part, 0, s_part->size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "erase 整分区失败: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_mutex);
        return ret;
    }
    memset(&s_header, 0, sizeof(s_header));
    s_header.magic = EMOTION_STORAGE_HEADER_MAGIC;
    s_header.version = 1;
    s_header.entry_count = 0;
    s_header.deleted_count = 0;
    memset(s_entries, 0, sizeof(s_entries));
    ret = rewrite_header_and_entries();
    xSemaphoreGive(s_mutex);
    return ret;
}

esp_err_t emotion_partition_storage_upsert(const char* name, const void* data, size_t size) {
    if (!name || !data || size == 0) return ESP_ERR_INVALID_ARG;
    if (size > MAX_SINGLE_FILE_SIZE) {
        ESP_LOGE(TAG, "文件 %s size=%u 超过 MAX_SINGLE_FILE_SIZE=%u",
                 name, (unsigned)size, (unsigned)MAX_SINGLE_FILE_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }
    if (strlen(name) >= EMOTION_STORAGE_NAME_MAX) {
        ESP_LOGE(TAG, "文件名 %s 超过 %d 字节", name, EMOTION_STORAGE_NAME_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_part) {
        esp_err_t r = emotion_partition_storage_init();
        if (r != ESP_OK) return r;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t ret = ESP_OK;

    uint32_t crc = esp_rom_crc32_le(0, (const uint8_t*)data, size);

    /* 1) 找到同名 entry → 覆盖（valid 或 deleted） */
    for (uint8_t i = 0; i < s_header.entry_count; i++) {
        if (strcmp(s_entries[i].name, name) == 0) {
            if (s_entries[i].size == size) {
                ESP_LOGI(TAG, "%s %s (offset=0x%x size=%u)",
                         s_entries[i].flags == EMOTION_FLAG_VALID ? "覆盖" : "复活",
                         name, (unsigned)s_entries[i].offset, (unsigned)size);
                ret = flash_write_padded(s_entries[i].offset, data, size);
                if (ret == ESP_OK) {
                    s_entries[i].crc32 = crc;
                    s_entries[i].flags = EMOTION_FLAG_VALID;
                    ret = rewrite_header_and_entries();
                }
                goto out;
            }
            /* 同名但 size 变化：把旧 entry 标记 deleted，data 区保留待 defrag 回收。
             * 后续按新文件流程在分区末尾追加新 entry + data。 */
            ESP_LOGW(TAG, "同名 %s 大小变化 (old=%u new=%u)，旧 entry 标记 deleted，按新文件追加",
                     name, (unsigned)s_entries[i].size, (unsigned)size);
            if (s_entries[i].flags == EMOTION_FLAG_VALID &&
                s_header.deleted_count < 0xFFFFFFFF) {
                s_header.deleted_count++;
            }
            s_entries[i].flags = EMOTION_FLAG_DELETED;
            /* 不要 break：可能存在多个同名 entry（历史遗留），全部标记 deleted */
        }
    }

    /* 2) 新文件：先尝试触发 defrag（如果 deleted_count 超阈值） */
    if (s_header.deleted_count >= EMOTION_DEFRAG_THRESHOLD) {
        ESP_LOGI(TAG, "deleted_count=%u 达阈值 %u，触发碎片整理",
                 (unsigned)s_header.deleted_count, EMOTION_DEFRAG_THRESHOLD);
        ret = emotion_partition_storage_defragment();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "defragment 失败: %s", esp_err_to_name(ret));
            goto out;
        }
    }

    /* 3) 找 entry 槽位（复用 deleted 或 append） */
    int slot = find_free_entry_slot();
    if (slot < 0) {
        ESP_LOGE(TAG, "entry 满了 (%d)，无法新增", EMOTION_STORAGE_MAX_ENTRIES);
        ret = ESP_ERR_NO_MEM;
        goto out;
    }

    /* 4) 找 data offset：所有 valid entry 的 data 之后的下一个对齐槽 */
    uint32_t off = align_up(compute_data_end(), EMOTION_STORAGE_ALIGN);
    uint32_t pad = align_up((uint32_t)size, EMOTION_STORAGE_ALIGN);
    if (!partition_has_room(off, pad)) {
        ESP_LOGE(TAG, "分区空间不足: need off=0x%x+%u, part_size=%lu",
                 (unsigned)off, (unsigned)pad, (unsigned long)s_part->size);
        ret = ESP_ERR_NO_MEM;
        goto out;
    }
    ESP_LOGI(TAG, "新增 %s (offset=0x%x size=%u pad=%u slot=%d)",
             name, (unsigned)off, (unsigned)size, (unsigned)pad, slot);
    ret = flash_write_padded(off, data, size);
    if (ret != ESP_OK) goto out;

    emotion_entry_t* e = &s_entries[slot];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, EMOTION_STORAGE_NAME_MAX - 1);
    e->name[EMOTION_STORAGE_NAME_MAX - 1] = '\0';
    e->offset = off;
    e->size = size;
    e->crc32 = crc;
    e->flags = EMOTION_FLAG_VALID;

    ret = rewrite_header_and_entries();

out:
    xSemaphoreGive(s_mutex);
    return ret;
}

esp_err_t emotion_partition_storage_delete(const char* name) {
    if (!name) return ESP_ERR_INVALID_ARG;
    if (!s_part) {
        esp_err_t r = emotion_partition_storage_init();
        if (r != ESP_OK) return r;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    for (uint8_t i = 0; i < s_header.entry_count; i++) {
        if (s_entries[i].flags == EMOTION_FLAG_VALID && strcmp(s_entries[i].name, name) == 0) {
            ESP_LOGI(TAG, "标记删除 %s (slot=%u data 暂不回收)", name, i);
            s_entries[i].flags = EMOTION_FLAG_DELETED;
            if (s_header.deleted_count < 0xFFFFFFFF) s_header.deleted_count++;
            ret = rewrite_header_and_entries();
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return ret;
}

esp_err_t emotion_partition_storage_defragment(void) {
    /* 假设调用方已持有 mutex */
    if (!s_part) return ESP_ERR_INVALID_STATE;

    /* 1) 收集所有 VALID entries（按原 offset 升序，保证稳定） */
    uint8_t valid_idx[EMOTION_STORAGE_MAX_ENTRIES];
    uint8_t valid_n = 0;
    for (uint8_t i = 0; i < s_header.entry_count; i++) {
        if (s_entries[i].flags == EMOTION_FLAG_VALID) {
            valid_idx[valid_n++] = i;
        }
    }

    /* 没有有效文件：整分区擦除重来 */
    if (valid_n == 0) {
        ESP_LOGI(TAG, "defragment: 无有效文件，擦整分区");
        esp_err_t ret = esp_partition_erase_range(s_part, 0, s_part->size);
        if (ret != ESP_OK) return ret;
        memset(&s_header, 0, sizeof(s_header));
        s_header.magic = EMOTION_STORAGE_HEADER_MAGIC;
        s_header.version = 1;
        s_header.deleted_count = 0;
        s_header.entry_count = 0;
        memset(s_entries, 0, sizeof(s_entries));
        return rewrite_header_and_entries();
    }

    /* 2) 按 offset 升序排列（稳定排序） */
    for (uint8_t i = 1; i < valid_n; i++) {
        uint8_t key = valid_idx[i];
        uint8_t j = i;
        while (j > 0 && s_entries[valid_idx[j-1]].offset > s_entries[key].offset) {
            valid_idx[j] = valid_idx[j-1];
            j--;
        }
        valid_idx[j] = key;
    }

    /* 3) 计算新 layout + 把所有 valid data 读到一个临时缓冲（顺序连接） */
    uint32_t total_data = 0;
    for (uint8_t i = 0; i < valid_n; i++) {
        emotion_entry_t* src = &s_entries[valid_idx[i]];
        total_data += align_up(src->size, EMOTION_STORAGE_ALIGN);
    }
    if (total_data > (s_part->size * 4) / 5) {
        ESP_LOGE(TAG, "defragment: 总数据 %u 字节太大", (unsigned)total_data);
        return ESP_ERR_NO_MEM;
    }

    uint8_t* blob = (uint8_t*)heap_caps_malloc(total_data, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!blob) blob = (uint8_t*)malloc(total_data);
    if (!blob) {
        ESP_LOGE(TAG, "defragment: 无法分配 %u 字节", (unsigned)total_data);
        return ESP_ERR_NO_MEM;
    }

    emotion_entry_t new_entries[EMOTION_STORAGE_MAX_ENTRIES];
    memset(new_entries, 0, sizeof(new_entries));
    uint32_t cur_off = EMOTION_STORAGE_DATA_RESERVE;
    for (uint8_t i = 0; i < valid_n; i++) {
        emotion_entry_t* src = &s_entries[valid_idx[i]];
        uint32_t pad = align_up(src->size, EMOTION_STORAGE_ALIGN);
        esp_err_t er = esp_partition_read(s_part, src->offset, blob + (cur_off - EMOTION_STORAGE_DATA_RESERVE), pad);
        if (er != ESP_OK) {
            ESP_LOGE(TAG, "defragment: 读 %s 失败: %s", src->name, esp_err_to_name(er));
            free(blob);
            return er;
        }
        new_entries[i].offset = cur_off;
        new_entries[i].size = src->size;
        new_entries[i].crc32 = src->crc32;
        new_entries[i].flags = EMOTION_FLAG_VALID;
        strncpy(new_entries[i].name, src->name, EMOTION_STORAGE_NAME_MAX - 1);
        cur_off += pad;
    }
    ESP_LOGI(TAG, "defragment: %u 个有效文件，新数据区大小 ~%u KB", valid_n,
             (unsigned)((cur_off - EMOTION_STORAGE_DATA_RESERVE) / 1024));

    /* 4) 擦整分区 */
    esp_err_t ret = esp_partition_erase_range(s_part, 0, s_part->size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "defragment: erase 失败: %s", esp_err_to_name(ret));
        free(blob);
        return ret;
    }

    /* 5) 更新 in-memory header + entries */
    memset(&s_header, 0, sizeof(s_header));
    s_header.magic = EMOTION_STORAGE_HEADER_MAGIC;
    s_header.version = 1;
    s_header.entry_count = valid_n;
    s_header.deleted_count = 0;
    memcpy(s_entries, new_entries, sizeof(emotion_entry_t) * valid_n);
    /* entry_count 之后位置清 0 防止读到旧 entry */
    memset(&s_entries[valid_n], 0, sizeof(emotion_entry_t) * (EMOTION_STORAGE_MAX_ENTRIES - valid_n));

    /* 6) 写 header + entries */
    ret = rewrite_header_and_entries();
    if (ret != ESP_OK) {
        free(blob);
        return ret;
    }

    /* 7) 写所有 data 到新 offset */
    for (uint8_t i = 0; i < valid_n; i++) {
        uint32_t off = s_entries[i].offset;
        uint32_t pad = align_up(s_entries[i].size, EMOTION_STORAGE_ALIGN);
        ret = flash_write_aligned(off, blob + (off - EMOTION_STORAGE_DATA_RESERVE), pad);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "defragment: 写 %s 失败: %s", s_entries[i].name, esp_err_to_name(ret));
            free(blob);
            return ret;
        }
    }

    free(blob);
    ESP_LOGI(TAG, "defragment: 完成");
    return ESP_OK;
}

bool emotion_partition_storage_find(const char* name, uint32_t* out_offset, uint32_t* out_size) {
    if (!s_part || !name) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool found = false;
    for (uint8_t i = 0; i < s_header.entry_count; i++) {
        if (s_entries[i].flags == EMOTION_FLAG_VALID && strcmp(s_entries[i].name, name) == 0) {
            if (out_offset) *out_offset = s_entries[i].offset;
            if (out_size)   *out_size = s_entries[i].size;
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return found;
}

esp_err_t emotion_partition_storage_read(const char* name, void* buf, size_t buf_size, size_t* out_size) {
    uint32_t off = 0, sz = 0;
    if (!emotion_partition_storage_find(name, &off, &sz)) return ESP_ERR_NOT_FOUND;
    if (buf_size < sz) return ESP_ERR_INVALID_SIZE;
    esp_err_t ret = esp_partition_read(s_part, off, buf, sz);
    if (ret == ESP_OK && out_size) *out_size = sz;
    return ret;
}

void emotion_partition_storage_enum(emotion_storage_enum_cb cb, void* user) {
    if (!s_part || !cb) return;
    /* 暂存 entries 快照，避免 callback 内 delete/upsert 持锁时再次递归 xSemaphoreTake 导致死锁 */
    emotion_entry_t snapshot[EMOTION_STORAGE_MAX_ENTRIES];
    uint8_t n = 0;
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        n = s_header.entry_count;
        memcpy(snapshot, s_entries, sizeof(emotion_entry_t) * n);
        xSemaphoreGive(s_mutex);
    }
    for (uint8_t i = 0; i < n; i++) {
        if (snapshot[i].flags == EMOTION_FLAG_VALID) {
            cb(snapshot[i].name, snapshot[i].offset, snapshot[i].size, user);
        }
    }
}

const void* emotion_partition_storage_get_partition(void) {
    return s_part;
}
