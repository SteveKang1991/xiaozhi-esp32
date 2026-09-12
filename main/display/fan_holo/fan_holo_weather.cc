#include "fan_holo_weather.h"
#include "board.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <lvgl.h>
#include "src/draw/lv_image_decoder_private.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define TAG "HoloWeather"

static const char* kSeniverseKey = "Su3de5Yi5TAd84mdI";

static std::string UrlEncode(const std::string& str) {
    std::string encoded;
    char hex[4];
    for (unsigned char c : str) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += static_cast<char>(c);
        } else if (c == ' ') {
            encoded += "%20";
        } else {
            snprintf(hex, sizeof(hex), "%%%02X", c);
            encoded += hex;
        }
    }
    return encoded;
}

static bool HttpGet(const std::string& url, std::string* body) {
    if (body == nullptr) {
        return false;
    }
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return false;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        return false;
    }
    http->SetHeader("Accept", "application/json");
    if (!http->Open("GET", url)) {
        ESP_LOGW(TAG, "open failed: %s", url.c_str());
        return false;
    }
    const int code = http->GetStatusCode();
    if (code != 200) {
        ESP_LOGW(TAG, "HTTP %d", code);
        http->Close();
        return false;
    }
    char buf[512];
    int n = 0;
    while ((n = http->Read(buf, sizeof(buf))) > 0) {
        body->append(buf, n);
    }
    http->Close();
    return !body->empty();
}

static void CopyText(char* dst, size_t n, const char* src) {
    if (dst == nullptr || n == 0) {
        return;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, n, "%s", src);
}

static int JsonInt(cJSON* item) {
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        return atoi(item->valuestring);
    }
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return 0;
}

/* precip 为 0~1 的概率，转成百分比。 */
static int JsonPrecipPercent(cJSON* item) {
    double v = 0;
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        v = atof(item->valuestring);
    } else if (cJSON_IsNumber(item)) {
        v = item->valuedouble;
    }
    if (v >= 0 && v <= 1.0) {
        return static_cast<int>(lround(v * 100.0));
    }
    return static_cast<int>(lround(v));
}

static cJSON* FirstResult(cJSON* root) {
    cJSON* results = (root != nullptr) ? cJSON_GetObjectItem(root, "results") : nullptr;
    if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) <= 0) {
        return nullptr;
    }
    return cJSON_GetArrayItem(results, 0);
}

const char* FanHoloWeatherIconLabel(int id) {
    if (id >= 0 && id <= 3) {
        return "晴";
    }
    if (id >= 4 && id <= 8) {
        return "云";
    }
    if (id == 9) {
        return "阴";
    }
    if (id >= 10 && id <= 12) {
        return "雷";
    }
    if (id >= 13 && id <= 20) {
        return "雨";
    }
    if (id >= 21 && id <= 25) {
        return "雪";
    }
    return "雾";
}

unsigned int FanHoloWeatherIconColor(int id) {
    if (id >= 0 && id <= 3) {
        return 0xF5A623;
    }
    if (id >= 10 && id <= 12) {
        return 0xF5C400;
    }
    if (id >= 13 && id <= 20) {
        return 0x5B9BD5;
    }
    if (id >= 21 && id <= 25) {
        return 0xE8F4FF;
    }
    return 0xA0A8B0;
}

void FanHoloWeatherIconSdPath(int icon_id, char* dst, size_t dst_len) {
    if (dst == nullptr || dst_len == 0) {
        return;
    }
    int code = icon_id;
    if (code < 0 || (code > 38 && code != 99)) {
        code = 99;
    }
    snprintf(dst, dst_len, "S:/sdcard/Weather/%d.png", code);
}

struct WeatherIconSlot {
    int code = -1;
    int max_w = 0;
    int max_h = 0;
    lv_image_dsc_t dsc{};
    uint8_t* pixels = nullptr;
};

static WeatherIconSlot s_icon_slots[4];

static void FreeWeatherIconSlot(WeatherIconSlot* slot) {
    if (slot == nullptr) {
        return;
    }
    if (slot->pixels != nullptr) {
        heap_caps_free(slot->pixels);
        slot->pixels = nullptr;
    }
    memset(&slot->dsc, 0, sizeof(slot->dsc));
    slot->code = -1;
    slot->max_w = 0;
    slot->max_h = 0;
}

static bool FillWeatherIconSlot(WeatherIconSlot* slot, int code, int max_w, int max_h) {
    if (slot == nullptr) {
        return false;
    }
    if (slot->code == code && slot->pixels != nullptr &&
        slot->max_w == max_w && slot->max_h == max_h) {
        return true;
    }
    FreeWeatherIconSlot(slot);

    char path[64];
    FanHoloWeatherIconSdPath(code, path, sizeof(path));
    lv_image_decoder_args_t args{};
    args.no_cache = true;
    lv_image_decoder_dsc_t dec{};
    if (lv_image_decoder_open(&dec, path, &args) != LV_RESULT_OK ||
        dec.decoded == nullptr || dec.decoded->data == nullptr) {
        lv_image_decoder_close(&dec);
        ESP_LOGW(TAG, "decode weather png failed: %s", path);
        return false;
    }

    const lv_draw_buf_t* buf = dec.decoded;
    const int sw = static_cast<int>(buf->header.w);
    const int sh = static_cast<int>(buf->header.h);
    const uint32_t sstride = buf->header.stride;
    int dw = sw;
    int dh = sh;
    if (dw > max_w || dh > max_h) {
        const int sx = max_w * 256 / dw;
        const int sy = max_h * 256 / dh;
        const int s = sx < sy ? sx : sy;
        dw = (dw * s + 128) / 256;
        dh = (dh * s + 128) / 256;
        if (dw < 1) {
            dw = 1;
        }
        if (dh < 1) {
            dh = 1;
        }
    }
    const uint32_t dstride = static_cast<uint32_t>(dw) * 4u;
    const size_t nbytes = static_cast<size_t>(dstride) * static_cast<size_t>(dh);
    uint8_t* pixels = static_cast<uint8_t*>(
        heap_caps_malloc(nbytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pixels == nullptr) {
        pixels = static_cast<uint8_t*>(malloc(nbytes));
    }
    if (pixels == nullptr) {
        lv_image_decoder_close(&dec);
        ESP_LOGW(TAG, "weather icon alloc failed %dx%d", dw, dh);
        return false;
    }

    const uint8_t* src = buf->data;
    for (int y = 0; y < dh; ++y) {
        const int syi = (dh == sh) ? y : (y * sh) / dh;
        const uint8_t* srow = src + static_cast<size_t>(syi) * sstride;
        uint8_t* drow = pixels + static_cast<size_t>(y) * dstride;
        if (dw == sw && sstride >= dstride) {
            memcpy(drow, srow, dstride);
        } else {
            for (int x = 0; x < dw; ++x) {
                const int sxi = (dw == sw) ? x : (x * sw) / dw;
                memcpy(drow + x * 4, srow + sxi * 4, 4);
            }
        }
    }
    lv_image_decoder_close(&dec);

    slot->pixels = pixels;
    slot->code = code;
    slot->max_w = max_w;
    slot->max_h = max_h;
    memset(&slot->dsc, 0, sizeof(slot->dsc));
    slot->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    slot->dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    slot->dsc.header.w = static_cast<uint32_t>(dw);
    slot->dsc.header.h = static_cast<uint32_t>(dh);
    slot->dsc.header.stride = dstride;
    slot->dsc.data = pixels;
    slot->dsc.data_size = nbytes;
    return true;
}

void FanHoloPrepareWeatherIcons(const IdleWeatherView& view) {
    if (!view.valid) {
        return;
    }
    const int codes[4] = {
        view.icon, view.days[0].icon, view.days[1].icon, view.days[2].icon};
    const int max_w[4] = {96, 62, 62, 62};
    const int max_h[4] = {96, 62, 62, 62};
    for (int i = 0; i < 4; ++i) {
        if (!lvgl_port_lock(pdMS_TO_TICKS(3000))) {
            ESP_LOGW(TAG, "prepare weather icons: lvgl lock timeout");
            return;
        }
        FillWeatherIconSlot(&s_icon_slots[i], codes[i], max_w[i], max_h[i]);
        lvgl_port_unlock();
    }
}

const lv_image_dsc_t* FanHoloWeatherIconDsc(int slot) {
    if (slot < 0 || slot > 3 || s_icon_slots[slot].pixels == nullptr) {
        return nullptr;
    }
    return &s_icon_slots[slot].dsc;
}

static void FillDay(cJSON* day, IdleWeatherDay* out, const char* title) {
    CopyText(out->title, sizeof(out->title), title);
    cJSON* date = cJSON_GetObjectItem(day, "date");
    if (cJSON_IsString(date) && strlen(date->valuestring) >= 10) {
        snprintf(out->date, sizeof(out->date), "%.2s/%.2s",
                 date->valuestring + 5, date->valuestring + 8);
    }
    out->icon = JsonInt(cJSON_GetObjectItem(day, "code_day"));
    cJSON* text_day = cJSON_GetObjectItem(day, "text_day");
    if (cJSON_IsString(text_day) && text_day->valuestring != nullptr) {
        CopyText(out->text, sizeof(out->text), text_day->valuestring);
    } else {
        CopyText(out->text, sizeof(out->text), FanHoloWeatherIconLabel(out->icon));
    }
    out->temp_max = JsonInt(cJSON_GetObjectItem(day, "high"));
    out->temp_min = JsonInt(cJSON_GetObjectItem(day, "low"));
    out->humidity = JsonInt(cJSON_GetObjectItem(day, "humidity"));
    out->precip = JsonPrecipPercent(cJSON_GetObjectItem(day, "precip"));
}

bool FanHoloFetchWeather(const std::string& city, IdleWeatherView* out) {
    if (out == nullptr || city.empty()) {
        return false;
    }
    *out = IdleWeatherView{};
    CopyText(out->city, sizeof(out->city), city.c_str());
    const std::string loc = UrlEncode(city);

    char now_url[768];
    snprintf(now_url, sizeof(now_url),
             "https://api.seniverse.com/v3/weather/now.json?key=%s&location=%s&language=zh-Hans&unit=c",
             kSeniverseKey, loc.c_str());
    std::string now_body;
    if (!HttpGet(now_url, &now_body)) {
        return false;
    }
    cJSON* now_root = cJSON_Parse(now_body.c_str());
    cJSON* now_res = FirstResult(now_root);
    cJSON* now = (now_res != nullptr) ? cJSON_GetObjectItem(now_res, "now") : nullptr;
    if (!cJSON_IsObject(now)) {
        if (now_root) {
            cJSON_Delete(now_root);
        }
        ESP_LOGW(TAG, "now parse fail");
        return false;
    }
    cJSON* loc_obj = cJSON_GetObjectItem(now_res, "location");
    if (cJSON_IsObject(loc_obj)) {
        cJSON* name = cJSON_GetObjectItem(loc_obj, "name");
        if (cJSON_IsString(name)) {
            CopyText(out->city, sizeof(out->city), name->valuestring);
        }
    }
    cJSON* text = cJSON_GetObjectItem(now, "text");
    if (cJSON_IsString(text)) {
        CopyText(out->text, sizeof(out->text), text->valuestring);
    }
    out->icon = JsonInt(cJSON_GetObjectItem(now, "code"));
    out->temp = JsonInt(cJSON_GetObjectItem(now, "temperature"));
    cJSON_Delete(now_root);

    char d_url[768];
    snprintf(d_url, sizeof(d_url),
             "https://api.seniverse.com/v3/weather/daily.json?key=%s&location=%s&language=zh-Hans&unit=c&days=3",
             kSeniverseKey, loc.c_str());
    std::string d_body;
    if (!HttpGet(d_url, &d_body)) {
        return false;
    }
    cJSON* d_root = cJSON_Parse(d_body.c_str());
    cJSON* d_res = FirstResult(d_root);
    cJSON* daily = (d_res != nullptr) ? cJSON_GetObjectItem(d_res, "daily") : nullptr;
    if (!cJSON_IsArray(daily) || cJSON_GetArraySize(daily) <= 0) {
        if (d_root) {
            cJSON_Delete(d_root);
        }
        ESP_LOGW(TAG, "daily parse fail");
        return false;
    }

    static const char* kTitles[] = {"今天", "明天", "后天"};
    const int n = cJSON_GetArraySize(daily);
    for (int i = 0; i < 3 && i < n; ++i) {
        FillDay(cJSON_GetArrayItem(daily, i), &out->days[i], kTitles[i]);
    }

    cJSON* today = cJSON_GetArrayItem(daily, 0);
    if (cJSON_IsObject(today)) {
        out->humidity = JsonInt(cJSON_GetObjectItem(today, "humidity"));
        out->precip = JsonPrecipPercent(cJSON_GetObjectItem(today, "precip"));
        cJSON* dir = cJSON_GetObjectItem(today, "wind_direction");
        if (cJSON_IsString(dir)) {
            CopyText(out->wind_dir, sizeof(out->wind_dir), dir->valuestring);
        }
        cJSON* scale = cJSON_GetObjectItem(today, "wind_scale");
        if (cJSON_IsString(scale)) {
            CopyText(out->wind_scale, sizeof(out->wind_scale), scale->valuestring);
        }
        cJSON* speed = cJSON_GetObjectItem(today, "wind_speed");
        if (cJSON_IsString(speed)) {
            CopyText(out->wind_speed, sizeof(out->wind_speed), speed->valuestring);
        }
    }
    cJSON_Delete(d_root);

    out->valid = true;
    ESP_LOGI(TAG, "weather %s now %d %s", out->city, out->temp, out->text);
    return true;
}
