#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"
#include "utils/md5.h"
#include "utils/emotion_partition_storage.h"
#include "boards/common/mjpeg_player.h"

#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#define TAG "Application"


// =====================================================
// 用户表情文件同步（开机阶段下载到 flash emotions 分区）
// =====================================================

static const char* kEmotionTag = "EmotionSync";

/**
 * 把表情 type + 分辨率打包成 flash 资产名（写入 emotion_partition_storage 用）。
 * 形如 "idle-240x290.mjpeg"。
 */
static std::string MakeEmotionAssetName(const std::string& type, int width, int height) {
    char suffix[64];
    snprintf(suffix, sizeof(suffix), "-%dx%d.mjpeg", width, height);
    return type + suffix;
}

/**
 * 兼容旧代码：返回 asset_name（与 MakeEmotionAssetName 同义）。
 * 旧版返回 "/sdcard/Emotion/<type>-<W>x<H>.mjpeg"，新版等价于纯文件名。
 */
static std::string MakeEmotionLocalPath(const std::string& type, int width, int height) {
    return MakeEmotionAssetName(type, width, height);
}

/** 检查 flash 中文件存在且大小匹配；如需 MD5 校验可读取整文件 */
static bool IsEmotionFileUpToDate(const std::string& asset_name, size_t expected_size,
                                  const std::string& expected_md5) {
    uint32_t off = 0, sz = 0;
    if (!emotion_partition_storage_find(asset_name.c_str(), &off, &sz)) return false;
    if (expected_size > 0 && sz != expected_size) return false;
    if (expected_md5.empty()) return true;
    /* 需要 MD5 校验：读出整个文件到 RAM 算 MD5 */
    if (sz > 4 * 1024 * 1024) {
        ESP_LOGW(kEmotionTag, "%s 太大 (%u) 跳过 MD5 校验", asset_name.c_str(), (unsigned)sz);
        return true;
    }
    uint8_t* buf = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (uint8_t*)malloc(sz);
    if (!buf) {
        ESP_LOGW(kEmotionTag, "%s MD5 校验内存不足，跳过", asset_name.c_str());
        return true;
    }
    esp_err_t er = emotion_partition_storage_read(asset_name.c_str(), buf, sz, NULL);
    bool ok = false;
    if (er == ESP_OK) {
        std::string md5 = MD5::Calculate(buf, sz);
        ok = (!md5.empty() && md5 == expected_md5);
    }
    free(buf);
    return ok;
}

/**
 * 老的 EnsureEmotionDir 仍保留以兼容其它代码（SD 卡仍可能使用）。
 * 但 emotion sync 不再依赖 SD 卡，所以返回 true 直接放行。
 */
static bool EnsureEmotionDir() {
    /* 确保 emotion_partition_storage 已初始化 */
    if (emotion_partition_storage_get_partition() == NULL) {
        esp_err_t r = emotion_partition_storage_init();
        if (r != ESP_OK) {
            ESP_LOGE(kEmotionTag, "emotion_partition_storage_init 失败: %s", esp_err_to_name(r));
            return false;
        }
    }
    return true;
}

void Application::CheckEmotionFiles() {
    if (ota_ == nullptr) {
        return;
    }

    if (!EnsureEmotionDir()) {
        ESP_LOGW(kEmotionTag, "Emotion directory unavailable, skip sync");
        return;
    }

    // 获取服务器表情列表
    EmotionFetchResult fetch_result;
    /* 用堆分配避免栈帧过大（MqttProtocol/WebsocketProtocol 内部有较大缓冲） */
    std::unique_ptr<MqttProtocol> mqtt_probe;
    std::unique_ptr<WebsocketProtocol> ws_probe;
    if (ota_->HasMqttConfig()) {
        mqtt_probe = std::make_unique<MqttProtocol>();
        fetch_result = mqtt_probe->FetchDeviceEmotions();
    } else if (ota_->HasWebsocketConfig()) {
        ws_probe = std::make_unique<WebsocketProtocol>();
        fetch_result = ws_probe->FetchDeviceEmotions();
    } else {
        mqtt_probe = std::make_unique<MqttProtocol>();
        fetch_result = mqtt_probe->FetchDeviceEmotions();
    }
    /* 显式释放：probe 在 activation task 中持有，protocol 可能占内存 */
    mqtt_probe.reset();
    ws_probe.reset();

    // HTTP 请求失败，跳过同步，保留本地文件
    if (!fetch_result.success) {
        ESP_LOGW(kEmotionTag, "Failed to fetch emotion list from server, preserving local files");
        return;
    }

    ESP_LOGI(kEmotionTag, "Fetched %d emotion(s) from server", (int)fetch_result.emotions.size());

    // 服务器返回空列表（用户已删除所有角色动画），清三个角色文件
    if (fetch_result.emotions.empty()) {
        auto display = Board::GetInstance().GetDisplay();
        int w = display ? display->width() : 0;
        int h = display ? display->height() : 0;
        if (w <= 0) w = 240;
        if (h <= 0) h = 290;
        std::vector<std::string> role_only_asset = {
            MakeEmotionAssetName("idle", w, h),
            MakeEmotionAssetName("listen", w, h),
            MakeEmotionAssetName("speak", w, h),
        };
        CleanOrphanEmotionFiles(role_only_asset);
        ESP_LOGI(kEmotionTag, "No role emotions on server, cleared role files only (flash)");
        return;
    }

    std::vector<EmotionInfo>& emotions = fetch_result.emotions;

    // 对比服务器列表与 flash 资产，同步表情
    std::vector<std::string> valid_asset_names;
    int download_count = 0;
    int total_count = 0;

    for (auto& info : emotions) {
        if (info.type.empty() || info.url.empty()) {
            continue;
        }
        int width = info.width > 0 ? info.width : 240;
        int height = info.height > 0 ? info.height : 290;
        info.local_path = MakeEmotionLocalPath(info.type, width, height);
        info.asset_name = MakeEmotionAssetName(info.type, width, height);

        // 检查本地 flash 中是否已存在且大小/MD5匹配
        if (IsEmotionFileUpToDate(info.asset_name, info.size, info.hash)) {
            ESP_LOGI(kEmotionTag, "Emotion %s is up-to-date, skip", info.type.c_str());
            valid_asset_names.push_back(info.asset_name);
            continue;
        }

        // 需要下载
        total_count++;
    }

    if (total_count == 0) {
        ESP_LOGI(kEmotionTag, "All emotions are up-to-date, checking orphan files in flash");
        CleanOrphanEmotionFiles(valid_asset_names);
        return;
    }

    // 确有需要下载的，显示下载提示
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(Lang::Strings::DOWNLOADING_EMOTIONS);
    display->SetEmotion("download");
    vTaskDelay(pdMS_TO_TICKS(2000));

    // 重新遍历，实际下载
    download_count = 0;
    for (auto& info : emotions) {
        if (info.type.empty() || info.url.empty()) {
            continue;
        }
        int width = info.width > 0 ? info.width : 240;
        int height = info.height > 0 ? info.height : 290;
        info.local_path = MakeEmotionLocalPath(info.type, width, height);
        info.asset_name = MakeEmotionAssetName(info.type, width, height);

        // 再次检查（避免多线程竞态）
        if (IsEmotionFileUpToDate(info.asset_name, info.size, info.hash)) {
            valid_asset_names.push_back(info.asset_name);
            continue;
        }

        // 需要下载
        download_count++;
        char progress_msg[64];
        snprintf(progress_msg, sizeof(progress_msg), Lang::Strings::DOWNLOADING_EMOTION_PROGRESS, download_count, total_count);
        display->SetChatMessage("system", progress_msg);

        ProcessEmotionFile(info);
        valid_asset_names.push_back(info.asset_name);
    }

    CleanOrphanEmotionFiles(valid_asset_names);

    //display->SetStatus(Lang::Strings::EMOTION_SYNC_COMPLETE);
    //display->SetChatMessage("system", Lang::Strings::EMOTION_SYNC_COMPLETE);
    vTaskDelay(pdMS_TO_TICKS(500));
}

bool Application::DownloadEmotionFile(const std::string& url, const std::string& asset_name,
                                       size_t expected_size) {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    http->SetTimeout(15000);

    if (!http->Open("GET", url)) {
        ESP_LOGE(kEmotionTag, "Failed to open HTTP: %s", url.c_str());
        return false;
    }

    int status = http->GetStatusCode();
    if (status != 200) {
        ESP_LOGE(kEmotionTag, "HTTP status %d for %s", status, url.c_str());
        http->Close();
        return false;
    }

    /* 一次性把整个 mjpeg 读到 PSRAM（典型 ≤2MB，可接受）。
     * 失败再退回到 64KB 分块流式缓冲。 */
    size_t initial_cap = expected_size > 0 ? expected_size : (2 * 1024 * 1024);
    if (initial_cap > 8 * 1024 * 1024) initial_cap = 8 * 1024 * 1024;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(initial_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (uint8_t*)malloc(initial_cap);
    if (!buf) {
        ESP_LOGE(kEmotionTag, "无法分配 %u 字节缓冲", (unsigned)initial_cap);
        http->Close();
        return false;
    }
    size_t total = 0;
    size_t cap = initial_cap;
    char chunk[4096];
    int got;
    while ((got = http->Read(chunk, sizeof(chunk))) > 0) {
        if (total + (size_t)got > cap) {
            size_t new_cap = cap * 2;
            if (new_cap > 8 * 1024 * 1024) new_cap = 8 * 1024 * 1024;
            if (total + (size_t)got > new_cap) new_cap = total + (size_t)got;
            uint8_t* nb = (uint8_t*)heap_caps_malloc(new_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!nb) nb = (uint8_t*)malloc(new_cap);
            if (!nb) {
                ESP_LOGE(kEmotionTag, "缓冲扩展失败 (need %u)", (unsigned)new_cap);
                free(buf);
                http->Close();
                return false;
            }
            memcpy(nb, buf, total);
            free(buf);
            buf = nb;
            cap = new_cap;
        }
        memcpy(buf + total, chunk, got);
        total += (size_t)got;
    }
    http->Close();

    if (expected_size > 0 && total != expected_size) {
        ESP_LOGE(kEmotionTag, "Size mismatch: got %u, expect %u", (unsigned)total, (unsigned)expected_size);
        free(buf);
        return false;
    }

    esp_err_t er = emotion_partition_storage_upsert(asset_name.c_str(), buf, total);
    free(buf);
    if (er != ESP_OK) {
        ESP_LOGE(kEmotionTag, "emotion_partition_storage_upsert(%s) 失败: %s",
                 asset_name.c_str(), esp_err_to_name(er));
        return false;
    }

    ESP_LOGI(kEmotionTag, "已写入 flash 资产: %s (%u bytes)", asset_name.c_str(), (unsigned)total);
    return true;
}

void Application::ProcessEmotionFile(const EmotionInfo& info) {
    const std::string& asset_name = info.asset_name;
    const std::string& url = info.url;

    // 检查 flash 中是否已存在且 MD5 匹配
    if (IsEmotionFileUpToDate(asset_name, info.size, info.hash)) {
        ESP_LOGI(kEmotionTag, "Emotion %s is up-to-date", asset_name.c_str());
        return;
    }

    if (!DownloadEmotionFile(url, asset_name, info.size)) {
        ESP_LOGE(kEmotionTag, "Failed to download emotion: %s", url.c_str());
        return;
    }

    /* 二次 MD5 校验（写入后读回再算） */
    if (!info.hash.empty()) {
        uint32_t off = 0, sz = 0;
        if (emotion_partition_storage_find(asset_name.c_str(), &off, &sz)
            && sz > 0 && sz <= 4 * 1024 * 1024) {
            uint8_t* buf = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!buf) buf = (uint8_t*)malloc(sz);
            if (buf) {
                if (emotion_partition_storage_read(asset_name.c_str(), buf, sz, NULL) == ESP_OK) {
                    std::string md5 = MD5::Calculate(buf, sz);
                    if (!md5.empty() && md5 != info.hash) {
                        ESP_LOGE(kEmotionTag, "MD5 mismatch for %s (写入 flash 后校验失败)，标记删除待重下",
                                 asset_name.c_str());
                        emotion_partition_storage_delete(asset_name.c_str());
                    }
                }
                free(buf);
            }
        }
    }

    ESP_LOGI(kEmotionTag, "Emotion ready (flash): %s", asset_name.c_str());
}

/**
 * 删除 flash asset 表中不在 valid_asset_names 列表里的"角色动画"文件。
 * 只删除 idle-/listen-/speak- 前缀的文件；default-* 等不出现在这里的不会被删。
 *
 * 不再操作 SD 卡（项目已不再用 SD 存表情）。
 */
static void CleanOrphanEmotionFiles_Cb(const char* name, uint32_t offset, uint32_t size, void* user);
struct OrphanCtx {
    const std::vector<std::string>* valid;
    int deleted = 0;
    int kept = 0;
};

static bool IsRoleAnimationName(const char* name) {
    if (!name) return false;
    return (strncmp(name, "idle-", 5) == 0
         || strncmp(name, "listen-", 7) == 0
         || strncmp(name, "speak-", 6) == 0);
}

static void CleanOrphanEmotionFiles_Cb(const char* name, uint32_t offset, uint32_t size, void* user) {
    (void)offset; (void)size;
    OrphanCtx* ctx = static_cast<OrphanCtx*>(user);
    if (!IsRoleAnimationName(name)) {
        ctx->kept++;
        return;
    }
    for (const auto& v : *ctx->valid) {
        if (v == name) {
            ctx->kept++;
            return;
        }
    }
    /* 不在 valid 列表 → 标记删除（不立即擦 flash） */
    esp_err_t er = emotion_partition_storage_delete(name);
    if (er == ESP_OK) {
        ESP_LOGI(kEmotionTag, "CleanOrphan: 标记删除 %s", name);
        ctx->deleted++;
    } else if (er == ESP_ERR_NOT_FOUND) {
        ctx->kept++;
    } else {
        ESP_LOGE(kEmotionTag, "CleanOrphan: 删除 %s 失败: %s", name, esp_err_to_name(er));
    }
}

void Application::CleanOrphanEmotionFiles(const std::vector<std::string>& valid_asset_names) {
    if (emotion_partition_storage_get_partition() == NULL) {
        esp_err_t r = emotion_partition_storage_init();
        if (r != ESP_OK) {
            ESP_LOGW(kEmotionTag, "CleanOrphan: emotion_partition_storage 不可用，跳过");
            return;
        }
    }
    OrphanCtx ctx{&valid_asset_names, 0, 0};
    emotion_partition_storage_enum(CleanOrphanEmotionFiles_Cb, &ctx);
    ESP_LOGI(kEmotionTag, "CleanOrphan 完成: 删 %d 个, 保留 %d 个", ctx.deleted, ctx.kept);
}

Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    // Setup display UI first, so subsequent state/status notifications are visible.
    auto display = board.GetDisplay();
    display->SetupUI();

    SetDeviceState(kDeviceStateStarting);
    // Print board name/version info
    //display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    if(!s_system_ready_) {
                        // WiFi or cellular with carrier info
                        std::string msg = Lang::Strings::CONNECT_TO;
                        msg += data;
                        msg += "...";
                        display->ShowNotification(msg.c_str(), 30000);
                    }
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                //msg += data;
                //display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS = 
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 4, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    //display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");
    display->SetSystemReady();
    s_system_ready_ = true;

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    Schedule([this]() {
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    // Sync emotion files from server
    CheckEmotionFiles();

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            Schedule([display, message = std::string(buffer)]() {
                display->SetChatMessage("system", message.c_str());
            });
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this, display]() {
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            display->SetChatMessage("system", "");
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    
    protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetRoleAnimation("idle");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        // 进入 listening 前必须彻底停止音乐播放（包括释放内存、flush codec output）。
        // 否则 play_thread 会持续往 codec 写音频，导致：
        // 1. TTS 输出与音乐重叠
        // 2. 麦克风拾取残余音乐被 STT 误识别
        // 3. 关闭音频通道回到 idle 后，play_thread 检测到 state==idle 又继续播放
        auto& board = Board::GetInstance();
        auto music = board.GetMusic();
        if (music && music->IsPlaying()) {
            music->StopStreaming();
        }

        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() {
                ContinueOpenAudioChannel(mode);
            });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            return;
        }
    }

    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (state == kDeviceStateIdle) {
        // 进入 listening 前必须彻底停止音乐播放（参见 HandleToggleChatEvent 注释）
        auto& board = Board::GetInstance();
        auto music = board.GetMusic();
        if (music && music->IsPlaying()) {
            music->StopStreaming();
        }

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() {
                ContinueOpenAudioChannel(kListeningModeManualStop);
            });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        // 先同步停止音乐播放（唤醒词打断）
        auto& board = Board::GetInstance();
        auto music = board.GetMusic();
        if (music) {
            music->StopStreaming();
        }

        audio_service_.EncodeWakeWord();
        auto wake_word = audio_service_.GetLastWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update),
            // then continue with OpenAudioChannel which may block for ~1 second
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        SetDeviceState(kDeviceStateConnecting);
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue());

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // Play popup sound and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            audio_service_.EnableWakeWordDetection(true);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // 先发送唤醒词元数据，让服务器知道接下来是唤醒词音频
    // 否则服务器可能先把音频发给ASR识别，导致误识别
    protocol_->SendWakeWordDetected(wake_word);
    // 然后发送唤醒词音频数据
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    SetListeningMode(GetDefaultListeningMode());
#else
    // Set flag to play popup sound after state changes to listening
    // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();

    // 检查当前是否在播放音乐
    auto music = board.GetMusic();
    bool is_music_playing = music && music->IsPlaying();
    
    switch (new_state) {
        case kDeviceStateUnknown:
            /* 系统启动阶段，不走角色动画——开机/下载/告警仍由 SetEmotion 走主题 GIF / 内置图标 */
            break;
        case kDeviceStateIdle:
            if (is_music_playing) {
                // 音乐播放中：显示音乐封面（黑色背景 + 专辑图），停止 MJPEG 动画
                display->SetStatus(Lang::Strings::MUSIC_PLAYING);
                display->ShowMusicCover(true);
            } else {
                // 非音乐播放：隐藏音乐封面，恢复 idle 角色动画
                display->ShowMusicCover(false, "");
                display->SetStatus(Lang::Strings::STANDBY);
                display->ClearChatMessages();
                display->SetRoleAnimation("idle");
            }
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            // 主动确保 codec input 已启用（唤醒词检测需要录音）
            // 这对于音乐播放中尤其重要，避免 wake word 收不到数据
            {
                auto codec = Board::GetInstance().GetAudioCodec();
                if (codec && !codec->input_enabled()) {
                    codec->EnableInput(true);
                }
            }
            break;
        case kDeviceStateConnecting:
            if(!s_system_ready_)
            {
                display->SetStatus(Lang::Strings::CONNECTING);
                display->SetEmotion("neutral");
                display->SetChatMessage("system", "");
            }
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetRoleAnimation("listen");

            // Make sure the audio processor is running
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                // For auto mode, wait for playback queue to be empty before enabling voice processing
                // This prevents audio truncation when STOP arrives late due to network jitter
                if (listening_mode_ == kListeningModeAutoStop) {
                    audio_service_.WaitForPlaybackQueueEmpty();
                }
                
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
            }

#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
            // Enable wake word detection in listening mode (configured via Kconfig)
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
            // Disable wake word detection in listening mode
            audio_service_.EnableWakeWordDetection(false);
#endif
            
            // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);
            display->SetRoleAnimation("speak");

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload = std::move(payload)]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::ResetProtocol() {
    Schedule([this]() {
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}

// 新增：接收外部音频数据（如音乐播放）
void Application::AddAudioData(AudioStreamPacket &&packet)
{
    auto codec = Board::GetInstance().GetAudioCodec();
    DeviceState current_state = state_machine_.GetState();
    // 仅在 idle 状态 + 音乐正在播放时输出音频
    auto& board = Board::GetInstance();
    auto music = board.GetMusic();
    bool is_music_playing = music && music->IsPlaying();
    if (is_music_playing && current_state == kDeviceStateIdle && codec->output_enabled())
    {
        // packet.payload包含的是原始PCM数据（int16_t）
        if (packet.payload.size() >= 2)
        {
            size_t num_samples = packet.payload.size() / sizeof(int16_t);
            std::vector<int16_t> pcm_data(num_samples);
            memcpy(pcm_data.data(), packet.payload.data(), packet.payload.size());

            // 检查采样率是否匹配，如果不匹配则进行简单重采样
            if (packet.sample_rate != codec->output_sample_rate())
            {
                // 验证采样率参数
                if (packet.sample_rate <= 0 || codec->output_sample_rate() <= 0)
                {
                    ESP_LOGE(TAG, "Invalid sample rates: %d -> %d",
                             packet.sample_rate, codec->output_sample_rate());
                    return;
                }

                std::vector<int16_t> resampled;

                ESP_LOGD(TAG, "Music Player: Resample from %d Hz to %d Hz (avoid I2S reconfig)",
                         packet.sample_rate, codec->output_sample_rate());

                // 关键修复：不要调用 SetOutputSampleRate 切换采样率，
                // 因为会 disable+reconfig I2S TX，影响共享 I2S bus 的 RX（麦克风），
                // 导致语音唤醒失效。改为在软件层做重采样。
                if (packet.sample_rate > codec->output_sample_rate())
                {
                    // 下采样到 codec 当前采样率
                    float downsample_ratio = static_cast<float>(packet.sample_rate) / codec->output_sample_rate();
                    size_t expected_size = static_cast<size_t>(pcm_data.size() / downsample_ratio + 0.5f);
                    resampled.resize(expected_size);

                    size_t resampled_index = 0;
                    float source_index = 0.0f;
                    for (size_t i = 0; i < pcm_data.size() && resampled_index < expected_size; i++)
                    {
                        size_t idx = static_cast<size_t>(source_index);
                        if (idx < pcm_data.size())
                        {
                            resampled[resampled_index++] = pcm_data[idx];
                        }
                        source_index += downsample_ratio;
                    }

                    pcm_data = std::move(resampled);
                    ESP_LOGD(TAG, "Downsampled music audio from %d to %d Hz",
                             packet.sample_rate, codec->output_sample_rate());
                }
                else
                {
                    // 上采样到 codec 当前采样率
                    float upsample_ratio = codec->output_sample_rate() / static_cast<float>(packet.sample_rate);
                    size_t expected_size = static_cast<size_t>(pcm_data.size() * upsample_ratio + 0.5f);
                    resampled.reserve(expected_size);

                    for (size_t i = 0; i < pcm_data.size(); ++i)
                    {
                        resampled.push_back(pcm_data[i]);

                        int interpolation_count = static_cast<int>(upsample_ratio) - 1;
                        if (interpolation_count > 0 && i + 1 < pcm_data.size())
                        {
                            int16_t current = pcm_data[i];
                            int16_t next = pcm_data[i + 1];
                            for (int j = 1; j <= interpolation_count; ++j)
                            {
                                float t = static_cast<float>(j) / (interpolation_count + 1);
                                int16_t interpolated = static_cast<int16_t>(current + (next - current) * t);
                                resampled.push_back(interpolated);
                            }
                        }
                        else if (interpolation_count > 0)
                        {
                            for (int j = 1; j <= interpolation_count; ++j)
                            {
                                resampled.push_back(pcm_data[i]);
                            }
                        }
                    }

                    pcm_data = std::move(resampled);
                    ESP_LOGD(TAG, "Upsampled music audio from %d to %d Hz",
                             packet.sample_rate, codec->output_sample_rate());
                }
            }

            // 确保音频输出已启用
            if (!codec->output_enabled())
            {
                codec->EnableOutput(true);
            }

            // 发送PCM数据到音频编解码器
            codec->OutputData(pcm_data);

            audio_service_.UpdateOutputTimestamp();
        }
    }
}

