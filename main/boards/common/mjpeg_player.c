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
/** LVGL 正在 flush（尤其 sw_rotate + 对话刷新）时，持锁前已提交的 DSI 传输可能仍在进行 */
#define MJPEG_DRAW_RETRY_MAX        20
#define MJPEG_LVGL_LOCK_TIMEOUT_MS  20
#define MJPEG_POST_LOCK_DRAIN_MS    0
#define MJPEG_DRAW_RETRY_US_MIN     1000
#define MJPEG_DRAW_RETRY_US_MAX     20000
/** 严格逐帧校验会重复解析 JPEG（extract+validate），会显著增加 read 侧 CPU 占用 */
#ifndef MJPEG_STRICT_FRAME_VALIDATE
#define MJPEG_STRICT_FRAME_VALIDATE 0
#endif

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
static bool s_roi_letterbox_drawn;
static bool s_output_fb_shared;
/** true：旧播放器标记停止但尚未清理，下次 start 时完成清理 */
static bool s_deferred_cleanup;

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
#define MJPEG_READ_TASK_CORE_ID   1
#define MJPEG_DECODE_TASK_CORE_ID 0
#define MJPEG_ROI_DRAW_LETTERBOX_ONCE 0
/** 顶/底 letterbox 黑条，单次 draw_bitmap 最大行数（高大于视频上下黑边） */
#define MJPEG_LBAND_MAX_LINES 16
#define MJPEG_BLACK_TILE_H MJPEG_LBAND_MAX_LINES
/** 顶/底 letterbox 黑条，单次 draw_bitmap 最大行数（高大于视频上下黑边） */

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

static uint8_t* s_mjpeg_black_tile = NULL;
static uint8_t* s_mjpeg_slab = NULL;
static size_t s_mjpeg_slab_max_w = 0;
static size_t s_mjpeg_black_tile_size = 0;
static size_t s_mjpeg_slab_size = 0;
static int s_panel_width = 0;
static int s_panel_height = 0;

static void mjpeg_init_black_tile_buffer(int panel_width, int panel_height)
{
    /* 尺寸未变则跳过，避免每次切换都 free+alloc */
    if (s_mjpeg_black_tile && s_panel_width == panel_width && s_panel_height == panel_height) {
        return;
    }

    s_panel_width = panel_width;
    s_panel_height = panel_height;

    if (s_mjpeg_black_tile) {
        free(s_mjpeg_black_tile);
        s_mjpeg_black_tile = NULL;
    }
    if (s_mjpeg_slab) {
        free(s_mjpeg_slab);
        s_mjpeg_slab = NULL;
    }

    int max_pillar_w = (panel_width > 240) ? ((panel_width - 240) / 2 + 16) : 16;

    s_mjpeg_black_tile_size = (size_t)panel_width * MJPEG_BLACK_TILE_H * sizeof(uint16_t);
    s_mjpeg_black_tile = (uint8_t*)heap_caps_aligned_alloc(64, s_mjpeg_black_tile_size, MALLOC_CAP_DMA);
    if (!s_mjpeg_black_tile) {
        s_mjpeg_black_tile = (uint8_t*)malloc(s_mjpeg_black_tile_size);
    }
    if (s_mjpeg_black_tile) {
        memset(s_mjpeg_black_tile, 0, s_mjpeg_black_tile_size);
    }

    s_mjpeg_slab_size = (size_t)max_pillar_w * 2 * MJPEG_BLACK_TILE_H * sizeof(uint16_t);
    s_mjpeg_slab = (uint8_t*)heap_caps_aligned_alloc(64, s_mjpeg_slab_size, MALLOC_CAP_DMA);
    if (!s_mjpeg_slab) {
        s_mjpeg_slab = (uint8_t*)malloc(s_mjpeg_slab_size);
    }
    if (s_mjpeg_slab) {
        s_mjpeg_slab_max_w = max_pillar_w;
        memset(s_mjpeg_slab, 0, s_mjpeg_slab_size);
    }
}

static esp_err_t mjpeg_panel_draw_bitmap_retry(esp_lcd_panel_handle_t panel, int x0, int y0, int x1, int y1, const void *data)
{
    esp_err_t ret = ESP_FAIL;
    for (int i = 0; i < MJPEG_DRAW_RETRY_MAX; i++) {
        ret = esp_lcd_panel_draw_bitmap(panel, x0, y0, x1, y1, data);
        if (ret == ESP_OK) {
            return ret;
        }
        /* previous draw not finished: 微秒级退避，避免 tick 量化导致每次至少 1 tick(常见 10ms) */
        uint32_t us = MJPEG_DRAW_RETRY_US_MIN + (uint32_t)i * MJPEG_DRAW_RETRY_US_MIN;
        if (us > MJPEG_DRAW_RETRY_US_MAX) {
            us = MJPEG_DRAW_RETRY_US_MAX;
        }
        esp_rom_delay_us(us);
        if (i >= (MJPEG_DRAW_RETRY_MAX - 2)) {
            taskYIELD();
        }
    }
    return ret;
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
    if (col_w <= 0 || col_w > (int)s_mjpeg_slab_max_w || body_h <= 0 || !s_mjpeg_slab) {
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
/* 单帧缓冲：720×1280 / 656×1232 等高分屏的 mjpeg 帧压缩后约 33-35KB，
 * 原 32KB 会被「帧过大」判定跳过。提升到 64KB 以容纳所有板子的高分帧。
 * 为抵消 64KB 增长、避免 PSRAM 碎片化时申请失败（largest≈184KB），
 * 把 NUM_DMA_BUFS 从 3 降到 2，总占用 128KB，留出余量。 */
#define FRAME_BUF_SIZE   (64 * 1024)
#define NUM_DMA_BUFS     2
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

        /* SD 卡可能刚被上次 start 释放，fopen 短暂失败时重试 5 次，每次 200ms */
        for (int open_retry = 0; open_retry < 5; open_retry++) {
            ctx.fp = fopen(s_cfg.file_path, "rb");
            if (ctx.fp) {
                break;
            }
            ESP_LOGW(TAG, "⚠️ fopen 重试 %d/5: %s", open_retry + 1, s_cfg.file_path);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
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
        //ESP_LOGI(TAG, "📄 文件大小: %.1f MB", file_size / (1024.0 * 1024.0));

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
                                 s_cfg.mjpeg_video_width, s_cfg.mjpeg_video_height)) {
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
            if (xQueueReceive(s_free_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
                if (!s_running) {
                    goto exit_loop;
                }
                continue;
            }

            if (frame_len > FRAME_BUF_SIZE) {
                skip_count++;
                ESP_LOGW(TAG, "⚠️ 帧过大(%dB > %dB)，跳过", frame_len, FRAME_BUF_SIZE);
                /* 归还缓冲，避免 free_queue 被耗尽 */
                msg.len = 0;
                xQueueSend(s_free_queue, &msg, pdMS_TO_TICKS(100));
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

            if (xQueueSend(s_frame_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
                if (!s_running) {
                    xQueueSend(s_free_queue, &msg, 0);
                    goto exit_loop;
                }
                xQueueSend(s_free_queue, &msg, 0);
                continue;
            }
        }

        /* 非循环模式才发送 EOF；循环模式回绕文件头继续读 */
        if (!s_cfg.loop || !s_running) {
            frame_msg_t eof = { .buf = NULL, .len = 0 };
            (void)xQueueSend(s_frame_queue, &eof, pdMS_TO_TICKS(100));
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
    (void)xQueueSend(s_frame_queue, &stop_msg, pdMS_TO_TICKS(100));

exit_loop:
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
    //ESP_LOGI(TAG, "📜 读取任务结束 (跳过帧: %lu)", (unsigned long)skip_count);
    s_read_task = NULL;
    vTaskDelete(NULL);
}

/* ═══════════════════════════════════════════════════════════
 *  解码任务：硬件 JPEG 解码 → 零拷贝送显
 * ═══════════════════════════════════════════════════════════ */

static void mjpeg_decode_task(void *arg)
{
    //ESP_LOGI(TAG, "🔧 解码任务启动");

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
    //ESP_LOGI(TAG, "✅ 硬件 JPEG 解码引擎已就绪 (超时=%dms)", engine_cfg.timeout_ms);

    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };

    /* 全屏 DPI：面板全尺寸；画布 / ROI：仅视频区域 */
    const uint32_t fb_size = (s_embed_lvgl || s_panel_roi_blit)
        ? ((uint32_t)s_cfg.mjpeg_video_width * (uint32_t)s_cfg.mjpeg_video_height * sizeof(uint16_t))
        : ((uint32_t)s_panel_width * (uint32_t)s_panel_height * sizeof(uint16_t));
    const uint32_t expect_decoded = (uint32_t)s_cfg.mjpeg_video_width * (uint32_t)s_cfg.mjpeg_video_height * sizeof(uint16_t);
    
    /* 全屏 letterbox 时解码输出小于整块 DPI，传入实际 JPEG 输出尺寸，减少硬解内部校验异常 */
    const uint32_t jpeg_out_buf_size = (expect_decoded <= fb_size) ? expect_decoded : fb_size;
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

        /* 立即归还 DMA 缓冲区，让读取任务继续工作 */
        frame_msg_t free_msg = { .buf = msg.buf, .len = 0 };
        (void)xQueueSend(s_free_queue, &free_msg, pdMS_TO_TICKS(100));

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

        const bool has_letterbox = !s_embed_lvgl && !s_panel_roi_blit
            && (s_panel_height > s_cfg.mjpeg_video_height);
        if (has_letterbox) {
            fill_letterbox_black_rgb565((uint16_t *)s_cfg.fb[fb_idx], s_panel_width,
                                        s_cfg.mjpeg_video_height, s_panel_height);
        }

        /* ROI 路径：解码到独立 fb，需要 cache write-back 让 panel memcpy 能读到新数据 */
        if (s_panel_roi_blit) {
            uint32_t n = (uint32_t)decoded_size;
            if (n == 0) {
                n = (uint32_t)expect_decoded;
            }
            n = (n + 63u) & ~63u;
            if (n > (uint32_t)((s_cfg.mjpeg_video_width * s_cfg.mjpeg_video_height * sizeof(uint16_t) + 63u) & ~63u)) {
                n = (uint32_t)((s_cfg.mjpeg_video_width * s_cfg.mjpeg_video_height * sizeof(uint16_t) + 63u) & ~63u);
            }
            if (((uintptr_t)s_cfg.fb[fb_idx] & 63u) == 0 && n >= 64u) {
                esp_cache_msync(s_cfg.fb[fb_idx], n, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            }
        } else {
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
                lv_canvas_set_buffer(cv, s_cfg.fb[fb_idx], s_cfg.mjpeg_video_width, s_cfg.mjpeg_video_height,
                                     LV_COLOR_FORMAT_RGB565);
                /* LVGL 9.4 无 lv_display_invalidate_area；画布整控件失效即可 */
                lv_obj_invalidate(cv);
                lvgl_port_unlock();
            }
        } else if (s_panel_roi_blit && s_cfg.panel) {
            const int draw_x0 = (int)s_cfg.panel_roi_x;
            const int draw_y0 = (int)s_cfg.panel_roi_y;
            const int w = (int)s_cfg.mjpeg_video_width;
            const int h = (int)s_cfg.mjpeg_video_height;
            /* 用户确认：mjpeg ROI 区域与 LVGL label 字幕区不重叠，DSI panel 的 ROI 区域
             * 与 LVGL flush 范围天然无竞争，无需持 LVGL 锁。直接 blit 全靠 mjpeg_panel_draw_bitmap_retry
             * 自带的 panel 忙重试保证（DMA2D 引擎互斥）。 */
            if (MJPEG_ROI_DRAW_LETTERBOX_ONCE && !s_roi_letterbox_drawn) {
                mjpeg_roi_letterbox_draw(s_cfg.panel, s_panel_width, s_panel_height, draw_x0, draw_y0, w, h);
                s_roi_letterbox_drawn = true;
            }
            /* y1 = draw_y0 + video_h（视频区紧贴屏底：ry=panel_height-video_h → y1=panel_height）。 */
            const int draw_y1 = draw_y0 + (int)s_cfg.mjpeg_video_height;
            esp_err_t blit = mjpeg_panel_draw_bitmap_retry(s_cfg.panel, draw_x0, draw_y0, draw_x0 + w, draw_y1, s_cfg.fb[fb_idx]);
            if (blit != ESP_OK) {
                ESP_LOGW(TAG, "⚠️ ROI draw失败: %s", esp_err_to_name(blit));
            }
        } else if (s_cfg.panel) {
            esp_err_t blit = mjpeg_panel_draw_bitmap_retry(s_cfg.panel, 0, 0,
                                      s_cfg.mjpeg_video_width, s_cfg.mjpeg_video_height,
                                      s_cfg.fb[fb_idx]);
            if (blit != ESP_OK) {
                ESP_LOGW(TAG, "⚠️ draw失败: %s", esp_err_to_name(blit));
            }
        }
        fb_idx = 1 - fb_idx;
        frame_count++;

#if MJPEG_PROFILE_FIRST_FRAMES > 0
        if (frame_count <= MJPEG_PROFILE_FIRST_FRAMES) {
            const int64_t t_after_decode = esp_timer_get_time();
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
        }

        /* 帧率控制 */
        mjpeg_apply_frame_pacing(t_start, frame_interval_us);
    }

exit:
    if (decoder) jpeg_del_decoder_engine(decoder);
    //ESP_LOGI(TAG, "🔧 解码任务结束");
    s_decode_task = NULL;
    vTaskDelete(NULL);
}

/* 提前声明：mjpeg_player_start 内调用 */
static void mjpeg_do_deferred_cleanup(void);

/* ═══════════════════════════════════════════════════════════
 *  公有 API
 * ═══════════════════════════════════════════════════════════ */

esp_err_t mjpeg_player_start(const mjpeg_player_cfg_t *cfg)
{
    /* 清理上次异步 stop 遗留的资源 */
    mjpeg_do_deferred_cleanup();

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
        return ESP_ERR_INVALID_STATE;
    }

    s_cfg = *cfg;
    s_embed_lvgl = (s_cfg.lv_video_canvas != NULL);
    s_panel_roi_blit = s_cfg.panel_blit_roi;
    s_roi_letterbox_drawn = false;

    int panel_w = (s_cfg.panel_width > 0) ? s_cfg.panel_width : s_cfg.mjpeg_video_width;
    int panel_h = (s_cfg.panel_height > 0) ? s_cfg.panel_height : s_cfg.mjpeg_video_height;
    mjpeg_init_black_tile_buffer(panel_w, panel_h);

    /* 解码输出缓冲/DMA缓冲只在首次启动时分配，之后复用 */
    const bool need_alloc_fb = (s_embed_lvgl || s_panel_roi_blit) && !s_cfg.fb[0];
    const bool need_alloc_dma = (s_dma_bufs[0] == NULL);
    if (need_alloc_fb || need_alloc_dma) {
        if (need_alloc_fb) {
            /* ROI 路径：分配独立 RGB565 缓冲（video_w*video_h*2 字节）。
             * 走 esp_lcd_panel_draw_bitmap 标准 memcpy 路径（do_copy=true），
             * panel 内部会按 (x0,y0,x1,y1) 把数据 memcpy 到 panel fb 对应区域，
             * 屏顶/屏底留出的状态栏/字幕栏不会被视频刷新覆盖。
             * 独立 buffer 也避免与 panel fb 冲突（panel fb 通常只有 1~2 个，
             * 复用会强制 do_copy=false → flush 整个 panel fb → 覆盖状态栏/字幕）。 */
            const size_t sz = (size_t)s_cfg.mjpeg_video_width * (size_t)s_cfg.mjpeg_video_height * sizeof(uint16_t);
            /* ESP32-P4 jpeg 硬解码器要求 output buffer 地址与 size 均 64 字节对齐；
             * 否则 jpeg_decoder_process() 返回 ESP_ERR_INVALID_ARG。 */
            const size_t sz_al = (sz + 63u) & ~63u;
            const int fb_count = 2;
            for (int i = 0; i < fb_count; i++) {
                s_cfg.fb[i] = heap_caps_aligned_alloc(64, sz_al, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
                if (!s_cfg.fb[i]) {
                    s_cfg.fb[i] = heap_caps_aligned_alloc(64, sz_al, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                }
                if (!s_cfg.fb[i]) {
                    s_cfg.fb[i] = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                }
                if (!s_cfg.fb[i]) {
                    for (int j = 0; j < i; j++) {
                        heap_caps_free(s_cfg.fb[j]);
                        s_cfg.fb[j] = NULL;
                    }
                    ESP_LOGE(TAG, "❌ ROI fb[%d] alloc NO_MEM (size=%u, free IRAM=%u, free SPIRAM=%u, largest=%u)",
                             i, (unsigned)sz_al,
                             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                    return ESP_ERR_NO_MEM;
                }
            }
            //ESP_LOGI(TAG, "✅ ROI 分配独立 fb (size=%u, a=%p b=%p)",
                     //(unsigned)sz_al, s_cfg.fb[0], s_cfg.fb[1]);
            s_output_fb_shared = false;
        } else if (!s_cfg.fb[0] || !s_cfg.fb[1]) {
            esp_err_t gf = mjpeg_get_frame_buffers(s_cfg.panel, &s_cfg.fb[0], &s_cfg.fb[1]);
            if (gf != ESP_OK || !s_cfg.fb[0] || !s_cfg.fb[1]) {
                return gf != ESP_OK ? gf : ESP_ERR_INVALID_STATE;
            }
        }

        /* 分配 DMA 输入缓冲区（只分配一次） */
        /* ESP32-P4 PSRAM 不在 DMA 总线，input 走 cache 写入即可，不强制 DMA caps；
         * 但 cache-line 64 字节对齐是 SOC_CACHE_WRITEBACK_SUPPORTED 的强制要求（esp_cache_msync） */
        for (int i = 0; i < NUM_DMA_BUFS; i++) {
            s_dma_bufs[i] = heap_caps_aligned_alloc(64, FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s_dma_bufs[i]) {
                s_dma_bufs[i] = heap_caps_malloc(FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            }
            if (!s_dma_bufs[i]) {
                for (int j = 0; j < i; j++) {
                    free(s_dma_bufs[j]);
                    s_dma_bufs[j] = NULL;
                }
                ESP_LOGE(TAG, "❌ dma_bufs[%d] alloc NO_MEM (size=%u, free IRAM=%u, free SPIRAM=%u, largest=%u)",
                         i, (unsigned)FRAME_BUF_SIZE,
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                return ESP_ERR_NO_MEM;
            }
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
                        } else {
                            if (s_preload_is_malloc) {
                                free(pb);
                            } else {
                                heap_caps_free(pb);
                            }
                            s_preload_is_malloc = false;
                        }
                    }
                }
            }
            fclose(pf);
        }
    }
#endif

    /* 队列需要新建 */
    s_frame_queue = xQueueCreate(NUM_DMA_BUFS + 2, sizeof(frame_msg_t));
    s_free_queue = xQueueCreate(NUM_DMA_BUFS, sizeof(frame_msg_t));

    /* DMA 缓冲重新入队 */
    for (int i = 0; i < NUM_DMA_BUFS; i++) {
        frame_msg_t msg = { .buf = s_dma_bufs[i], .len = 0 };
        xQueueSend(s_free_queue, &msg, 0);
    }

    /* 创建读/解码任务 */
    BaseType_t ret;
    ret = xTaskCreatePinnedToCore(mjpeg_read_task, "mjpeg_read", 8192, NULL, MJPEG_READ_TASK_PRIORITY, &s_read_task,
                                  MJPEG_READ_TASK_CORE_ID);
    if (ret != pdPASS) {
        mjpeg_release_preload_buf();
        s_running = false;
        ESP_LOGE(TAG, "❌ xTaskCreate read failed (free IRAM=%u, free SPIRAM=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return ESP_ERR_NO_MEM;
    }

    ret = xTaskCreatePinnedToCore(mjpeg_decode_task, "mjpeg_dec", 8192, NULL, MJPEG_DECODE_TASK_PRIORITY,
                                  &s_decode_task, MJPEG_DECODE_TASK_CORE_ID);
    if (ret != pdPASS) {
        vTaskDelete(s_read_task);
        s_read_task = NULL;
        mjpeg_release_preload_buf();
        s_running = false;
        ESP_LOGE(TAG, "❌ xTaskCreate decode failed (free IRAM=%u, free SPIRAM=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/* 延迟清理：任务已自行退出（异步 stop 场景），在下次 start 前完成资源回收 */
static void mjpeg_do_deferred_cleanup(void)
{
    if (!s_deferred_cleanup) return;
    s_deferred_cleanup = false;

    /* 只等 decode_task 退出（read_task 已在 mjpeg_player_stop 里同步等完） */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(200);
    while (s_decode_task) {
        if (xTaskGetTickCount() > deadline) {
            ESP_LOGW(TAG, "⚠️ decode_task 退出超时，强制 vTaskDelete");
            if (s_decode_task) {
                vTaskDelete(s_decode_task);
                s_decode_task = NULL;
            }
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    /* 给 idle hook 回收 TCB/栈 */
    vTaskDelay(pdMS_TO_TICKS(20));

    if (s_frame_queue) {
        frame_msg_t msg;
        while (xQueueReceive(s_frame_queue, &msg, 0) == pdTRUE) { }
        while (xQueueReceive(s_free_queue, &msg, 0) == pdTRUE) { }
        vQueueDelete(s_frame_queue);
        vQueueDelete(s_free_queue);
        s_frame_queue = NULL;
        s_free_queue = NULL;
    }

    mjpeg_release_preload_buf();

    if (s_embed_lvgl || s_panel_roi_blit) {
        /* 释放 ROI/lvgl 路径下自申请的 fb 缓冲（panel fb 路径下此时已是 panel fb 指针，free 会 double-free，必须 s_output_fb_shared 判定） */
        if (!s_output_fb_shared) {
            if (s_cfg.fb[0] && s_cfg.fb[0] != s_cfg.fb[1]) {
                heap_caps_free(s_cfg.fb[0]);
            }
            if (s_cfg.fb[1]) {
                heap_caps_free(s_cfg.fb[1]);
            }
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
}

void mjpeg_player_stop(void)
{
    if (!s_running) return;
    s_running = false;

    /* 同步等待 read_task 退出：read task 持有 read_buf(256KB PSRAM) + io_buf(32KB PSRAM),
     * 不释放就立刻 start 会撞 SPIRAM NO_MEM。最多 1500ms,超时则强制 vTaskDelete。 */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1500);
    while (s_read_task) {
        if (xTaskGetTickCount() > deadline) {
            ESP_LOGW(TAG, "⚠️ read_task 退出超时，强制删除");
            if (s_read_task) {
                vTaskDelete(s_read_task);
                s_read_task = NULL;
            }
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    /* 给 idle hook 回收 read_task 的栈 + TCB */
    vTaskDelay(pdMS_TO_TICKS(15));

    /* decode_task 退出较慢（持锁 blit），且不占大块堆，异步退出即可 */
    s_deferred_cleanup = true;
}

bool mjpeg_player_is_running(void)
{
    return s_running;
}
