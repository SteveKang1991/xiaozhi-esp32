#include "ota.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"
#include <esp_lvgl_port.h>
#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#ifdef SOC_HMAC_SUPPORTED
#include <esp_hmac.h>
#endif

#include <cstring>
#include <vector>
#include <sstream>
#include <algorithm>

#define TAG "Ota"

namespace {

/**
 * LvglBurstLock - brief mutex acquisition used during download.
 *
 * Phase 0: We need to protect esp_ota_write() calls from LVGL flush callbacks,
 * but we do NOT want to hold the lock continuously (that would block lv_timer_handler
 * and prevent lv_async_call callbacks from running, freezing the OTA progress UI).
 *
 * This guard acquires the lock for a configurable burst, enough to cover a single
 * esp_ota_write() (~hundreds of microseconds), then immediately releases it.
 * After each burst, lv_timer_handler() gets a chance to process lv_async_call
 * callbacks queued by OtaScreen::Update().
 *
 * Usage: for each HTTP-read burst, hold lock around the esp_ota_write call.
 */
class LvglBurstLock {
public:
    explicit LvglBurstLock(int timeout_ms = 10) : acquired_(false), timeout_ms_(timeout_ms) {
        Acquire();
    }
    ~LvglBurstLock() { Release(); }

    void Acquire() {
        if (!acquired_ && lvgl_port_lock(timeout_ms_)) {
            acquired_ = true;
        }
    }
    void Release() {
        if (acquired_) {
            lvgl_port_unlock();
            acquired_ = false;
        }
    }

private:
    bool acquired_;
    int timeout_ms_;
};

/**
 * LvglQueueDrain - stops the tick timer before a full pause.
 *
 * Phase 1: Before we begin holding the LVGL lock for the entire flash-write
 * phase, we must stop the tick timer so lv_timer_handler() is never invoked.
 *
 * IMPORTANT: Do NOT call lv_timer_handler() here! lv_timer_handler() can
 * trigger a new flush (lvgl_port_flush_callback) which blocks on a semaphore
 * waiting for the DSI ISR. If that ISR hasn't fired yet (e.g. due to tick
 * timer being stopped), calling lv_timer_handler() while holding the mutex
 * deadlocks both tasks:
 *   - OTA task: holds the mutex, waiting for lv_timer_handler() to return
 *   - lvgl_port task: holds the mutex (wait-list), waiting for DSI ISR to give semaphore
 *
 * By skipping lv_timer_handler() entirely, we avoid this deadlock. Phase 2
 * will retry the flush after acquiring the mutex — at that point the DSI ISR
 * will have fired (timer was resumed) or the lock timeout will abort OTA.
 */
class LvglQueueDrain {
public:
    LvglQueueDrain() : timer_stopped_(false) {
        if (lvgl_port_stop() == ESP_OK) {
            timer_stopped_ = true;
            ESP_LOGI(TAG, "Tick timer stopped (skipping drain — avoids flush deadlock)");
        } else {
            ESP_LOGW(TAG, "Failed to stop timer in drain");
        }
    }
    ~LvglQueueDrain() {
        // Timer is NOT resumed here — LvglOtaLock resumes it on success or failure.
        // LvglOtaLock always calls lvgl_port_resume() in its destructor.
    }
    bool IsDrained() const { return timer_stopped_; }
private:
    bool timer_stopped_;
};

/**
 * LvglOtaLock - full LVGL lock matching MetalioClaw4's pause behaviour.
 *
 * Phase 2: While esp_ota_write() is in progress, we MUST prevent lv_timer_handler
 * from ever running, because even if the timer is stopped, lv_timer_handler()
 * checks the flush pending flag and may trigger DSI DMA while MSPI is handling
 * flash writes — this is the blue-screen race.
 *
 * This guard takes the recursive mutex and keeps it held for the entire Phase 2
 * section.  The tick timer is also stopped so no timer fires.  The LVGL task
 * is effectively paused.  On destruction, the mutex is released and the timer
 * restarted.
 *
 * This is equivalent to esp_lv_adapter_pause(-1) in esp_lvgl_adapter.
 */
class LvglOtaLock {
public:
    LvglOtaLock() : locked_(false) {
        // Stop tick timer first, before acquiring the lock.  This prevents
        // the timer ISR from firing while we acquire the lock.
        lvgl_port_stop();
        // Now acquire the mutex — lvgl_port task will block immediately.
        if (lvgl_port_lock(portMAX_DELAY)) {
            locked_ = true;
            ESP_LOGI(TAG, "LVGL fully locked (OTA Phase 2)");
        } else {
            ESP_LOGW(TAG, "LVGL lock failed, resuming timer");
            lvgl_port_resume();
        }
    }

    ~LvglOtaLock() {
        if (locked_) {
            lvgl_port_unlock();
            locked_ = false;
        }
        lvgl_port_resume();
        ESP_LOGI(TAG, "LVGL lock released, timer resumed");
    }

    bool IsLocked() const { return locked_; }

private:
    bool locked_;
};

}  // namespace


Ota::Ota() {
#ifdef ESP_EFUSE_BLOCK_USR_DATA
    // Read Serial Number from efuse user_data
    uint8_t serial_number[33] = {0};
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, serial_number, 32 * 8) == ESP_OK) {
        if (serial_number[0] == 0) {
            has_serial_number_ = false;
        } else {
            serial_number_ = std::string(reinterpret_cast<char*>(serial_number), 32);
            has_serial_number_ = true;
        }
    }
#endif
}

Ota::~Ota() {
}

std::string Ota::GetCheckVersionUrl() {
    Settings settings("wifi", false);
    std::string url = settings.GetString("ota_url");
    if (url.empty()) {
        url = CONFIG_OTA_URL;
    }
    return url;
}

std::unique_ptr<Http> Ota::SetupHttp() {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    auto user_agent = SystemInfo::GetUserAgent();
    http->SetHeader("Activation-Version", has_serial_number_ ? "2" : "1");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", board.GetUuid());
    if (has_serial_number_) {
        http->SetHeader("Serial-Number", serial_number_.c_str());
        ESP_LOGI(TAG, "Setup HTTP, User-Agent: %s, Serial-Number: %s", user_agent.c_str(), serial_number_.c_str());
    }
    http->SetHeader("User-Agent", user_agent);
    http->SetHeader("Accept-Language", Lang::CODE);
    http->SetHeader("Content-Type", "application/json");

    return http;
}

/* 
 * Specification: https://ccnphfhqs21z.feishu.cn/wiki/FjW6wZmisimNBBkov6OcmfvknVd
 */
esp_err_t Ota::CheckVersion() {
    auto& board = Board::GetInstance();
    auto app_desc = esp_app_get_description();

    // Check if there is a new firmware version available
    current_version_ = app_desc->version;
    ESP_LOGI(TAG, "Current version: %s", current_version_.c_str());

    std::string url = GetCheckVersionUrl();
    if (url.length() < 10) {
        ESP_LOGE(TAG, "Check version URL is not properly set");
        return ESP_ERR_INVALID_ARG;
    }

    auto http = SetupHttp();

    std::string data = board.GetSystemInfoJson();
    std::string method = data.length() > 0 ? "POST" : "GET";
    http->SetContent(std::move(data));

    if (!http->Open(method, url)) {
        int last_error = http->GetLastError();
        ESP_LOGE(TAG, "Failed to open HTTP connection, code=0x%x", last_error);
        return last_error;
    }

    auto status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to check version, status code: %d", status_code);
        return status_code;
    }

    data = http->ReadAll();
    http->Close();

    // Response: { "firmware": { "version": "1.0.0", "url": "http://" } }
    // Parse the JSON response and check if the version is newer
    // If it is, set has_new_version_ to true and store the new version and URL
    
    cJSON *root = cJSON_Parse(data.c_str());
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    has_activation_code_ = false;
    has_activation_challenge_ = false;
    cJSON *activation = cJSON_GetObjectItem(root, "activation");
    if (cJSON_IsObject(activation)) {
        cJSON* message = cJSON_GetObjectItem(activation, "message");
        if (cJSON_IsString(message)) {
            activation_message_ = message->valuestring;
        }
        cJSON* code = cJSON_GetObjectItem(activation, "code");
        if (cJSON_IsString(code)) {
            activation_code_ = code->valuestring;
            has_activation_code_ = true;
        }
        cJSON* challenge = cJSON_GetObjectItem(activation, "challenge");
        if (cJSON_IsString(challenge)) {
            activation_challenge_ = challenge->valuestring;
            has_activation_challenge_ = true;
        }
        cJSON* timeout_ms = cJSON_GetObjectItem(activation, "timeout_ms");
        if (cJSON_IsNumber(timeout_ms)) {
            activation_timeout_ms_ = timeout_ms->valueint;
        }
    }

    has_mqtt_config_ = false;
    cJSON *mqtt = cJSON_GetObjectItem(root, "mqtt");
    if (cJSON_IsObject(mqtt)) {
        Settings settings("mqtt", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, mqtt) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (settings.GetInt(item->string) != item->valueint) {
                    settings.SetInt(item->string, item->valueint);
                }
            }
        }
        has_mqtt_config_ = true;
    } else {
        ESP_LOGI(TAG, "No mqtt section found !");
    }

    has_websocket_config_ = false;
    cJSON *websocket = cJSON_GetObjectItem(root, "websocket");
    if (cJSON_IsObject(websocket)) {
        Settings settings("websocket", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, websocket) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (settings.GetInt(item->string) != item->valueint) {
                    settings.SetInt(item->string, item->valueint);
                }
            }
        }
        has_websocket_config_ = true;
    } else {
        ESP_LOGI(TAG, "No websocket section found!");
    }

    has_server_time_ = false;
    cJSON *server_time = cJSON_GetObjectItem(root, "server_time");
    if (cJSON_IsObject(server_time)) {
        cJSON *timestamp = cJSON_GetObjectItem(server_time, "timestamp");
        cJSON *timezone_offset = cJSON_GetObjectItem(server_time, "timezone_offset");
        
        if (cJSON_IsNumber(timestamp)) {
            // 设置系统时间
            struct timeval tv;
            double ts = timestamp->valuedouble;
            
            // 如果有时区偏移，计算本地时间
            if (cJSON_IsNumber(timezone_offset)) {
                ts += (timezone_offset->valueint * 60 * 1000); // 转换分钟为毫秒
            }
            
            tv.tv_sec = (time_t)(ts / 1000);  // 转换毫秒为秒
            tv.tv_usec = (suseconds_t)((long long)ts % 1000) * 1000;  // 剩余的毫秒转换为微秒
            settimeofday(&tv, NULL);
            has_server_time_ = true;
        }
    } else {
        ESP_LOGW(TAG, "No server_time section found!");
    }

    has_new_version_ = false;
    cJSON *firmware = cJSON_GetObjectItem(root, "firmware");
    if (cJSON_IsObject(firmware)) {
        cJSON *version = cJSON_GetObjectItem(firmware, "version");
        if (cJSON_IsString(version)) {
            firmware_version_ = version->valuestring;
        }
        cJSON *url = cJSON_GetObjectItem(firmware, "url");
        if (cJSON_IsString(url)) {
            firmware_url_ = url->valuestring;
        }

        if (cJSON_IsString(version) && cJSON_IsString(url)) {
            // Check if the version is newer, for example, 0.1.0 is newer than 0.0.1
            has_new_version_ = IsNewVersionAvailable(current_version_, firmware_version_);
            if (has_new_version_) {
                ESP_LOGI(TAG, "New version available: %s", firmware_version_.c_str());
            } else {
                ESP_LOGI(TAG, "Current is the latest version");
            }
            // If the force flag is set to 1, the given version is forced to be installed
            cJSON *force = cJSON_GetObjectItem(firmware, "force");
            if (cJSON_IsNumber(force) && force->valueint == 1) {
                has_new_version_ = true;
            }
        }
    } else {
        ESP_LOGW(TAG, "No firmware section found!");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

void Ota::MarkCurrentVersionValid() {
    auto partition = esp_ota_get_running_partition();
    if (strcmp(partition->label, "factory") == 0) {
        ESP_LOGI(TAG, "Running from factory partition, skipping");
        return;
    }

    ESP_LOGI(TAG, "Running partition: %s", partition->label);
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get state of partition");
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking firmware as valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

bool Ota::Upgrade(const std::string& firmware_url, OtaProgressCallback callback) {
    ESP_LOGI(TAG, "Upgrading firmware from %s", firmware_url.c_str());
    esp_ota_handle_t update_handle = 0;
    auto update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get update partition");
        return false;
    }

    ESP_LOGI(TAG, "Writing to partition %s at offset 0x%lx", update_partition->label, update_partition->address);

    // Layout: esp_image_header(24) + esp_image_segment_header(8) + esp_app_desc(128) = 160
    constexpr size_t kAppDescOffset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    constexpr size_t kMinHeaderSize = kAppDescOffset + sizeof(esp_app_desc_t);

    bool image_header_checked = false;
    char pending_buf[1024];
    size_t pending_len = 0;

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    if (!http->Open("GET", firmware_url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return false;
    }

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to get firmware, status code: %d", http->GetStatusCode());
        return false;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length == 0) {
        ESP_LOGE(TAG, "Failed to get content length");
        return false;
    }

    // Phase 0: download header.  512-byte stack buffer — short SPI flash burst per write.
    char buffer[512];
    size_t total_read = 0, recent_read = 0;
    auto last_calc_time = esp_timer_get_time();

    while (true) {
        int ret = http->Read(buffer, sizeof(buffer));
        if (ret < 0) {
            ESP_LOGE(TAG, "Failed to read HTTP data: %s", esp_err_to_name(ret));
            return false;
        }

        recent_read += ret;
        total_read += ret;

        // Report progress every second or on last chunk.
        // Phase 0: lv_timer_handler runs normally, lv_async_call works → progress updates.
        if (esp_timer_get_time() - last_calc_time >= 1000000 || ret == 0) {
            size_t progress = content_length > 0 ? total_read * 100 / content_length : 0;
            ESP_LOGI(TAG, "Progress: %u%% (%u/%u), Speed: %uB/s",
                     progress, (unsigned)total_read, (unsigned)content_length, (unsigned)recent_read);
            if (callback) {
                callback(progress, total_read, content_length, recent_read);
            }
            last_calc_time = esp_timer_get_time();
            recent_read = 0;
        }

        if (ret == 0) {
            break;
        }

        // ── Phase 0: accumulate header bytes until esp_app_desc is complete ──
        if (!image_header_checked) {
            if (pending_len + static_cast<size_t>(ret) > sizeof(pending_buf)) {
                ESP_LOGE(TAG, "Pending buffer overflow: %u + %d", (unsigned)pending_len, ret);
                return false;
            }
            memcpy(pending_buf + pending_len, buffer, ret);
            pending_len += ret;

            if (pending_len >= (int)kMinHeaderSize) {
                esp_app_desc_t new_app_info;
                memcpy(&new_app_info, pending_buf + kAppDescOffset, sizeof(esp_app_desc_t));

                auto current_version = esp_app_get_description()->version;
                ESP_LOGI(TAG, "Current version: %s, New version: %s",
                         current_version, new_app_info.version);

                // ── Phase 1: stop LVGL tick timer ──
                // We stop the tick timer here (no lv_timer_handler call — that
                // would deadlock if a DSI flush is in-flight). The timer stays
                // stopped until LvglOtaLock takes over.
                ESP_LOGI(TAG, "Stopping LVGL tick timer (OTA Phase 1)...");
                LvglQueueDrain drain;

                // ── Phase 2: acquire LVGL lock ──
                // LvglOtaLock stops the tick timer (idempotent), then acquires the
                // LVGL mutex with portMAX_DELAY. lv_timer_handler() is blocked for
                // the entire flash-write period — no DSI DMA during esp_ota_write.
                LvglOtaLock lock;
                if (!lock.IsLocked()) {
                    ESP_LOGE(TAG, "Failed to acquire LVGL lock for Phase 2");
                    return false;
                }

                esp_err_t begin_err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
                if (begin_err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to begin OTA: %s", esp_err_to_name(begin_err));
                    return false;
                }

                // Write header to flash
                auto err = esp_ota_write(update_handle, pending_buf, pending_len);
                pending_len = 0;
                if (err != ESP_OK) {
                    esp_ota_abort(update_handle);
                    ESP_LOGE(TAG, "Failed to write OTA header data: %s", esp_err_to_name(err));
                    return false;
                }
                image_header_checked = true;
                // lock stays in scope — LVGL fully paused for remaining flash writes
            }
            continue;
        }

        // ── Phase 2 (continued): write each HTTP chunk to flash ──
        // lock is still in scope — LVGL mutex is held, timer is stopped.
        auto err = esp_ota_write(update_handle, buffer, ret);
        if (err != ESP_OK) {
            esp_ota_abort(update_handle);
            ESP_LOGE(TAG, "Failed to write OTA data: %s", esp_err_to_name(err));
            return false;
        }
    }
    http->Close();  // lock exits scope here → mutex released, timer resumed

    esp_err_t err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        } else {
            ESP_LOGE(TAG, "Failed to end OTA: %s", esp_err_to_name(err));
        }
        return false;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Firmware upgrade successful");
    return true;
}

bool Ota::StartUpgrade(OtaProgressCallback callback) {
    return Upgrade(firmware_url_, std::move(callback));
}


std::vector<int> Ota::ParseVersion(const std::string& version) {
    std::vector<int> versionNumbers;
    std::stringstream ss(version);
    std::string segment;
    
    while (std::getline(ss, segment, '.')) {
        versionNumbers.push_back(std::stoi(segment));
    }
    
    return versionNumbers;
}

bool Ota::IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion) {
    std::vector<int> current = ParseVersion(currentVersion);
    std::vector<int> newer = ParseVersion(newVersion);
    
    for (size_t i = 0; i < std::min(current.size(), newer.size()); ++i) {
        if (newer[i] > current[i]) {
            return true;
        } else if (newer[i] < current[i]) {
            return false;
        }
    }
    
    return newer.size() > current.size();
}

std::string Ota::GetActivationPayload() {
    if (!has_serial_number_) {
        return "{}";
    }

    std::string hmac_hex;
#ifdef SOC_HMAC_SUPPORTED
    uint8_t hmac_result[32]; // SHA-256 输出为32字节
    
    // 使用Key0计算HMAC
    esp_err_t ret = esp_hmac_calculate(HMAC_KEY0, (uint8_t*)activation_challenge_.data(), activation_challenge_.size(), hmac_result);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HMAC calculation failed: %s", esp_err_to_name(ret));
        return "{}";
    }

    for (size_t i = 0; i < sizeof(hmac_result); i++) {
        char buffer[3];
        sprintf(buffer, "%02x", hmac_result[i]);
        hmac_hex += buffer;
    }
#endif

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "algorithm", "hmac-sha256");
    cJSON_AddStringToObject(payload, "serial_number", serial_number_.c_str());
    cJSON_AddStringToObject(payload, "challenge", activation_challenge_.c_str());
    cJSON_AddStringToObject(payload, "hmac", hmac_hex.c_str());
    auto json_str = cJSON_PrintUnformatted(payload);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(payload);

    ESP_LOGI(TAG, "Activation payload: %s", json.c_str());
    return json;
}

esp_err_t Ota::Activate() {
    if (!has_activation_challenge_) {
        ESP_LOGW(TAG, "No activation challenge found");
        return ESP_FAIL;
    }

    std::string url = GetCheckVersionUrl();
    if (url.back() != '/') {
        url += "/activate";
    } else {
        url += "activate";
    }

    auto http = SetupHttp();

    std::string data = GetActivationPayload();
    http->SetContent(std::move(data));

    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return ESP_FAIL;
    }
    
    auto status_code = http->GetStatusCode();
    if (status_code == 202) {
        return ESP_ERR_TIMEOUT;
    }
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to activate, code: %d, body: %s", status_code, http->ReadAll().c_str());
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Activation successful");
    return ESP_OK;
}
