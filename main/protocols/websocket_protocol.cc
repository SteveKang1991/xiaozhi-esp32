#include "websocket_protocol.h"
#include "board.h"
#include "system_info.h"
#include "application.h"
#include "settings.h"

#include <cstring>
#include <cJSON.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include "assets/lang_config.h"

#define TAG "WS"

WebsocketProtocol::WebsocketProtocol() {
    event_group_handle_ = xEventGroupCreate();
}

WebsocketProtocol::~WebsocketProtocol() {
    vEventGroupDelete(event_group_handle_);
}

bool WebsocketProtocol::Start() {
    // Only connect to server when audio channel is needed
    return true;
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (version_ == 2) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol2) + packet->payload.size());
        auto bp2 = (BinaryProtocol2*)serialized.data();
        bp2->version = htons(version_);
        bp2->type = 0;
        bp2->reserved = 0;
        bp2->timestamp = htonl(packet->timestamp);
        bp2->payload_size = htonl(packet->payload.size());
        memcpy(bp2->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else if (version_ == 3) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol3) + packet->payload.size());
        auto bp3 = (BinaryProtocol3*)serialized.data();
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons(packet->payload.size());
        memcpy(bp3->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else {
        return websocket_->Send(packet->payload.data(), packet->payload.size(), true);
    }
}

bool WebsocketProtocol::SendText(const std::string& text) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (!websocket_->Send(text)) {
        ESP_LOGE(TAG, "Failed to send text: %s", text.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    return true;
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

EmotionFetchResult WebsocketProtocol::FetchDeviceEmotions() {
    EmotionFetchResult result;

    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);

    std::string mac = SystemInfo::GetMacAddress();
    std::string url = "https://ai.fanfuture.cn/api/device/emotions?hardware_id=" + mac;

    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP for emotion list");
        return result;  // success = false
    }

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "HTTP status %d for emotion list", http->GetStatusCode());
        http->Close();
        return result;  // success = false
    }

    std::string response;
    char buffer[1024];
    int read;
    while ((read = http->Read(buffer, sizeof(buffer))) > 0) {
        response.append(buffer, read);
    }
    http->Close();

    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse emotion list JSON");
        return result;  // success = false
    }

    cJSON* data = cJSON_GetObjectItem(root, "data");
    cJSON* arr = data ? cJSON_GetObjectItem(data, "emotions") : nullptr;
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        result.success = true;  // 请求成功，但列表为空
        return result;
    }

    result.success = true;  // 标记为成功

    int count = cJSON_GetArraySize(arr);
    for (int i = 0; i < count; i++) {
        cJSON* item = cJSON_GetArrayItem(arr, i);
        EmotionInfo info;
        cJSON* t = cJSON_GetObjectItem(item, "type");
        cJSON* u = cJSON_GetObjectItem(item, "url");
        cJSON* h = cJSON_GetObjectItem(item, "hash");
        cJSON* s = cJSON_GetObjectItem(item, "size");
        cJSON* w = cJSON_GetObjectItem(item, "width");
        cJSON* ht = cJSON_GetObjectItem(item, "height");
        
        if (cJSON_IsString(t)) info.type = t->valuestring;
        if (cJSON_IsString(u)) info.url = u->valuestring;
        if (cJSON_IsString(h)) info.hash = h->valuestring;
        if (cJSON_IsNumber(s)) info.size = (size_t)s->valuedouble;
        if (cJSON_IsNumber(w)) info.width = (int)w->valueint;
        if (cJSON_IsNumber(ht)) info.height = (int)ht->valueint;
        result.emotions.push_back(std::move(info));
    }

    cJSON_Delete(root);
    return result;
}

SDAssetsFetchResult WebsocketProtocol::FetchDeviceSDAssetsFiles() {
    SDAssetsFetchResult result;

    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);

    // 固件端同时传 MAC 和板子型号：
    //   - hardware_id : 设备 MAC（保留字段，后端当前不用，便于以后做按设备差异化）
    //   - boardAgent  : 编译期板子型号，后端真正按它返回文件列表
    std::string board_agent = std::string(BOARD_TYPE);
    std::string mac = SystemInfo::GetMacAddress();
    std::string url = "https://ai.fanfuture.cn/api/device/sd_assets?boardAgent=" + board_agent
                    + "&hardware_id=" + mac;

    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP for SD assets list");
        return result;
    }

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "HTTP status %d for SD assets list", http->GetStatusCode());
        http->Close();
        return result;
    }

    std::string response;
    char buffer[1024];
    int read;
    while ((read = http->Read(buffer, sizeof(buffer))) > 0) {
        response.append(buffer, read);
    }
    http->Close();

    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse SD assets JSON");
        return result;
    }

    cJSON* data = cJSON_GetObjectItem(root, "data");
    cJSON* arr = data ? cJSON_GetObjectItem(data, "files") : nullptr;
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        result.success = true;
        return result;
    }

    result.success = true;

    int count = cJSON_GetArraySize(arr);
    for (int i = 0; i < count; i++) {
        cJSON* item = cJSON_GetArrayItem(arr, i);
        SDAssetsFileInfo info;
        cJSON* p = cJSON_GetObjectItem(item, "path");
        cJSON* u = cJSON_GetObjectItem(item, "url");
        cJSON* s = cJSON_GetObjectItem(item, "size");
        if (cJSON_IsString(p)) info.path = p->valuestring;
        if (cJSON_IsString(u)) info.url = u->valuestring;
        if (cJSON_IsNumber(s)) info.size = (size_t)s->valuedouble;
        if (!info.path.empty() && !info.url.empty()) {
            result.files.push_back(std::move(info));
        }
    }

    cJSON_Delete(root);
    return result;
}

void WebsocketProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;  // Websocket doesn't need to send goodbye message
    websocket_.reset();
}

bool WebsocketProtocol::OpenAudioChannel() {
    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    std::string token = settings.GetString("token");
    int version = settings.GetInt("version");
    if (version != 0) {
        version_ = version;
    }

    error_occurred_ = false;

    auto network = Board::GetInstance().GetNetwork();
    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    if (!token.empty()) {
        // If token not has a space, add "Bearer " prefix
        if (token.find(" ") == std::string::npos) {
            token = "Bearer " + token;
        }
        websocket_->SetHeader("Authorization", token.c_str());
    }
    websocket_->SetHeader("Protocol-Version", std::to_string(version_).c_str());
    websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            if (on_incoming_audio_ != nullptr) {
                if (version_ == 2) {
                    BinaryProtocol2* bp2 = (BinaryProtocol2*)data;
                    bp2->version = ntohs(bp2->version);
                    bp2->type = ntohs(bp2->type);
                    bp2->timestamp = ntohl(bp2->timestamp);
                    bp2->payload_size = ntohl(bp2->payload_size);
                    auto payload = (uint8_t*)bp2->payload;
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = bp2->timestamp,
                        .payload = std::vector<uint8_t>(payload, payload + bp2->payload_size)
                    }));
                } else if (version_ == 3) {
                    BinaryProtocol3* bp3 = (BinaryProtocol3*)data;
                    bp3->type = bp3->type;
                    bp3->payload_size = ntohs(bp3->payload_size);
                    auto payload = (uint8_t*)bp3->payload;
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>(payload, payload + bp3->payload_size)
                    }));
                } else {
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + len)
                    }));
                }
            }
        } else {
            // Parse JSON data
            auto root = cJSON_ParseWithLength(data, len);
            auto type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type)) {
                if (strcmp(type->valuestring, "hello") == 0) {
                    ParseServerHello(root);
                } else {
                    if (on_incoming_json_ != nullptr) {
                        on_incoming_json_(root);
                    }
                }
            } else {
                ESP_LOGE(TAG, "Missing message type, data: %s", std::string(data, len).c_str());
            }
            cJSON_Delete(root);
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Websocket disconnected");
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });

    ESP_LOGI(TAG, "Connecting to websocket server: %s with version: %d", url.c_str(), version_);
    if (!websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to websocket server, code=%d", websocket_->GetLastError());
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    // Send hello message to describe the client
    auto message = GetHelloMessage();
    if (!SendText(message)) {
        return false;
    }

    // Wait for server hello
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }

    return true;
}

std::string WebsocketProtocol::GetHelloMessage() {
    // keys: message type, version, audio_params (format, sample_rate, channels)
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version_);
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void WebsocketProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (transport == nullptr || strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring);
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }

    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}
