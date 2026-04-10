/**
 * @file mjpeg_player.c
 * @brief 🎬 MJPEG 零拷贝播放器 — 异步读取+硬件解码流水线
 *
 * 架构：
 *   读取任务（SD→提取帧→校验→DMA缓冲区）
 *        ↓ frame_queue
 *   解码任务（硬件JPEG解码→面板帧缓冲区→送显）
 *        ↓ free_queue
 *   读取任务（回收DMA缓冲区，继续读取）
 *
 * 优化点：
 *   1. 异步流水线：SD 读取与 JPEG 硬件解码并行执行
 *   2. 零拷贝输出：JPEG 解码直接写入 DPI 面板帧缓冲区
 *   3. 双 DMA 输入缓冲区 ping-pong
 *   4. Ring buffer 读取：read position 跟踪，减少 memmove
 *   5. setvbuf 加速 SD 文件读取
 *   6. 帧预校验：EOI + 头部解析 + scan 数据完整性
 */
#include "mjpeg_player.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_cache.h"
#include "driver/jpeg_decode.h"

static const char *TAG = "🎬 MJPEG播放器";

#define READ_BUF_SIZE    (512 * 1024)
#define FRAME_BUF_SIZE   (300 * 1024)
#define NUM_DMA_BUFS     2
#define FILE_IO_BUF_SIZE (64 * 1024)

/* ─────────────── 帧消息（队列传递） ─────────────── */
typedef struct {
    uint8_t *buf;
    int len;    /* >0: 有效帧, 0: 文件结束, -1: 停止信号 */
} frame_msg_t;

/* ─────────────── 读取上下文（Ring Buffer） ─────────────── */
typedef struct {
    uint8_t *buf;
    int capacity;
    int start;
    int end;
    FILE *fp;
    bool eof;
} read_ctx_t;

/* ─────────────── 播放器状态 ─────────────── */
static volatile bool s_running = false;
static TaskHandle_t s_read_task = NULL;
static TaskHandle_t s_decode_task = NULL;
static mjpeg_player_cfg_t s_cfg;
static QueueHandle_t s_frame_queue;
static QueueHandle_t s_free_queue;
static uint8_t *s_dma_bufs[NUM_DMA_BUFS];

/* ═══════════════════════════════════════════════════════════
 *  Ring Buffer 操作
 * ═══════════════════════════════════════════════════════════ */

static inline int ctx_avail(const read_ctx_t *ctx)
{
    return ctx->end - ctx->start;
}

/** 填充读取缓冲区（需要时紧凑数据） */
static bool ctx_fill(read_ctx_t *ctx)
{
    if (ctx->eof) return ctx_avail(ctx) > 0;

    /* 末尾空间不足 1/4 时紧凑到头部 */
    if (ctx->capacity - ctx->end < ctx->capacity / 4 && ctx->start > 0) {
        int len = ctx_avail(ctx);
        memmove(ctx->buf, ctx->buf + ctx->start, len);
        ctx->start = 0;
        ctx->end = len;
    }

    int space = ctx->capacity - ctx->end;
    if (space > 0) {
        int got = fread(ctx->buf + ctx->end, 1, space, ctx->fp);
        ctx->end += got;
        if (got == 0) ctx->eof = true;
    }
    return ctx_avail(ctx) > 0;
}

/* ═══════════════════════════════════════════════════════════
 *  帧提取器（Marker 感知 + FF D8 截断检测）
 * ═══════════════════════════════════════════════════════════ */

/**
 * 从读取上下文中提取下一个完整 JPEG 帧
 *
 * 关键改进：在 scan 数据中遇到 FF D8（下一帧 SOI）时，
 * 判定当前帧损坏，丢弃并从新 SOI 重新开始。
 */
static bool extract_frame(read_ctx_t *ctx, const uint8_t **out_data, int *out_len)
{
    while (s_running) {
        int avail = ctx_avail(ctx);
        if (avail < 4) {
            if (!ctx_fill(ctx)) return false;
            continue;
        }

        uint8_t *base = ctx->buf + ctx->start;

        /* 1. 找 SOI (FF D8) */
        int soi = -1;
        for (int i = 0; i < avail - 1; i++) {
            if (base[i] == 0xFF && base[i + 1] == 0xD8) {
                soi = i;
                break;
            }
        }
        if (soi < 0) {
            ctx->start = ctx->end - 1;
            if (!ctx_fill(ctx)) return false;
            continue;
        }
        if (soi > 0) {
            ctx->start += soi;
            avail -= soi;
            base = ctx->buf + ctx->start;
        }

        /* 2. 解析 marker 结构，寻找 EOI */
        int pos = 2;
        bool in_scan = false;
        bool restart = false;

        while (pos < avail - 1) {
            if (in_scan) {
                /* ── entropy-coded data 扫描 ── */
                if (base[pos] != 0xFF) { pos++; continue; }
                if (pos + 1 >= avail) break;
                uint8_t next = base[pos + 1];

                if (next == 0x00)                          { pos += 2; continue; }  /* 字节填充 */
                if (next == 0xFF)                          { pos += 1; continue; }  /* FF 填充 */
                if (next >= 0xD0 && next <= 0xD7)          { pos += 2; continue; }  /* RST */
                if (next == 0xD9) {
                    /* ✅ 找到 EOI — 帧完整 */
                    *out_data = base;
                    *out_len = pos + 2;
                    ctx->start += pos + 2;
                    return true;
                }
                if (next == 0xD8) {
                    /* ❌ scan 数据中出现下一帧 SOI — 当前帧截断 */
                    ctx->start += pos;
                    restart = true;
                    break;
                }
                /* 其他 marker 在 scan 中（异常），跳过 */
                pos += 2;
            } else {
                /* ── marker 段解析 ── */
                if (base[pos] != 0xFF) { pos++; continue; }
                if (pos + 1 >= avail) break;
                uint8_t marker = base[pos + 1];

                if (marker == 0xD9) {
                    /* EOI before SOS — 空帧，跳过 */
                    ctx->start += pos + 2;
                    restart = true;
                    break;
                }
                if (marker == 0xD8) {
                    /* 连续 SOI — 从此处重新开始 */
                    ctx->start += pos;
                    restart = true;
                    break;
                }
                if (marker == 0x00 || marker == 0xFF) { pos += 1; continue; }

                if (marker == 0xDA) {
                    /* SOS — 进入 scan 模式 */
                    if (pos + 3 >= avail) break;
                    int seg_len = (base[pos + 2] << 8) | base[pos + 3];
                    if (seg_len < 2) { pos += 2; continue; }
                    pos += 2 + seg_len;
                    in_scan = true;
                    continue;
                }
                if ((marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
                    pos += 2;
                    continue;
                }

                /* 带长度字段的 marker 段 */
                if (pos + 3 >= avail) break;
                int seg_len = (base[pos + 2] << 8) | base[pos + 3];
                if (seg_len < 2) { pos += 2; continue; }
                int seg_end = pos + 2 + seg_len;
                if (seg_end > avail) break;  /* 数据不够，需要继续读取 */
                pos = seg_end;
            }
        }

        if (restart) continue;

        /* 数据不足，尝试读取更多 */
        if (ctx->eof) return false;
        if (avail >= ctx->capacity) {
            ESP_LOGW(TAG, "⚠️ 帧超过 %dKB，跳过", ctx->capacity / 1024);
            ctx->start += 2;
            continue;
        }
        ctx_fill(ctx);
    }
    return false;
}

/* ═══════════════════════════════════════════════════════════
 *  帧校验（软件层面拦截损坏帧）
 * ═══════════════════════════════════════════════════════════ */

static bool validate_frame(const uint8_t *data, int len,
                            uint16_t width, uint16_t height)
{
    if (len < 100) return false;

    /* 1. EOI 标记 */
    if (data[len - 2] != 0xFF || data[len - 1] != 0xD9) return false;

    /* 2. 头部解析 */
    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(data, len, &info) != ESP_OK) return false;

    /* 3. 分辨率匹配 */
    if (info.width != width || info.height != height) return false;

    /* 4. scan 数据最小长度检查 */
    int min_scan = (width * height) / 512;
    if (min_scan < 256) min_scan = 256;

    for (int i = 0; i < len - 1; i++) {
        if (data[i] == 0xFF && data[i + 1] == 0xDA) {
            if (i + 3 >= len) return false;
            int sos_len = (data[i + 2] << 8) | data[i + 3];
            int scan_start = i + 2 + sos_len;
            if (scan_start >= len) return false;
            return (len - 2 - scan_start) >= min_scan;
        }
    }
    return false;
}

/* ═══════════════════════════════════════════════════════════
 *  读取任务：从 SD 提取帧 → 校验 → 入队
 * ═══════════════════════════════════════════════════════════ */

static void mjpeg_read_task(void *arg)
{
    ESP_LOGI(TAG, "📜 读取任务启动");

    uint32_t skip_count = 0;
    uint8_t *read_buf = malloc(READ_BUF_SIZE);
    uint8_t *io_buf = malloc(FILE_IO_BUF_SIZE);
    if (!read_buf) {
        ESP_LOGE(TAG, "❌ 分配读取缓冲区失败");
        s_running = false;
        goto exit;
    }

    read_ctx_t ctx = {
        .buf = read_buf,
        .capacity = READ_BUF_SIZE,
    };

    do {
        ctx.fp = fopen(s_cfg.file_path, "rb");
        if (!ctx.fp) {
            ESP_LOGE(TAG, "❌ 无法打开文件: %s", s_cfg.file_path);
            break;
        }
        if (io_buf) {
            setvbuf(ctx.fp, (char *)io_buf, _IOFBF, FILE_IO_BUF_SIZE);
        }

        fseek(ctx.fp, 0, SEEK_END);
        long file_size = ftell(ctx.fp);
        fseek(ctx.fp, 0, SEEK_SET);
        ESP_LOGI(TAG, "📄 文件大小: %.1f MB", file_size / (1024.0 * 1024.0));

        ctx.start = 0;
        ctx.end = 0;
        ctx.eof = false;

        const uint8_t *frame_data;
        int frame_len;

        while (s_running && extract_frame(&ctx, &frame_data, &frame_len)) {
            if (!validate_frame(frame_data, frame_len,
                                 s_cfg.screen_width, s_cfg.screen_height)) {
                skip_count++;
                if (skip_count <= 5 || skip_count % 100 == 0) {
                    ESP_LOGW(TAG, "🛡️ 帧校验失败 #%lu（帧=%d字节）",
                             (unsigned long)skip_count, frame_len);
                }
                continue;
            }

            /* 获取空闲 DMA 缓冲区 */
            frame_msg_t msg;
            if (xQueueReceive(s_free_queue, &msg, pdMS_TO_TICKS(200)) != pdTRUE) {
                continue;
            }

            /* 拷贝到 DMA 缓冲区 + Cache 刷新 */
            memcpy(msg.buf, frame_data, frame_len);
            esp_cache_msync(msg.buf, frame_len,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
            msg.len = frame_len;

            xQueueSend(s_frame_queue, &msg, portMAX_DELAY);
        }

        fclose(ctx.fp);
        ctx.fp = NULL;

        /* 发送文件结束标记（不携带 DMA 缓冲区） */
        frame_msg_t eof = { .buf = NULL, .len = 0 };
        xQueueSend(s_frame_queue, &eof, portMAX_DELAY);

    } while (s_running && s_cfg.loop);

    /* 发送停止信号 */
    frame_msg_t stop_msg = { .buf = NULL, .len = -1 };
    xQueueSend(s_frame_queue, &stop_msg, portMAX_DELAY);

exit:
    free(read_buf);
    free(io_buf);
    ESP_LOGI(TAG, "📜 读取任务结束 (跳过帧: %lu)", (unsigned long)skip_count);
    s_read_task = NULL;
    vTaskDelete(NULL);
}

/* ═══════════════════════════════════════════════════════════
 *  解码任务：硬件 JPEG 解码 → 零拷贝送显
 * ═══════════════════════════════════════════════════════════ */

static void mjpeg_decode_task(void *arg)
{
    ESP_LOGI(TAG, "🔧 解码任务启动");

    jpeg_decode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,
        .timeout_ms = 200,
    };
    jpeg_decoder_handle_t decoder = NULL;
    esp_err_t ret = jpeg_new_decoder_engine(&engine_cfg, &decoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ 创建硬件解码引擎失败: %s", esp_err_to_name(ret));
        s_running = false;
        goto exit;
    }
    ESP_LOGI(TAG, "✅ 硬件 JPEG 解码引擎已就绪 (超时=%dms)", engine_cfg.timeout_ms);

    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };

    uint32_t fb_size = s_cfg.screen_width * s_cfg.screen_height * sizeof(uint16_t);
    int fb_idx = 0;
    uint32_t frame_count = 0;
    uint32_t decode_errors = 0;
    int consecutive_errors = 0;
    int64_t start_time = esp_timer_get_time();

    const int64_t frame_interval_us = s_cfg.target_fps > 0 ?
        (1000000 / s_cfg.target_fps) : 0;

    while (s_running) {
        frame_msg_t msg;
        if (xQueueReceive(s_frame_queue, &msg, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }

        /* 停止信号 */
        if (msg.len < 0) break;

        /* 文件结束标记 — 打印统计，重置计数器 */
        if (msg.len == 0) {
            if (frame_count > 0) {
                int64_t elapsed = esp_timer_get_time() - start_time;
                ESP_LOGI(TAG, "✅ 播放结束: %lu帧, %.1f fps, %.1f秒, 错误%lu",
                         (unsigned long)frame_count,
                         frame_count * 1e6f / elapsed,
                         elapsed / 1e6f,
                         (unsigned long)decode_errors);
            }
            frame_count = 0;
            decode_errors = 0;
            start_time = esp_timer_get_time();
            continue;
        }

        int64_t t_start = esp_timer_get_time();

        /* 硬件解码 → 面板帧缓冲区 */
        uint32_t decoded_size = 0;
        ret = jpeg_decoder_process(decoder, &decode_cfg,
                                    msg.buf, (uint32_t)msg.len,
                                    (uint8_t *)s_cfg.fb[fb_idx], fb_size,
                                    &decoded_size);

        /* 立即归还 DMA 缓冲区，让读取任务继续工作 */
        frame_msg_t free_msg = { .buf = msg.buf, .len = 0 };
        xQueueSend(s_free_queue, &free_msg, portMAX_DELAY);

        if (ret != ESP_OK || decoded_size == 0) {
            decode_errors++;
            consecutive_errors++;
            if (decode_errors <= 5 || decode_errors % 50 == 0) {
                ESP_LOGW(TAG, "⚠️ 解码失败 #%lu (ret=%s, 帧=%d字节)",
                         (unsigned long)decode_errors, esp_err_to_name(ret), msg.len);
            }
            /* 重建解码引擎，重置 DMA2D 状态 */
            jpeg_del_decoder_engine(decoder);
            decoder = NULL;
            vTaskDelay(pdMS_TO_TICKS(5));
            ret = jpeg_new_decoder_engine(&engine_cfg, &decoder);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "❌ 重建解码引擎失败");
                break;
            }
            if (consecutive_errors >= 10) {
                ESP_LOGE(TAG, "❌ 连续 %d 帧解码失败", consecutive_errors);
                consecutive_errors = 0;
            }
            continue;
        }
        consecutive_errors = 0;

        if (frame_count == 0) {
            ESP_LOGI(TAG, "🎬 首帧: JPEG=%d字节, RGB565=%lu字节, 零拷贝模式",
                     msg.len, (unsigned long)decoded_size);
        }

        /* 送显 */
        esp_lcd_panel_draw_bitmap(s_cfg.panel, 0, 0,
                                   s_cfg.screen_width, s_cfg.screen_height,
                                   s_cfg.fb[fb_idx]);
        fb_idx = 1 - fb_idx;
        frame_count++;

        if (frame_count % 200 == 0) {
            int64_t elapsed = esp_timer_get_time() - start_time;
            ESP_LOGI(TAG, "📊 %lu帧, %.1f fps, 错误%lu",
                     (unsigned long)frame_count,
                     frame_count * 1e6f / elapsed,
                     (unsigned long)decode_errors);
        }

        /* 帧率控制 */
        if (frame_interval_us > 0) {
            int64_t elapsed = esp_timer_get_time() - t_start;
            if (elapsed < frame_interval_us) {
                vTaskDelay(pdMS_TO_TICKS((frame_interval_us - elapsed) / 1000));
            }
        }
    }

exit:
    if (decoder) jpeg_del_decoder_engine(decoder);
    ESP_LOGI(TAG, "🔧 解码任务结束");
    s_decode_task = NULL;
    vTaskDelete(NULL);
}

/* ═══════════════════════════════════════════════════════════
 *  公有 API
 * ═══════════════════════════════════════════════════════════ */

esp_err_t mjpeg_player_start(const mjpeg_player_cfg_t *cfg)
{
    if (!cfg || !cfg->file_path || !cfg->panel || !cfg->fb[0] || !cfg->fb[1]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running) {
        ESP_LOGW(TAG, "⚠️ 播放器已在运行");
        return ESP_ERR_INVALID_STATE;
    }

    s_cfg = *cfg;
    s_running = true;

    /* 创建队列 */
    s_frame_queue = xQueueCreate(NUM_DMA_BUFS + 2, sizeof(frame_msg_t));
    s_free_queue = xQueueCreate(NUM_DMA_BUFS, sizeof(frame_msg_t));

    /* 分配 DMA 输入缓冲区 */
    jpeg_decode_memory_alloc_cfg_t in_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };

    for (int i = 0; i < NUM_DMA_BUFS; i++) {
        size_t actual = 0;
        s_dma_bufs[i] = jpeg_alloc_decoder_mem(FRAME_BUF_SIZE, &in_mem_cfg, &actual);
        if (!s_dma_bufs[i]) {
            ESP_LOGE(TAG, "❌ 分配 DMA 缓冲区 %d 失败", i);
            s_running = false;
            return ESP_ERR_NO_MEM;
        }
        frame_msg_t msg = { .buf = s_dma_bufs[i], .len = 0 };
        xQueueSend(s_free_queue, &msg, 0);
        ESP_LOGI(TAG, "💾 DMA 缓冲区 %d: %zuKB", i, actual / 1024);
    }

    uint32_t fb_size = s_cfg.screen_width * s_cfg.screen_height * sizeof(uint16_t);
    ESP_LOGI(TAG, "💾 异步流水线: 读取=%dKB, DMA=%dKB×%d, 输出=面板FB×2(%luKB×2)",
             READ_BUF_SIZE / 1024, FRAME_BUF_SIZE / 1024, NUM_DMA_BUFS,
             (unsigned long)(fb_size / 1024));

    /* 创建双任务 */
    BaseType_t ret;
    ret = xTaskCreate(mjpeg_decode_task, "mjpeg_dec", 8192,
                       NULL, configMAX_PRIORITIES - 2, &s_decode_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "❌ 创建解码任务失败");
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    ret = xTaskCreate(mjpeg_read_task, "mjpeg_read", 8192,
                       NULL, configMAX_PRIORITIES - 3, &s_read_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "❌ 创建读取任务失败");
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "🚀 异步流水线已启动: %s", s_cfg.file_path);
    return ESP_OK;
}

void mjpeg_player_stop(void)
{
    if (!s_running) return;
    ESP_LOGI(TAG, "⏹️ 正在停止播放...");
    s_running = false;

    /* 等待任务退出 */
    while (s_read_task || s_decode_task) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* 回收 DMA 缓冲区 */
    frame_msg_t msg;
    while (xQueueReceive(s_free_queue, &msg, 0) == pdTRUE) {
        /* 缓冲区已在 s_dma_bufs 中跟踪 */
    }
    while (xQueueReceive(s_frame_queue, &msg, 0) == pdTRUE) {
        /* 清空队列 */
    }
    for (int i = 0; i < NUM_DMA_BUFS; i++) {
        if (s_dma_bufs[i]) {
            free(s_dma_bufs[i]);
            s_dma_bufs[i] = NULL;
        }
    }

    vQueueDelete(s_frame_queue);
    vQueueDelete(s_free_queue);
    s_frame_queue = NULL;
    s_free_queue = NULL;

    ESP_LOGI(TAG, "💾 播放器资源已释放");
}

bool mjpeg_player_is_running(void)
{
    return s_running;
}
