#include "application.h"
#include "board.h"
#include "display.h"
#include "upgrade_screen.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"
#include "utils/md5.h"
#include "boards/common/mjpeg_player.h"

#include <cstring>
#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>
#include <sys/stat.h>
#include <dirent.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <unistd.h>

#define TAG "Application"


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
    if (tts_stop_grace_timer_ != nullptr) {
        esp_timer_stop(tts_stop_grace_timer_);
        esp_timer_delete(tts_stop_grace_timer_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
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
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
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
                if (ShouldDropWakeStageAudio()) {
                    continue;
                }
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
        // Mark as first boot after BluFi if coming from wifi_configuring state
        if (state == kDeviceStateWifiConfiguring) {
            first_boot_after_blufi_ = true;
            ESP_LOGI(TAG, "Network connected after BluFi provisioning, will reboot after OTA check");
        }
        
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
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
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
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");
    display->SetSystemReady();

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

    // Initialize the protocol
    InitializeProtocol();

    // Sync SD-asset files (music background .bin, etc.) from server to SD card
    CheckSDAssetsFiles();

    // Sync emotion files (idle/listen/speak MJPEG) from server to SD card
    CheckEmotionFiles();

    // 拉取设备信息，加载自定义唤醒词拼音
    CheckDeviceWakeWord();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckDeviceWakeWord() {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    if (network == nullptr) {
        ESP_LOGW(TAG, "No network, skip wake word fetch");
        return;
    }

    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        ESP_LOGE(TAG, "Failed to create HTTP for device info");
        return;
    }

    std::string mac = SystemInfo::GetMacAddress();
    std::string uuid = board.GetUuid();
    std::string url = "https://ai.fanfuture.cn/api/device/info?hardware_id=" + mac;
    http->SetHeader("Device-Id", mac.c_str());
    http->SetHeader("Client-Id", uuid.c_str());

    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open device info: %s", url.c_str());
        return;
    }
    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Device info HTTP %d", http->GetStatusCode());
        http->Close();
        return;
    }

    std::string response;
    char buffer[512];
    int read;
    while ((read = http->Read(buffer, sizeof(buffer))) > 0) {
        response.append(buffer, read);
    }
    http->Close();

    cJSON* root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse device info JSON");
        return;
    }

    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsObject(data)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Device info missing data object");
        return;
    }

    cJSON* command_item = cJSON_GetObjectItem(data, "assistant_command");
    cJSON* name_item = cJSON_GetObjectItem(data, "assistant_name");
    std::string command = cJSON_IsString(command_item) ? command_item->valuestring : "";
    std::string text = cJSON_IsString(name_item) ? name_item->valuestring : "";
    cJSON_Delete(root);

    if (command.empty()) {
        ESP_LOGW(TAG, "Device info has empty assistant_command");
        return;
    }

    ESP_LOGI(TAG, "Device wake word: %s (%s)", command.c_str(), text.c_str());
    audio_service_.UpdateCustomWakeWord(command, text);
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

        // Reboot after first OTA check following BluFi provisioning
        if (first_boot_after_blufi_) {
            ESP_LOGI(TAG, "First OTA check after BluFi completed, rebooting device...");
            vTaskDelay(pdMS_TO_TICKS(1000)); // Wait 1 second before reboot
            esp_restart();
        }

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

// =====================================================
// 用户表情文件同步（开机阶段下载到 SD 卡）
// =====================================================

static const char* kEmotionTag = "EmotionSync";
static constexpr const char* kEmotionDir = "/sdcard/Emotion";

/**
 * 根据表情类型和分辨率生成本地保存路径
 * 格式: /sdcard/Emotion/{type}-{width}x{height}.mjpeg
 */
static std::string MakeEmotionLocalPath(const std::string& type, int width, int height) {
    char suffix[64];
    snprintf(suffix, sizeof(suffix), "-%dx%d.mjpeg", width, height);
    return std::string(kEmotionDir) + "/" + type + suffix;
}

static bool EnsureEmotionDir() {
    struct stat st = {};
    if (stat(kEmotionDir, &st) == 0 && S_ISDIR(st.st_mode)) {
        return true;
    }
    if (mkdir(kEmotionDir, 0755) != 0 && errno != EEXIST) {
        return false;
    }
    return stat(kEmotionDir, &st) == 0 && S_ISDIR(st.st_mode);
}

void Application::CheckEmotionFiles() {
    if (ota_ == nullptr) {
        return;
    }

    auto display = Board::GetInstance().GetDisplay();

    if (!EnsureEmotionDir()) {
        ESP_LOGW(kEmotionTag, "Emotion directory unavailable, skip sync");
        return;
    }

    // 获取服务器表情列表
    EmotionFetchResult fetch_result;
    if (ota_->HasMqttConfig()) {
        MqttProtocol probe;
        fetch_result = probe.FetchDeviceEmotions();
    } else if (ota_->HasWebsocketConfig()) {
        WebsocketProtocol probe;
        fetch_result = probe.FetchDeviceEmotions();
    } else {
        MqttProtocol probe;
        fetch_result = probe.FetchDeviceEmotions();
    }

    // HTTP 请求失败，跳过同步，保留本地文件
    if (!fetch_result.success) {
        ESP_LOGW(kEmotionTag, "Failed to fetch emotion list from server, preserving local files");
        return;
    }

    ESP_LOGI(kEmotionTag, "Fetched %d emotion(s) from server", (int)fetch_result.emotions.size());

    // 服务器返回空列表（用户已删除所有角色动画），清三个角色文件
    if (fetch_result.emotions.empty()) {
        // 构造三个角色文件的本地路径，确保即使服务器没有，CleanOrphan 也能找到并删除旧的
        int w = display ? display->width() : 0;
        int h = display ? display->height() : 0;
        if (w <= 0) w = 240;
        if (h <= 0) h = 290;
        std::vector<std::string> role_only = {
            MakeEmotionLocalPath("idle", w, h),
            MakeEmotionLocalPath("listen", w, h),
            MakeEmotionLocalPath("speak", w, h),
        };
        CleanOrphanEmotionFiles(role_only);
        ESP_LOGI(kEmotionTag, "No role emotions on server, cleared role files only");
        return;
    }

    std::vector<EmotionInfo>& emotions = fetch_result.emotions;

    // 对比服务器列表与SD卡，同步表情
    std::vector<std::string> valid_paths;
    int download_count = 0;
    int total_count = 0;

    for (auto& info : emotions) {
        if (info.type.empty() || info.url.empty()) {
            continue;
        }
        int width = info.width > 0 ? info.width : 240;
        int height = info.height > 0 ? info.height : 290;
        info.local_path = MakeEmotionLocalPath(info.type, width, height);

        // 检查本地是否已存在且MD5匹配
        struct stat st = {};
        if (stat(info.local_path.c_str(), &st) == 0 && st.st_size > 0 && st.st_size == (off_t)info.size) {
            std::string local_md5 = MD5::Calculate(info.local_path);
            if (!local_md5.empty() && local_md5 == info.hash) {
                ESP_LOGI(kEmotionTag, "Emotion %s is up-to-date, skip", info.type.c_str());
                valid_paths.push_back(info.local_path);  // 已有文件也要标记为有效，防止清理时被删
                continue;  // 已是最新，跳过
            }
        }

        // 需要下载
        total_count++;
    }

    // 如果没有需要下载的，直接清理角色动画同步（服务器没有的角色要删掉）
    if (total_count == 0) {
        ESP_LOGI(kEmotionTag, "All emotions are up-to-date, checking role sync");
        CleanOrphanEmotionFiles(valid_paths);
        return;
    }

    // 显示全屏下载界面
    UpgradeScreen::Show(Lang::Strings::DOWNLOADING_EMOTIONS, "");
    
    SetDeviceState(kDeviceStateUpgrading);

    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(500));

    // 重新遍历，实际下载
    download_count = 0;
    // 累计下载字节, 用于驱动 UI 进度条
    size_t bytes_done_total = 0;
    size_t bytes_total_sum = 0;
    size_t latest_speed_bps = 0;
    // 先按 info.size 算出待下载总字节（已存在的跳过）
    for (const auto& info : emotions) {
        if (info.type.empty() || info.url.empty()) continue;
        int width = info.width > 0 ? info.width : 240;
        int height = info.height > 0 ? info.height : 290;
        std::string local_path = MakeEmotionLocalPath(info.type, width, height);
        struct stat st = {};
        if (stat(local_path.c_str(), &st) == 0 && st.st_size > 0 && st.st_size == (off_t)info.size) {
            std::string local_md5 = MD5::Calculate(local_path);
            if (!local_md5.empty() && local_md5 == info.hash) {
                continue;
            }
        }
        bytes_total_sum += static_cast<size_t>(info.size);
    }

    for (auto& info : emotions) {
        if (info.type.empty() || info.url.empty()) {
            continue;
        }
        int width = info.width > 0 ? info.width : 240;
        int height = info.height > 0 ? info.height : 290;
        info.local_path = MakeEmotionLocalPath(info.type, width, height);

        // 检查本地是否已存在且MD5匹配
        struct stat st = {};
        if (stat(info.local_path.c_str(), &st) == 0 && st.st_size > 0 && st.st_size == (off_t)info.size) {
            std::string local_md5 = MD5::Calculate(info.local_path);
            if (!local_md5.empty() && local_md5 == info.hash) {
                valid_paths.push_back(info.local_path);  // 已有文件也要标记为有效
                continue;  // 已是最新，跳过
            }
        }

        // 需要下载
        download_count++;
        char progress_msg[64];
        snprintf(progress_msg, sizeof(progress_msg), Lang::Strings::DOWNLOADING_EMOTION_PROGRESS, download_count, total_count);
        UpgradeScreen::SetStatusMessage(progress_msg);

        auto per_file_cb = [&](size_t got_bytes, size_t file_total, size_t speed_bps) {
            latest_speed_bps = speed_bps;
            // 若 Content-Length 不可知, 用 expected_size 补上
            size_t known_total = file_total > 0 ? file_total : static_cast<size_t>(info.size);
            size_t display_done = bytes_done_total + got_bytes;
            size_t display_total = bytes_total_sum;
            int progress = total_count > 0 ? (download_count * 100 / total_count) : 100;
            if (display_total > 0) {
                int byte_pct = static_cast<int>(display_done * 100ULL / display_total);
                if (byte_pct < progress) progress = byte_pct;
            }
            (void)known_total;
            UpgradeScreen::Update(progress, display_done, display_total, speed_bps);
        };

        size_t file_start_bytes = bytes_done_total;
        ProcessEmotionFile(info, per_file_cb);
        // 把这个文件实际下载的字节数加到累计里
        struct stat st2 = {};
        if (stat(info.local_path.c_str(), &st2) == 0) {
            bytes_done_total = file_start_bytes + static_cast<size_t>(st2.st_size);
        }
        valid_paths.push_back(info.local_path);

        int progress = total_count > 0 ? (download_count * 100 / total_count) : 100;
        UpgradeScreen::Update(progress, bytes_done_total, bytes_total_sum, latest_speed_bps);
    }

    // 清理孤儿文件
    CleanOrphanEmotionFiles(valid_paths);

    // 下载完成
    UpgradeScreen::Dismiss();
    audio_service_.Start();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    display->SetStatus(Lang::Strings::EMOTION_SYNC_COMPLETE);
    display->SetChatMessage("system", Lang::Strings::EMOTION_SYNC_COMPLETE);
    vTaskDelay(pdMS_TO_TICKS(500));
}

void Application::ProcessEmotionFile(const EmotionInfo& info, DownloadProgressCallback progress_cb) {
    const std::string& local = info.local_path;

    // 检查本地是否已存在
    struct stat st = {};
    if (stat(local.c_str(), &st) == 0 && st.st_size > 0 && st.st_size == (off_t)info.size) {
        std::string local_md5 = MD5::Calculate(local);
        if (!local_md5.empty() && local_md5 == info.hash) {
            ESP_LOGI(kEmotionTag, "Emotion %s is up-to-date", local.c_str());
            return;
        }
    }

    if (stat(local.c_str(), &st) == 0) {
        unlink(local.c_str());
    }

    if (!DownloadEmotionFile(info.url, local, info.size, progress_cb)) {
        ESP_LOGE(kEmotionTag, "Failed to download emotion: %s", info.url.c_str());
        return;
    }

    std::string final_md5 = MD5::Calculate(local);
    if (!final_md5.empty() && !info.hash.empty() && final_md5 != info.hash) {
        ESP_LOGE(kEmotionTag, "MD5 mismatch for %s", local.c_str());
        unlink(local.c_str());
        return;
    }

    ESP_LOGI(kEmotionTag, "Emotion ready: %s", local.c_str());
}

bool Application::DownloadEmotionFile(const std::string& url, const std::string& local_path,
                                       size_t expected_size, DownloadProgressCallback progress_cb) {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);

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

    // Content-Length 优先, 没有则用 expected_size
    size_t content_length = expected_size;
    {
        std::string cl = http->GetResponseHeader("Content-Length");
        if (!cl.empty()) {
            char* end = nullptr;
            unsigned long long v = strtoull(cl.c_str(), &end, 10);
            if (end != cl.c_str() && v > 0) {
                content_length = static_cast<size_t>(v);
            }
        }
        if (content_length == 0) {
            size_t bl = http->GetBodyLength();
            if (bl > 0) content_length = bl;
        }
    }

    // 先下载到 .tmp 再 rename，避免断电/中断导致损坏
    std::string tmp_path = local_path + ".tmp";
    FILE* fp = fopen(tmp_path.c_str(), "wb");
    if (!fp) {
        ESP_LOGE(kEmotionTag, "Failed to open file: %s", tmp_path.c_str());
        http->Close();
        return false;
    }

    char buffer[4096];
    int read;
    size_t total = 0;
    int64_t last_report_us = esp_timer_get_time();
    size_t last_report_bytes = 0;
    size_t current_speed_bps = 0;
    while ((read = http->Read(buffer, sizeof(buffer))) > 0) {
        if (fwrite(buffer, 1, read, fp) != (size_t)read) {
            ESP_LOGE(kEmotionTag, "Write failed");
            fclose(fp);
            unlink(tmp_path.c_str());
            http->Close();
            return false;
        }
        total += read;

        if (progress_cb) {
            const int64_t now_us = esp_timer_get_time();
            const int64_t delta_us = now_us - last_report_us;
            if (delta_us >= 250000) {
                const size_t delta_bytes = total - last_report_bytes;
                current_speed_bps = static_cast<size_t>(
                    (static_cast<uint64_t>(delta_bytes) * 1000000ULL) / delta_us);
                last_report_us = now_us;
                last_report_bytes = total;
                progress_cb(total, content_length, current_speed_bps);
            }
        }
    }
    fclose(fp);
    http->Close();

    if (progress_cb) {
        progress_cb(total, content_length, current_speed_bps);
    }

    if (expected_size > 0 && total != expected_size) {
        ESP_LOGE(kEmotionTag, "Size mismatch: got %u, expect %u", (unsigned)total, (unsigned)expected_size);
        unlink(tmp_path.c_str());
        return false;
    }

    if (rename(tmp_path.c_str(), local_path.c_str()) != 0) {
        ESP_LOGE(kEmotionTag, "Rename failed: %s -> %s", tmp_path.c_str(), local_path.c_str());
        unlink(tmp_path.c_str());
        return false;
    }

    ESP_LOGI(kEmotionTag, "Downloaded %s: %u bytes", local_path.c_str(), (unsigned)total);
    return true;
}

// =====================================================
// SD 资源文件同步（音乐背景图 .bin 等）
// 与 CheckEmotionFiles 的区别：
//   - 不做 MD5 校验，不做大小的严格校验
//   - 不删除本地文件，只做新增
//   - UI 复用下载表情的提示语
// =====================================================

static const char* kSDAssetsTag = "SDAssetsSync";

/**
 * 同步 SD 卡资源文件（音乐背景图 .bin 等）
 * 流程：
 *   1. 从服务器拉取本机型需要的文件列表（{path, url}）
 *   2. 检查本地 path 是否存在
 *      - 已存在：跳过
 *      - 不存在：下载到对应 path
 *   3. 不会删除本地任何文件
 */
void Application::CheckSDAssetsFiles() {
    if (ota_ == nullptr) {
        return;
    }

    // 通过 HTTP probe MqttProtocol/WebsocketProtocol 拉取 SD 资源列表
    SDAssetsFetchResult fetch_result;
    if (ota_->HasMqttConfig()) {
        MqttProtocol probe;
        fetch_result = probe.FetchDeviceSDAssetsFiles();
    } else if (ota_->HasWebsocketConfig()) {
        WebsocketProtocol probe;
        fetch_result = probe.FetchDeviceSDAssetsFiles();
    } else {
        MqttProtocol probe;
        fetch_result = probe.FetchDeviceSDAssetsFiles();
    }

    if (!fetch_result.success) {
        ESP_LOGW(kSDAssetsTag, "Failed to fetch SD assets list from server, skip sync");
        return;
    }

    if (fetch_result.files.empty()) {
        ESP_LOGI(kSDAssetsTag, "Server returned empty SD assets list, nothing to sync");
        return;
    }

    ESP_LOGI(kSDAssetsTag, "Fetched %d SD asset file(s) from server", (int)fetch_result.files.size());

    // 检查每个文件，本地缺失的下载
    int download_count = 0;
    int total_count = 0;
    auto display = Board::GetInstance().GetDisplay();

    for (const auto& info : fetch_result.files) {
        if (info.path.empty() || info.url.empty()) {
            continue;
        }

        std::string local_path = info.path;
        struct stat st = {};
        if (stat(local_path.c_str(), &st) == 0 && st.st_size > 0) {
            ESP_LOGI(kSDAssetsTag, "SD asset %s already exists (%ld bytes), skip",
                     local_path.c_str(), (long)st.st_size);
            continue;
        }

        total_count++;
    }

    if (total_count == 0) {
        ESP_LOGI(kSDAssetsTag, "All SD assets already present");
        return;
    }

    // 显示全屏下载界面
    UpgradeScreen::Show(Lang::Strings::DOWNLOADING_SD_ASSETS, "");

    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateUpgrading);

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(500));

    // 实际下载
    download_count = 0;
    // 累计下载字节, 用于驱动 UI 进度条
    size_t bytes_done_total = 0;
    size_t bytes_total_sum = 0;
    size_t latest_speed_bps = 0;

    // 预扫一遍, 估算所有要下载的文件大小（HTTP 不可知时用 0）
    for (const auto& info : fetch_result.files) {
        if (info.path.empty() || info.url.empty()) continue;
        std::string local_path = info.path;
        struct stat st = {};
        if (stat(local_path.c_str(), &st) == 0 && st.st_size > 0) continue;
        // 这里无法预知真实大小, 给个经验值（设为 0 表示未知, 不影响进度条计算）
        bytes_total_sum += static_cast<size_t>(info.size > 0 ? info.size : 0);
    }

    for (const auto& info : fetch_result.files) {
        if (info.path.empty() || info.url.empty()) {
            continue;
        }

        std::string local_path = info.path;
        struct stat st = {};
        if (stat(local_path.c_str(), &st) == 0 && st.st_size > 0) {
            continue;  // 已存在
        }

        download_count++;
        char progress_msg[64];
        snprintf(progress_msg, sizeof(progress_msg), Lang::Strings::DOWNLOADING_SD_ASSETS_PROGRESS,
                 download_count, total_count);
        UpgradeScreen::SetStatusMessage(progress_msg);

        // 回调里: 累加已下载字节并刷新 UI
        auto per_file_cb = [&](size_t got_bytes, size_t /*file_total*/, size_t speed_bps) {
            // 这里 got_bytes 是当前文件的累计, 我们用 (bytes_done_total + got_bytes) 作为 UI 显示
            latest_speed_bps = speed_bps;
            size_t display_done = bytes_done_total + got_bytes;
            size_t display_total = bytes_total_sum;
            int progress = total_count > 0 ? (download_count * 100 / total_count) : 100;
            // bytes 维度进度: 当确切知道大小时算字节进度
            if (display_total > 0) {
                int byte_pct = static_cast<int>(display_done * 100ULL / display_total);
                if (byte_pct < progress) progress = byte_pct;
            }
            UpgradeScreen::Update(progress, display_done, display_total, speed_bps);
        };

        if (DownloadSDAssetsFile(info.url, local_path, per_file_cb)) {
            // 把这个文件实际下载的字节数加到累计里
            // 重新 stat 文件以获得真实大小
            struct stat st2 = {};
            if (stat(local_path.c_str(), &st2) == 0) {
                bytes_done_total += static_cast<size_t>(st2.st_size);
            }
            int progress = total_count > 0 ? (download_count * 100 / total_count) : 100;
            UpgradeScreen::Update(progress, bytes_done_total, bytes_total_sum, latest_speed_bps);
            ESP_LOGI(kSDAssetsTag, "SD asset ready: %s", local_path.c_str());
        } else {
            ESP_LOGE(kSDAssetsTag, "Failed to download SD asset: %s -> %s",
                     info.url.c_str(), local_path.c_str());
        }
    }

    UpgradeScreen::Dismiss();
    audio_service_.Start();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    if (display) {
        display->SetStatus(Lang::Strings::SD_ASSETS_SYNC_COMPLETE);
        display->SetChatMessage("system", Lang::Strings::SD_ASSETS_SYNC_COMPLETE);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/**
 * 下载 SD 资源文件到本地路径
 * - 先写到 .tmp 再 rename，保证断电不会损坏
 * - 失败时清理 .tmp
 * - 不做 MD5 校验（按需求保持简单）
 */
bool Application::DownloadSDAssetsFile(const std::string& url, const std::string& local_path,
                                       DownloadProgressCallback progress_cb) {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);

    if (!http->Open("GET", url)) {
        ESP_LOGE(kSDAssetsTag, "Failed to open HTTP: %s", url.c_str());
        return false;
    }

    int status = http->GetStatusCode();
    if (status != 200) {
        ESP_LOGE(kSDAssetsTag, "HTTP status %d for %s", status, url.c_str());
        http->Close();
        return false;
    }

    // 文件总字节数, 不确定时为 0
    size_t content_length = 0;
    {
        std::string cl = http->GetResponseHeader("Content-Length");
        if (!cl.empty()) {
            char* end = nullptr;
            unsigned long long v = strtoull(cl.c_str(), &end, 10);
            if (end != cl.c_str() && v > 0) {
                content_length = static_cast<size_t>(v);
            }
        }
        // 某些实现把长度直接放 GetBodyLength()
        if (content_length == 0) {
            size_t bl = http->GetBodyLength();
            if (bl > 0) content_length = bl;
        }
    }

    // 确保目标目录存在
    auto pos = local_path.find_last_of('/');
    if (pos != std::string::npos && pos > 0) {
        std::string parent_dir = local_path.substr(0, pos);
        struct stat st = {};
        if (stat(parent_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            // 递归创建父目录
            std::string accum;
            for (size_t i = 1; i < parent_dir.size(); ++i) {
                if (parent_dir[i] == '/' || i == parent_dir.size() - 1) {
                    accum = parent_dir.substr(0, i + 1);
                    struct stat sub_st = {};
                    if (stat(accum.c_str(), &sub_st) != 0) {
                        mkdir(accum.c_str(), 0755);
                    }
                }
            }
        }
    }

    // 先写到 .tmp 再 rename
    std::string tmp_path = local_path + ".tmp";
    FILE* fp = fopen(tmp_path.c_str(), "wb");
    if (!fp) {
        ESP_LOGE(kSDAssetsTag, "Failed to open file: %s", tmp_path.c_str());
        http->Close();
        return false;
    }

    char buffer[4096];
    int read;
    size_t total = 0;
    // 网速统计: 每 ~256ms 触发一次回调
    int64_t last_report_us = esp_timer_get_time();
    size_t last_report_bytes = 0;
    size_t current_speed_bps = 0;
    while ((read = http->Read(buffer, sizeof(buffer))) > 0) {
        if (fwrite(buffer, 1, read, fp) != (size_t)read) {
            ESP_LOGE(kSDAssetsTag, "Write failed for %s", tmp_path.c_str());
            fclose(fp);
            unlink(tmp_path.c_str());
            http->Close();
            return false;
        }
        total += read;

        if (progress_cb) {
            const int64_t now_us = esp_timer_get_time();
            const int64_t delta_us = now_us - last_report_us;
            if (delta_us >= 250000) {  // 250ms
                const size_t delta_bytes = total - last_report_bytes;
                current_speed_bps = static_cast<size_t>(
                    (static_cast<uint64_t>(delta_bytes) * 1000000ULL) / delta_us);
                last_report_us = now_us;
                last_report_bytes = total;
                progress_cb(total, content_length, current_speed_bps);
            }
        }
    }
    fclose(fp);
    http->Close();

    // 兜底再汇报一次最终值
    if (progress_cb) {
        progress_cb(total, content_length, current_speed_bps);
    }

    if (total == 0) {
        ESP_LOGE(kSDAssetsTag, "Downloaded 0 bytes for %s", url.c_str());
        unlink(tmp_path.c_str());
        return false;
    }

    if (rename(tmp_path.c_str(), local_path.c_str()) != 0) {
        ESP_LOGE(kSDAssetsTag, "Rename failed: %s -> %s", tmp_path.c_str(), local_path.c_str());
        unlink(tmp_path.c_str());
        return false;
    }

    ESP_LOGI(kSDAssetsTag, "Downloaded %s: %u bytes", local_path.c_str(), (unsigned)total);
    return true;
}

void Application::CleanOrphanEmotionFiles(const std::vector<std::string>& valid_paths) {
    DIR* dir = opendir(kEmotionDir);
    if (!dir) {
        ESP_LOGW(kEmotionTag, "CleanOrphan: cannot open %s", kEmotionDir);
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.empty() || name[0] == '.' || name == "System Volume Information") {
            continue;
        }
        // 只清理角色动画（idle-、listen-、speak-），不碰其他表情文件
        if (name.find(".mjpeg") == std::string::npos) {
            continue;
        }
        if (name.rfind("idle-", 0) != 0 && name.rfind("listen-", 0) != 0 && name.rfind("speak-", 0) != 0) {
            continue;
        }

        std::string full_path = std::string(kEmotionDir) + "/" + name;
        bool is_valid = false;
        for (const auto& p : valid_paths) {
            if (p == full_path) {
                is_valid = true;
                break;
            }
        }
        if (!is_valid) {
            ESP_LOGI(kEmotionTag, "CleanOrphan: removing %s", full_path.c_str());
            unlink(full_path.c_str());
        }
    }
    closedir(dir);
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
        /* 关键修复:tts stop 触发 SetDeviceState(Listening) 后到 grace 窗口结束之前,
         * 仍然接受 UDP 音频帧,确保服务器估算提前导致的末帧不被丢弃。
         * 正常情况下 Listening 收到的包应当是麦克风上传/历史数据,不应当入解码队列。 */
        if (GetDeviceState() == kDeviceStateSpeaking || tts_stop_grace_accept_audio_) {
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
                        /* 关键修复：tts stop (MQTT) 与 UDP 末帧不同通道异步,
                         * stop 可能早到几十~几百 ms 导致末帧被错过 Speaking 状态丢弃。
                         * 开启 400ms "末帧缓冲窗口":窗口期间即使 state 已转 Listening,
                         * OnIncomingAudio 仍然把 UDP 帧入队播放。
                         * 配合之前的 WaitForPlaybackQueueEmpty 保证播放完整。 */
                        audio_service_.WaitForPlaybackQueueEmpty();
                        tts_stop_grace_accept_audio_ = true;
                        if (tts_stop_grace_timer_ == nullptr) {
                            esp_timer_create_args_t grace_args = {
                                .callback = [](void* arg) {
                                    Application* app = (Application*)arg;
                                    app->tts_stop_grace_accept_audio_ = false;
                                },
                                .arg = this,
                                .dispatch_method = ESP_TIMER_TASK,
                                .name = "tts_stop_grace",
                                .skip_unhandled_events = true,
                            };
                            esp_timer_create(&grace_args, &tts_stop_grace_timer_);
                        }
                        esp_timer_stop(tts_stop_grace_timer_);
                        esp_timer_start_once(tts_stop_grace_timer_, 400000);  // 400ms
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
                /* 进入待机后，对话阶段（Listening/Speaking）的角色动画由 application 统一控制，
                 * 不再跟随 LLM 下发的 emotion。LLM 下发的 emotion 在此处跳过即可。 */
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    auto state = GetDeviceState();
                    if (state == kDeviceStateListening || state == kDeviceStateSpeaking) {
                        return;
                    }
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
        play_popup_on_listening_ = true;
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
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    play_popup_on_listening_ = true;
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
        play_popup_on_listening_ = true;
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
        if (music && music->IsPlaying()) {
            music->StopStreaming();
            SetDeviceState(kDeviceStateConnecting);
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
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
    // 只上报唤醒事件。本地缓存的唤醒词 Opus / 尾音不得当聊天发给服务端，
    // 否则相近音节（如「星黎」→「心灵」）会被 ASR 成第二轮对话。
    while (audio_service_.PopWakeWordPacket()) {
    }
    while (audio_service_.PopPacketFromSendQueue()) {
    }
    audio_service_.ClearSendAndEncodeQueues();
    BeginWakeAudioHold();
    protocol_->SendWakeWordDetected(wake_word);
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
}

void Application::BeginWakeAudioHold() {
    hold_wake_audio_upload_ = true;
    hold_wake_audio_until_us_ = esp_timer_get_time() + 1500000;
}

bool Application::ShouldDropWakeStageAudio() {
    if (!hold_wake_audio_upload_) {
        return false;
    }
    if (GetDeviceState() == kDeviceStateSpeaking) {
        hold_wake_audio_upload_ = false;
        return false;
    }
    if (esp_timer_get_time() >= hold_wake_audio_until_us_) {
        hold_wake_audio_upload_ = false;
        return false;
    }
    return true;
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
                display->ShowMusicCover(true, "");
            } else {
                // 非音乐播放：隐藏音乐封面，恢复 idle 角色动画
                display->ShowMusicCover(false, "");
                display->SetStatus(Lang::Strings::STANDBY);
                display->SetRoleAnimation("idle");
            }
            /* 任何回到 idle 的转场都清空字幕(也包括音乐播放路径,
             * 否则播放恢复后仍能看到上一次 AI 句子末段的残留)。 */
            display->ClearChatMessages();
            hold_wake_audio_upload_ = false;
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
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetRoleAnimation("listen");
            /* speak -> listen 转场时清掉上一句 AI 字幕,避免末尾字符残影 */
            display->ClearChatMessages();

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
                if (hold_wake_audio_upload_) {
                    audio_service_.ClearSendAndEncodeQueues();
                }
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
            hold_wake_audio_upload_ = false;
            display->SetStatus(Lang::Strings::SPEAKING);
            display->SetRoleAnimation("speak");

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            /* 复位解码器:进入 Speaking 时主动清空解码器状态,避免上一轮唤醒词/音乐的
             * 残留包混入新一句 TTS 导致第一帧噪声。ResetDecoder 不会清掉正在播放的
             * 任务,只在 idle/connecting→speaking 这条路径上有意义。 */
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
    // 关闭背光,避免重启瞬间显示垃圾数据导致蓝屏闪烁
    if (Backlight* bl = Board::GetInstance().GetBacklight()) {
        bl->SetBrightness(0, false);
    }
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

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "" : version.c_str();

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    // Show upgrade screen — full-screen overlay that replaces all UI during upgrade.
    // Shows title, version, progress bar, bytes, speed, and elapsed time.
    UpgradeScreen::Show(Lang::Strings::OTA_UPGRADE, version_info.c_str());

    SetDeviceState(kDeviceStateUpgrading);

    audio_service_.PlaySound(Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(2000));

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(500));

    // UpgradeScreen::Update dispatches UI updates via lv_async_call, so it is
    // safe to call from the OTA download task (which is not the LVGL task).
    bool upgrade_success = Ota::Upgrade(upgrade_url, [](int progress, size_t downloaded, size_t total, size_t speed) {
        UpgradeScreen::Update(progress, downloaded, total, speed);
    });

    if (!upgrade_success) {
        // Upgrade failed, dismiss upgrade screen and show error
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        UpgradeScreen::Dismiss();
        audio_service_.Start();
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success — show completion on upgrade screen then reboot
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        UpgradeScreen::SetStatusMessage("升级成功, 正在重启...");
        vTaskDelay(pdMS_TO_TICKS(1000));
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
    if (packet.payload.size() < 2) {
        return;
    }
    AddAudioData(reinterpret_cast<int16_t*>(packet.payload.data()),
                  packet.payload.size() / sizeof(int16_t),
                  packet.sample_rate);
}

void Application::AddAudioData(int16_t* pcm, size_t num_samples, int sample_rate)
{
    if (pcm == nullptr || num_samples == 0) {
        return;
    }

    auto codec = Board::GetInstance().GetAudioCodec();
    DeviceState current_state = state_machine_.GetState();
    auto& board = Board::GetInstance();
    auto music = board.GetMusic();
    bool is_music_playing = music && music->IsPlaying();
    if (!is_music_playing || current_state != kDeviceStateIdle || !codec || !codec->output_enabled()) {
        return;
    }

    const int16_t* out_pcm = pcm;
    size_t out_samples = num_samples;

    // 重采样缓冲复用，避免每帧 vector 分配造成 PSRAM 抖动
    thread_local static std::vector<int16_t> resample_buf;

    if (sample_rate != codec->output_sample_rate())
    {
        if (sample_rate <= 0 || codec->output_sample_rate() <= 0)
        {
            ESP_LOGE(TAG, "Invalid sample rates: %d -> %d",
                     sample_rate, codec->output_sample_rate());
            return;
        }

        const int src_rate = sample_rate;
        const int dst_rate = codec->output_sample_rate();

        if (src_rate > dst_rate)
        {
            size_t expected_size = (num_samples * (size_t)dst_rate + (size_t)src_rate / 2) / (size_t)src_rate;
            if (expected_size == 0) {
                expected_size = 1;
            }
            resample_buf.resize(expected_size);
            for (size_t i = 0; i < expected_size; i++) {
                size_t idx = (i * (size_t)src_rate) / (size_t)dst_rate;
                if (idx >= num_samples) {
                    idx = num_samples - 1;
                }
                resample_buf[i] = pcm[idx];
            }
        }
        else
        {
            float upsample_ratio = (float)dst_rate / (float)src_rate;
            size_t expected_size = (size_t)(num_samples * upsample_ratio + 0.5f);
            resample_buf.clear();
            resample_buf.reserve(expected_size);
            int interpolation_count = (int)upsample_ratio - 1;
            for (size_t i = 0; i < num_samples; ++i)
            {
                resample_buf.push_back(pcm[i]);
                if (interpolation_count > 0 && i + 1 < num_samples)
                {
                    int16_t current = pcm[i];
                    int16_t next = pcm[i + 1];
                    for (int j = 1; j <= interpolation_count; ++j)
                    {
                        float t = (float)j / (interpolation_count + 1);
                        resample_buf.push_back((int16_t)(current + (next - current) * t));
                    }
                }
                else if (interpolation_count > 0)
                {
                    for (int j = 1; j <= interpolation_count; ++j)
                    {
                        resample_buf.push_back(pcm[i]);
                    }
                }
            }
        }
        out_pcm = resample_buf.data();
        out_samples = resample_buf.size();
    }

    if (!codec->output_enabled())
    {
        codec->EnableOutput(true);
    }

    codec->OutputData(out_pcm, (int)out_samples);
    audio_service_.UpdateOutputTimestamp();
}

