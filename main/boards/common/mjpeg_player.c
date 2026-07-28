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
#include "esp_partition.h"
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
/* LVGL 与面板共用同一 SPI 时，draw_bitmap 与 LVGL flush 仍须互斥，否则可能花屏/卡死；ROI 用短超时即可。 */
#define MJPEG_ROI_LVGL_LOCK_MS      5
#define MJPEG_LVGL_LOCK_TIMEOUT_MS  40
#define MJPEG_POST_LOCK_DRAIN_MS    0
/* ROI band 行数：esp_lcd_panel_io 内部已按 max_transfer_sz 自动 DMA 分包，
 * 14 行 ≈ 6.7KB < 16KB 单 DMA 限制。21 次小传输在 TTS 期间因 SRAM 拥塞反复
 * 触发 ESP_ERR_NO_MEM 重试（最长 300ms/帧），改成 14 行仍是必需，但减少重试次数。 */
#define MJPEG_ROI_BAND_LINES        14
/* TTS 期间 SRAM 紧张时 draw_bitmap 内部 RAM 分配失败 → ESP_ERR_NO_MEM。
 * 60 次重试 × 5ms 上限 = 300ms 极端长卡顿，缩短到 10 次 × 1ms = 10ms 上限。 */
#define MJPEG_DRAW_RETRY_MAX        10
#define MJPEG_DRAW_RETRY_US_MIN     200
#define MJPEG_DRAW_RETRY_US_MAX     1000
#ifndef MJPEG_STRICT_FRAME_VALIDATE
#define MJPEG_STRICT_FRAME_VALIDATE 0
#endif
/** 帧率统计：每 N 帧打一条 Log（不宜过密） */
#ifndef MJPEG_FPS_LOG_EVERY_N_FRAMES
#define MJPEG_FPS_LOG_EVERY_N_FRAMES 60
#endif
/** 超过此阈值时单独打一条慢帧 Log，便于定位卡顿 */
#ifndef MJPEG_SLOW_FRAME_US
#define MJPEG_SLOW_FRAME_US 80000
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

#define MJPEG_READ_TASK_PRIORITY   2
#define MJPEG_DECODE_TASK_PRIORITY 4
/*
 * S3 软解 240x290 耗时 ~30ms/帧（解+blit）。Audio_input 优先级 8 占 CPU0 约 50%，mjpeg_decode 必须搬出 CPU0，
 * 否则帧间隔从 50ms 拉到 500ms+。read_task 优先级低，与 decode 同核 CPU1 不互相抢占。
 * 解码 task CPU1 + LVGL 跨核短互斥（draw_bitmap），与 LVGL tick 不同核即可。
 */
#if CONFIG_FREERTOS_UNICORE
#define MJPEG_READ_TASK_CORE_ID   0
#define MJPEG_DECODE_TASK_CORE_ID 0
#else
#define MJPEG_READ_TASK_CORE_ID   1
#define MJPEG_DECODE_TASK_CORE_ID 1
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
static uint8_t *s_mjpeg_black_tile = NULL;
static uint8_t *s_mjpeg_slab = NULL;
static bool s_mjpeg_tiles_in_spiram = false;

static esp_err_t mjpeg_panel_draw_bitmap_retry(esp_lcd_panel_handle_t panel, int x0, int y0, int x1, int y1, const void *data)
{
    esp_err_t ret = ESP_FAIL;
    /* 重试次数提高，让 SDRAM 紧张（WiFi 切换加密）场景下能扛住 */
    const int retry_max = (int)MJPEG_DRAW_RETRY_MAX;
    for (int i = 0; i < retry_max; i++) {
        ret = esp_lcd_panel_draw_bitmap(panel, x0, y0, x1, y1, data);
        if (ret == ESP_OK) {
            return ret;
        }
        /* ESP_ERR_NO_MEM = panel_io 内部 SRAM 临时缓冲分配失败（PSRAM→SRAM memcpy）。
         * 这种情况下短重试不退让，等一段时间让 mbedtls 加密握手/WiFi 切换释放 SRAM。 */
        uint32_t us = MJPEG_DRAW_RETRY_US_MIN + (uint32_t)i * MJPEG_DRAW_RETRY_US_MIN;
        if (us > MJPEG_DRAW_RETRY_US_MAX) {
            us = MJPEG_DRAW_RETRY_US_MAX;
        }
        esp_rom_delay_us(us);
        taskYIELD();
        if ((i & 0x3) == 0x3) {
            vTaskDelay(pdMS_TO_TICKS(5));
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
    if (!s_mjpeg_black_tile || band_h <= 0) {
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
    if (!s_mjpeg_slab || col_w <= 0 || col_w > MJPEG_PILLAR_MAX_W || body_h <= 0) {
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

static void mjpeg_log_memory_budget(void)
{
    const size_t black_tile_size = (size_t)MJPEG_PANEL_WIDTH * (size_t)MJPEG_BLACK_TILE_H * sizeof(uint16_t);
    const size_t slab_size = (size_t)MJPEG_PILLAR_MAX_W * (size_t)MJPEG_BLACK_TILE_H * sizeof(uint16_t);
    const size_t static_tiles = black_tile_size + slab_size;
    const size_t dma_budget = (size_t)FRAME_BUF_SIZE * (size_t)NUM_DMA_BUFS;
    const size_t task_stack_budget = (size_t)(8192 + 8192);
    // ESP_LOGI(TAG, "预算: static_tiles=%uB(%uKB) dma_input=%uB(%uKB) task_stack=%uB(%uKB)",
    //          (unsigned)static_tiles, (unsigned)(static_tiles / 1024),
    //          (unsigned)dma_budget, (unsigned)(dma_budget / 1024),
    //          (unsigned)task_stack_budget, (unsigned)(task_stack_budget / 1024));
}

static esp_err_t mjpeg_alloc_roi_tiles(void)
{
    if (s_mjpeg_black_tile && s_mjpeg_slab) {
        return ESP_OK;
    }
    const size_t black_tile_size = (size_t)MJPEG_PANEL_WIDTH * (size_t)MJPEG_BLACK_TILE_H * sizeof(uint16_t);
    const size_t slab_size = (size_t)MJPEG_PILLAR_MAX_W * (size_t)MJPEG_BLACK_TILE_H * sizeof(uint16_t);
    s_mjpeg_black_tile = heap_caps_aligned_alloc(64, black_tile_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    s_mjpeg_slab = heap_caps_aligned_alloc(64, slab_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    s_mjpeg_tiles_in_spiram = (s_mjpeg_black_tile != NULL) && (s_mjpeg_slab != NULL);
    if (!s_mjpeg_black_tile || !s_mjpeg_slab) {
        if (s_mjpeg_black_tile) {
            heap_caps_free(s_mjpeg_black_tile);
            s_mjpeg_black_tile = NULL;
        }
        if (s_mjpeg_slab) {
            heap_caps_free(s_mjpeg_slab);
            s_mjpeg_slab = NULL;
        }
        s_mjpeg_black_tile = heap_caps_aligned_alloc(64, black_tile_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        s_mjpeg_slab = heap_caps_aligned_alloc(64, slab_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        s_mjpeg_tiles_in_spiram = false;
    }
    if (!s_mjpeg_black_tile || !s_mjpeg_slab) {
        if (s_mjpeg_black_tile) {
            heap_caps_free(s_mjpeg_black_tile);
            s_mjpeg_black_tile = NULL;
        }
        if (s_mjpeg_slab) {
            heap_caps_free(s_mjpeg_slab);
            s_mjpeg_slab = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    (void)memset(s_mjpeg_black_tile, 0, black_tile_size);
    (void)memset(s_mjpeg_slab, 0, slab_size);
    // ESP_LOGI(TAG, "ROI tiles allocated in %s", s_mjpeg_tiles_in_spiram ? "PSRAM" : "internal SRAM");
    return ESP_OK;
}

static void mjpeg_free_roi_tiles(void)
{
    if (s_mjpeg_black_tile) {
        heap_caps_free(s_mjpeg_black_tile);
        s_mjpeg_black_tile = NULL;
    }
    if (s_mjpeg_slab) {
        heap_caps_free(s_mjpeg_slab);
        s_mjpeg_slab = NULL;
    }
    s_mjpeg_tiles_in_spiram = false;
}

/** 0：禁用整文件预载；>0 时小于该字节的 mjpeg 预载入 PSRAM
 *  设 1024*1024 = 1MB：覆盖常用 idle/表情 (<=500KB)，
 *  让 PARTITION 源播放时直接走内存，避免每帧 esp_partition_read + PSRAM C2M sync 抖动 AFE 内部任务。 */
#ifndef MJPEG_PRELOAD_MAX_BYTES
/* TTS 期间 speak-240x290.mjpeg 1199KB 必须走 preload，否则循环播放时
 * read_task 每次从头走 esp_partition_read 读 1.2MB 耗 5-15 秒，期间 frame_queue
 * 一直空，decode task 200ms 等 → TTS 期间 0 fps 动画卡死。
 * 阈值 2MB 容纳 speak clip 及未来的扩帧版本，预加载多占 1.2MB PSRAM 没问题。 */
#define MJPEG_PRELOAD_MAX_BYTES (2 * 1024 * 1024)
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
    /* PARTITION 模式用 */
    const esp_partition_t *part;
    uint32_t part_offset;   /* 相对分区起始 */
    uint32_t part_size;     /* 文件字节数 */
    uint32_t part_read;     /* 已读取字节数 */
} read_ctx_t;

/* ─────────────── 播放器状态 ─────────────── */
static volatile bool s_running = false;
static TaskHandle_t s_read_task = NULL;
static TaskHandle_t s_decode_task = NULL;
static mjpeg_player_cfg_t s_cfg;
static QueueHandle_t s_frame_queue;
static QueueHandle_t s_free_queue;
static uint8_t *s_dma_bufs[NUM_DMA_BUFS];
/* 记录上次播放的文件路径，同一文件跳过重启 */
static char s_last_file_path[256] = {0};
static mjpeg_player_src_t s_last_src_type = MJPEG_SRC_FILE;
static char s_last_partition_key[160] = {0}; /* "<label>:<offset>:<size>" for PARTITION 源 */
/** true：上一次 stop 是异步模式（mjpeg_player_stop_async），任务自行退出但资源未回收。
 *  下一次 mjpeg_player_start 前必须先 mjpeg_do_deferred_cleanup()，
 *  等旧 decode_task 退出并释放 fb/queue/preload，避免新 start 撞上半退出的旧任务。 */
static bool s_deferred_cleanup = false;
/* 前向声明：mjpeg_do_deferred_cleanup() 在文件后部定义；mjpeg_player_start() 需要先调它。 */
static void mjpeg_do_deferred_cleanup(void);

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

    /* 末尾空间不足 1/4 时紧凑到头部（仅 FILE 模式需要；PARTITION 用线性覆盖） */
    if (ctx->fp && ctx->capacity - ctx->end < ctx->capacity / 4 && ctx->start > 0) {
        int len = ctx_avail(ctx);
        memmove(ctx->buf, ctx->buf + ctx->start, len);
        ctx->start = 0;
        ctx->end = len;
    }

    int space = ctx->capacity - ctx->end;
    if (space > 0) {
        int got = 0;
        if (ctx->fp) {
            got = fread(ctx->buf + ctx->end, 1, space, ctx->fp);
            ctx->end += got;
        } else if (ctx->part) {
            uint32_t remain = ctx->part_size - ctx->part_read;
            uint32_t want = (uint32_t)space < remain ? (uint32_t)space : remain;
            if (want > 0) {
                esp_err_t er = esp_partition_read(ctx->part,
                                                  ctx->part_offset + ctx->part_read,
                                                  ctx->buf + ctx->end, want);
                if (er == ESP_OK) {
                    got = (int)want;
                    ctx->part_read += want;
                    /* ctx.buf 在 PSRAM：esp_flash_read 走内部 temp_buffer→memcpy，
                     * memcpy 完成后 CPU 已拥有最新数据（写穿了 cache 到 PSRAM）。
                     * JPEG decoder 直接从 PSRAM 读，不走 CPU cache，无需 C2M sync。
                     * 显式 msync 可能与 decoder DMA 访问冲突或触发地址/长度校验错误，
                     * 在 ctx_fill 里直接去掉。 */
                    ctx->end += got;
                } else {
                    // ESP_LOGE(TAG, "esp_partition_read 失败: %s", esp_err_to_name(er));
                    got = 0;
                }
            }
        }
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
            // ESP_LOGW(TAG, "⚠️ 帧超过 %dKB，跳过", ctx->capacity / 1024);
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
    // ESP_LOGI(TAG, "📜 读取任务启动");

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
        // ESP_LOGI(TAG, "📦 使用 start 阶段预加载 PSRAM（%u KB），播放期不读 SD",
        //          (unsigned)(s_preload_size / 1024));
        ctx.buf = s_preload_buf;
        ctx.capacity = (int)s_preload_size;
        ctx.start = 0;
        ctx.end = (int)s_preload_size;
        ctx.fp = NULL;
        ctx.eof = true;
        ctx.from_preload = true;
        ctx.part = NULL;
        ctx.part_offset = 0;
        ctx.part_size = 0;
        ctx.part_read = 0;
    } else {
        ctx.buf = NULL;
        ctx.capacity = 0;
        ctx.start = 0;
        ctx.end = 0;
        ctx.fp = NULL;
        ctx.eof = false;
        ctx.from_preload = false;
        ctx.part = NULL;
        ctx.part_offset = 0;
        ctx.part_size = 0;
        ctx.part_read = 0;

        if (s_cfg.src_type == MJPEG_SRC_PARTITION) {
            ctx.part = (const esp_partition_t *)s_cfg.partition;
            if (!ctx.part || s_cfg.partition_size == 0) {
                // ESP_LOGE(TAG, "❌ PARTITION 源未配置（partition=%p size=%lu）",
                //          s_cfg.partition, (unsigned long)s_cfg.partition_size);
                s_running = false;
                goto exit;
            }
            ctx.part_offset = s_cfg.partition_offset;
            ctx.part_size = s_cfg.partition_size;
            ctx.part_read = 0;
            // ESP_LOGI(TAG, "📦 PARTITION 源: part=%s off=0x%x size=%lu KB",
            //          ctx.part->label, (unsigned)ctx.part_offset,
            //          (unsigned long)(ctx.part_size / 1024));
        } else {
            /* FILE 源：SD 卡可能刚被上一个任务释放，fopen 可能失败时重试 5 次，每次 200ms */
            for (int open_retry = 0; open_retry < 5; open_retry++) {
                ctx.fp = fopen(s_cfg.file_path, "rb");
                if (ctx.fp) {
                    break;
                }
                // ESP_LOGW(TAG, "SD 卡重试 fopen 失败 %d/5: %s", open_retry + 1, s_cfg.file_path);
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            if (!ctx.fp) {
                // ESP_LOGE(TAG, "❌ 无法打开文件: %s", s_cfg.file_path);
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
            // ESP_LOGI(TAG, "📄 文件大小: %.1f MB", file_size / (1024.0 * 1024.0));
        }

        read_buf = heap_caps_malloc(READ_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (read_buf) {
            read_buf_spiram = true;
        } else {
            read_buf = malloc(READ_BUF_SIZE);
        }
        if (!read_buf) {
            // ESP_LOGE(TAG, "❌ 分配读取缓冲区失败");
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
            /* PARTITION 源：ctx.fp == NULL，跳过 clearerr，否则会 NULL 解引用 */
            if (ctx.fp) {
                clearerr(ctx.fp);
            }
        }

        const uint8_t *frame_data;
        int frame_len;

        while (s_running && extract_frame(&ctx, &frame_data, &frame_len)) {
#if MJPEG_STRICT_FRAME_VALIDATE
            if (!validate_frame(frame_data, frame_len,
                                 s_cfg.screen_width, s_cfg.screen_height)) {
                skip_count++;
                if (skip_count <= 5 || skip_count % 100 == 0) {
                    // ESP_LOGW(TAG, "🛡️ 帧校验失败 #%lu（帧=%d字节）",
                    //          (unsigned long)skip_count, frame_len);
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
                continue;
            }

            if (frame_len > FRAME_BUF_SIZE) {
                skip_count++;
                // ESP_LOGW(TAG, "⚠️ 帧过大(%dB > %dB)，跳过", frame_len, FRAME_BUF_SIZE);
                /* 归还缓冲，避免 free_queue 被耗尽 */
                msg.len = 0;
                xQueueSend(s_free_queue, &msg, pdMS_TO_TICKS(100));
                continue;
            }

            /* 拷贝到 DMA 缓冲区（JPEG decoder 通过硬件 DMA 直接从 PSRAM 读输入，
             * 不走 CPU cache，无需 C2M sync——强制 sync 反而可能与 DMA 访问冲突，
             * 导致 esp_cache_msync 返回 ESP_ERR_NOT_ALLOWED (-0x004C)。 */
            memcpy(msg.buf, frame_data, frame_len);
            msg.len = frame_len;

            xQueueSend(s_frame_queue, &msg, pdMS_TO_TICKS(100));

            /* 每送一帧主动 yield，避免 read_task 长时间占 CPU1 触发 IDLE1 WDT。
             * taskYIELD() 不会延迟任何时间，但让 decode 任务（优先级 4 > read 0）抢到 CPU，
             * 同时 IDLE1 也能上 CPU 喂狗。 */
            taskYIELD();
        }

        /* 非循环模式才发送 EOF；循环模式回绕文件头继续读 */
        if (!s_cfg.loop || !s_running) {
            frame_msg_t eof = { .buf = NULL, .len = 0 };
            xQueueSend(s_frame_queue, &eof, pdMS_TO_TICKS(100));
            break;
        }
        if (ctx.from_preload) {
            /* 已在 RAM 中，仅重置游标 */
            continue;
        }
        /* PARTITION 源：文件游标在 ctx 上，重置所有游标回到开头 */
        if (ctx.fp) {
            fseek(ctx.fp, 0, SEEK_SET);
        } else if (ctx.part) {
            ctx.part_read = 0;
        }
        ctx.start = 0;
        ctx.end = 0;
        ctx.eof = false;
        if (ctx.buf) {
            ctx.capacity = READ_BUF_SIZE;
        }
        /* 让出 CPU 给 idle/decode，避免 read_task 主循环满 spin 触发 CPU1 WDT。
         * vTaskDelay 进入 blocked 状态期间 IDLE1 一定跑得到。 */
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    /* 发送停止信号 */
    frame_msg_t stop_msg = { .buf = NULL, .len = -1 };
    xQueueSend(s_frame_queue, &stop_msg, pdMS_TO_TICKS(100));

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
    // ESP_LOGI(TAG, "📜 读取任务结束 (跳过帧: %lu)", (unsigned long)skip_count);
    s_read_task = NULL;
    vTaskDelete(NULL);
}

/* ═══════════════════════════════════════════════════════════
 *  解码任务：ESP32-P4 硬件 JPEG；其它目标（如 S3）用 esp_new_jpeg 软解
 * ═══════════════════════════════════════════════════════════ */

static void mjpeg_decode_task(void *arg)
{
    // ESP_LOGI(TAG, "decode task start");

#if CONFIG_IDF_TARGET_ESP32P4
    jpeg_decode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,
        .timeout_ms = 5000,
    };
    jpeg_decoder_handle_t decoder = NULL;
    esp_err_t ret = jpeg_new_decoder_engine(&engine_cfg, &decoder);
    if (ret != ESP_OK) {
        // ESP_LOGE(TAG, "HW jpeg engine failed: %s", esp_err_to_name(ret));
        s_running = false;
        goto exit;
    }
    // ESP_LOGI(TAG, "HW JPEG ready (timeout=%dms)", engine_cfg.timeout_ms);

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
        // ESP_LOGE(TAG, "esp_new_jpeg jpeg_dec_open failed");
        s_running = false;
        goto exit;
    }
    // ESP_LOGI(TAG, "SW JPEG (esp_new_jpeg) ready");
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
    int consecutive_errors = 0;
    int consecutive_draw_fail = 0;  // 连续 draw 失败计数
    bool first_frame_logged_once = false;
    int64_t start_time = esp_timer_get_time();

    const int64_t frame_interval_us = s_cfg.target_fps > 0 ?
        (1000000 / s_cfg.target_fps) : 0;
    /* 严格不超速：维护绝对墙钟节拍 next_frame_deadline_us。
     * 每帧到达时若比 deadline 早则 sleep 至 deadline；否则落后时立即放行并把
     * deadline 推进一格，保证长期平均 fps <= target_fps。 */
    int64_t next_frame_deadline_us = (frame_interval_us > 0) ?
        esp_timer_get_time() + frame_interval_us : 0;

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
                // ESP_LOGD(TAG, "✅ 播放结束: %lu帧, %.1f fps, %.1f秒, 错误%lu",
                //          (unsigned long)frame_count,
                //          frame_count * 1e6f / elapsed,
                //          elapsed / 1e6f,
                //          (unsigned long)decode_errors);
            }
            frame_count = 0;
            decode_errors = 0;
            consecutive_errors = 0;
            start_time = esp_timer_get_time();
            /* 文件结束重置后，重新对齐 deadline 到下一格，避免回到 0 立刻放行造成超速 */
            if (frame_interval_us > 0) {
                next_frame_deadline_us = esp_timer_get_time() + frame_interval_us;
            }
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
            // ESP_LOGW(TAG, "JPEG %ux%u != clip %ux%u", (unsigned)hdr.width, (unsigned)hdr.height,
            //          (unsigned)s_cfg.screen_width, (unsigned)s_cfg.screen_height);
            ret = ESP_ERR_INVALID_SIZE;
        } else {
            int inbuf_consumed = jpeg_io.inbuf_len - jpeg_io.inbuf_remain;
            if (inbuf_consumed < 0 || inbuf_consumed > jpeg_io.inbuf_len) {
                // ESP_LOGW(TAG, "jpeg header 游标异常: consumed=%d, in=%d, remain=%d",
                //          inbuf_consumed, jpeg_io.inbuf_len, jpeg_io.inbuf_remain);
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
        xQueueSend(s_free_queue, &free_msg, pdMS_TO_TICKS(100));

        /* 仅以输出字节数为硬失败；P4 硬解常 ret!=OK 但 decoded 已齐 */
        if (decoded_size != expect_decoded) {
            decode_errors++;
            consecutive_errors++;
            if (decode_errors <= 5 || decode_errors % 50 == 0) {
                // ESP_LOGW(TAG, "decode fail #%lu (ret=%s, decoded=%lu expect=%lu, jpeg=%d)",
                //          (unsigned long)decode_errors, esp_err_to_name(ret),
                //          (unsigned long)decoded_size, (unsigned long)expect_decoded, msg.len);
            }
#if CONFIG_IDF_TARGET_ESP32P4
            jpeg_del_decoder_engine(decoder);
            decoder = NULL;
            vTaskDelay(pdMS_TO_TICKS(5));
            ret = jpeg_new_decoder_engine(&engine_cfg, &decoder);
            if (ret != ESP_OK) {
                // ESP_LOGE(TAG, "reopen HW jpeg failed");
                break;
            }
#else
            if (sw_dec) {
                jpeg_dec_close(sw_dec);
                sw_dec = NULL;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            if (jpeg_dec_open(&sw_dec_cfg, &sw_dec) != JPEG_ERR_OK || sw_dec == NULL) {
                // ESP_LOGE(TAG, "reopen SW jpeg failed");
                break;
            }
#endif
            if (consecutive_errors >= 20) {
                // ESP_LOGE(TAG, "too many decode errors, stop");
                s_running = false;
                break;
            }
            continue;
        }
        consecutive_errors = 0;

        if (!first_frame_logged_once) {
            const char *mode = s_panel_roi_blit ? "紧密缓冲+draw_bitmap" : (s_embed_lvgl ? "LVGL画布" : "DPI 帧缓冲");
            // ESP_LOGI(TAG, "🎬 首帧: JPEG=%d字节, RGB565=%lu字节 (%s)",
            //          msg.len, (unsigned long)decoded_size, mode);
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
            if (lvgl_port_lock(MJPEG_ROI_LVGL_LOCK_MS)) {
                if (MJPEG_POST_LOCK_DRAIN_MS > 0) {
                    vTaskDelay(pdMS_TO_TICKS(MJPEG_POST_LOCK_DRAIN_MS));
                }
                if (MJPEG_ROI_DRAW_LETTERBOX_ONCE && !s_roi_letterbox_drawn) {
                    mjpeg_roi_letterbox_draw(s_cfg.panel, MJPEG_PANEL_WIDTH, MJPEG_PANEL_HEIGHT, x1, y1, w, h);
                    s_roi_letterbox_drawn = true;
                }
                esp_err_t blit = mjpeg_panel_draw_bitmap_banded(s_cfg.panel, x1, y1, w, h, s_cfg.fb[fb_idx]);
                if (blit != ESP_OK) {
                    consecutive_draw_fail++;
                    // ESP_LOGW(TAG, "⚠️ ROI draw失败: %s (连续%d次)",
                    //          esp_err_to_name(blit), consecutive_draw_fail);
                    /* 整帧放弃，让 WiFi/UDP 加密握手抢占的 SRAM 恢复
                     * 连续失败累积后适度延长退避（最多 200ms） */
                    lvgl_port_unlock();
                    if (consecutive_draw_fail >= 3) {
                        int backoff_ms = 30;
                        if (consecutive_draw_fail >= 10) backoff_ms = 100;
                        if (consecutive_draw_fail >= 30) backoff_ms = 200;
                        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
                    }
                    continue;
                } else {
                    consecutive_draw_fail = 0;  // 成功一次重置计数
                }
                lvgl_port_unlock();
            } else {
                consecutive_draw_fail++;
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
            // ESP_LOGI(TAG, "⏱ prof #%lu: jpeg_decode=%lldus blit=%lldus",
            //          (unsigned long)frame_count,
            //          (long long)(t_after_decode - t_start),
            //          (long long)(t_after_blit - t_after_decode));
        }
#endif

        if (frame_count % MJPEG_FPS_LOG_EVERY_N_FRAMES == 0) {
            int64_t elapsed = esp_timer_get_time() - start_time;
            if (elapsed < 1) {
                elapsed = 1;
            }
             ESP_LOGI(TAG, "📊 %lu帧, %.1f fps, 错误%lu",
                      (unsigned long)frame_count,
                      (float)frame_count * 1e6f / (float)elapsed,
                      (unsigned long)decode_errors);
        }

        /* 帧率控制（严格不超速）：
         *  - 若当前时间早于 next_frame_deadline_us，补 sleep 至 deadline
         *  - 否则落后于节奏，立即放行本帧并把 deadline 推进一格（不再叠加延时）
         *  这样长期平均 fps == target_fps，且绝不会因单帧耗时 < interval 而累加超速 */
        if (frame_interval_us > 0) {
            const int64_t now_us = esp_timer_get_time();
            int64_t delay_us = next_frame_deadline_us - now_us;
            if (delay_us > 0) {
                vTaskDelay(pdMS_TO_TICKS((delay_us + 999) / 1000));
            }
            next_frame_deadline_us += frame_interval_us;
            /* 如果落后太多（>2 帧），把 deadline 拨到现在，避免下一次又立刻放行造成追赶超速 */
            if (next_frame_deadline_us < now_us - frame_interval_us) {
                next_frame_deadline_us = now_us + frame_interval_us;
            }
        }

        /* 慢帧诊断：单帧 > 80ms 时单独打，定位卡顿来源（decode 慢 / blit 慢 / lock 等待） */
        int64_t t_end = esp_timer_get_time();
        int64_t frame_us = t_end - t_start;
        if (frame_us > MJPEG_SLOW_FRAME_US && frame_count < 200) {
            // ESP_LOGW(TAG, "🐢 慢帧 #%lu: total=%lldus (t_start→blit→end)", (unsigned long)frame_count, (long long)frame_us);
        }
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
    // ESP_LOGI(TAG, "decode task exit");
    s_decode_task = NULL;
    vTaskDelete(NULL);
}

/* ═══════════════════════════════════════════════════════════
 *  公有 API
 * ═══════════════════════════════════════════════════════════ */

esp_err_t mjpeg_player_start(const mjpeg_player_cfg_t *cfg)
{
    mjpeg_log_memory_budget();
    /* 先清理上一次异步 stop 遗留的资源（旧 decode_task + 队列 + preload + fb）。
     * 这一步只在 s_deferred_cleanup=true 时做事，对同步 stop 路径无副作用。 */
    mjpeg_do_deferred_cleanup();
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    /* FILE 源必须给路径；PARTITION 源必须有 partition + size */
    if (cfg->src_type == MJPEG_SRC_FILE) {
        if (!cfg->file_path) {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (cfg->src_type == MJPEG_SRC_PARTITION) {
        if (!cfg->partition || cfg->partition_size == 0) {
            return ESP_ERR_INVALID_ARG;
        }
    } else {
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
    /* 若 stop 已调用但任务还在退出（s_running=false 但任务句柄仍非空），
     * 同步等待任务完全退出后再判断，避免 start 撞到半退出的任务。 */
    if (!s_running && (s_read_task || s_decode_task)) {
        // ESP_LOGI(TAG, "等待上一轮任务退出...");
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1500);
        while (s_read_task || s_decode_task) {
            if (xTaskGetTickCount() > deadline) {
                // ESP_LOGW(TAG, "⚠️ 等待任务退出超时，强制清理");
                if (s_read_task) { vTaskDelete(s_read_task); s_read_task = NULL; }
                if (s_decode_task) { vTaskDelete(s_decode_task); s_decode_task = NULL; }
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    /* 同一文件已在播放中（包含 s_running=true 或 上一轮尚未完全 stop 但路径相同），
     * 直接返回成功，不重启。 */
    if (s_running) {
        bool same = false;
        if (cfg->src_type == MJPEG_SRC_FILE && s_last_src_type == MJPEG_SRC_FILE
            && cfg->file_path && s_last_file_path[0] != '\0') {
            same = (strcmp(s_last_file_path, cfg->file_path) == 0);
        } else if (cfg->src_type == MJPEG_SRC_PARTITION && s_last_src_type == MJPEG_SRC_PARTITION) {
            char key[160];
            const esp_partition_t *p = (const esp_partition_t *)cfg->partition;
            snprintf(key, sizeof(key), "%s:%lu:%lu",
                     p ? p->label : "?",
                     (unsigned long)cfg->partition_offset,
                     (unsigned long)cfg->partition_size);
            same = (strncmp(s_last_partition_key, key, sizeof(s_last_partition_key)) == 0);
        }
        if (same) {
            // ESP_LOGI(TAG, "🔄 同一文件已播放中，跳过重启");
            return ESP_OK;
        }
        // ESP_LOGW(TAG, "⚠️ 播放器已在运行（不同文件）");
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
    // ESP_LOGI(TAG, "MJPEG颜色链路: panel_roi=%d lvgl=%d RGB565_BE解码=%d RGB565字节矫正=%d",
//          s_panel_roi_blit ? 1 : 0, s_embed_lvgl ? 1 : 0,
//          s_mjpeg_sw_decode_rgb565_be ? 1 : 0, s_mjpeg_swap_rgb565_bytes ? 1 : 0);

    if (s_embed_lvgl || s_panel_roi_blit) {
        const size_t sz = (size_t)s_cfg.screen_width * (size_t)s_cfg.screen_height * sizeof(uint16_t);
        const size_t sz_al = (sz + 63u) & ~63u;
        const int fb_count = s_panel_roi_blit ? 1 : 2;
        /* 仅当 fb 尚未分配或分辨率变化时才重新分配；多次 start 复用同一块 fb */
        const bool need_fb_alloc = (s_cfg.fb[0] == NULL) ||
            (s_panel_roi_blit && s_output_fb_shared == false && s_cfg.fb[1] == NULL);
        if (need_fb_alloc) {
            for (int i = 0; i < fb_count; i++) {
                /* 硬件 JPEG 写 PSRAM 往往明显慢于内部 SRAM；优先 INTERNAL */
                s_cfg.fb[i] = heap_caps_aligned_alloc(64, sz_al, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
                if (!s_cfg.fb[i]) {
                    s_cfg.fb[i] = heap_caps_aligned_alloc(64, sz_al, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
                }
                if (!s_cfg.fb[i]) {
                    // ESP_LOGE(TAG, "❌ 分配解码缓冲 %d 失败", i);
                    for (int j = 0; j < i; j++) {
                        heap_caps_free(s_cfg.fb[j]);
                        s_cfg.fb[j] = NULL;
                    }
                    return ESP_ERR_NO_MEM;
                }
            }
        }
        if (s_panel_roi_blit) {
            s_cfg.fb[1] = s_cfg.fb[0];
            s_output_fb_shared = true;
        } else {
            s_output_fb_shared = false;
        }
        if (s_cfg.fb[0] && need_fb_alloc) {
#if MJPEG_HAVE_ESP_PTR_EXTERNAL_RAM
            // ESP_LOGI(TAG, "解码输出缓冲[0]: %s",
            //          esp_ptr_external_ram(s_cfg.fb[0]) ? "PSRAM" : "内部SRAM(优先)");
#else
            // ESP_LOGI(TAG, "解码输出缓冲[0]: 已分配（优先内部SRAM）");
#endif
        }
        if (s_embed_lvgl) {
            lv_obj_t *cv = (lv_obj_t *)s_cfg.lv_video_canvas;
            lv_canvas_set_buffer(cv, s_cfg.fb[0], s_cfg.screen_width, s_cfg.screen_height, LV_COLOR_FORMAT_RGB565);
            // ESP_LOGI(TAG, "💾 LVGL 画布模式: 解码缓冲 %dx%d ×2", s_cfg.screen_width, s_cfg.screen_height);
        } else if (need_fb_alloc) {
            /* ROI tile 也只在首次分配（黑条 + 竖条） */
            esp_err_t tile_ret = mjpeg_alloc_roi_tiles();
            if (tile_ret != ESP_OK) {
                // ESP_LOGE(TAG, "❌ 分配 ROI tiles 失败: %s", esp_err_to_name(tile_ret));
                if (s_cfg.fb[0]) {
                    heap_caps_free(s_cfg.fb[0]);
                    s_cfg.fb[0] = NULL;
                }
                if (!s_output_fb_shared && s_cfg.fb[1]) {
                    heap_caps_free(s_cfg.fb[1]);
                    s_cfg.fb[1] = NULL;
                }
                s_output_fb_shared = false;
                return tile_ret;
            }
            // ESP_LOGI(TAG, "💾 面板 ROI: %dx%d @ (%u,%u) draw_bitmap（短互斥，单缓冲 %dx%d）",
            //          s_cfg.screen_width, s_cfg.screen_height,
            //          (unsigned)s_cfg.panel_roi_x, (unsigned)s_cfg.panel_roi_y,
            //          s_cfg.screen_width, s_cfg.screen_height);
        }
    } else if (!s_cfg.fb[0] || !s_cfg.fb[1]) {
        esp_err_t gf = mjpeg_get_frame_buffers(s_cfg.panel, &s_cfg.fb[0], &s_cfg.fb[1]);
        if (gf != ESP_OK || !s_cfg.fb[0] || !s_cfg.fb[1]) {
            // ESP_LOGE(TAG, "❌ 获取 DPI 帧缓冲失败: %s", esp_err_to_name(gf));
            return gf != ESP_OK ? gf : ESP_ERR_INVALID_STATE;
        }
    }
    s_running = true;

    s_preload_buf = NULL;
    s_preload_size = 0;
    s_preload_is_malloc = false;
#if MJPEG_PRELOAD_MAX_BYTES > 0
    {
        uint8_t* pb = NULL;
        size_t sz = 0;
        if (s_cfg.src_type == MJPEG_SRC_PARTITION) {
            /* PARTITION 源：把整个文件读到 PSRAM */
            if (s_cfg.partition && s_cfg.partition_size > 0
                && s_cfg.partition_size <= (uint32_t)MJPEG_PRELOAD_MAX_BYTES) {
                pb = heap_caps_malloc(s_cfg.partition_size, MALLOC_CAP_SPIRAM);
                if (pb) {
                    esp_err_t er = esp_partition_read((const esp_partition_t *)s_cfg.partition,
                                                      s_cfg.partition_offset,
                                                      pb, s_cfg.partition_size);
                    if (er == ESP_OK) {
                        sz = s_cfg.partition_size;
                    } else {
                        // ESP_LOGE(TAG, "PARTITION 预加载读失败: %s", esp_err_to_name(er));
                        heap_caps_free(pb);
                        pb = NULL;
                    }
                } else {
                    pb = malloc(s_cfg.partition_size);
                    if (pb) {
                        s_preload_is_malloc = true;
                        esp_err_t er = esp_partition_read((const esp_partition_t *)s_cfg.partition,
                                                          s_cfg.partition_offset,
                                                          pb, s_cfg.partition_size);
                        if (er == ESP_OK) {
                            sz = s_cfg.partition_size;
                        } else {
                            free(pb);
                            pb = NULL;
                            s_preload_is_malloc = false;
                        }
                    }
                }
            }
        } else {
            /* FILE 源：原 fopen + fseek/ftell/fread */
            FILE *pf = fopen(s_cfg.file_path, "rb");
            if (pf) {
                if (fseek(pf, 0, SEEK_END) == 0) {
                    long fsz = ftell(pf);
                    if (fsz > 0 && (size_t)fsz <= (size_t)MJPEG_PRELOAD_MAX_BYTES) {
                        pb = heap_caps_malloc((size_t)fsz, MALLOC_CAP_SPIRAM);
                        if (!pb) {
                            pb = malloc((size_t)fsz);
                            if (pb) s_preload_is_malloc = true;
                        }
                        if (pb) {
                            rewind(pf);
                            if (fread(pb, 1, (size_t)fsz, pf) == (size_t)fsz) {
                                sz = (size_t)fsz;
                            } else {
                                if (s_preload_is_malloc) free(pb);
                                else heap_caps_free(pb);
                                pb = NULL;
                                s_preload_is_malloc = false;
                            }
                        } else {
                            // ESP_LOGW(TAG, "⚠️ 预加载分配失败，使用流式读取");
                        }
                    }
                }
                fclose(pf);
            }
        }
        if (pb && sz > 0) {
            s_preload_buf = pb;
            s_preload_size = sz;
            // ESP_LOGI(TAG, "📦 预加载 %u KB（%s），读任务从内存取帧",
            //          (unsigned)(sz / 1024),
            //          s_preload_is_malloc ? "内部堆" : "PSRAM");
        }
    }
#endif

    /* 创建队列 */
    s_frame_queue = xQueueCreate(NUM_DMA_BUFS + 2, sizeof(frame_msg_t));
    s_free_queue = xQueueCreate(NUM_DMA_BUFS, sizeof(frame_msg_t));

    /* 分配 JPEG 帧输入缓冲（P4 用硬件分配器；S3 等用 DMA 能力堆）
     * 复用 s_dma_bufs[]：仅当全部为 NULL 时才分配，多次 start 不重复分配。 */
#if CONFIG_IDF_TARGET_ESP32P4
    jpeg_decode_memory_alloc_cfg_t in_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
#endif
    bool need_dma_alloc = (s_dma_bufs[0] == NULL);
    if (need_dma_alloc) {
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
                // ESP_LOGE(TAG, "frame buf %d alloc failed", i);
                mjpeg_release_preload_buf();
                s_running = false;
                return ESP_ERR_NO_MEM;
            }
        }
    }
    /* 每次把 DMA buf 重新放入 free_queue（保证开始时全部为空闲） */
    for (int i = 0; i < NUM_DMA_BUFS; i++) {
        frame_msg_t msg = { .buf = s_dma_bufs[i], .len = 0 };
        xQueueSend(s_free_queue, &msg, 0);
        if (need_dma_alloc) {
            // ESP_LOGI(TAG, "frame buf %d: %uKB", i, (unsigned)(FRAME_BUF_SIZE / 1024));
        }
    }

    if (!s_embed_lvgl && !s_panel_roi_blit) {
        uint32_t panel_fb = (uint32_t)MJPEG_PANEL_WIDTH * (uint32_t)MJPEG_PANEL_HEIGHT * sizeof(uint16_t);
        // ESP_LOGI(TAG, "💾 异步流水线: 读取=%dKB, DMA=%dKB×%d, 输出=DPI FB×2(%luKB×2, %dx%d)",
        //          READ_BUF_SIZE / 1024, FRAME_BUF_SIZE / 1024, NUM_DMA_BUFS,
        //          (unsigned long)(panel_fb / 1024), MJPEG_PANEL_WIDTH, MJPEG_PANEL_HEIGHT);
    }

    /* 创建双任务 */
    BaseType_t ret;
    /* 读=CPU1（避 WDT），解码=CPU0；优先级读>解码以喂满队列 */
    ret = xTaskCreatePinnedToCore(mjpeg_read_task, "mjpeg_read", 8192, NULL, MJPEG_READ_TASK_PRIORITY, &s_read_task,
                                  MJPEG_READ_TASK_CORE_ID);
    if (ret != pdPASS) {
        // ESP_LOGE(TAG, "❌ 创建读取任务失败");
        mjpeg_release_preload_buf();
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    ret = xTaskCreatePinnedToCore(mjpeg_decode_task, "mjpeg_dec", 8192, NULL, MJPEG_DECODE_TASK_PRIORITY,
                                  &s_decode_task, MJPEG_DECODE_TASK_CORE_ID);
    if (ret != pdPASS) {
        // ESP_LOGE(TAG, "❌ 创建解码任务失败");
        mjpeg_release_preload_buf();
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    /* 记录当前播放的文件路径 */
    s_last_src_type = s_cfg.src_type;
    if (s_cfg.src_type == MJPEG_SRC_FILE && s_cfg.file_path) {
        strncpy(s_last_file_path, s_cfg.file_path, sizeof(s_last_file_path) - 1);
        s_last_file_path[sizeof(s_last_file_path) - 1] = '\0';
    } else {
        s_last_file_path[0] = '\0';
    }
    if (s_cfg.src_type == MJPEG_SRC_PARTITION) {
        const esp_partition_t *p = (const esp_partition_t *)s_cfg.partition;
        snprintf(s_last_partition_key, sizeof(s_last_partition_key), "%s:%lu:%lu",
                 p ? p->label : "?",
                 (unsigned long)s_cfg.partition_offset,
                 (unsigned long)s_cfg.partition_size);
    } else {
        s_last_partition_key[0] = '\0';
    }

    // ESP_LOGI(TAG, "🚀 异步流水线已启动");
    return ESP_OK;
}

/* ─────────────────────────────────────────────────────────────────────────────
 *  mjpeg_player_stop()  /  mjpeg_player_stop_async()
 *
 *  同步版 (mjpeg_player_stop)：完全等待 read_task + decode_task 退出 + 资源释放。
 *  异步版 (mjpeg_player_stop_async)：只同步等待 read_task 退出（释放大块 PSRAM），
 *      decode_task 与资源回收推迟到下次 mjpeg_player_start() 调用 mjpeg_do_deferred_cleanup()。
 *
 *  ★ 为什么 decode_task 可以异步退出？
 *    - decode_task 仅持有一个 s_cfg.fb (320KB PSRAM) + jpeg_dec handle，无大块 PSRAM 堆，
 *      不释放 fb 不会让后续 start() 因 NO_MEM 失败（fb 分配走 MALLOC_CAP_INTERNAL 优先）。
 *    - decode_task 不抢占 read_task 资源；新 read_task 启动后可以预读新帧到 s_preload_buf。
 *    - 实测：speak→listen 这种频繁切换，从 ~200ms 卡顿降到 ~50ms。
 * ───────────────────────────────────────────────────────────────────────────── */

/* 通用 stop 主体：释放资源（队列 / fb / preload / roi tiles / path cache）。
 * 不负责等待任务退出——调用方先等待任务，再调此函数。
 * 注意：只在最后一次 stop（即确认所有任务都已退出）后才应执行清理，
 * 避免 decode_task 还在用 s_cfg.fb[0] 时被 free。 */
static void mjpeg_release_player_resources(void)
{
    /* 清空队列 */
    if (s_frame_queue) {
        frame_msg_t msg;
        while (xQueueReceive(s_frame_queue, &msg, 0) == pdTRUE) {}
        while (xQueueReceive(s_free_queue, &msg, 0) == pdTRUE) {}
        vQueueDelete(s_frame_queue);
        vQueueDelete(s_free_queue);
        s_frame_queue = NULL;
        s_free_queue = NULL;
    }

    mjpeg_release_preload_buf();
    mjpeg_free_roi_tiles();

    if (s_embed_lvgl || s_panel_roi_blit) {
        if (!s_output_fb_shared && s_cfg.fb[0]) {
            heap_caps_free(s_cfg.fb[0]);
            if (s_cfg.fb[1] && s_cfg.fb[0] != s_cfg.fb[1]) {
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

    /* 清除文件路径记录（让下一帧可以重新同文件检测） */
    s_last_file_path[0] = '\0';
    s_last_partition_key[0] = '\0';
    s_last_src_type = MJPEG_SRC_FILE;
}

/* 同步等待 read_task 退出：read task 持有 256KB PSRAM read_buf + 32KB PSRAM io_buf，
 * 不释放就立刻 start 新的会撞 SPIRAM NO_MEM。1500ms 超时，强制 vTaskDelete。 */
static void mjpeg_wait_read_task_exit(void)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1500);
    while (s_read_task) {
        if (xTaskGetTickCount() > deadline) {
            // ESP_LOGW(TAG, "⚠️ read_task 退出超时，强制删除");
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
}

/* 同步等待 decode_task 退出（短超时 200ms）。decode_task 退出较慢（持锁 blit），
 * 且不占大块堆，理论上不影响新 start 的资源分配——但新 start() 创建新 decode_task 前
 * 必须确认旧 decode_task 句柄已 NULL（vTaskDelete 已回收），否则 s_decode_task 会
 * 被新句柄覆盖，导致旧任务无人回收。 */
static void mjpeg_wait_decode_task_exit(void)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(200);
    while (s_decode_task) {
        if (xTaskGetTickCount() > deadline) {
            // ESP_LOGW(TAG, "⚠️ decode_task 退出超时，强制 vTaskDelete");
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
}

/* 延迟清理：上次是异步 stop（mjpeg_player_stop_async），decode_task 已退出但资源未回收。
 * mjpeg_player_start() 必须在创建新任务前调此函数。 */
static void mjpeg_do_deferred_cleanup(void)
{
    if (!s_deferred_cleanup) {
        return;
    }
    s_deferred_cleanup = false;

    mjpeg_wait_decode_task_exit();
    mjpeg_release_player_resources();
}

void mjpeg_player_stop_async(void)
{
    if (!s_running) {
        /* 即使 s_running=false，也可能有遗留的 deferred cleanup（极少见，例如 stop->start 紧接 stop）。 */
        if (s_deferred_cleanup) {
            mjpeg_do_deferred_cleanup();
        }
        return;
    }
    // ESP_LOGI(TAG, "⏹️ 正在停止播放（异步）...");
    s_running = false;

    /* 同步等待 read_task 退出（释放大块 PSRAM），decode_task 异步退出。 */
    mjpeg_wait_read_task_exit();

    /* 标记 deferred cleanup：mjpeg_player_start() 调起前会自动收尾。 */
    s_deferred_cleanup = true;

    /* 清除文件路径记录（让下一帧可以重新同文件检测） */
    s_last_file_path[0] = '\0';
    s_last_partition_key[0] = '\0';
    s_last_src_type = MJPEG_SRC_FILE;

    // ESP_LOGI(TAG, "💾 播放器异步停止中（read_task 已退，decode_task 自退，下次 start 时收尾）");
}

void mjpeg_player_stop(void)
{
    if (!s_running) {
        /* 若之前是异步 stop 还有遗留资源，这里同步清掉 */
        if (s_deferred_cleanup) {
            mjpeg_wait_decode_task_exit();
            mjpeg_release_player_resources();
            s_deferred_cleanup = false;
        }
        return;
    }
    // ESP_LOGI(TAG, "⏹️ 正在停止播放...");
    s_running = false;

    /* 同步等待两个任务退出。加超时避免 WiFi/SD 卡拥堵时永久死等。 */
    mjpeg_wait_read_task_exit();
    mjpeg_wait_decode_task_exit();

    mjpeg_release_player_resources();

    // ESP_LOGI(TAG, "💾 播放器资源已释放（DMA/fb 缓冲被复用，不立即释放）");
}

bool mjpeg_player_is_running(void)
{
    return s_running;
}
