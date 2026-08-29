#include <esp_check.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/param.h>

#include "esp_jpeg_common.h"
#include "esp_jpeg_dec.h"

#include "jpeg_to_image.h"

#ifdef CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE
#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL MAX(CONFIG_LOG_DEFAULT_LEVEL, ESP_LOG_DEBUG)
#endif  // CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE
#include <esp_log.h>

#ifdef CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_DECODER
#include "driver/jpeg_decode.h"
#endif

#define TAG "jpeg_to_image"
/* Full RGB565 beyond this is too large even in PSRAM for the cover path. */
#define JPEG_COVER_FULL_DECODE_MAX_PIXELS (320u * 320u)

static bool jpeg_peek_sof_size(const uint8_t* src, size_t len, uint16_t* w, uint16_t* h) {
    if (src == NULL || len < 4 || src[0] != 0xFF || src[1] != 0xD8) {
        return false;
    }
    size_t i = 2;
    while (i + 8 < len) {
        if (src[i] != 0xFF) {
            i++;
            continue;
        }
        while (i < len && src[i] == 0xFF) {
            i++;
        }
        if (i >= len) {
            return false;
        }
        uint8_t marker = src[i++];
        if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            continue;
        }
        if (i + 2 > len) {
            return false;
        }
        uint16_t seglen = ((uint16_t)src[i] << 8) | src[i + 1];
        if (seglen < 2 || i + seglen > len) {
            return false;
        }
        if ((marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC5 && marker <= 0xC7) ||
            (marker >= 0xC9 && marker <= 0xCB) || (marker >= 0xCD && marker <= 0xCF)) {
            if (seglen < 8) {
                return false;
            }
            *h = ((uint16_t)src[i + 3] << 8) | src[i + 4];
            *w = ((uint16_t)src[i + 5] << 8) | src[i + 6];
            return *w > 0 && *h > 0;
        }
        i += seglen;
    }
    return false;
}

static uint16_t jpeg_fit_dim(uint16_t src, uint16_t longest, uint16_t max_side) {
    uint32_t scaled = ((uint32_t)src * max_side) / longest;
    if (scaled < 8) {
        scaled = 8;
    }
    scaled &= ~7u;
    uint16_t min_allowed = (src / 8) & ~7u;
    if (min_allowed < 8) {
        min_allowed = 8;
    }
    if (scaled < min_allowed) {
        scaled = min_allowed;
    }
    return (uint16_t)scaled;
}

static void jpeg_dec_fail_out(uint8_t** out, size_t* out_len, size_t* width, size_t* height, size_t* stride) {
    *out = NULL;
    *out_len = 0;
    *width = 0;
    *height = 0;
    *stride = 0;
}

/* esp_new_jpeg scale runs after a full-size decode. Block mode keeps only one MCU strip. */
static esp_err_t decode_cover_block(const uint8_t* src, size_t src_len, uint8_t** out, size_t* out_len, size_t* width,
                                    size_t* height, size_t* stride, size_t max_side) {
    esp_err_t ret = ESP_OK;
    jpeg_error_t jpeg_ret = JPEG_ERR_OK;
    uint8_t* block_buf = NULL;
    uint8_t* dest = NULL;
    jpeg_dec_handle_t jpeg_dec = NULL;
    jpeg_dec_io_t jpeg_io = {0};
    jpeg_dec_header_info_t out_info = {0};

    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    config.rotate = JPEG_ROTATE_0D;
    config.block_enable = true;

    jpeg_ret = jpeg_dec_open(&config, &jpeg_dec);
    if (jpeg_ret != JPEG_ERR_OK) {
        ret = ESP_FAIL;
        goto fail;
    }

    jpeg_io.inbuf = (uint8_t*)src;
    jpeg_io.inbuf_len = (int)src_len;
    jpeg_ret = jpeg_dec_parse_header(jpeg_dec, &jpeg_io, &out_info);
    if (jpeg_ret != JPEG_ERR_OK || out_info.width == 0 || out_info.height == 0) {
        ret = ESP_ERR_INVALID_ARG;
        goto fail;
    }

    const uint16_t src_w = out_info.width;
    const uint16_t src_h = out_info.height;
    uint16_t longest = src_w > src_h ? src_w : src_h;
    uint16_t dst_w = src_w;
    uint16_t dst_h = src_h;
    if (longest > max_side) {
        dst_w = jpeg_fit_dim(src_w, longest, (uint16_t)max_side);
        dst_h = jpeg_fit_dim(src_h, longest, (uint16_t)max_side);
    }

    int block_len = 0;
    jpeg_ret = jpeg_dec_get_outbuf_len(jpeg_dec, &block_len);
    if (jpeg_ret != JPEG_ERR_OK || block_len <= 0) {
        ret = ESP_FAIL;
        goto fail;
    }
    int process_count = 0;
    jpeg_ret = jpeg_dec_get_process_count(jpeg_dec, &process_count);
    if (jpeg_ret != JPEG_ERR_OK || process_count <= 0) {
        ret = ESP_FAIL;
        goto fail;
    }

    block_buf = (uint8_t*)heap_caps_aligned_calloc(16, 1, (size_t)block_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    dest = (uint8_t*)heap_caps_aligned_calloc(16, 1, (size_t)dst_w * dst_h * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!block_buf || !dest) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    jpeg_io.outbuf = block_buf;

    uint16_t src_y0 = 0;
    for (int b = 0; b < process_count; b++) {
        jpeg_ret = jpeg_dec_process(jpeg_dec, &jpeg_io);
        if (jpeg_ret != JPEG_ERR_OK) {
            ret = ESP_FAIL;
            goto fail;
        }
        const int row_bytes = (int)src_w * 2;
        if (row_bytes <= 0 || jpeg_io.out_size < row_bytes) {
            ret = ESP_FAIL;
            goto fail;
        }
        const int rows = jpeg_io.out_size / row_bytes;
        const uint16_t* src_px = (const uint16_t*)block_buf;
        uint16_t* dst_px = (uint16_t*)dest;
        for (uint16_t dy = 0; dy < dst_h; dy++) {
            uint16_t sy = (uint16_t)(((uint32_t)dy * src_h) / dst_h);
            if (sy < src_y0 || sy >= (uint16_t)(src_y0 + rows)) {
                continue;
            }
            const uint16_t* src_row = src_px + (size_t)(sy - src_y0) * src_w;
            uint16_t* dst_row = dst_px + (size_t)dy * dst_w;
            for (uint16_t dx = 0; dx < dst_w; dx++) {
                uint16_t sx = (uint16_t)(((uint32_t)dx * src_w) / dst_w);
                dst_row[dx] = src_row[sx];
            }
        }
        src_y0 = (uint16_t)(src_y0 + rows);
    }

    ESP_LOGI(TAG, "Cover JPEG %ux%u -> %ux%u (block x%d, strip=%d)", (unsigned)src_w, (unsigned)src_h, (unsigned)dst_w,
             (unsigned)dst_h, process_count, block_len);
    jpeg_dec_close(jpeg_dec);
    heap_caps_free(block_buf);
    *out = dest;
    *out_len = (size_t)dst_w * dst_h * 2;
    *width = dst_w;
    *height = dst_h;
    *stride = (size_t)dst_w * 2;
    return ESP_OK;

fail:
    if (jpeg_dec) {
        jpeg_dec_close(jpeg_dec);
    }
    heap_caps_free(block_buf);
    heap_caps_free(dest);
    jpeg_dec_fail_out(out, out_len, width, height, stride);
    return ret;
}

static esp_err_t decode_full_rgb565(const uint8_t* src, size_t src_len, uint8_t** out, size_t* out_len, size_t* width,
                                    size_t* height, size_t* stride) {
    ESP_LOGD(TAG, "Decoding JPEG with software decoder");
    esp_err_t ret = ESP_OK;
    jpeg_error_t jpeg_ret = JPEG_ERR_OK;
    uint8_t* out_buf = NULL;
    jpeg_dec_io_t jpeg_io = {0};
    jpeg_dec_header_info_t out_info = {0};
    int outbuf_len = 0;

    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    config.rotate = JPEG_ROTATE_0D;

    jpeg_dec_handle_t jpeg_dec = NULL;
    jpeg_ret = jpeg_dec_open(&config, &jpeg_dec);
    if (jpeg_ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open JPEG decoder");
        ret = ESP_FAIL;
        goto jpeg_dec_failed;
    }

    jpeg_io.inbuf = (uint8_t*)src;
    jpeg_io.inbuf_len = (int)src_len;

    jpeg_ret = jpeg_dec_parse_header(jpeg_dec, &jpeg_io, &out_info);
    if (jpeg_ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "Failed to parse JPEG header");
        ret = ESP_ERR_INVALID_ARG;
        goto jpeg_dec_failed;
    }

    jpeg_ret = jpeg_dec_get_outbuf_len(jpeg_dec, &outbuf_len);
    if (jpeg_ret != JPEG_ERR_OK || outbuf_len <= 0) {
        outbuf_len = (int)out_info.width * (int)out_info.height * 2;
    }

    out_buf = (uint8_t*)heap_caps_aligned_calloc(16, 1, (size_t)outbuf_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for JPEG output buffer");
        ret = ESP_ERR_NO_MEM;
        goto jpeg_dec_failed;
    }

    jpeg_io.outbuf = out_buf;
    jpeg_ret = jpeg_dec_process(jpeg_dec, &jpeg_io);
    if (jpeg_ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "Failed to decode JPEG");
        ret = ESP_FAIL;
        goto jpeg_dec_failed;
    }

    *out = out_buf;
    *out_len = (size_t)out_info.width * (size_t)out_info.height * 2;
    *width = (size_t)out_info.width;
    *height = (size_t)out_info.height;
    *stride = (size_t)out_info.width * 2;
    jpeg_dec_close(jpeg_dec);
    return ret;

jpeg_dec_failed:
    if (jpeg_dec) {
        jpeg_dec_close(jpeg_dec);
    }
    if (out_buf) {
        heap_caps_free(out_buf);
    }
    jpeg_dec_fail_out(out, out_len, width, height, stride);
    return ret;
}

static esp_err_t decode_with_new_jpeg(const uint8_t* src, size_t src_len, uint8_t** out, size_t* out_len, size_t* width,
                                      size_t* height, size_t* stride, size_t max_side) {
    if (max_side == 0) {
        return decode_full_rgb565(src, src_len, out, out_len, width, height, stride);
    }

    /* Scale-after-decode in esp_new_jpeg still materializes the full RGB image. */
    esp_err_t ret = decode_cover_block(src, src_len, out, out_len, width, height, stride, max_side);
    if (ret == ESP_OK) {
        return ret;
    }
    ESP_LOGW(TAG, "Cover block decode failed (%s), try small full decode", esp_err_to_name(ret));
    uint16_t pw = 0, ph = 0;
    if (jpeg_peek_sof_size(src, src_len, &pw, &ph)) {
        uint32_t pixels = (uint32_t)pw * (uint32_t)ph;
        if (pixels > 0 && pixels <= JPEG_COVER_FULL_DECODE_MAX_PIXELS) {
            return decode_full_rgb565(src, src_len, out, out_len, width, height, stride);
        }
        ESP_LOGW(TAG, "Cover skip full decode %ux%u", (unsigned)pw, (unsigned)ph);
    }
    jpeg_dec_fail_out(out, out_len, width, height, stride);
    return ESP_ERR_NOT_SUPPORTED;
}

#ifdef CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_DECODER
static esp_err_t decode_with_hardware_jpeg(const uint8_t* src, size_t src_len, uint8_t** out, size_t* out_len,
                                           size_t* width, size_t* height, size_t* stride) {
    ESP_LOGD(TAG, "Decoding JPEG with hardware decoder");
    esp_err_t ret = ESP_OK;

    jpeg_decoder_handle_t jpeg_dec = NULL;
    uint8_t* bit_stream = NULL;
    uint8_t* out_buf = NULL;
    size_t out_buf_len = 0;
    size_t tx_buffer_size = 0;
    size_t rx_buffer_size = 0;

    jpeg_decode_engine_cfg_t eng_cfg = {
        .intr_priority = 1,
        .timeout_ms = 1000,
    };

    jpeg_decode_cfg_t decode_cfg_rgb = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };

    ret = jpeg_new_decoder_engine(&eng_cfg, &jpeg_dec);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create JPEG decoder engine");
        goto jpeg_hw_dec_failed;
    }

    jpeg_decode_memory_alloc_cfg_t tx_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };

    jpeg_decode_memory_alloc_cfg_t rx_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };

    bit_stream = (uint8_t*)jpeg_alloc_decoder_mem(src_len, &tx_mem_cfg, &tx_buffer_size);
    if (bit_stream == NULL || tx_buffer_size < src_len) {
        ESP_LOGE(TAG, "Failed to allocate memory for JPEG bit stream");
        ret = ESP_ERR_NO_MEM;
        goto jpeg_hw_dec_failed;
    }

    memcpy(bit_stream, src, src_len);

    jpeg_decode_picture_info_t header_info;
    ESP_GOTO_ON_ERROR(jpeg_decoder_get_info(bit_stream, src_len, &header_info), jpeg_hw_dec_failed, TAG,
                      "Failed to get JPEG header info");

    ESP_LOGD(TAG, "JPEG header info: width=%d, height=%d, sample_method=%d", header_info.width, header_info.height,
             (int)header_info.sample_method);

    switch (header_info.sample_method) {
        case JPEG_DOWN_SAMPLING_GRAY:
        case JPEG_DOWN_SAMPLING_YUV444:
            out_buf_len = header_info.width * header_info.height * 2;
            *stride = header_info.width * 2;
            break;
        case JPEG_DOWN_SAMPLING_YUV422:
        case JPEG_DOWN_SAMPLING_YUV420:
            out_buf_len = ((header_info.width + 15) & ~15) * ((header_info.height + 15) & ~15) * 2;
            *stride = ((header_info.width + 15) & ~15) * 2;
            break;
        default:
            ESP_LOGE(TAG, "Unsupported JPEG sample method");
            ret = ESP_ERR_NOT_SUPPORTED;
            goto jpeg_hw_dec_failed;
    }

    out_buf = (uint8_t*)jpeg_alloc_decoder_mem(out_buf_len, &rx_mem_cfg, &rx_buffer_size);
    if (out_buf == NULL || rx_buffer_size < out_buf_len) {
        ESP_LOGE(TAG, "Failed to allocate memory for JPEG output buffer");
        ret = ESP_ERR_NO_MEM;
        goto jpeg_hw_dec_failed;
    }

    uint32_t out_size = 0;

    ESP_GOTO_ON_ERROR(
        jpeg_decoder_process(jpeg_dec, &decode_cfg_rgb, bit_stream, src_len, out_buf, out_buf_len, &out_size),
        jpeg_hw_dec_failed, TAG, "Failed to decode JPEG");

    ESP_LOGD(TAG, "Expected %d bytes, got %" PRIu32 " bytes", out_buf_len, out_size);

    if (out_size != out_buf_len) {
        ESP_LOGE(TAG, "Decoded image size mismatch: Expected %zu bytes, got %" PRIu32 " bytes", out_buf_len, out_size);
        ret = ESP_ERR_INVALID_SIZE;
        goto jpeg_hw_dec_failed;
    }

    if (header_info.sample_method == JPEG_DOWN_SAMPLING_GRAY) {
        // convert GRAY8 to RGB565
        uint32_t i = header_info.width * header_info.height;
        do {
            --i;
            uint8_t r = (out_buf[i] >> 3) & 0x1F;
            uint8_t g = (out_buf[i] >> 2) & 0x3F;
            // b is same as r
            uint16_t rgb565 = (r << 11) | (g << 5) | r;
            out_buf[2 * i + 1] = (rgb565 >> 8) & 0xFF;
            out_buf[2 * i] = rgb565 & 0xFF;
        } while (i != 0);
        out_size = header_info.width * header_info.height * 2;
        ESP_LOGD(TAG, "Converted GRAY8 to RGB565, new size: %zu", out_size);
    }

    ESP_LOG_BUFFER_HEXDUMP(TAG, out_buf, MIN(out_size, 256), ESP_LOG_DEBUG);

    *out = out_buf;
    out_buf = NULL;
    *out_len = (size_t)out_size;
    jpeg_del_decoder_engine(jpeg_dec);
    jpeg_dec = NULL;
    heap_caps_free(bit_stream);
    bit_stream = NULL;
    *width = header_info.width;
    *height = header_info.height;

    return ret;

jpeg_hw_dec_failed:
    if (out_buf) {
        heap_caps_free(out_buf);
        out_buf = NULL;
    }
    if (bit_stream) {
        heap_caps_free(bit_stream);
        bit_stream = NULL;
    }
    if (jpeg_dec) {
        jpeg_del_decoder_engine(jpeg_dec);
        jpeg_dec = NULL;
    }
    *out = NULL;
    *out_len = 0;
    *width = 0;
    *height = 0;
    *stride = 0;
    return ret;
}
#endif  // CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_DECODER

esp_err_t jpeg_to_image(const uint8_t* src, size_t src_len, uint8_t** out, size_t* out_len, size_t* width,
                        size_t* height, size_t* stride) {
#ifdef CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
#endif  // CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE
    if (src == NULL || src_len == 0 || out == NULL || out_len == NULL || width == NULL || height == NULL ||
        stride == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }
#ifdef CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_DECODER
    esp_err_t ret = decode_with_hardware_jpeg(src, src_len, out, out_len, width, height, stride);
    if (ret == ESP_OK) {
        return ret;
    }
    ESP_LOGW(TAG, "Failed to decode with hardware JPEG, fallback to software decoder");
    // Fallback to esp_new_jpeg
#endif
    return decode_with_new_jpeg(src, src_len, out, out_len, width, height, stride, 0);
}

esp_err_t jpeg_to_image_fit(const uint8_t* src, size_t src_len, uint8_t** out, size_t* out_len, size_t* width,
                            size_t* height, size_t* stride, size_t max_side) {
    if (src == NULL || src_len == 0 || out == NULL || out_len == NULL || width == NULL || height == NULL ||
        stride == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }
    /* Cover path: software decode with scale. Hardware JPEG is DMA/internal-first. */
    return decode_with_new_jpeg(src, src_len, out, out_len, width, height, stride, max_side);
}
