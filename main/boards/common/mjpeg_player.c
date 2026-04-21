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
 *   3. 多枚 DMA 输入缓冲（默认 3）：加深流水线，读卡与解码更好重叠
 *   4. Ring buffer 读取：read position 跟踪，减少 memmove
 *   5. setvbuf 加速 SD 文件读取
 *   6. 帧预校验：EOI + SOF 快速读分辨率（避免每帧 jpeg_decoder_get_info）
 *   7. 视频高度小于面板时，下方 letterbox 固定填 RGB565 黑（0x0000）
 *   8. 小文件预载入 PSRAM（≤12MB）：读任务仅从内存取帧
 *   9. 大环形缓冲 / 5×DMA / 256KB stdio 缓冲；读任务优先级高于解码
 *  10. ROI 直写 DPI（PSRAM）后按 64B 对齐做一次 C2M，与 memcpy 带宽共同决定 ~4–7ms 波动
 */
#include "mjpeg_player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#if __has_include("esp_memory_utils.h")
#include "esp_memory_utils.h"
#define MJPEG_HAVE_ESP_PTR_EXTERNAL_RAM 1
#else
#define MJPEG_HAVE_ESP_PTR_EXTERNAL_RAM 0
#endif
#include "driver/jpeg_decode.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

static const char *TAG = "🎬 MJPEG播放器";
/* Panel geometry for fanfuture-p4-jd9165-wifi6-touch-lcd-7b */
#define MJPEG_PANEL_WIDTH  1024
#define MJPEG_PANEL_HEIGHT 600

static esp_err_t mjpeg_get_frame_buffers(esp_lcd_panel_handle_t panel, void **fb0, void **fb1)
{
    void *single_fb = NULL;
    esp_err_t ret = esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &single_fb);
    if (ret != ESP_OK) {
        return ret;
    }
    if (fb0) {
        *fb0 = single_fb;
    }
    if (fb1) {
        *fb1 = NULL;
    }
    return ESP_OK;
}

/** true：解码到自分配缓冲 + LVGL lv_canvas */
static bool s_embed_lvgl;
/** true：小缓冲解码 + esp_lcd_panel_draw_bitmap 仅 ROI，不经 LVGL */
static bool s_panel_roi_blit;
/** true：ROI 像素直接写入 DPI 帧缓冲（stride 拷贝），绕过 panel draw_bitmap */
static bool s_roi_write_dpi_fb;
static void *s_dpi_fb0;
static void *s_dpi_fb1;

/** 首若干帧打印 decode/blit 耗时，便于确认瓶颈（非 0 启用） */
#ifndef MJPEG_PROFILE_FIRST_FRAMES
#define MJPEG_PROFILE_FIRST_FRAMES 0
#endif

static void mjpeg_copy_rgb565_to_fb_stride(uint16_t *dst_base, int dst_stride_px,
                                         int x0, int y0, int w, int h,
                                         const uint16_t *src)
{
    uint16_t *row = dst_base + (size_t)y0 * (size_t)dst_stride_px + (size_t)x0;
    const size_t row_bytes = (size_t)w * sizeof(uint16_t);
    for (int y = 0; y < h; y++) {
        memcpy(row, src, row_bytes);
        row += dst_stride_px;
        src += w;
    }
}

/** ROI 模式：面板行数大于视频时在物理帧缓冲顶/底刷黑（RGB565 0），避免上下白边 */
static void mjpeg_roi_fill_letterbox_bands_rgb565(uint16_t *base, int stride_px, int panel_h,
                                                  int roi_y, int roi_h)
{
    if (roi_y < 0 || roi_h <= 0 || panel_h <= 0) {
        return;
    }
    const size_t row_bytes = (size_t)stride_px * sizeof(uint16_t);
    if (roi_y > 0) {
        uint16_t *row = base;
        for (int y = 0; y < roi_y; y++) {
            memset(row, 0, row_bytes);
            row += stride_px;
        }
    }
    const int y_after = roi_y + roi_h;
    if (y_after < panel_h) {
        uint16_t *row = base + (size_t)y_after * (size_t)stride_px;
        for (int y = y_after; y < panel_h; y++) {
            memset(row, 0, row_bytes);
            row += stride_px;
        }
    }
}

/**
 * CPU memcpy 解码图到 DPI 帧缓冲（常在 PSRAM）后，须把脏 cache 写回内存，DSI 才能读到新像素。
 * 仅对 ROI 覆盖区间做一次 64B 对齐的 C2M，避免整屏 msync；耗时仍与 w×h×2 及总线竞争相关
 *（prof 里 roi_out 4–7ms 主要来自 memcpy + 本次同步）。
 */
static void mjpeg_cache_sync_fb_roi(void *fb_base, int stride_px, int x, int y, int w, int h)
{
    const size_t bpp = sizeof(uint16_t);
    uintptr_t start = (uintptr_t)fb_base + ((size_t)y * (size_t)stride_px + (size_t)x) * bpp;
    size_t nbytes = (size_t)h * (size_t)w * bpp;
    const uintptr_t line = 64;
    uintptr_t al_start = start & ~(line - 1);
    uintptr_t al_end = (start + nbytes + line - 1) & ~(line - 1);
    size_t al_len = (size_t)(al_end - al_start);
    if (al_len > 0) {
        esp_cache_msync((void *)al_start, al_len, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
}

/** 环形读缓冲：越大越利于顺序 fread，减轻 SD 等待（瓶颈常在读任务） */
#define READ_BUF_SIZE    (1024 * 1024)
#define FRAME_BUF_SIZE   (300 * 1024)
/** 多枚 JPEG DMA 输入缓冲，加深读卡与解码流水线 */
#define NUM_DMA_BUFS     5
#define FILE_IO_BUF_SIZE (256 * 1024)
/** 小于此大小的 MJPEG 整文件预载入 PSRAM，播放期不再 fread（根治 SD 拖后腿） */
#ifndef MJPEG_PRELOAD_MAX_BYTES
#define MJPEG_PRELOAD_MAX_BYTES (12 * 1024 * 1024)
#endif

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
    bool from_preload; /*!< true：buf 指向整文件镜像，无 fp */
} read_ctx_t;

/* ─────────────── 播放器状态 ─────────────── */
static volatile bool s_running = false;
static TaskHandle_t s_read_task = NULL;
static TaskHandle_t s_decode_task = NULL;
static mjpeg_player_cfg_t s_cfg;
static QueueHandle_t s_frame_queue;
static QueueHandle_t s_free_queue;
static uint8_t *s_dma_bufs[NUM_DMA_BUFS];

/** mjpeg_player_start 预加载整文件；stop 释放 */
static uint8_t *s_preload_buf = NULL;
static size_t s_preload_size = 0;
static bool s_preload_is_malloc = false;

static void mjpeg_release_preload_buf(void)
{
    if (!s_preload_buf) {
        return;
    }
    if (s_preload_is_malloc) {
        free(s_preload_buf);
    } else {
        heap_caps_free(s_preload_buf);
    }
    s_preload_buf = NULL;
    s_preload_size = 0;
    s_preload_is_malloc = false;
}

static inline void mjpeg_apply_frame_pacing(int64_t t_start, int64_t frame_interval_us)
{
    if (frame_interval_us > 0) {
        int64_t elapsed = esp_timer_get_time() - t_start;
        if (elapsed < frame_interval_us) {
            vTaskDelay(pdMS_TO_TICKS((frame_interval_us - elapsed) / 1000));
        }
    }
}

/** 面板高于视频时，将下方未解码行刷为纯黑（RGB565 0x0000），避免显存残留 */
static void fill_letterbox_black_rgb565(uint16_t *fb, uint32_t width_px,
                                         uint16_t video_h, uint16_t panel_h)
{
    if (panel_h <= video_h || video_h == 0) {
        return;
    }
    uint16_t *dst = fb + (uint32_t)video_h * width_px;
    const uint32_t rows = (uint32_t)panel_h - (uint32_t)video_h;
    const size_t row_bytes = (size_t)width_px * sizeof(uint16_t);
    for (uint32_t y = 0; y < rows; y++) {
        memset(dst, 0, row_bytes);
        dst += width_px;
    }
}

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
    if (ctx->eof) {
        return ctx_avail(ctx) > 0;
    }
    if (!ctx->fp) {
        return ctx_avail(ctx) > 0;
    }

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
            /* EOF 尾巴可能只剩 1~3 字节，此时必须退出，避免 read 任务空转触发 WDT */
            if (ctx->eof) {
                return false;
            }
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

/**
 * 仅从 SOF 段读取宽高，避免每帧 jpeg_decoder_get_info 与硬件解码重复解析。
 */
static bool jpeg_quick_sof_dimensions(const uint8_t *d, int len, uint16_t *out_w, uint16_t *out_h)
{
    if (len < 10 || d[0] != 0xFF || d[1] != 0xD8) {
        return false;
    }
    int p = 2;
    while (p + 4 <= len) {
        if (d[p] != 0xFF) {
            p++;
            continue;
        }
        uint8_t marker = d[p + 1];
        if (marker == 0xD8) {
            p += 2;
            continue;
        }
        if (marker == 0xD9) {
            return false;
        }
        if (marker == 0x00 || marker == 0xFF) {
            p++;
            continue;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            p += 2;
            continue;
        }
        uint16_t seglen = ((uint16_t)d[p + 2] << 8) | d[p + 3];
        if (seglen < 2 || p + 2 + seglen > len) {
            break;
        }
        if ((marker >= 0xC0 && marker <= 0xC3) ||
            (marker >= 0xC5 && marker <= 0xC7) ||
            (marker >= 0xC9 && marker <= 0xCB) ||
            (marker >= 0xCD && marker <= 0xCF)) {
            if (seglen < 8) {
                return false;
            }
            *out_h = ((uint16_t)d[p + 5] << 8) | d[p + 6];
            *out_w = ((uint16_t)d[p + 7] << 8) | d[p + 8];
            return (*out_w > 0 && *out_h > 0);
        }
        p += 2 + seglen;
    }
    return false;
}

static bool validate_frame(const uint8_t *data, int len,
                            uint16_t width, uint16_t height)
{
    if (len < 100) return false;

    /* 1. EOI 标记 */
    if (data[len - 2] != 0xFF || data[len - 1] != 0xD9) return false;

	/* 2. 头部解析 */
    uint16_t w = 0, h = 0;

    if (!jpeg_quick_sof_dimensions(data, len, &w, &h)) return false;
	/* 3. 分辨率匹配 */
    return (w == width && h == height);
}

/* ═══════════════════════════════════════════════════════════
 *  读取任务：从 SD 提取帧 → 校验 → 入队
 * ═══════════════════════════════════════════════════════════ */

static void mjpeg_read_task(void *arg)
{
    ESP_LOGI(TAG, "📜 读取任务启动");

    uint32_t skip_count = 0;
    FILE *opened_fp = NULL;
    uint8_t *read_buf = NULL;
    uint8_t *io_buf = malloc(FILE_IO_BUF_SIZE);
    read_ctx_t ctx;

    if (s_preload_buf && s_preload_size > 0) {
        ESP_LOGI(TAG, "📦 使用 start 阶段预加载 PSRAM（%u KB），播放期不读 SD",
                 (unsigned)(s_preload_size / 1024));
        ctx.buf = s_preload_buf;
        ctx.capacity = (int)s_preload_size;
        ctx.start = 0;
        ctx.end = (int)s_preload_size;
        ctx.fp = NULL;
        ctx.eof = true;
        ctx.from_preload = true;
    } else {
        ctx.buf = NULL;
        ctx.capacity = 0;
        ctx.start = 0;
        ctx.end = 0;
        ctx.fp = NULL;
        ctx.eof = false;
        ctx.from_preload = false;

        ctx.fp = fopen(s_cfg.file_path, "rb");
        if (!ctx.fp) {
            ESP_LOGE(TAG, "❌ 无法打开文件: %s", s_cfg.file_path);
            s_running = false;
            goto exit;
        }
        opened_fp = ctx.fp;
        if (io_buf) {
            setvbuf(ctx.fp, (char *)io_buf, _IOFBF, FILE_IO_BUF_SIZE);
        }

        fseek(ctx.fp, 0, SEEK_END);
        long file_size = ftell(ctx.fp);
        fseek(ctx.fp, 0, SEEK_SET);
        ESP_LOGI(TAG, "📄 文件大小: %.1f MB", file_size / (1024.0 * 1024.0));

        read_buf = malloc(READ_BUF_SIZE);
        if (!read_buf) {
            ESP_LOGE(TAG, "❌ 分配读取缓冲区失败");
            s_running = false;
            goto exit;
        }
        ctx.buf = read_buf;
        ctx.capacity = READ_BUF_SIZE;
    }

    while (s_running) {
        ctx.start = 0;
        if (ctx.from_preload) {
            ctx.end = (int)s_preload_size;
            ctx.eof = true;
        } else {
            ctx.end = 0;
            ctx.eof = false;
            clearerr(ctx.fp);
        }

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
            if (xQueueReceive(s_free_queue, &msg, portMAX_DELAY) != pdTRUE) {
                continue;
            }

            /* 拷贝到 DMA 缓冲区 + Cache 刷新（长度按 cache 线对齐，满足 DMA 读可见性） */
            memcpy(msg.buf, frame_data, frame_len);
            uint32_t sync_len = (uint32_t)frame_len;
            if (sync_len > FRAME_BUF_SIZE) {
                sync_len = FRAME_BUF_SIZE;
            }
            sync_len = (sync_len + 63u) & ~63u;
            if (sync_len > FRAME_BUF_SIZE) {
                sync_len = FRAME_BUF_SIZE;
            }
            esp_cache_msync(msg.buf, sync_len,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
            msg.len = frame_len;

            xQueueSend(s_frame_queue, &msg, portMAX_DELAY);
        }

        /* 非循环模式才发送 EOF；循环模式回绕文件头继续读 */
        if (!s_cfg.loop || !s_running) {
            frame_msg_t eof = { .buf = NULL, .len = 0 };
            xQueueSend(s_frame_queue, &eof, portMAX_DELAY);
            break;
        }
        if (ctx.from_preload) {
            /* 已在 RAM 中，仅重置游标 */
            continue;
        }
        fseek(ctx.fp, 0, SEEK_SET);
    }

    /* 发送停止信号 */
    frame_msg_t stop_msg = { .buf = NULL, .len = -1 };
    xQueueSend(s_frame_queue, &stop_msg, portMAX_DELAY);

exit:
    if (opened_fp) {
        fclose(opened_fp);
        opened_fp = NULL;
    }
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
        .timeout_ms = 5000,
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

    /* 全屏 DPI：面板全尺寸；画布 / ROI：仅视频区域 */
    const uint32_t fb_size = (s_embed_lvgl || s_panel_roi_blit)
        ? ((uint32_t)s_cfg.screen_width * (uint32_t)s_cfg.screen_height * sizeof(uint16_t))
        : ((uint32_t)MJPEG_PANEL_WIDTH * (uint32_t)MJPEG_PANEL_HEIGHT * sizeof(uint16_t));
    const uint32_t expect_decoded = (uint32_t)s_cfg.screen_width * (uint32_t)s_cfg.screen_height * sizeof(uint16_t);
    
    /* 全屏 letterbox 时解码输出小于整块 DPI，传入实际 JPEG 输出尺寸，减少硬解内部校验异常 */
    const uint32_t jpeg_out_buf_size = (expect_decoded <= fb_size) ? expect_decoded : fb_size;
    int fb_idx = 0;
    uint32_t frame_count = 0;
    uint32_t decode_errors = 0;
    int consecutive_errors = 0;
    bool first_frame_logged_once = false;
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

        /* 文件结束 — 仅重置统计；保持解码器与双缓冲索引 */
        if (msg.len == 0) {
            if (frame_count > 0) {
                int64_t elapsed = esp_timer_get_time() - start_time;
                ESP_LOGD(TAG, "✅ 播放结束: %lu帧, %.1f fps, %.1f秒, 错误%lu",
                         (unsigned long)frame_count,
                         frame_count * 1e6f / elapsed,
                         elapsed / 1e6f,
                         (unsigned long)decode_errors);
            }
            frame_count = 0;
            decode_errors = 0;
            consecutive_errors = 0;
            start_time = esp_timer_get_time();
            continue;
        }

        int64_t t_start = esp_timer_get_time();

        /* 硬件解码 → 面板帧缓冲区 */
        uint32_t decoded_size = 0;
        ret = jpeg_decoder_process(decoder, &decode_cfg,
                                    msg.buf, (uint32_t)msg.len,
                                    (uint8_t *)s_cfg.fb[fb_idx], jpeg_out_buf_size,
                                    &decoded_size);
        const int64_t t_after_decode = esp_timer_get_time();

        /* 立即归还 DMA 缓冲区，让读取任务继续工作 */
        frame_msg_t free_msg = { .buf = msg.buf, .len = 0 };
        xQueueSend(s_free_queue, &free_msg, portMAX_DELAY);

        /* 仅以输出字节数为硬失败；P4 硬解常 ret!=OK 但 decoded 已齐 */
        if (decoded_size != expect_decoded) {
            decode_errors++;
            consecutive_errors++;
            if (decode_errors <= 5 || decode_errors % 50 == 0) {
                ESP_LOGW(TAG, "⚠️ 解码失败 #%lu (ret=%s, decoded=%lu expect=%lu, jpeg=%d)",
                         (unsigned long)decode_errors, esp_err_to_name(ret),
                         (unsigned long)decoded_size, (unsigned long)expect_decoded, msg.len);
            }
            jpeg_del_decoder_engine(decoder);
            decoder = NULL;
            vTaskDelay(pdMS_TO_TICKS(5));
            ret = jpeg_new_decoder_engine(&engine_cfg, &decoder);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "❌ 重建解码引擎失败");
                break;
            }
            if (consecutive_errors >= 20) {
                ESP_LOGE(TAG, "❌ 连续 %d 帧解码失败，停止播放", consecutive_errors);
                s_running = false;
                break;
            }
            continue;
        }
        consecutive_errors = 0;

        if (!first_frame_logged_once) {
            ESP_LOGI(TAG, "🎬 首帧: JPEG=%d字节, RGB565=%lu字节, 零拷贝模式",
                     msg.len, (unsigned long)decoded_size);
            first_frame_logged_once = true;
        }

        const bool has_letterbox = !s_embed_lvgl && !s_panel_roi_blit
            && (MJPEG_PANEL_HEIGHT > s_cfg.screen_height);
        if (has_letterbox) {
            fill_letterbox_black_rgb565((uint16_t *)s_cfg.fb[fb_idx], MJPEG_PANEL_WIDTH,
                                        s_cfg.screen_height, MJPEG_PANEL_HEIGHT);
        }

        /* M2C */
        {
            uint32_t n = (has_letterbox || s_embed_lvgl || s_panel_roi_blit) ? fb_size : decoded_size;
            if (n == 0) {
                n = expect_decoded;
            }
            n = (n + 63u) & ~63u;
            if (n > fb_size) {
                n = (fb_size + 63u) & ~63u;
            }
            if (((uintptr_t)s_cfg.fb[fb_idx] & 63u) == 0 && n >= 64u) {
                esp_cache_msync(s_cfg.fb[fb_idx], n, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            }
        }

        if (s_embed_lvgl && s_cfg.lv_video_canvas) {
            lv_obj_t *cv = (lv_obj_t *)s_cfg.lv_video_canvas;
            if (lvgl_port_lock(5000)) {
                lv_canvas_set_buffer(cv, s_cfg.fb[fb_idx], s_cfg.screen_width, s_cfg.screen_height,
                                     LV_COLOR_FORMAT_RGB565);
                /* LVGL 9.4 无 lv_display_invalidate_area；画布整控件失效即可 */
                lv_obj_invalidate(cv);
                lvgl_port_unlock();
            }
        } else if (s_panel_roi_blit && s_cfg.panel) {
            /* ROI：与 LVGL 共用 DPI 双缓冲时必须互斥，避免与 flush/sw_rotate 竞态 */
            const int x1 = (int)s_cfg.panel_roi_x;
            const int y1 = (int)s_cfg.panel_roi_y;
            const int w = (int)s_cfg.screen_width;
            const int h = (int)s_cfg.screen_height;
            if (lvgl_port_lock(5000)) {
                if (s_roi_write_dpi_fb && s_dpi_fb0 && s_dpi_fb1) {
                    void *dst_fb = (fb_idx & 1) ? s_dpi_fb1 : s_dpi_fb0;
                    uint16_t *dst16 = (uint16_t *)dst_fb;
                    mjpeg_roi_fill_letterbox_bands_rgb565(dst16, MJPEG_PANEL_WIDTH, MJPEG_PANEL_HEIGHT, y1, h);
                    mjpeg_copy_rgb565_to_fb_stride(dst16, MJPEG_PANEL_WIDTH,
                                                   x1, y1, w, h,
                                                   (const uint16_t *)s_cfg.fb[fb_idx]);
                    /* 顶/底黑边 + ROI 均可能改动，整幅 C2M 一次避免漏同步条带 */
                    mjpeg_cache_sync_fb_roi(dst_fb, MJPEG_PANEL_WIDTH, 0, 0,
                                            MJPEG_PANEL_WIDTH, MJPEG_PANEL_HEIGHT);
                } else {
                    const int x2 = x1 + w;
                    const int y2 = y1 + h;
                    esp_lcd_panel_draw_bitmap(s_cfg.panel, x1, y1, x2, y2, s_cfg.fb[fb_idx]);
                }
                lvgl_port_unlock();
            }
        } else if (s_cfg.panel) {
            esp_lcd_panel_draw_bitmap(s_cfg.panel, 0, 0,
                                       s_cfg.screen_width, s_cfg.screen_height,
                                       s_cfg.fb[fb_idx]);
        }
        fb_idx = 1 - fb_idx;
        frame_count++;

#if MJPEG_PROFILE_FIRST_FRAMES > 0
        if (frame_count <= MJPEG_PROFILE_FIRST_FRAMES) {
            const int64_t t_after_blit = esp_timer_get_time();
            ESP_LOGI(TAG, "⏱ prof #%lu: jpeg_decode=%lldus roi_out=%lldus (roi_dpi=%d)",
                     (unsigned long)frame_count,
                     (long long)(t_after_decode - t_start),
                     (long long)(t_after_blit - t_after_decode),
                     (int)s_roi_write_dpi_fb);
        }
#endif

        if (frame_count % 200 == 0) {
            int64_t elapsed = esp_timer_get_time() - start_time;
            ESP_LOGI(TAG, "📊 %lu帧, %.1f fps, 错误%lu",
                     (unsigned long)frame_count,
                     frame_count * 1e6f / elapsed,
                     (unsigned long)decode_errors);
        }

        /* 帧率控制 */
        mjpeg_apply_frame_pacing(t_start, frame_interval_us);
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
    if (!cfg || !cfg->file_path) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!cfg->lv_video_canvas && !cfg->panel) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->panel_blit_roi) {
        if (!cfg->panel || cfg->lv_video_canvas) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (s_running) {
        ESP_LOGW(TAG, "⚠️ 播放器已在运行");
        return ESP_ERR_INVALID_STATE;
    }

    s_cfg = *cfg;
    s_embed_lvgl = (s_cfg.lv_video_canvas != NULL);
    s_panel_roi_blit = s_cfg.panel_blit_roi;
    s_roi_write_dpi_fb = false;
    s_dpi_fb0 = NULL;
    s_dpi_fb1 = NULL;

    if (s_embed_lvgl || s_panel_roi_blit) {
        const size_t sz = (size_t)s_cfg.screen_width * (size_t)s_cfg.screen_height * sizeof(uint16_t);
        const size_t sz_al = (sz + 63u) & ~63u;
        for (int i = 0; i < 2; i++) {
            /* 硬件 JPEG 写 PSRAM 往往明显慢于内部 SRAM；优先 INTERNAL */
            s_cfg.fb[i] = heap_caps_aligned_alloc(64, sz_al, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
            if (!s_cfg.fb[i]) {
                s_cfg.fb[i] = heap_caps_aligned_alloc(64, sz_al, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
            }
            if (!s_cfg.fb[i]) {
                ESP_LOGE(TAG, "❌ 分配解码缓冲 %d 失败", i);
                for (int j = 0; j < i; j++) {
                    heap_caps_free(s_cfg.fb[j]);
                    s_cfg.fb[j] = NULL;
                }
                return ESP_ERR_NO_MEM;
            }
        }
        if (s_cfg.fb[0]) {
#if MJPEG_HAVE_ESP_PTR_EXTERNAL_RAM
            ESP_LOGI(TAG, "解码输出缓冲[0]: %s",
                     esp_ptr_external_ram(s_cfg.fb[0]) ? "PSRAM" : "内部SRAM(优先)");
#else
            ESP_LOGI(TAG, "解码输出缓冲[0]: 已分配（优先内部SRAM）");
#endif
        }
        if (s_embed_lvgl) {
            lv_obj_t *cv = (lv_obj_t *)s_cfg.lv_video_canvas;
            lv_canvas_set_buffer(cv, s_cfg.fb[0], s_cfg.screen_width, s_cfg.screen_height, LV_COLOR_FORMAT_RGB565);
            ESP_LOGI(TAG, "💾 LVGL 画布模式: 解码缓冲 %dx%d ×2", s_cfg.screen_width, s_cfg.screen_height);
        } else {
            ESP_LOGI(TAG, "💾 面板 ROI 直送显: %dx%d @ (%u,%u) ×2",
                     s_cfg.screen_width, s_cfg.screen_height,
                     (unsigned)s_cfg.panel_roi_x, (unsigned)s_cfg.panel_roi_y);
            esp_err_t gfb = mjpeg_get_frame_buffers(s_cfg.panel, &s_dpi_fb0, &s_dpi_fb1);
            if (gfb == ESP_OK && s_dpi_fb0 && s_dpi_fb1) {
                s_roi_write_dpi_fb = true;
                ESP_LOGI(TAG, "ROI: 直接写入 DPI 帧缓冲（绕过 esp_lcd_panel_draw_bitmap）");
            } else if (gfb == ESP_OK && s_dpi_fb0 && !s_dpi_fb1) {
                ESP_LOGI(TAG, "ROI: 检测到单帧缓冲面板，使用 draw_bitmap 路径");
            } else {
                ESP_LOGW(TAG, "ROI: 无法获取 DPI FB (%s)，仍用 draw_bitmap",
                         esp_err_to_name(gfb));
            }
        }
    } else if (!s_cfg.fb[0] || !s_cfg.fb[1]) {
        esp_err_t gf = mjpeg_get_frame_buffers(s_cfg.panel, &s_cfg.fb[0], &s_cfg.fb[1]);
        if (gf != ESP_OK || !s_cfg.fb[0] || !s_cfg.fb[1]) {
            ESP_LOGE(TAG, "❌ 获取 DPI 帧缓冲失败: %s", esp_err_to_name(gf));
            return gf != ESP_OK ? gf : ESP_ERR_INVALID_STATE;
        }
    }
    s_running = true;

    s_preload_buf = NULL;
    s_preload_size = 0;
    s_preload_is_malloc = false;
    {
        FILE *pf = fopen(s_cfg.file_path, "rb");
        if (pf) {
            if (fseek(pf, 0, SEEK_END) == 0) {
                long sz = ftell(pf);
                if (sz > 0 && (size_t)sz <= MJPEG_PRELOAD_MAX_BYTES) {
                    uint8_t *pb = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM);
                    if (!pb) {
                        pb = malloc((size_t)sz);
                        if (pb) {
                            s_preload_is_malloc = true;
                        }
                    }
                    if (pb) {
                        rewind(pf);
                        if (fread(pb, 1, (size_t)sz, pf) == (size_t)sz) {
                            s_preload_buf = pb;
                            s_preload_size = (size_t)sz;
                            ESP_LOGI(TAG, "📦 小文件预加载 %u KB（%s），读任务从内存取帧",
                                     (unsigned)(s_preload_size / 1024),
                                     s_preload_is_malloc ? "内部堆" : "PSRAM");
                        } else {
                            if (s_preload_is_malloc) {
                                free(pb);
                            } else {
                                heap_caps_free(pb);
                            }
                            s_preload_is_malloc = false;
                        }
                    } else {
                        ESP_LOGW(TAG, "⚠️ 预加载分配失败，使用 SD 流式读取");
                    }
                }
            }
            fclose(pf);
        }
    }

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
            mjpeg_release_preload_buf();
            s_running = false;
            return ESP_ERR_NO_MEM;
        }
        frame_msg_t msg = { .buf = s_dma_bufs[i], .len = 0 };
        xQueueSend(s_free_queue, &msg, 0);
        ESP_LOGI(TAG, "💾 DMA 缓冲区 %d: %uKB", i, (unsigned)(actual / 1024));
    }

    if (!s_embed_lvgl && !s_panel_roi_blit) {
        uint32_t panel_fb = (uint32_t)MJPEG_PANEL_WIDTH * (uint32_t)MJPEG_PANEL_HEIGHT * sizeof(uint16_t);
        ESP_LOGI(TAG, "💾 异步流水线: 读取=%dKB, DMA=%dKB×%d, 输出=DPI FB×2(%luKB×2, %dx%d)",
                 READ_BUF_SIZE / 1024, FRAME_BUF_SIZE / 1024, NUM_DMA_BUFS,
                 (unsigned long)(panel_fb / 1024), MJPEG_PANEL_WIDTH, MJPEG_PANEL_HEIGHT);
    }

    /* 创建双任务 */
    BaseType_t ret;
    /* 读任务优先：尽快填满 frame_queue，避免解码任务空等 SD（prof 里 jpeg 仅 ~2ms 仍只有 ~17fps 即此因） */
    ret = xTaskCreate(mjpeg_read_task, "mjpeg_read", 8192,
                       NULL, configMAX_PRIORITIES - 1, &s_read_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "❌ 创建读取任务失败");
        mjpeg_release_preload_buf();
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    ret = xTaskCreate(mjpeg_decode_task, "mjpeg_dec", 8192,
                       NULL, configMAX_PRIORITIES - 2, &s_decode_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "❌ 创建解码任务失败");
        mjpeg_release_preload_buf();
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

    mjpeg_release_preload_buf();

    if (s_embed_lvgl || s_panel_roi_blit) {
        heap_caps_free(s_cfg.fb[0]);
        heap_caps_free(s_cfg.fb[1]);
        s_cfg.fb[0] = NULL;
        s_cfg.fb[1] = NULL;
        s_embed_lvgl = false;
        s_roi_write_dpi_fb = false;
        s_dpi_fb0 = NULL;
        s_dpi_fb1 = NULL;
        if (s_panel_roi_blit) {
            (void)lvgl_port_resume();
            s_panel_roi_blit = false;
        }
    } else {
        (void)lvgl_port_resume();
    }

    ESP_LOGI(TAG, "💾 播放器资源已释放");
}

bool mjpeg_player_is_running(void)
{
    return s_running;
}
