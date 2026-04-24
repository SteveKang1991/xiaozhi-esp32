/**
 * @file mjpeg_player.c
 * @brief 🎬 MJPEG 异步读 + 硬件解码 + 送显
 *
 * 架构：
 *   读取任务（SD→提取帧→校验→DMA 输入缓冲）→ frame_queue
 *   解码任务（硬解 RGB565）→ 紧密缓冲 →（ROI 模式）esp_lcd_panel_draw_bitmap /（全屏）DPI 帧缓冲
 *        → free_queue
 *
 * ROI 模式：硬解只支持紧密输出，用 draw_bitmap 送 ROI，避免对 DPI 做整块 stride 手写 memcpy；
 *   C2M 仅对解码输出小块或 letterbox 小瓦片。播放任务为中等优先级，减小环形缓冲/并发 DMA
 *   以减轻对 WiFi/语音/UI 的挤压。
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
#include "esp_rom_sys.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_lcd_mipi_dsi.h"
#endif
#if __has_include("esp_memory_utils.h")
#include "esp_memory_utils.h"
#define MJPEG_HAVE_ESP_PTR_EXTERNAL_RAM 1
#else
#define MJPEG_HAVE_ESP_PTR_EXTERNAL_RAM 0
#endif
#if CONFIG_IDF_TARGET_ESP32P4
#include "driver/jpeg_decode.h"
#else
#include "esp_jpeg_common.h"
#include "esp_jpeg_dec.h"
#endif
#include "lvgl.h"
#include "esp_lvgl_port.h"

static const char *TAG = "MJPEG";
/* Physical panel size for letterbox tiles / ROI math (ST7789 240x320 等) */
#ifndef MJPEG_PANEL_WIDTH
#define MJPEG_PANEL_WIDTH  240
#endif
#ifndef MJPEG_PANEL_HEIGHT
#define MJPEG_PANEL_HEIGHT 320
#endif
/** LVGL 正在 flush（尤其 sw_rotate + 对话刷新）时，持锁前已提交的 DSI 传输可能仍在进行 */
#define MJPEG_DRAW_RETRY_MAX        20
#define MJPEG_LVGL_LOCK_TIMEOUT_MS  40
#define MJPEG_POST_LOCK_DRAIN_MS    0
#define MJPEG_DRAW_RETRY_US_MIN     500
#define MJPEG_DRAW_RETRY_US_MAX     5000
#define MJPEG_ROI_BAND_LINES        32
#define MJPEG_LVGL_LOCK_TIMEOUT_RELAX_MS 40
#define MJPEG_LVGL_LOCK_TIMEOUT_BUSY_MS  8
#define MJPEG_LVGL_BUSY_HOLD_MS         3000
/** 严格逐帧校验会重复解析 JPEG（extract+validate），会显著增加 read 侧 CPU 占用 */
#ifndef MJPEG_STRICT_FRAME_VALIDATE
#define MJPEG_STRICT_FRAME_VALIDATE 0
#endif

static esp_err_t mjpeg_get_frame_buffers(esp_lcd_panel_handle_t panel, void **fb0, void **fb1)
{
#if CONFIG_IDF_TARGET_ESP32P4
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
#else
    (void)panel;
    if (fb0) {
        *fb0 = NULL;
    }
    if (fb1) {
        *fb1 = NULL;
    }
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/** true：解码到自分配缓冲 + LVGL lv_canvas */
static bool s_embed_lvgl;
/** true：小缓冲解码 + esp_lcd_panel_draw_bitmap 仅 ROI，不经 LVGL */
static bool s_panel_roi_blit;
static bool s_roi_letterbox_drawn;
/* SPI LCD 直写路径需要与面板颜色字节序对齐；LVGL 路径已由 swap_bytes 处理 */
static bool s_mjpeg_swap_rgb565_bytes;
/* 若 esp_new_jpeg 支持 RGB565_BE，则优先直接输出目标字节序，避免每帧 CPU swap */
static bool s_mjpeg_sw_decode_rgb565_be;
static bool s_output_fb_shared;

/** 首若干帧打印 decode/blit 耗时，便于确认瓶颈（非 0 启用） */
#ifndef MJPEG_PROFILE_FIRST_FRAMES
#define MJPEG_PROFILE_FIRST_FRAMES 0
#endif

#define MJPEG_READ_TASK_PRIORITY   4
#define MJPEG_DECODE_TASK_PRIORITY 3
/*
 * mjpeg_read 在 extract_frame 扫 JPEG 时可能长时间占满循环，须离开 CPU0，否则会饿死 IDLE0 触发看门狗。
 * mjpeg_decode 多数时间在等队列、硬解、持锁 blit，不易长时间占满；与 LVGL 同核利于观感帧率。
 */
#if CONFIG_FREERTOS_UNICORE
#define MJPEG_READ_TASK_CORE_ID   0
#define MJPEG_DECODE_TASK_CORE_ID 0
#else
#define MJPEG_READ_TASK_CORE_ID   1
#define MJPEG_DECODE_TASK_CORE_ID 0
#endif
#define MJPEG_ROI_DRAW_LETTERBOX_ONCE 0
/** 顶/底 letterbox 黑条，单次 draw_bitmap 最大行数（高大于视频上下黑边） */
#define MJPEG_LBAND_MAX_LINES 16
/** 左右黑边最大半宽，窄屏竖屏时需更大余量 */
#ifndef MJPEG_PILLAR_MAX_W
#define MJPEG_PILLAR_MAX_W 128
#endif

/** 源缓冲供 DMA/DSI 前：C2M，仅对实际长度（64B 对齐区间） */
static void mjpeg_cache_c2m_cpu_tight(const void *ptr, size_t nbytes)
{
    if (!ptr || nbytes == 0) {
        return;
    }
    const uintptr_t line = 64;
    uintptr_t start = (uintptr_t)ptr;
    uintptr_t al_start = start & ~(line - 1);
    uintptr_t al_end = (start + nbytes + line - 1) & ~(line - 1);
    size_t al_len = (size_t)(al_end - al_start);
    if (al_len == 0) {
        al_len = line;
    }
    esp_cache_msync((void *)al_start, al_len, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

#define MJPEG_BLACK_TILE_H MJPEG_LBAND_MAX_LINES
static uint8_t s_mjpeg_black_tile[((size_t)MJPEG_PANEL_WIDTH) * (size_t)MJPEG_BLACK_TILE_H * sizeof(uint16_t)]
    __attribute__((aligned(64)));
static uint8_t s_mjpeg_slab[((size_t)MJPEG_PILLAR_MAX_W) * (size_t)MJPEG_BLACK_TILE_H * sizeof(uint16_t)]
    __attribute__((aligned(64)));

static esp_err_t mjpeg_panel_draw_bitmap_retry(esp_lcd_panel_handle_t panel, int x0, int y0, int x1, int y1, const void *data)
{
    esp_err_t ret = ESP_FAIL;
    for (int i = 0; i < MJPEG_DRAW_RETRY_MAX; i++) {
        ret = esp_lcd_panel_draw_bitmap(panel, x0, y0, x1, y1, data);
        if (ret == ESP_OK) {
            return ret;
        }
        /* previous draw not finished: 指数退避 + 让出调度，等待 SPI 队列回收 */
        uint32_t us = MJPEG_DRAW_RETRY_US_MIN + (uint32_t)i * MJPEG_DRAW_RETRY_US_MIN;
        if (us > MJPEG_DRAW_RETRY_US_MAX) {
            us = MJPEG_DRAW_RETRY_US_MAX;
        }
        esp_rom_delay_us(us);
        taskYIELD();
        if ((i & 0x3) == 0x3) {
            vTaskDelay(1);
        }
    }
    return ret;
}

/* 参考分行送显方案：将 ROI 分块推屏，避免单次大传输挤爆 SPI panel IO 队列 */
static esp_err_t mjpeg_panel_draw_bitmap_banded(esp_lcd_panel_handle_t panel, int x0, int y0, int w, int h, const void *data)
{
    if (!panel || !data || w <= 0 || h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t *src = (const uint8_t *)data;
    const int band_h = MJPEG_ROI_BAND_LINES;
    for (int y = 0; y < h; y += band_h) {
        int ch = h - y;
        if (ch > band_h) {
            ch = band_h;
        }
        const uint8_t *band_ptr = src + (size_t)y * (size_t)w * sizeof(uint16_t);
        /* SPI DMA reads from memory directly; flush D-cache for each ROI band first. */
        mjpeg_cache_c2m_cpu_tight(band_ptr, (size_t)w * (size_t)ch * sizeof(uint16_t));
        esp_err_t r = mjpeg_panel_draw_bitmap_retry(panel, x0, y0 + y, x0 + w, y0 + y + ch, band_ptr);
        if (r != ESP_OK) {
            return r;
        }
    }
    return ESP_OK;
}

static inline void mjpeg_rgb565_swap_bytes_inplace(void *buf, size_t pixel_count)
{
    uint16_t *p16 = (uint16_t *)buf;
    size_t i = 0;
    size_t n32 = pixel_count >> 1; /* 2 pixels per 32-bit word */
    uint32_t *p32 = (uint32_t *)buf;
    for (size_t j = 0; j < n32; ++j) {
        uint32_t v = p32[j];
        p32[j] = ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
        i += 2;
    }
    if (i < pixel_count) {
        uint16_t v = p16[i];
        p16[i] = (uint16_t)((v << 8) | (v >> 8));
    }
}

/** 全宽水平黑条 (y0..y0+band_h)，分片 draw_bitmap，不经手写 memcpy 进帧缓冲 */
static void mjpeg_h_band_black(esp_lcd_panel_handle_t panel, int y0, int band_h, int panel_w)
{
    if (band_h <= 0) {
        return;
    }
    for (int y = 0; y < band_h; ) {
        int ch = band_h - y;
        if (ch > MJPEG_BLACK_TILE_H) {
            ch = MJPEG_BLACK_TILE_H;
        }
        const size_t bbytes = (size_t)panel_w * (size_t)ch * sizeof(uint16_t);
        mjpeg_cache_c2m_cpu_tight(s_mjpeg_black_tile, bbytes);
        (void)mjpeg_panel_draw_bitmap_retry(panel, 0, y0 + y, panel_w, y0 + y + ch, s_mjpeg_black_tile);
        y += ch;
    }
}

/** 竖条 (x0,y0) 起 col_w×body_h 区域刷黑，用于 ROI 左右边 */
static void mjpeg_pillar_bands(esp_lcd_panel_handle_t panel, int x0, int col_w, int y0, int body_h)
{
    if (col_w <= 0 || col_w > MJPEG_PILLAR_MAX_W || body_h <= 0) {
        return;
    }
    for (int y = 0; y < body_h; ) {
        int ch = body_h - y;
        if (ch > MJPEG_BLACK_TILE_H) {
            ch = MJPEG_BLACK_TILE_H;
        }
        const size_t bbytes = (size_t)col_w * (size_t)ch * sizeof(uint16_t);
        (void)memset(s_mjpeg_slab, 0, bbytes);
        mjpeg_cache_c2m_cpu_tight(s_mjpeg_slab, bbytes);
        (void)mjpeg_panel_draw_bitmap_retry(panel, x0, y0 + y, x0 + col_w, y0 + y + ch, s_mjpeg_slab);
        y += ch;
    }
}

/** 顶/底 + 中栏左右黑边（与视频、UI 不重叠时可在无 lvgl 锁下调用） */
static void mjpeg_roi_letterbox_draw(esp_lcd_panel_handle_t panel, int panel_w, int panel_h, int rx, int ry, int rw,
                                    int rh)
{
    mjpeg_h_band_black(panel, 0, ry, panel_w);
    mjpeg_pillar_bands(panel, 0, rx, ry, rh);
    mjpeg_pillar_bands(panel, rx + rw, panel_w - (rx + rw), ry, rh);
    mjpeg_h_band_black(panel, ry + rh, panel_h - (ry + rh), panel_w);
}

/** 环形读缓冲（节内存；过小易拖慢流式读） */
#define READ_BUF_SIZE    (256 * 1024)
#define FRAME_BUF_SIZE   (320 * 1024)
#define NUM_DMA_BUFS     3
#define FILE_IO_BUF_SIZE (32 * 1024)
/** 0：禁用整文件预载；>0 时小于该字节的 mjpeg 预载入 PSRAM */
#ifndef MJPEG_PRELOAD_MAX_BYTES
#define MJPEG_PRELOAD_MAX_BYTES 0
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
#if MJPEG_STRICT_FRAME_VALIDATE
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
#endif

/* ═══════════════════════════════════════════════════════════
 *  读取任务：从 SD 提取帧 → 校验 → 入队
 * ═══════════════════════════════════════════════════════════ */

static void mjpeg_read_task(void *arg)
{
    ESP_LOGI(TAG, "📜 读取任务启动");

    uint32_t skip_count = 0;
    FILE *opened_fp = NULL;
    uint8_t *read_buf = NULL;
    uint8_t *io_buf = NULL;
    bool read_buf_spiram = false;
    bool io_buf_spiram = false;
    read_ctx_t ctx;

    /* 读环 + stdio 缓冲放 PSRAM，省出 ~288KB 内部 SRAM，避免与 WiFi/SDIO(transport copy_buff) 抢堆 */
    io_buf = heap_caps_malloc(FILE_IO_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (io_buf) {
        io_buf_spiram = true;
    } else {
        io_buf = malloc(FILE_IO_BUF_SIZE);
    }

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

        read_buf = heap_caps_malloc(READ_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (read_buf) {
            read_buf_spiram = true;
        } else {
            read_buf = malloc(READ_BUF_SIZE);
        }
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
#if MJPEG_STRICT_FRAME_VALIDATE
            if (!validate_frame(frame_data, frame_len,
                                 s_cfg.screen_width, s_cfg.screen_height)) {
                skip_count++;
                if (skip_count <= 5 || skip_count % 100 == 0) {
                    ESP_LOGW(TAG, "🛡️ 帧校验失败 #%lu（帧=%d字节）",
                             (unsigned long)skip_count, frame_len);
                }
                continue;
            }
#else
            /* extract_frame 已保证 SOI/EOI 边界，生产资源建议关闭重复解析提升吞吐 */
            if (frame_len < 100) {
                skip_count++;
                continue;
            }
#endif

            /* 获取空闲 DMA 缓冲区 */
            frame_msg_t msg;
            if (xQueueReceive(s_free_queue, &msg, portMAX_DELAY) != pdTRUE) {
                continue;
            }

            if (frame_len > FRAME_BUF_SIZE) {
                skip_count++;
                ESP_LOGW(TAG, "⚠️ 帧过大(%dB > %dB)，跳过", frame_len, FRAME_BUF_SIZE);
                /* 归还缓冲，避免 free_queue 被耗尽 */
                msg.len = 0;
                xQueueSend(s_free_queue, &msg, portMAX_DELAY);
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
    if (read_buf) {
        if (read_buf_spiram) {
            heap_caps_free(read_buf);
        } else {
            free(read_buf);
        }
    }
    if (io_buf) {
        if (io_buf_spiram) {
            heap_caps_free(io_buf);
        } else {
            free(io_buf);
        }
    }
    ESP_LOGI(TAG, "📜 读取任务结束 (跳过帧: %lu)", (unsigned long)skip_count);
    s_read_task = NULL;
    vTaskDelete(NULL);
}

/* ═══════════════════════════════════════════════════════════
 *  解码任务：ESP32-P4 硬件 JPEG；其它目标（如 S3）用 esp_new_jpeg 软解
 * ═══════════════════════════════════════════════════════════ */

static void mjpeg_decode_task(void *arg)
{
    ESP_LOGI(TAG, "decode task start");

#if CONFIG_IDF_TARGET_ESP32P4
    jpeg_decode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,
        .timeout_ms = 5000,
    };
    jpeg_decoder_handle_t decoder = NULL;
    esp_err_t ret = jpeg_new_decoder_engine(&engine_cfg, &decoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HW jpeg engine failed: %s", esp_err_to_name(ret));
        s_running = false;
        goto exit;
    }
    ESP_LOGI(TAG, "HW JPEG ready (timeout=%dms)", engine_cfg.timeout_ms);

    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };
#else
    jpeg_dec_config_t sw_dec_cfg = DEFAULT_JPEG_DEC_CONFIG();
#if defined(JPEG_PIXEL_FORMAT_RGB565_BE)
    sw_dec_cfg.output_type = s_mjpeg_sw_decode_rgb565_be ? JPEG_PIXEL_FORMAT_RGB565_BE : JPEG_PIXEL_FORMAT_RGB565_LE;
#else
    sw_dec_cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
#endif
    sw_dec_cfg.rotate = JPEG_ROTATE_0D;
    jpeg_dec_handle_t sw_dec = NULL;
    if (jpeg_dec_open(&sw_dec_cfg, &sw_dec) != JPEG_ERR_OK || sw_dec == NULL) {
        ESP_LOGE(TAG, "esp_new_jpeg jpeg_dec_open failed");
        s_running = false;
        goto exit;
    }
    ESP_LOGI(TAG, "SW JPEG (esp_new_jpeg) ready");
    esp_err_t ret = ESP_OK;
#endif

    /* 全屏 DPI：面板全尺寸；画布 / ROI：仅视频区域 */
    const uint32_t fb_size = (s_embed_lvgl || s_panel_roi_blit)
        ? ((uint32_t)s_cfg.screen_width * (uint32_t)s_cfg.screen_height * sizeof(uint16_t))
        : ((uint32_t)MJPEG_PANEL_WIDTH * (uint32_t)MJPEG_PANEL_HEIGHT * sizeof(uint16_t));
    const uint32_t expect_decoded = (uint32_t)s_cfg.screen_width * (uint32_t)s_cfg.screen_height * sizeof(uint16_t);
#if CONFIG_IDF_TARGET_ESP32P4
    const uint32_t jpeg_out_buf_size = (expect_decoded <= fb_size) ? expect_decoded : fb_size;
#else
    const uint32_t jpeg_out_buf_size = expect_decoded;
#endif
    int fb_idx = 0;
    uint32_t frame_count = 0;
    uint32_t decode_errors = 0;
    uint32_t lock_timeouts = 0;
    int64_t lock_busy_until_us = 0;
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

        uint32_t decoded_size = 0;
#if CONFIG_IDF_TARGET_ESP32P4
        ret = jpeg_decoder_process(decoder, &decode_cfg,
                                    msg.buf, (uint32_t)msg.len,
                                    (uint8_t *)s_cfg.fb[fb_idx], jpeg_out_buf_size,
                                    &decoded_size);
#else
        jpeg_dec_io_t jpeg_io = {0};
        jpeg_dec_header_info_t hdr = {0};
        jpeg_io.inbuf = msg.buf;
        jpeg_io.inbuf_len = msg.len;
        jpeg_error_t jer = jpeg_dec_parse_header(sw_dec, &jpeg_io, &hdr);
        if (jer != JPEG_ERR_OK) {
            ret = ESP_FAIL;
        } else if ((uint32_t)hdr.width != (uint32_t)s_cfg.screen_width
                   || (uint32_t)hdr.height != (uint32_t)s_cfg.screen_height) {
            ESP_LOGW(TAG, "JPEG %ux%u != clip %ux%u", (unsigned)hdr.width, (unsigned)hdr.height,
                     (unsigned)s_cfg.screen_width, (unsigned)s_cfg.screen_height);
            ret = ESP_ERR_INVALID_SIZE;
        } else {
            int inbuf_consumed = jpeg_io.inbuf_len - jpeg_io.inbuf_remain;
            if (inbuf_consumed < 0 || inbuf_consumed > jpeg_io.inbuf_len) {
                ESP_LOGW(TAG, "jpeg header 游标异常: consumed=%d, in=%d, remain=%d",
                         inbuf_consumed, jpeg_io.inbuf_len, jpeg_io.inbuf_remain);
                ret = ESP_FAIL;
                goto sw_decode_done;
            }
            jpeg_io.inbuf = msg.buf + inbuf_consumed;
            jpeg_io.inbuf_len = jpeg_io.inbuf_remain;
            jpeg_io.outbuf = (uint8_t *)s_cfg.fb[fb_idx];
            jer = jpeg_dec_process(sw_dec, &jpeg_io);
            if (jer == JPEG_ERR_OK) {
                decoded_size = expect_decoded;
                ret = ESP_OK;
            } else {
                ret = ESP_FAIL;
            }
        }
sw_decode_done:
#endif
        const int64_t t_after_decode = esp_timer_get_time();
#if MJPEG_PROFILE_FIRST_FRAMES <= 0
        (void)t_after_decode;
#endif

        /* 立即归还 DMA 缓冲区，让读取任务继续工作 */
        frame_msg_t free_msg = { .buf = msg.buf, .len = 0 };
        xQueueSend(s_free_queue, &free_msg, portMAX_DELAY);

        /* 仅以输出字节数为硬失败；P4 硬解常 ret!=OK 但 decoded 已齐 */
        if (decoded_size != expect_decoded) {
            decode_errors++;
            consecutive_errors++;
            if (decode_errors <= 5 || decode_errors % 50 == 0) {
                ESP_LOGW(TAG, "decode fail #%lu (ret=%s, decoded=%lu expect=%lu, jpeg=%d)",
                         (unsigned long)decode_errors, esp_err_to_name(ret),
                         (unsigned long)decoded_size, (unsigned long)expect_decoded, msg.len);
            }
#if CONFIG_IDF_TARGET_ESP32P4
            jpeg_del_decoder_engine(decoder);
            decoder = NULL;
            vTaskDelay(pdMS_TO_TICKS(5));
            ret = jpeg_new_decoder_engine(&engine_cfg, &decoder);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "reopen HW jpeg failed");
                break;
            }
#else
            if (sw_dec) {
                jpeg_dec_close(sw_dec);
                sw_dec = NULL;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            if (jpeg_dec_open(&sw_dec_cfg, &sw_dec) != JPEG_ERR_OK || sw_dec == NULL) {
                ESP_LOGE(TAG, "reopen SW jpeg failed");
                break;
            }
#endif
            if (consecutive_errors >= 20) {
                ESP_LOGE(TAG, "too many decode errors, stop");
                s_running = false;
                break;
            }
            continue;
        }
        consecutive_errors = 0;

        if (!first_frame_logged_once) {
            const char *mode = s_panel_roi_blit ? "紧密缓冲+draw_bitmap" : (s_embed_lvgl ? "LVGL画布" : "DPI 帧缓冲");
            ESP_LOGI(TAG, "🎬 首帧: JPEG=%d字节, RGB565=%lu字节 (%s)",
                     msg.len, (unsigned long)decoded_size, mode);
            first_frame_logged_once = true;
        }

        const bool has_letterbox = !s_embed_lvgl && !s_panel_roi_blit
            && (MJPEG_PANEL_HEIGHT > s_cfg.screen_height);
        if (has_letterbox) {
            fill_letterbox_black_rgb565((uint16_t *)s_cfg.fb[fb_idx], MJPEG_PANEL_WIDTH,
                                        s_cfg.screen_height, MJPEG_PANEL_HEIGHT);
        }

        /* ROI draw_bitmap 直接从解码输出读数据：跳过整帧 M2C，减少每帧 cache 维护开销 */
        if (!s_panel_roi_blit) {
            uint32_t n = (has_letterbox || s_embed_lvgl) ? (uint32_t)fb_size : (uint32_t)decoded_size;
            if (n == 0) {
                n = (uint32_t)expect_decoded;
            }
            n = (n + 63u) & ~63u;
            if (n > (uint32_t)fb_size) {
                n = (uint32_t)((fb_size + 63u) & ~63u);
            }
            if (((uintptr_t)s_cfg.fb[fb_idx] & 63u) == 0 && n >= 64u) {
                esp_cache_msync(s_cfg.fb[fb_idx], n, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            }
        }

        if (s_embed_lvgl && s_cfg.lv_video_canvas) {
            lv_obj_t *cv = (lv_obj_t *)s_cfg.lv_video_canvas;
            if (lvgl_port_lock(MJPEG_LVGL_LOCK_TIMEOUT_MS)) {
                lv_canvas_set_buffer(cv, s_cfg.fb[fb_idx], s_cfg.screen_width, s_cfg.screen_height,
                                     LV_COLOR_FORMAT_RGB565);
                /* LVGL 9.4 无 lv_display_invalidate_area；画布整控件失效即可 */
                lv_obj_invalidate(cv);
                lvgl_port_unlock();
            }
        } else if (s_panel_roi_blit && s_cfg.panel) {
            const int x1 = (int)s_cfg.panel_roi_x;
            const int y1 = (int)s_cfg.panel_roi_y;
            const int w = (int)s_cfg.screen_width;
            const int h = (int)s_cfg.screen_height;
            if (s_mjpeg_swap_rgb565_bytes) {
                mjpeg_rgb565_swap_bytes_inplace(s_cfg.fb[fb_idx], (size_t)w * (size_t)h);
            }
            /* DSI 与 LVGL 共用 panel：须持锁，且锁内首帧前短暂退让，避免紧接 LVGL flush 的未完成传输 */
            const int64_t now_us = esp_timer_get_time();
            const uint32_t lock_timeout_ms =
                (now_us < lock_busy_until_us) ? MJPEG_LVGL_LOCK_TIMEOUT_BUSY_MS : MJPEG_LVGL_LOCK_TIMEOUT_RELAX_MS;
            if (lvgl_port_lock((int)lock_timeout_ms)) {
                if (MJPEG_POST_LOCK_DRAIN_MS > 0) {
                    vTaskDelay(pdMS_TO_TICKS(MJPEG_POST_LOCK_DRAIN_MS));
                }
                if (MJPEG_ROI_DRAW_LETTERBOX_ONCE && !s_roi_letterbox_drawn) {
                    mjpeg_roi_letterbox_draw(s_cfg.panel, MJPEG_PANEL_WIDTH, MJPEG_PANEL_HEIGHT, x1, y1, w, h);
                    s_roi_letterbox_drawn = true;
                }
                esp_err_t blit = mjpeg_panel_draw_bitmap_banded(s_cfg.panel, x1, y1, w, h, s_cfg.fb[fb_idx]);
                if (blit != ESP_OK) {
                    ESP_LOGW(TAG, "⚠️ ROI draw失败: %s", esp_err_to_name(blit));
                }
                lvgl_port_unlock();
            } else {
                lock_timeouts++;
                /* 一旦发生锁竞争，接下来 3s 进入短等待模式，优先保障语音实时性 */
                lock_busy_until_us = now_us + (int64_t)MJPEG_LVGL_BUSY_HOLD_MS * 1000LL;
                static uint32_t s_lock_fail_log;
                uint32_t now = (uint32_t)(now_us / 1000000);
                if (now != s_lock_fail_log) {
                    s_lock_fail_log = now;
                    ESP_LOGW(TAG, "⚠️ lvgl_port_lock 超时(%lums)，跳过本帧 ROI", (unsigned long)lock_timeout_ms);
                }
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
            ESP_LOGI(TAG, "⏱ prof #%lu: jpeg_decode=%lldus blit=%lldus",
                     (unsigned long)frame_count,
                     (long long)(t_after_decode - t_start),
                     (long long)(t_after_blit - t_after_decode));
        }
#endif

        if (frame_count % 200 == 0) {
            int64_t elapsed = esp_timer_get_time() - start_time;
            ESP_LOGI(TAG, "📊 %lu帧, %.1f fps, 错误%lu",
                     (unsigned long)frame_count,
                     frame_count * 1e6f / elapsed,
                     (unsigned long)decode_errors);
            if (lock_timeouts > 0) {
                ESP_LOGW(TAG, "📉 ROI跳帧累计: lvgl锁超时=%lu", (unsigned long)lock_timeouts);
            }
        }

        /* 帧率控制 */
        mjpeg_apply_frame_pacing(t_start, frame_interval_us);
    }

exit:
#if CONFIG_IDF_TARGET_ESP32P4
    if (decoder) {
        jpeg_del_decoder_engine(decoder);
    }
#else
    if (sw_dec) {
        jpeg_dec_close(sw_dec);
    }
#endif
    ESP_LOGI(TAG, "decode task exit");
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
    s_roi_letterbox_drawn = false;
    s_mjpeg_swap_rgb565_bytes = false;
    s_mjpeg_sw_decode_rgb565_be = false;
    if (s_panel_roi_blit && !s_embed_lvgl) {
#if !CONFIG_IDF_TARGET_ESP32P4
        /* S3 软解 + SPI panel 直写：优先尝试让解码器直接输出 BE，减少每帧 CPU swap */
#if defined(JPEG_PIXEL_FORMAT_RGB565_BE)
        s_mjpeg_sw_decode_rgb565_be = true;
        s_mjpeg_swap_rgb565_bytes = false;
#else
        s_mjpeg_swap_rgb565_bytes = true;
#endif
#endif
    }
    ESP_LOGI(TAG, "MJPEG颜色链路: panel_roi=%d lvgl=%d RGB565_BE解码=%d RGB565字节矫正=%d",
             s_panel_roi_blit ? 1 : 0, s_embed_lvgl ? 1 : 0,
             s_mjpeg_sw_decode_rgb565_be ? 1 : 0, s_mjpeg_swap_rgb565_bytes ? 1 : 0);

    if (s_embed_lvgl || s_panel_roi_blit) {
        const size_t sz = (size_t)s_cfg.screen_width * (size_t)s_cfg.screen_height * sizeof(uint16_t);
        const size_t sz_al = (sz + 63u) & ~63u;
        const int fb_count = s_panel_roi_blit ? 1 : 2;
        for (int i = 0; i < fb_count; i++) {
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
        if (s_panel_roi_blit) {
            s_cfg.fb[1] = s_cfg.fb[0];
            s_output_fb_shared = true;
        } else {
            s_output_fb_shared = false;
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
            (void)memset(s_mjpeg_black_tile, 0, sizeof(s_mjpeg_black_tile));
            (void)memset(s_mjpeg_slab, 0, sizeof(s_mjpeg_slab));
            ESP_LOGI(TAG, "💾 面板 ROI: %dx%d @ (%u,%u) draw_bitmap（短互斥，单缓冲 %dx%d）",
                     s_cfg.screen_width, s_cfg.screen_height,
                     (unsigned)s_cfg.panel_roi_x, (unsigned)s_cfg.panel_roi_y,
                     s_cfg.screen_width, s_cfg.screen_height);
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
#if MJPEG_PRELOAD_MAX_BYTES > 0
    {
        FILE *pf = fopen(s_cfg.file_path, "rb");
        if (pf) {
            if (fseek(pf, 0, SEEK_END) == 0) {
                long sz = ftell(pf);
                if (sz > 0 && (size_t)sz <= (size_t)MJPEG_PRELOAD_MAX_BYTES) {
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
#endif

    /* 创建队列 */
    s_frame_queue = xQueueCreate(NUM_DMA_BUFS + 2, sizeof(frame_msg_t));
    s_free_queue = xQueueCreate(NUM_DMA_BUFS, sizeof(frame_msg_t));

    /* 分配 JPEG 帧输入缓冲（P4 用硬件分配器；S3 等用 DMA 能力堆） */
#if CONFIG_IDF_TARGET_ESP32P4
    jpeg_decode_memory_alloc_cfg_t in_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
#endif
    for (int i = 0; i < NUM_DMA_BUFS; i++) {
#if CONFIG_IDF_TARGET_ESP32P4
        size_t actual = 0;
        s_dma_bufs[i] = jpeg_alloc_decoder_mem(FRAME_BUF_SIZE, &in_mem_cfg, &actual);
#else
        s_dma_bufs[i] = heap_caps_malloc(FRAME_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_dma_bufs[i]) {
            s_dma_bufs[i] = heap_caps_malloc(FRAME_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
        }
        size_t actual = FRAME_BUF_SIZE;
#endif
        if (!s_dma_bufs[i]) {
            ESP_LOGE(TAG, "frame buf %d alloc failed", i);
            mjpeg_release_preload_buf();
            s_running = false;
            return ESP_ERR_NO_MEM;
        }
        frame_msg_t msg = { .buf = s_dma_bufs[i], .len = 0 };
        xQueueSend(s_free_queue, &msg, 0);
        ESP_LOGI(TAG, "frame buf %d: %uKB", i, (unsigned)(actual / 1024));
    }

    if (!s_embed_lvgl && !s_panel_roi_blit) {
        uint32_t panel_fb = (uint32_t)MJPEG_PANEL_WIDTH * (uint32_t)MJPEG_PANEL_HEIGHT * sizeof(uint16_t);
        ESP_LOGI(TAG, "💾 异步流水线: 读取=%dKB, DMA=%dKB×%d, 输出=DPI FB×2(%luKB×2, %dx%d)",
                 READ_BUF_SIZE / 1024, FRAME_BUF_SIZE / 1024, NUM_DMA_BUFS,
                 (unsigned long)(panel_fb / 1024), MJPEG_PANEL_WIDTH, MJPEG_PANEL_HEIGHT);
    }

    /* 创建双任务 */
    BaseType_t ret;
    /* 读=CPU1（避 WDT），解码=CPU0；优先级读>解码以喂满队列 */
    ret = xTaskCreatePinnedToCore(mjpeg_read_task, "mjpeg_read", 8192, NULL, MJPEG_READ_TASK_PRIORITY, &s_read_task,
                                  MJPEG_READ_TASK_CORE_ID);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "❌ 创建读取任务失败");
        mjpeg_release_preload_buf();
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    ret = xTaskCreatePinnedToCore(mjpeg_decode_task, "mjpeg_dec", 8192, NULL, MJPEG_DECODE_TASK_PRIORITY,
                                  &s_decode_task, MJPEG_DECODE_TASK_CORE_ID);
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
#if CONFIG_IDF_TARGET_ESP32P4
            free(s_dma_bufs[i]);
#else
            heap_caps_free(s_dma_bufs[i]);
#endif
            s_dma_bufs[i] = NULL;
        }
    }

    vQueueDelete(s_frame_queue);
    vQueueDelete(s_free_queue);
    s_frame_queue = NULL;
    s_free_queue = NULL;

    mjpeg_release_preload_buf();

    if (s_embed_lvgl || s_panel_roi_blit) {
        if (s_cfg.fb[0]) {
            heap_caps_free(s_cfg.fb[0]);
        }
        if (!s_output_fb_shared && s_cfg.fb[1]) {
            heap_caps_free(s_cfg.fb[1]);
        }
        s_cfg.fb[0] = NULL;
        s_cfg.fb[1] = NULL;
        s_embed_lvgl = false;
        s_output_fb_shared = false;
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
