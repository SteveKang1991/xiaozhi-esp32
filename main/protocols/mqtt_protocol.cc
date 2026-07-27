#include "mqtt_protocol.h"
#include "board.h"
#include "application.h"
#include "settings.h"

#include <esp_log.h>
#include <cstring>
#include <arpa/inet.h>
#include "assets/lang_config.h"
#include "system_info.h"

#define TAG "MQTT"

MqttProtocol::MqttProtocol() {
    event_group_handle_ = xEventGroupCreate();

    // Initialize reconnect timer
    esp_timer_create_args_t reconnect_timer_args = {
        .callback = [](void* arg) {
            MqttProtocol* protocol = (MqttProtocol*)arg;
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                ESP_LOGI(TAG, "Reconnecting to MQTT server");
                auto alive = protocol->alive_;  // Capture alive flag
                app.Schedule([protocol, alive]() {
                    if (*alive) {
                        protocol->StartMqttClient(false);
                    }
                });
            }
        },
        .arg = this,
    };
    esp_timer_create(&reconnect_timer_args, &reconnect_timer_);
}

MqttProtocol::~MqttProtocol() {
    ESP_LOGI(TAG, "MqttProtocol deinit");
    
    // Mark as dead first to prevent any pending scheduled tasks from executing
    *alive_ = false;
    
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
        esp_timer_delete(reconnect_timer_);
    }

    udp_.reset();
    mqtt_.reset();
    
    if (event_group_handle_ != nullptr) {
        vEventGroupDelete(event_group_handle_);
    }
}

bool MqttProtocol::Start() {
    return StartMqttClient(false);
}

bool MqttProtocol::StartMqttClient(bool report_error) {
    if (mqtt_ != nullptr) {
        ESP_LOGW(TAG, "Mqtt client already started");
        mqtt_.reset();
    }

    Settings settings("mqtt", false);
    auto endpoint = settings.GetString("endpoint");
    auto client_id = settings.GetString("client_id");
    auto username = settings.GetString("username");
    auto password = settings.GetString("password");
    int keepalive_interval = settings.GetInt("keepalive", 240);
    publish_topic_ = settings.GetString("publish_topic");
    subscribe_topic_= settings.GetString("subscribe_topic");
    //endpoint = "192.168.0.105";
    if (endpoint.empty()) {
        ESP_LOGW(TAG, "MQTT endpoint is not specified");
        if (report_error) {
            SetError(Lang::Strings::SERVER_NOT_FOUND);
        }
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    mqtt_ = network->CreateMqtt(0);
    mqtt_->SetKeepAlive(keepalive_interval);

    mqtt_->OnDisconnected([this]() {
        if (on_disconnected_ != nullptr) {
            on_disconnected_();
        }
        ESP_LOGI(TAG, "MQTT disconnected, schedule reconnect in %d seconds", MQTT_RECONNECT_INTERVAL_MS / 1000);
        esp_timer_start_once(reconnect_timer_, MQTT_RECONNECT_INTERVAL_MS * 1000);
    });

    mqtt_->OnConnected([this]() {
        if (on_connected_ != nullptr) {
            on_connected_();
        }
        esp_timer_stop(reconnect_timer_);
    });

    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        cJSON* root = cJSON_Parse(payload.c_str());
        //ESP_LOGE(TAG, "Failed to parse json message %s", payload.c_str());
        if (root == nullptr) {
            ESP_LOGE(TAG, "Failed to parse json message %s", payload.c_str());
            return;
        }
        cJSON* type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGE(TAG, "Message type is invalid");
            cJSON_Delete(root);
            return;
        }

        if (strcmp(type->valuestring, "hello") == 0) {
            ParseServerHello(root);
        } else if (strcmp(type->valuestring, "goodbye") == 0) {
            auto session_id = cJSON_GetObjectItem(root, "session_id");
            ESP_LOGI(TAG, "Received goodbye message, session_id: %s", session_id ? session_id->valuestring : "null");
            if (session_id == nullptr || session_id_ == session_id->valuestring) {
                auto alive = alive_;  // Capture alive flag
                Application::GetInstance().Schedule([this, alive]() {
                    if (*alive) {
                        // Server initiated goodbye, don't send goodbye back to avoid ping-pong
                        CloseAudioChannel(false);
                    }
                });
            }
        } else if (on_incoming_json_ != nullptr) {
            on_incoming_json_(root);
        }
        cJSON_Delete(root);
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    ESP_LOGI(TAG, "Connecting to endpoint %s", endpoint.c_str());
    std::string broker_address;
    int broker_port = 8883;
    size_t pos = endpoint.find(':');
    if (pos != std::string::npos) {
        broker_address = endpoint.substr(0, pos);
        broker_port = std::stoi(endpoint.substr(pos + 1));
    } else {
        broker_address = endpoint;
    }
    if (!mqtt_->Connect(broker_address, 1883, client_id, username, password)) {
        ESP_LOGE(TAG, "Failed to connect to endpoint, code=%d", mqtt_->GetLastError());
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }
    mqtt_->Subscribe(subscribe_topic_.c_str(), 0);
    SendText(R"({"session_id":"3144dff0","type":"goodbye"})");
    //发布一个暂停的
    ESP_LOGI(TAG, "Connected to endpoint");
    return true;
}

bool MqttProtocol::SendText(const std::string& text) {

    if (publish_topic_.empty()) {
        return false;
    }

    std::string device_id = SystemInfo::GetMacAddress();
    std::string payload;
    if (text.back() == '}') {
        payload = text.substr(0, text.size() - 1) + 
                 ",\"device_id\":\"" + device_id + "\"}";
    } else{
        payload = text;
    }


    if (!mqtt_->Publish(publish_topic_, payload)) {
        ESP_LOGE(TAG, "Failed to publish message");
        if (on_network_error_ != nullptr) {
            on_network_error_(Lang::Strings::SERVER_ERROR);
        }
        return false;
    }

        //ESP_LOGI(TAG, "申请消息通道 topic %s  payload %s",publish_topic_.c_str(),payload.c_str());

    return true;
  
}

bool MqttProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (udp_ == nullptr) {
        return false;
    }

    // 将 vector<unsigned char> 转换为 string
    std::string macAddress =  SystemInfo::GetMacAddress();
     
    std::string macPrefix = "MAC:" + macAddress + "|";
    std::string raw_data = macPrefix + std::string(packet->payload.begin(), packet->payload.end());

    return udp_->Send(raw_data)>0;
}


void MqttProtocol::CloseAudioChannel(bool send_goodbye) {
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        udp_.reset();
    }

    ESP_LOGI(TAG, "Closing audio channel, send_goodbye: %d", send_goodbye);

    // Only send goodbye when client initiates the close
    // Don't send if server already sent goodbye (to avoid ping-pong)
    if (send_goodbye) {
        std::string message = "{";
        message += "\"session_id\":\"" + session_id_ + "\",";
        message += "\"type\":\"goodbye\"";
        message += "}";
        SendText(message);
    }

    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

bool MqttProtocol::OpenAudioChannel() {
    if (mqtt_ == nullptr || !mqtt_->IsConnected()) {
        ESP_LOGI(TAG, "MQTT is not connected, try to connect now");
        if (!StartMqttClient(true)) {
            return false;
        }
    }

    error_occurred_ = false;
    session_id_ = "";
    xEventGroupClearBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);

    auto message = GetHelloMessage();
    if (!SendText(message)) {
        return false;
    }

    // 等待服务器响应
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & MQTT_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    std::lock_guard<std::mutex> lock(channel_mutex_);
    auto network = Board::GetInstance().GetNetwork();
    udp_ = network->CreateUdp(2);
    
    udp_->OnMessage([this](const std::string& data) {
        // 数据包大小校验
        if (data.size() < 16) {
            return;
        }
        // 解析包头
        uint32_t sample_rate = ntohl(*(uint32_t*)&data[0]);
        uint32_t frame_duration = ntohl(*(uint32_t*)&data[4]);
        uint32_t timestamp = ntohl(*(uint32_t*)&data[8]);
        uint32_t sequence = ntohl(*(uint32_t*)&data[12]);
        
        // 创建数据包
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = sample_rate;
        packet->frame_duration = frame_duration;
        packet->timestamp = timestamp;
        // 提取payload（跳过16字节包头）
        packet->payload.assign(data.begin()+16, data.end());
        
        // 空payload检查
        if (packet->payload.empty()) {
            return;
        }
        
        // 调用回调
        if (on_incoming_audio_ != nullptr) {
            on_incoming_audio_(std::move(packet));
        }
        
        // 更新状态
        remote_sequence_ = sequence;
        last_incoming_time_ = std::chrono::steady_clock::now();
    });


    udp_->Connect(udp_server_, udp_port_);

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

std::string MqttProtocol::GetHelloMessage() {
    // 发送 hello 消息申请 UDP 通道
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 3);
    cJSON_AddStringToObject(root, "transport", "udp");
    cJSON_AddStringToObject(root, "device_id",SystemInfo::GetMacAddress().c_str());
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
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

void MqttProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (transport == nullptr || strcmp(transport->valuestring, "udp") != 0) {
        ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring);
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    // Get sample rate from hello message
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

    auto udp = cJSON_GetObjectItem(root, "udp");
    if (!cJSON_IsObject(udp)) {
        ESP_LOGE(TAG, "UDP is not specified");
        return;
    }
    udp_server_ = cJSON_GetObjectItem(udp, "server")->valuestring;
    udp_port_ = cJSON_GetObjectItem(udp, "port")->valueint;
    auto key = cJSON_GetObjectItem(udp, "key")->valuestring;
    auto nonce = cJSON_GetObjectItem(udp, "nonce")->valuestring;

    auto encryption = cJSON_GetObjectItem(udp, "encryption")->valuestring;
    ESP_LOGI(TAG, "UDP server: %s, port: %d, encryption: %s", udp_server_.c_str(), udp_port_, encryption);
    aes_nonce_ = DecodeHexString(nonce);
    mbedtls_aes_init(&aes_ctx_);
    mbedtls_aes_setkey_enc(&aes_ctx_, (const unsigned char*)DecodeHexString(key).c_str(), 128);
    local_sequence_ = 0;
    remote_sequence_ = 0;
    xEventGroupSetBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);
}

static const char hex_chars[] = "0123456789ABCDEF";
// 辅助函数，将单个十六进制字符转换为对应的数值
static inline uint8_t CharToHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;  // 对于无效输入，返回0
}

std::string MqttProtocol::DecodeHexString(const std::string& hex_string) {
    std::string decoded;
    decoded.reserve(hex_string.size() / 2);
    for (size_t i = 0; i < hex_string.size(); i += 2) {
        char byte = (CharToHex(hex_string[i]) << 4) | CharToHex(hex_string[i + 1]);
        decoded.push_back(byte);
    }
    return decoded;
}

bool MqttProtocol::IsAudioChannelOpened() const {
    return udp_ != nullptr && !error_occurred_ && !IsTimeout();
}

EmotionFetchResult MqttProtocol::FetchDeviceEmotions() {
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
