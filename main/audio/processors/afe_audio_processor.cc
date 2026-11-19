#include "afe_audio_processor.h"
#include <esp_log.h>

#define PROCESSOR_RUNNING 0x01

#define TAG "AfeAudioProcessor"

AfeAudioProcessor::AfeAudioProcessor()
    : afe_data_(nullptr) {
    event_group_ = xEventGroupCreate();
}

void AfeAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;

    // Pre-allocate output buffer capacity
    output_buffer_.reserve(frame_samples_);

    int ref_num = codec_->input_reference() ? 1 : 0;

    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - ref_num; i++) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; i++) {
        input_format.push_back('R');
    }

    srmodel_list_t *models;
    if (models_list == nullptr) {
        models = esp_srmodel_init("model");
    } else {
        models = models_list;
    }

    char* ns_model_name = esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL);
    char* vad_model_name = esp_srmodel_filter(models, ESP_VADN_PREFIX, NULL);
    
    afe_config_t* afe_config = afe_config_init(input_format.c_str(), NULL, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
    afe_config->vad_mode = VAD_MODE_0;
    afe_config->vad_min_noise_ms = 100;
    if (vad_model_name != nullptr) {
        afe_config->vad_model_name = vad_model_name;
    }

    if (ns_model_name != nullptr) {
        afe_config->ns_init = true;
        afe_config->ns_model_name = ns_model_name;
        afe_config->afe_ns_mode = AFE_NS_MODE_NET;
    } else {
        afe_config->ns_init = false;
    }

    afe_config->agc_init = false;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

#ifdef CONFIG_USE_DEVICE_AEC
    afe_config->aec_init = true;
    afe_config->vad_init = false;
#else
    afe_config->aec_init = false;
    afe_config->vad_init = true;
#endif

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_ = afe_iface_->create_from_config(afe_config);

    /* 缓冲满时库会在 feed() 里打警告。那一行若从高优先级任务写到 USB 串口，会空转到看门狗。 */
    esp_log_level_set("AFE", ESP_LOG_ERROR);

    /*
     * 重活不能留在 audio_input（CPU0 优先级 8），否则麦克风读不让出，空闲任务喂不了看门狗。
     * 也不能放到 CPU1 优先级 6：mjpeg_read 在 CPU1 优先级 4，会被整段回声消除压住，动画播不出来。
     * afe_feed：CPU0 优先级 4，低于播放(6)和 opus(5)，高于动画解码(3)。重活只在这边。
     * audio_communication：只取结果，阻塞等待。放 CPU1 优先级 5，高于 mjpeg_read(4)，
     * 否则读卡扫 JPEG 时取不走数据，录音会断。它几乎一直堵在 fetch 上，不会占住核。
     */
    xTaskCreatePinnedToCore([](void* arg) {
        auto this_ = (AfeAudioProcessor*)arg;
        this_->FeedTask();
        vTaskDelete(NULL);
    }, "afe_feed", 6144, this, 4, &feed_task_, 0);

    xTaskCreatePinnedToCore([](void* arg) {
        auto this_ = (AfeAudioProcessor*)arg;
        this_->AudioProcessorTask();
        vTaskDelete(NULL);
    }, "audio_communication", 4096, this, 5, NULL, 1);
}

AfeAudioProcessor::~AfeAudioProcessor() {
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }
    vEventGroupDelete(event_group_);
}

size_t AfeAudioProcessor::GetFeedSize() {
    if (afe_data_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeAudioProcessor::Feed(std::vector<int16_t>&& data) {
    if (afe_data_ == nullptr || feed_task_ == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    if (!IsRunning()) {
        return;
    }
    const size_t chunk_size = afe_iface_->get_feed_chunksize(afe_data_) * codec_->input_channels();
    const size_t max_samples = chunk_size * 6;
    input_buffer_.insert(input_buffer_.end(), data.begin(), data.end());
    while (chunk_size > 0 && input_buffer_.size() > max_samples) {
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + chunk_size);
    }
    xTaskNotifyGive(feed_task_);
}

void AfeAudioProcessor::Start() {
    xEventGroupSetBits(event_group_, PROCESSOR_RUNNING);
    if (feed_task_ != nullptr) {
        xTaskNotifyGive(feed_task_);
    }
}

void AfeAudioProcessor::Stop() {
    xEventGroupClearBits(event_group_, PROCESSOR_RUNNING);

    std::lock_guard<std::mutex> process_lock(process_mutex_);
    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    if (afe_data_ != nullptr) {
        afe_iface_->reset_buffer(afe_data_);
    }
    input_buffer_.clear();
    if (feed_task_ != nullptr) {
        xTaskNotifyGive(feed_task_);
    }
}

bool AfeAudioProcessor::IsRunning() {
    return xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING;
}

void AfeAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = callback;
}

void AfeAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = callback;
}

void AfeAudioProcessor::FeedTask() {
    const size_t chunk = afe_iface_->get_feed_chunksize(afe_data_) * codec_->input_channels();
    std::vector<int16_t> chunk_data;
    if (chunk > 0) {
        chunk_data.reserve(chunk);
    }

    int backlog_chunks = 0;
    while (true) {
        xEventGroupWaitBits(event_group_, PROCESSOR_RUNNING, pdFALSE, pdTRUE, portMAX_DELAY);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
        if (chunk == 0 || (xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING) == 0) {
            backlog_chunks = 0;
            continue;
        }

        while (true) {
            bool more = false;
            {
                std::lock_guard<std::mutex> lock(input_buffer_mutex_);
                if (input_buffer_.size() < chunk) {
                    break;
                }
                chunk_data.assign(input_buffer_.begin(), input_buffer_.begin() + chunk);
                input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + chunk);
                more = input_buffer_.size() >= chunk;
            }

            {
                std::lock_guard<std::mutex> process_lock(process_mutex_);
                if ((xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING) == 0) {
                    break;
                }
                afe_iface_->feed(afe_data_, chunk_data.data());
            }

            if (!more) {
                backlog_chunks = 0;
                break;
            }
            /* 积压时让出 CPU0，空闲任务和解码才能跑。 */
            if (++backlog_chunks >= 4) {
                backlog_chunks = 0;
                vTaskDelay(1);
            }
        }
    }
}

void AfeAudioProcessor::AudioProcessorTask() {
    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    ESP_LOGI(TAG, "Audio communication task started, feed size: %d fetch size: %d",
        feed_size, fetch_size);

    while (true) {
        xEventGroupWaitBits(event_group_, PROCESSOR_RUNNING, pdFALSE, pdTRUE, portMAX_DELAY);

        auto res = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
        if ((xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING) == 0) {
            continue;
        }
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            if (res != nullptr) {
                ESP_LOGI(TAG, "Error code: %d", res->ret_value);
            }
            continue;
        }

        // VAD state change
        if (vad_state_change_callback_) {
            if (res->vad_state == VAD_SPEECH && !is_speaking_) {
                is_speaking_ = true;
                vad_state_change_callback_(true);
            } else if (res->vad_state == VAD_SILENCE && is_speaking_) {
                is_speaking_ = false;
                vad_state_change_callback_(false);
            }
        }

        if (output_callback_) {
            size_t samples = res->data_size / sizeof(int16_t);

            // Add data to buffer
            output_buffer_.insert(output_buffer_.end(), res->data, res->data + samples);

            // Output complete frames when buffer has enough data
            while (output_buffer_.size() >= frame_samples_) {
                if (output_buffer_.size() == frame_samples_) {
                    // If buffer size equals frame size, move the entire buffer
                    output_callback_(std::move(output_buffer_));
                    output_buffer_.clear();
                    output_buffer_.reserve(frame_samples_);
                } else {
                    // If buffer size exceeds frame size, copy one frame and remove it
                    output_callback_(std::vector<int16_t>(output_buffer_.begin(), output_buffer_.begin() + frame_samples_));
                    output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
                }
            }
        }
    }
}

void AfeAudioProcessor::EnableDeviceAec(bool enable) {
    if (enable) {
#if CONFIG_USE_DEVICE_AEC
        afe_iface_->disable_vad(afe_data_);
        afe_iface_->enable_aec(afe_data_);
#else
        ESP_LOGE(TAG, "Device AEC is not supported");
#endif
    } else {
        afe_iface_->disable_aec(afe_data_);
        afe_iface_->enable_vad(afe_data_);
    }
}
