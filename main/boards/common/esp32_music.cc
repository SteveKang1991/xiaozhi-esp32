#include "esp32_music.h"
#include "board.h"
#include "system_info.h"
#include "audio/audio_codec.h"
#include "application.h"
#include "protocols/protocol.h"
#include "display/display.h"
#include "boards/common/mjpeg_player.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_pthread.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <cstring>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cctype> // 为isdigit函数
#include <thread> // 为线程ID比较
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "assets/lang_config.h"

#define TAG "Esp32Music"

// ========== 简单的ESP32认证函数 ==========

/**
 * @brief 获取设备MAC地址
 * @return MAC地址字符串
 */
static std::string get_device_mac()
{
    return SystemInfo::GetMacAddress();
}

/**
 * @brief 获取设备芯片ID
 * @return 芯片ID字符串
 */
static std::string get_device_chip_id()
{
    // 使用MAC地址作为芯片ID，去除冒号分隔符
    std::string mac = SystemInfo::GetMacAddress();
    // 去除所有冒号
    mac.erase(std::remove(mac.begin(), mac.end(), ':'), mac.end());
    return mac;
}

// URL编码函数
static std::string url_encode(const std::string &str)
{
    std::string encoded;
    char hex[4];

    for (size_t i = 0; i < str.length(); i++)
    {
        unsigned char c = str[i];

        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
        {
            encoded += c;
        }
        else if (c == ' ')
        {
            encoded += '+'; // 空格编码为'+'或'%20'
        }
        else
        {
            snprintf(hex, sizeof(hex), "%%%02X", c);
            encoded += hex;
        }
    }
    return encoded;
}

// 在文件开头添加一个辅助函数，统一处理URL构建

Esp32Music::Esp32Music() : last_downloaded_data_(), current_music_url_(), current_song_name_(),
                           song_name_displayed_(false), current_lyric_url_(),
                           current_lyric_text_(), lyrics_(),
                           current_lyric_index_(-1), lyric_thread_(), is_lyric_running_(false),
                           display_mode_(DISPLAY_MODE_LYRICS), is_playing_(false), is_downloading_(false),
                           play_thread_(), download_thread_(),
                           audio_buffer_(), buffer_mutex_(), buffer_cv_(), buffer_size_(0),
                           mp3_decoder_(nullptr), mp3_frame_info_(),
                           mp3_decoder_initialized_(false),
                           final_pcm_data_fft(nullptr)
{
    download_start_time_ms_ = 0;
    ESP_LOGI(TAG, "Music player initialized with default spectrum display mode");
    /* 预分配 mono buffer，避免每帧 resize 导致 PSRAM 碎片化。
     * MP3 输出最大 2304 samples（双声道），转单声道最多 1152。 */
    mono_buffer_.reserve(1152);
    chunk_pool_.reserve(CHUNK_POOL_KEEP);
    InitializeMp3Decoder();
}

Esp32Music::~Esp32Music()
{
    ESP_LOGI(TAG, "Destroying music player - stopping all operations");

    // 停止所有操作
    is_downloading_ = false;
    is_playing_ = false;
    is_lyric_running_ = false;

    // 通知所有等待的线程
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }

    // 等待下载线程结束，设置5秒超时
    if (download_thread_.joinable())
    {
        ESP_LOGI(TAG, "Waiting for download thread to finish (timeout: 5s)");
        auto start_time = std::chrono::steady_clock::now();

        // 等待线程结束
        bool thread_finished = false;
        while (!thread_finished)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now() - start_time)
                               .count();

            if (elapsed >= 5)
            {
                ESP_LOGW(TAG, "Download thread join timeout after 5 seconds");
                break;
            }

            // 再次设置停止标志，确保线程能够检测到
            is_downloading_ = false;

            // 通知条件变量
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                buffer_cv_.notify_all();
            }

            // 检查线程是否已经结束
            if (!download_thread_.joinable())
            {
                thread_finished = true;
            }

            // 定期打印等待信息
            if (elapsed > 0 && elapsed % 1 == 0)
            {
                ESP_LOGI(TAG, "Still waiting for download thread to finish... (%ds)", (int)elapsed);
            }
        }

        if (download_thread_.joinable())
        {
            download_thread_.join();
        }
        ESP_LOGI(TAG, "Download thread finished");
    }

    // 等待播放线程结束，设置3秒超时
    if (play_thread_.joinable())
    {
        ESP_LOGI(TAG, "Waiting for playback thread to finish (timeout: 3s)");
        auto start_time = std::chrono::steady_clock::now();

        bool thread_finished = false;
        while (!thread_finished)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now() - start_time)
                               .count();

            if (elapsed >= 3)
            {
                ESP_LOGW(TAG, "Playback thread join timeout after 3 seconds");
                break;
            }

            // 再次设置停止标志
            is_playing_ = false;

            // 通知条件变量
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                buffer_cv_.notify_all();
            }

            // 检查线程是否已经结束
            if (!play_thread_.joinable())
            {
                thread_finished = true;
            }
        }

        if (play_thread_.joinable())
        {
            play_thread_.join();
        }
        ESP_LOGI(TAG, "Playback thread finished");
    }

    // 等待歌词线程结束
    if (lyric_thread_.joinable())
    {
        ESP_LOGI(TAG, "Waiting for lyric thread to finish");
        lyric_thread_.join();
        ESP_LOGI(TAG, "Lyric thread finished");
    }

    // 清理缓冲区和MP3解码器
    ClearAudioBuffer();
    CleanupMp3Decoder();

    ReleaseFftPcm();

    if (mp3_input_buffer_) {
        heap_caps_free(mp3_input_buffer_);
        mp3_input_buffer_ = nullptr;
    }
    if (pcm_decode_buffer_) {
        heap_caps_free(pcm_decode_buffer_);
        pcm_decode_buffer_ = nullptr;
    }
    if (download_scratch_) {
        heap_caps_free(download_scratch_);
        download_scratch_ = nullptr;
    }
    if (silence_flush_) {
        heap_caps_free(silence_flush_);
        silence_flush_ = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(chunk_pool_mutex_);
        for (uint8_t* p : chunk_pool_) {
            if (p) {
                heap_caps_free(p);
            }
        }
        chunk_pool_.clear();
    }

    ESP_LOGI(TAG, "Music player destroyed successfully");
}

// ============================================================
// 私有辅助方法：启动歌词线程（仅在歌词显示模式下生效）
// ============================================================
void Esp32Music::StopLyricThread()
{
    is_lyric_running_ = false;
    if (lyric_thread_.joinable()) {
        lyric_thread_.join();
    }
}

bool Esp32Music::EnsureDecodeBuffers()
{
    if (mp3_input_buffer_ == nullptr) {
        mp3_input_buffer_ = (uint8_t*)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    }
    if (pcm_decode_buffer_ == nullptr) {
        pcm_decode_buffer_ = (int16_t*)heap_caps_malloc(2304 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    }
    if (download_scratch_ == nullptr) {
        download_scratch_ = (char*)heap_caps_malloc(STREAM_CHUNK_SIZE, MALLOC_CAP_SPIRAM);
    }
    if (silence_flush_ == nullptr) {
        silence_flush_ = (int16_t*)heap_caps_malloc(4096 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (silence_flush_) {
            memset(silence_flush_, 0, 4096 * sizeof(int16_t));
        }
    }
    if (!mp3_input_buffer_ || !pcm_decode_buffer_ || !download_scratch_) {
        ESP_LOGE(TAG, "Failed to allocate reusable decode/download buffers");
        return false;
    }
    return true;
}

void Esp32Music::StartLyricThreadIfNeeded(const std::string& song_name)
{
    if (display_mode_ != DISPLAY_MODE_LYRICS) {
        ESP_LOGI(TAG, "Lyrics available but spectrum display mode is active, skipping");
        return;
    }

    ESP_LOGI(TAG, "Loading lyrics for: %s (lyrics display mode)", song_name.c_str());

    StopLyricThread();
    current_lyric_index_ = -1;
    is_lyric_running_ = true;

    esp_pthread_cfg_t lyric_cfg = esp_pthread_get_default_config();
    lyric_cfg.stack_size = 4096;
    lyric_cfg.prio = 3;  // 低优先级，不阻塞音频线程
    lyric_cfg.thread_name = "lyric_disp";
    esp_pthread_set_cfg(&lyric_cfg);

    try {
        lyric_thread_ = std::thread(&Esp32Music::LyricDisplayThread, this);
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Failed to create lyric thread: %s, continuing without lyrics", e.what());
        is_lyric_running_ = false;
    }
}

// ============================================================
// 私有方法：解析音乐详情 JSON 并启动播放与歌词线程。
//   is_backup_api = false  -> 主接口 qq_plus
//     字段: name/singer/picture/musicurl/interval/viplrc/lrctxt
//   is_backup_api = true   -> 备用接口 wyvip
//     字段: name/songname/picture/url/vipmusic.duration/music.lrc/music.lrcurl
//
// 返回 true  = 成功，播放已启动
// 返回 false = audio_url 缺失或无效（让调用方决定是否尝试备用 URL）
// ============================================================
bool Esp32Music::HandleMusicDetailsJson(cJSON* response_json, const std::string& song_name, bool is_backup_api)
{
    cJSON* data = cJSON_GetObjectItem(response_json, "data");

    // 提取各字段（根据 API 类型取不同路径）
    cJSON* name = cJSON_GetObjectItem(data, "name");
    cJSON* singer = cJSON_GetObjectItem(data, is_backup_api ? "songname" : "singer");
    cJSON* picture = cJSON_GetObjectItem(data, "picture");

    cJSON* interval = nullptr;
    cJSON* lyric_url = nullptr;
    cJSON* lrctxt = nullptr;

    if (!is_backup_api) {
        // 主接口
        cJSON* audio_url = cJSON_GetObjectItem(data, "musicurl");
        interval = cJSON_GetObjectItem(data, "interval");
        lyric_url = cJSON_GetObjectItem(data, "viplrc");
        lrctxt = cJSON_GetObjectItem(data, "lrctxt");

        if (!cJSON_IsString(audio_url) || !audio_url->valuestring || strlen(audio_url->valuestring) == 0) {
            ESP_LOGE(TAG, "Primary API: audio URL not found or empty");
            return false;
        }
        current_music_url_ = audio_url->valuestring;
    } else {
        // 备用接口
        // 音频 URL 必须从 data->vipmusic->url 取（直链，不走 302 重定向）
        // data->url 是 music.163.com 的外链，会 302 跳转，无法直接播放
        cJSON* vipmusic = cJSON_GetObjectItem(data, "vipmusic");
        cJSON* audio_url = vipmusic ? cJSON_GetObjectItem(vipmusic, "url") : nullptr;
        interval = vipmusic ? cJSON_GetObjectItem(vipmusic, "duration") : nullptr;

        // 歌词在 data->music->lrc / lrcurl
        cJSON* music_obj = cJSON_GetObjectItem(data, "music");
        lrctxt = music_obj ? cJSON_GetObjectItem(music_obj, "lrc") : nullptr;
        lyric_url = music_obj ? cJSON_GetObjectItem(music_obj, "lrcurl") : nullptr;

        if (!cJSON_IsString(audio_url) || !audio_url->valuestring || strlen(audio_url->valuestring) == 0) {
            ESP_LOGE(TAG, "Backup API: audio URL not found or empty (vipmusic.url)");
            return false;
        }
        current_music_url_ = audio_url->valuestring;
    }
    ESP_LOGI(TAG, "Starting streaming playback for: %s (API=%s)",
             song_name.c_str(), is_backup_api ? "backup" : "primary");
    song_name_displayed_ = false;

    if (cJSON_IsString(picture) && picture->valuestring && strlen(picture->valuestring) > 0) {
        current_picture_url_ = picture->valuestring;
    } else {
        current_picture_url_.clear();
        ESP_LOGW(TAG, "No picture URL in music details response");
    }

    // 下发歌名/歌手/时长到显示端
    {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        if (display) {
            const char* name_str   = (cJSON_IsString(name)   && name->valuestring)   ? name->valuestring   : "";
            const char* singer_str = (cJSON_IsString(singer) && singer->valuestring) ? singer->valuestring : "";

            int interval_sec = 0;
            if (is_backup_api) {
                // 备用接口时长格式固定为 MM:SS
                if (cJSON_IsString(interval) && interval->valuestring) {
                    int mm = 0, ss = 0;
                    if (sscanf(interval->valuestring, "%d:%d", &mm, &ss) == 2) {
                        interval_sec = mm * 60 + ss;
                    } else {
                        interval_sec = atoi(interval->valuestring);
                    }
                }
            } else {
                // 主接口：interval 可能为整数或字符串（格式: 239 / 03:59 / 3:59 / 03:59.500）
                if (cJSON_IsNumber(interval)) {
                    interval_sec = interval->valueint;
                } else if (cJSON_IsString(interval) && interval->valuestring) {
                    const char* s = interval->valuestring;
                    int mm = 0, ss = 0;
                    if (sscanf(s, "%d:%d", &mm, &ss) == 2) {
                        interval_sec = mm * 60 + ss;
                    } else {
                        float fsec = 0.0f;
                        if (sscanf(s, "%f", &fsec) == 1) {
                            interval_sec = (int)fsec;
                        } else {
                            interval_sec = atoi(s);
                        }
                    }
                }
            }
            display->SetMusicInfo(name_str, singer_str, interval_sec);
            display->ShowMusicCover(true, current_picture_url_);
        }
    }

    if (!StartStreaming(current_music_url_)) {
        return false;
    }

    // 处理歌词：优先使用内嵌 lrctxt，其次使用 lyric_url
    if (cJSON_IsString(lrctxt) && lrctxt->valuestring && strlen(lrctxt->valuestring) > 0) {
        current_lyric_text_ = lrctxt->valuestring;
        current_lyric_url_.clear();
        ESP_LOGI(TAG, "Lyrics inline in details response (%d bytes), skip URL download",
                 current_lyric_text_.length());
    } else if (cJSON_IsString(lyric_url) && lyric_url->valuestring && strlen(lyric_url->valuestring) > 0) {
        current_lyric_url_ = lyric_url->valuestring;
        current_lyric_text_.clear();
    } else {
        current_lyric_url_.clear();
        current_lyric_text_.clear();
    }

    // 有歌词则启动歌词线程
    if (!current_lyric_text_.empty() || !current_lyric_url_.empty()) {
        StartLyricThreadIfNeeded(song_name);
    } else {
        ESP_LOGW(TAG, "No lyric data for this song");
    }

    return true;
}
bool Esp32Music::Download(const std::string &song_name, const std::string &artist_name)
{
    if (is_playing_) {
        ESP_LOGW(TAG, "Already in playback mode, skip: %s", song_name.c_str());
        return false;
    }
    is_playing_ = true;

    ESP_LOGI(TAG, "Starting to get music details for: %s", song_name.c_str());

    last_downloaded_data_.clear();
    current_lyric_text_.clear();
    current_lyric_url_.clear();
    current_picture_url_.clear();
    current_song_name_ = song_name;

    auto network = Board::GetInstance().GetNetwork();

    // ------------------------------------------------------------
    // 尝试主 URL：qq_plus
    // ------------------------------------------------------------
    {
        std::string primary_url = "https://api.yaohud.cn/api/music/qq_plus?key=hjUDBRMTeQtM2qmDFqJ"
                                 "&msg=" + url_encode(song_name + " " + artist_name) + "&n=1&size=mp3";

        auto http = network->CreateHttp(0);
        http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
        http->SetHeader("Accept", "application/json");

        if (!http->Open("GET", primary_url)) {
            ESP_LOGE(TAG, "Failed to connect to primary music API");
        } else {
            int status_code = http->GetStatusCode();
            if (status_code == 200) {
                std::string response_data = http->ReadAll();
                http->Close();

                ESP_LOGI(TAG, "Primary HTTP GET Status = %d, content_length = %d",
                         status_code, response_data.length());

                if (!response_data.empty()) {
                    cJSON* json = cJSON_Parse(response_data.c_str());
                    if (json) {
                        if (HandleMusicDetailsJson(json, song_name, false)) {
                            cJSON_Delete(json);
                            return true;
                        }
                        ESP_LOGW(TAG, "Primary URL has no audio for: %s, trying backup", song_name.c_str());
                        cJSON_Delete(json);
                    } else {
                        ESP_LOGE(TAG, "Failed to parse primary JSON response");
                    }
                } else {
                    ESP_LOGE(TAG, "Empty response from primary music API");
                }
            } else {
                ESP_LOGE(TAG, "Primary HTTP GET failed with status code: %d", status_code);
                http->Close();
            }
        }
    }

    // ------------------------------------------------------------
    // 尝试备用 URL：wyvip（网易云曲库，曲库更全）
    // ------------------------------------------------------------
    {
        ESP_LOGW(TAG, "Trying backup music API for: %s", song_name.c_str());

        std::string backup_url = "https://api.yaohud.cn/api/music/wyvip?key=hjUDBRMTeQtM2qmDFqJ"
                                "&msg=" + url_encode(song_name) + "&n=1&level=standard&g=1";

        auto http_b = network->CreateHttp(0);
        http_b->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
        http_b->SetHeader("Accept", "application/json");

        if (!http_b->Open("GET", backup_url)) {
            ESP_LOGE(TAG, "Failed to connect to backup music API");
        } else {
            int status_code = http_b->GetStatusCode();
            if (status_code == 200) {
                std::string response_data = http_b->ReadAll();
                http_b->Close();

                ESP_LOGI(TAG, "Backup HTTP GET Status = %d, content_length = %d",
                         status_code, response_data.length());

                if (!response_data.empty()) {
                    cJSON* json = cJSON_Parse(response_data.c_str());
                    if (json) {
                        if (HandleMusicDetailsJson(json, song_name, true)) {
                            cJSON_Delete(json);
                            return true;
                        }
                        ESP_LOGE(TAG, "Failed to find music: 没有找到歌曲 '%s'", song_name.c_str());
                        cJSON_Delete(json);
                    } else {
                        ESP_LOGE(TAG, "Failed to parse backup JSON response");
                    }
                } else {
                    ESP_LOGE(TAG, "Empty response from backup music API");
                }
            } else {
                ESP_LOGE(TAG, "Backup HTTP GET failed with status code: %d", status_code);
                http_b->Close();
            }
        }
    }

    is_playing_ = false;
    return false;
}

std::string Esp32Music::GetDownloadResult()
{
    return last_downloaded_data_;
}

// 开始流式播放
bool Esp32Music::StartStreaming(const std::string &music_url)
{
    if (music_url.empty())
    {
        ESP_LOGE(TAG, "Music URL is empty");
        return false;
    }

    ESP_LOGD(TAG, "Starting streaming for URL: %s", music_url.c_str());

    // 上一首歌线程若已结束但未 join，这里收尸。不要 SignalPlaybackAbort：
    // 那会清掉 Download 刚置上的 is_playing_，也会把正在播的歌掐掉再开第二首。
    if (download_thread_.joinable())
    {
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();
        }
        download_thread_.join();
    }
    if (play_thread_.joinable())
    {
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();
        }
        play_thread_.join();
    }
    StopLyricThread();

    if (!EnsureDecodeBuffers()) {
        suppress_play_exit_ui_ = false;
        is_downloading_ = false;
        is_playing_ = false;
        return false;
    }
    download_start_time_ms_ = esp_timer_get_time() / 1000;

    // 旧歌已 join：清下载缓冲并重建 Helix。I2S 静音冲刷留给开播前那一次，避免叠两次静音拉长换歌间隔。
    ResetCodecAndDecoderState(true, false);

    // 停止 MJPEG 动画（释放 MJPEG 占用的内存给音乐播放用）
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    if (display)
    {
        if (mjpeg_player_is_running())
        {
            mjpeg_player_stop();
            ESP_LOGI(TAG, "MJPEG stopped for music playback");
        }
    }

    // 播放线程钉在 CPU0，避开 CPU1 上的 LVGL+FFT。
    // 下载线程同核更低优先级，保证 I2S 喂数不被 HTTP memcpy 抢占导致 underrun。
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 12288;
    cfg.prio = 3;
    cfg.thread_name = "audio_dl";
    cfg.pin_to_core = 0;
    esp_pthread_set_cfg(&cfg);

    is_downloading_ = true;
    try {
        download_thread_ = std::thread(&Esp32Music::DownloadAudioStream, this, music_url);
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Failed to create download thread: %s", e.what());
        is_downloading_ = false;
        suppress_play_exit_ui_ = false;
        return false;
    }

    cfg.stack_size = 16384;
    cfg.prio = 6;
    cfg.thread_name = "audio_stream";
    cfg.pin_to_core = 0;
    esp_pthread_set_cfg(&cfg);

    is_playing_ = true;
    try {
        play_thread_ = std::thread(&Esp32Music::PlayAudioStream, this);
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Failed to create play thread: %s", e.what());
        is_playing_ = false;
        suppress_play_exit_ui_ = false;
        if (download_thread_.joinable()) {
            download_thread_.join();
        }
        return false;
    }

    ESP_LOGI(TAG, "Streaming threads started successfully");
    suppress_play_exit_ui_ = false;

    return true;
}

// 停止流式播放
bool Esp32Music::StopStreaming()
{
    ESP_LOGI(TAG, "Stopping music streaming - current state: downloading=%d, playing=%d",
             is_downloading_.load(), is_playing_.load());

    // 检查是否有流式播放正在进行
    if (!is_playing_ && !is_downloading_)
    {
        StopLyricThread();
        ESP_LOGW(TAG, "No streaming in progress");
        return true;
    }

    // 停止下载和播放标志
    is_downloading_ = false;
    is_playing_ = false;

    // 清空歌名显示 + 重置 music UI 上的进度 / 歌词
    auto &board = Board::GetInstance();
    auto display = board.GetDisplay();
    if (display)
    {
        display->SetMusicInfo("", "", -1);  // interval=-1 保留总时长不刷 0
        /* 传 "" 强制清掉当前/下一句歌词 label；display 内部对 "" 走
         * "去抖后 != lyric" 路径立即写入空串。 */
        display->SetMusicProgress(0, "", "");
        /* 显式隐藏音乐封面。
         *
         * 之前依赖 application.cc 的 HandleStateChangedEvent 在状态切到 Idle 且
         * !is_music_playing 时调 ShowMusicCover(false)，但播放音乐时设备本来就
         * 处在 kDeviceStateIdle（music 在 idle 状态下后台播放），music 停止时
         * 状态没变化，state machine 的 TransitionTo 是 no-op，
         * HandleStateChangedEvent 不会被触发，封面就永远留在屏上了。
         *
         * 这里直接调一次，确保中断 / 自然结束 / 主动 stop 三种路径都能隐藏。 */
        display->ShowMusicCover(false, "");
        ESP_LOGI(TAG, "Cleared song name display and hid music cover");
    }

    // 通知所有等待的线程
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }

    // 等待线程结束（避免重复代码，让StopStreaming也能等待线程完全停止）
    if (download_thread_.joinable())
    {
        download_thread_.join();
        ESP_LOGI(TAG, "Download thread joined in StopStreaming");
    }

    // 等待播放线程结束，使用更安全的方式
    if (play_thread_.joinable())
    {
        // 先设置停止标志
        is_playing_ = false;

        // 通知条件变量，确保线程能够退出
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();
        }

        // 阻塞等待 play_thread 退出。
        // play_thread 内部会检查 is_playing_ 后退出循环并做清理（释放 buffer、更新 display），
        // 清理过程一般在 100ms 内完成。
        //
        // 修复说明：原代码使用
        //   while (...) { if (!play_thread_.joinable()) break; }
        // 是错的：joinable() 在 join() 调用前始终为 true，所以循环一定会等满 1 秒
        // 后 detach 线程。detach 后 play_thread 在后台继续运行，不断往 codec output
        // 写残余 PCM 数据，被麦克风拾取识别成 STT，造成音乐播放时唤醒后误识别。
        // 直接 join() 是最可靠的方式。
        play_thread_.join();
        ESP_LOGI(TAG, "Play thread joined in StopStreaming");
    }

    ResetSampleRate();

    StopLyricThread();

    ResetCodecAndDecoderState(true);

    current_song_name_.clear();
    current_picture_url_.clear();
    current_lyric_text_.clear();
    current_lyric_url_.clear();
    current_lyric_text_.shrink_to_fit();
    current_lyric_url_.shrink_to_fit();
    {
        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        lyrics_.clear();
    }

    // FFT PCM 缓冲跨歌曲复用，避免每首歌 malloc/free 2.3KB 打碎片。
    // 真正释放在析构。

    if (display)
    {
        display->EnableFft(false);
        ESP_LOGI(TAG, "Stopped FFT display in StopStreaming");
    }

    ESP_LOGI(TAG, "Music streaming stop signal sent");
    return true;
}

// 流式下载音频数据
void Esp32Music::DownloadAudioStream(const std::string &music_url)
{
    ESP_LOGD(TAG, "Starting audio stream download from: %s", music_url.c_str());

    // 验证URL有效性
    if (music_url.empty() || music_url.find("http") != 0)
    {
        ESP_LOGE(TAG, "Invalid URL format: %s", music_url.c_str());
        SignalPlaybackAbort();
        return;
    }

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);

    // 设置基本请求头
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    http->SetHeader("Accept", "*/*");
    http->SetHeader("Range", "bytes=0-"); // 支持断点续传

    // 添加ESP32认证头
    //add_auth_headers(http.get());

    if (!http->Open("GET", music_url))
    {
        ESP_LOGE(TAG, "Failed to connect to music stream URL");
        http->Close();
        SignalPlaybackAbort();
        return;
    }
    if (!is_playing_.load() || !is_downloading_.load()) {
        http->Close();
        return;
    }

    int status_code = http->GetStatusCode();
    
    if (status_code != 200 && status_code != 206)
    { // 206 for partial content
        ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
        http->Close();
        SignalPlaybackAbort();
        return;
    }

    ESP_LOGI(TAG, "Started downloading audio stream, status: %d", status_code);

    const size_t chunk_size = STREAM_CHUNK_SIZE;
    char *buffer = download_scratch_;
    if (!buffer)
    {
        ESP_LOGE(TAG, "Download scratch buffer missing");
        http->Close();
        SignalPlaybackAbort();
        return;
    }
    size_t total_downloaded = 0;
    size_t resume_from = 0;   // 断点续传偏移
    int retry_count = 0;
    const int max_retries = 5;
    int64_t last_progress_time = esp_timer_get_time() / 1000;  // 记录上次有数据的时间

    while (is_downloading_ && is_playing_)
    {
        // 初始下载超时检测（一直没拿到任何数据）
        if (total_downloaded == 0) {
            int64_t elapsed_since_start = esp_timer_get_time() / 1000 - download_start_time_ms_;
            if (elapsed_since_start > DOWNLOAD_TIMEOUT_MS) {
                ESP_LOGW(TAG, "Audio download timeout (%lld ms), no data received, aborting",
                         (long long)elapsed_since_start);
                http->Close();
                SignalPlaybackAbort();
                return;
            }
        }
        int bytes_read = http->Read(buffer, chunk_size);

        // 进度超时检测（有数据但长时间没新数据）
        if (bytes_read > 0) {
            int64_t elapsed_since_progress = esp_timer_get_time() / 1000 - last_progress_time;
            if (elapsed_since_progress > 5000) {
                ESP_LOGW(TAG, "Audio download stalled (%lld ms no progress, downloaded=%u), aborting",
                         (long long)elapsed_since_progress, (unsigned)total_downloaded);
                http->Close();
                SignalPlaybackAbort();
                return;
            }
        }

        if (bytes_read < 0)
        {
            // SSL/HTTP 连接断开 - 尝试断点续传重连
            int err = bytes_read;
            ESP_LOGW(TAG, "Audio read error: %d, attempting reconnect (resume at %u, retry %d/%d)",
                     err, (unsigned)resume_from, retry_count + 1, max_retries);
            http->Close();

            if (retry_count >= max_retries)
            {
                ESP_LOGE(TAG, "Max retries reached, aborting download");
                break;
            }
            retry_count++;
            vTaskDelay(pdMS_TO_TICKS(500 + retry_count * 500)); // 递增退避

            // 用 Range 头重连，从上次成功位置续传
            http->SetHeader("Range", "bytes=" + std::to_string(resume_from) + "-");
            if (!http->Open("GET", music_url))
            {
                ESP_LOGE(TAG, "Reconnect failed, retry %d/%d", retry_count, max_retries);
                continue;
            }
            int rc = http->GetStatusCode();
            if (rc != 200 && rc != 206)
            {
                ESP_LOGE(TAG, "Reconnect HTTP status: %d", rc);
                http->Close();
                continue;
            }
            ESP_LOGI(TAG, "Reconnected, resuming from byte %u", (unsigned)resume_from);
            continue;
        }
        if (bytes_read == 0)
        {
            if (total_downloaded == 0) {
                ESP_LOGW(TAG, "Audio stream ended with no data received, aborting");
                http->Close();
                is_downloading_ = false;
                return;
            }
            ESP_LOGI(TAG, "Audio stream download completed, total: %u bytes", (unsigned)total_downloaded);
            break;
        }
        // 读取成功，重置重试计数
        retry_count = 0;
        last_progress_time = esp_timer_get_time() / 1000;

        // 打印数据块信息
        // ESP_LOGI(TAG, "Downloaded chunk: %d bytes at offset %d", bytes_read, total_downloaded);

        // 安全地打印数据块的十六进制内容（前16字节）
        if (bytes_read >= 16)
        {
            // ESP_LOGI(TAG, "Data: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X ...",
            //         (unsigned char)buffer[0], (unsigned char)buffer[1], (unsigned char)buffer[2], (unsigned char)buffer[3],
            //         (unsigned char)buffer[4], (unsigned char)buffer[5], (unsigned char)buffer[6], (unsigned char)buffer[7],
            //         (unsigned char)buffer[8], (unsigned char)buffer[9], (unsigned char)buffer[10], (unsigned char)buffer[11],
            //         (unsigned char)buffer[12], (unsigned char)buffer[13], (unsigned char)buffer[14], (unsigned char)buffer[15]);
        }
        else
        {
            ESP_LOGI(TAG, "Data chunk too small: %d bytes", bytes_read);
        }

        // 尝试检测文件格式（检查文件头）
        if (total_downloaded == 0 && bytes_read >= 4)
        {
            if (memcmp(buffer, "ID3", 3) == 0)
            {
                ESP_LOGI(TAG, "Detected MP3 file with ID3 tag");
            }
            else if (buffer[0] == 0xFF && (buffer[1] & 0xE0) == 0xE0)
            {
                ESP_LOGI(TAG, "Detected MP3 file header");
            }
            else if (memcmp(buffer, "RIFF", 4) == 0)
            {
                ESP_LOGI(TAG, "Detected WAV file");
            }
            else if (memcmp(buffer, "fLaC", 4) == 0)
            {
                ESP_LOGI(TAG, "Detected FLAC file");
            }
            else if (memcmp(buffer, "OggS", 4) == 0)
            {
                ESP_LOGI(TAG, "Detected OGG file");
            }
            else
            {
                ESP_LOGI(TAG, "Unknown audio format, first 4 bytes: %02X %02X %02X %02X",
                         (unsigned char)buffer[0], (unsigned char)buffer[1],
                         (unsigned char)buffer[2], (unsigned char)buffer[3]);
            }
        }

        // 创建音频数据块
        uint8_t *chunk_data = AllocStreamChunk();
        if (!chunk_data)
        {
            ESP_LOGE(TAG, "Failed to allocate memory for audio chunk");
            break;
        }
        memcpy(chunk_data, buffer, bytes_read);

        // 等待缓冲区有空间
        {
            std::unique_lock<std::mutex> lock(buffer_mutex_);
            buffer_cv_.wait(lock, [this]
                            { return buffer_size_ < MAX_BUFFER_SIZE || !is_downloading_; });

            if (is_downloading_)
            {
                audio_buffer_.push(AudioChunk(chunk_data, bytes_read));
                buffer_size_ += bytes_read;
                total_downloaded += bytes_read;
                resume_from += bytes_read;   // 断点续传偏移

                // 通知播放线程有新数据
                buffer_cv_.notify_one();

                if (total_downloaded % (256 * 1024) == 0)
                { // 每256KB打印一次进度
                    ESP_LOGI(TAG, "Downloaded %d bytes, buffer size: %d", total_downloaded, buffer_size_);
                }

                // 重置进度超时计时
                last_progress_time = esp_timer_get_time() / 1000;
            }
            else
            {
                FreeStreamChunk(chunk_data);
                break;
            }
        }
    }

    http->Close();
    is_downloading_ = false;

    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }

    ESP_LOGI(TAG, "Audio stream download thread finished");
}

// 流式播放音频数据
void Esp32Music::PlayAudioStream()
{
    ESP_LOGI(TAG, "Starting audio stream playback");

    current_play_time_ms_ = 0;
    played_pcm_samples_ = 0;
    last_frame_time_ms_ = 0;
    total_frames_decoded_ = 0;
    size_t total_played = 0;
    int bytes_left = 0;
    uint8_t *read_ptr = nullptr;
    bool id3_processed = false;
    uint8_t *mp3_input_buffer = mp3_input_buffer_;
    int16_t *pcm_buffer = pcm_decode_buffer_;
    int fade_in_remaining = 2048;  // 约 46ms@44.1k，压掉开播瞬间直流台阶
    int warmup_frames_skip = 2;     // Helix 首帧 overlap 常带爆破音，丢掉再出声

    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec || !codec->output_enabled())
    {
        ESP_LOGE(TAG, "Audio codec not available or not enabled");
        SignalPlaybackAbort();
        goto playback_cleanup;
    }

    if (!mp3_decoder_initialized_)
    {
        ESP_LOGE(TAG, "MP3 decoder not initialized");
        SignalPlaybackAbort();
        goto playback_cleanup;
    }

    // 等待缓冲区有足够数据开始播放。必须用 wait_for：超时写在 predicate 里但
    // 不 notify 时 wait() 永远不会醒（403 后 play 会卡住直到下一首歌 StartStreaming）。
    {
        std::unique_lock<std::mutex> lock(buffer_mutex_);
        const bool ready = buffer_cv_.wait_for(
            lock, std::chrono::milliseconds(DOWNLOAD_TIMEOUT_MS), [this] {
                return buffer_size_ >= MIN_BUFFER_SIZE
                    || (!is_downloading_ && !audio_buffer_.empty())
                    || !is_playing_.load();
            });

        if (!is_playing_.load() || !ready
            || (buffer_size_ < MIN_BUFFER_SIZE && audio_buffer_.empty())) {
            ESP_LOGW(TAG, "Audio buffer not ready (ready=%d playing=%d buffer=%d downloading=%d), aborting playback",
                     (int)ready, (int)is_playing_.load(), (int)buffer_size_, (int)is_downloading_.load());
            is_downloading_ = false;
            is_playing_ = false;
            goto playback_cleanup;
        }

        ESP_LOGI(TAG, "Starting playback with buffer: %d bytes", (int)buffer_size_);
    }

    // 缓冲已够：重建解码器并冲 I2S，但不要清掉刚下好的 MP3
    ResetCodecAndDecoderState(false, true);

    if (!mp3_input_buffer || !pcm_buffer)
    {
        ESP_LOGE(TAG, "Decode buffers missing");
        SignalPlaybackAbort();
        goto playback_cleanup;
    }

    while (is_playing_)
    {
        // 检查设备状态，只有在空闲状态才播放音乐
        auto &app = Application::GetInstance();
        DeviceState current_state = app.GetDeviceState();

        // 状态检查：音乐播放只在 idle 状态下进行
        // 参考参考项目：不是直接 break 退出，而是循环等待 + 不输出音频
        if (current_state != kDeviceStateIdle)
        {
            // 如果当前不在 idle 状态（用户按键唤醒、语音唤醒、说话等）
            // 不 break，等 StopStreaming 主动设置 is_playing_ = false 来结束
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // 设备状态检查通过，显示当前播放的歌名
        if (!song_name_displayed_ && !current_song_name_.empty())
        {
            auto &board = Board::GetInstance();
            auto display = board.GetDisplay();
            if (display)
            {
                // 格式化歌名显示为《歌名》播放中...
                //std::string formatted_song_name = "《" + current_song_name_ + "》播放中...";
                //display->SetMusicInfo(formatted_song_name.c_str());
                //ESP_LOGI(TAG, "Displaying song name: %s", formatted_song_name.c_str());
                song_name_displayed_ = true;
            }

            // 根据显示模式启动相应的显示功能
            if (display)
            {
                // 在歌词显示模式下同时启用 FFT 频谱显示（底部叠加显示）
                display->EnableFft(true);
                ESP_LOGI(TAG, "Display start() called for spectrum visualization + lyrics mode");
            }
        }

        // 如果需要更多MP3数据，从缓冲区读取
        if (bytes_left < 4096)
        { // 保持至少4KB数据用于解码
            AudioChunk chunk;
            {
                std::unique_lock<std::mutex> lock(buffer_mutex_);
                if (audio_buffer_.empty())
                {
                    if (!is_downloading_)
                    {
                        ESP_LOGI(TAG, "Playback finished, total played: %d bytes", total_played);
                        break;
                    }
                    // 等待新数据
                    buffer_cv_.wait(lock, [this]
                                    { return !audio_buffer_.empty() || !is_downloading_; });
                    if (audio_buffer_.empty())
                    {
                        continue;
                    }
                }

                chunk = audio_buffer_.front();
                audio_buffer_.pop();
                buffer_size_ -= chunk.size;

                // 通知下载线程缓冲区有空间
                buffer_cv_.notify_one();
            }

            // 将新数据添加到MP3输入缓冲区
            if (chunk.data && chunk.size > 0)
            {
                // 移动剩余数据到缓冲区开头
                if (bytes_left > 0 && read_ptr != mp3_input_buffer)
                {
                    memmove(mp3_input_buffer, read_ptr, bytes_left);
                }

                // 检查缓冲区空间
                size_t space_available = 8192 - bytes_left;
                size_t copy_size = std::min(chunk.size, space_available);

                // 复制新数据
                memcpy(mp3_input_buffer + bytes_left, chunk.data, copy_size);
                bytes_left += copy_size;
                read_ptr = mp3_input_buffer;

                // 检查并跳过ID3标签（仅在开始时处理一次）
                if (!id3_processed && bytes_left >= 10)
                {
                    size_t id3_skip = SkipId3Tag(read_ptr, bytes_left);
                    if (id3_skip > 0)
                    {
                        read_ptr += id3_skip;
                        bytes_left -= id3_skip;
                        ESP_LOGI(TAG, "Skipped ID3 tag: %u bytes", (unsigned int)id3_skip);
                    }
                    id3_processed = true;
                }

                // 释放chunk内存
                FreeStreamChunk(chunk.data);
            }
        }

        // 尝试找到MP3帧同步
        int sync_offset = MP3FindSyncWord(read_ptr, bytes_left);
        if (sync_offset < 0)
        {
            ESP_LOGW(TAG, "No MP3 sync word found, skipping %d bytes", bytes_left);
            bytes_left = 0;
            continue;
        }

        // 跳过到同步位置
        if (sync_offset > 0)
        {
            read_ptr += sync_offset;
            bytes_left -= sync_offset;
        }

        // 解码MP3帧（pcm_buffer 已在函数开头从堆分配）
        int decode_result = MP3Decode(mp3_decoder_, &read_ptr, &bytes_left, pcm_buffer, 0);

        if (decode_result == 0)
        {
            // 解码成功，获取帧信息
            MP3GetLastFrameInfo(mp3_decoder_, &mp3_frame_info_);
            total_frames_decoded_++;

            // 基本的帧信息有效性检查，防止除零错误
            if (mp3_frame_info_.samprate == 0 || mp3_frame_info_.nChans == 0)
            {
                ESP_LOGW(TAG, "Invalid frame info: rate=%d, channels=%d, skipping",
                         mp3_frame_info_.samprate, mp3_frame_info_.nChans);
                continue;
            }

            if (warmup_frames_skip > 0) {
                warmup_frames_skip--;
                continue;
            }

            // 用累计样本算播放时间，避免每帧 (samples*1000/rate) 整除把时钟越走越慢
            const int ch = mp3_frame_info_.nChans;
            const int rate = mp3_frame_info_.samprate;
            const int samples_per_ch = mp3_frame_info_.outputSamps / ch;
            played_pcm_samples_ += samples_per_ch;
            current_play_time_ms_ = played_pcm_samples_ * 1000 / rate;

            ESP_LOGD(TAG, "Frame %d: time=%lldms, rate=%d, ch=%d",
                     total_frames_decoded_, (long long)current_play_time_ms_.load(),
                     mp3_frame_info_.samprate, mp3_frame_info_.nChans);

            // 歌词/进度改由 FFT 任务 TickPlaybackUi 刷新，play_thread 不再抢 LVGL 锁。

            // 将PCM数据发送到Application的音频解码队列
            if (mp3_frame_info_.outputSamps > 0)
            {
                int16_t *final_pcm_data = pcm_buffer;
                int final_sample_count = mp3_frame_info_.outputSamps;

                // 如果是双通道，转换为单通道混合
                if (mp3_frame_info_.nChans == 2)
                {
                    // 双通道转单通道：将左右声道混合
                    int stereo_samples = mp3_frame_info_.outputSamps; // 包含左右声道的总样本数
                    int mono_samples = stereo_samples / 2;            // 实际的单声道样本数

                    mono_buffer_.resize(mono_samples);

                    for (int i = 0; i < mono_samples; ++i)
                    {
                        // 混合左右声道 (L + R) / 2
                        int left = pcm_buffer[i * 2];      // 左声道
                        int right = pcm_buffer[i * 2 + 1]; // 右声道
                        mono_buffer_[i] = (int16_t)((left + right) / 2);
                    }

                    final_pcm_data = mono_buffer_.data();
                    final_sample_count = mono_samples;

                    ESP_LOGD(TAG, "Converted stereo to mono: %d -> %d samples",
                             stereo_samples, mono_samples);
                }
                else if (mp3_frame_info_.nChans == 1)
                {
                    // 已经是单声道，无需转换
                    ESP_LOGD(TAG, "Already mono audio: %d samples", final_sample_count);
                }
                else
                {
                    ESP_LOGW(TAG, "Unsupported channel count: %d, treating as mono",
                             mp3_frame_info_.nChans);
                }

                PublishPcmForFft(final_pcm_data, final_sample_count);

                if (fade_in_remaining > 0) {
                    const int n = std::min(final_sample_count, fade_in_remaining);
                    const int start = 2048 - fade_in_remaining;
                    for (int i = 0; i < n; ++i) {
                        final_pcm_data[i] = (int16_t)(((int32_t)final_pcm_data[i] * (start + i)) / 2048);
                    }
                    fade_in_remaining -= n;
                }

                ESP_LOGD(TAG, "Sending %d PCM samples (rate=%d, channels=%d->1) to Application",
                         final_sample_count, mp3_frame_info_.samprate, mp3_frame_info_.nChans);

                app.AddAudioData(final_pcm_data, (size_t)final_sample_count, mp3_frame_info_.samprate);
                total_played += final_sample_count * (int)sizeof(int16_t);

                // 打印播放进度
                if (total_played % (128 * 1024) == 0)
                {
                    ESP_LOGI(TAG, "Played %d bytes, buffer: %d bytes", total_played, buffer_size_);
                }
            }
        }
        else
        {
            ESP_LOGW(TAG, "MP3 decode failed with error: %d", decode_result);
            if (bytes_left > 1)
            {
                read_ptr++;
                bytes_left--;
            }
            else
            {
                bytes_left = 0;
            }
        }
    }

playback_cleanup:
    is_playing_ = false;

    // 切歌 / 自然结束都要清残留；换歌时 suppress_play_exit_ui_ 只跳 UI，不清音频
    ResetCodecAndDecoderState(true);

    const bool do_exit_ui = !suppress_play_exit_ui_.load();
    if (do_exit_ui)
    {
        StopLyricThread();
        auto &board = Board::GetInstance();
        auto display = board.GetDisplay();
        if (display)
        {
            display->SetMusicInfo("", "", 0);
            display->SetMusicProgress(0, "", "");
            display->ShowMusicCover(false, "");
            display->EnableFft(false);
            ESP_LOGI(TAG, "Left music UI (cover/fft/lyrics cleared)");
        }
        /* 自然结束 / 拉流失败 / 主动 stop：回到 listen，不要停在 idle。
         * 换歌路径 suppress_play_exit_ui_ 为 true，不会走进这里。
         * 切状态放到主循环，走 HandleStateChangedEvent 的 Listening 分支。 */
        Application::GetInstance().Schedule([]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() != kDeviceStateListening) {
                app.SetDeviceState(kDeviceStateListening);
            }
        });
        ResetSampleRate();
    }

    ESP_LOGI(TAG, "Audio stream playback finished, total played: %d bytes", total_played);
}

// 清空音频缓冲区
void Esp32Music::ClearAudioBuffer()
{
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    while (!audio_buffer_.empty())
    {
        AudioChunk chunk = audio_buffer_.front();
        audio_buffer_.pop();
        if (chunk.data)
        {
            FreeStreamChunk(chunk.data);
        }
    }
    buffer_size_ = 0;
    TrimChunkPool();
    ESP_LOGI(TAG, "Audio buffer cleared");
}

void Esp32Music::FlushCodecSilence()
{
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec || !codec->output_enabled() || !silence_flush_) {
        return;
    }
    codec->OutputData(silence_flush_, 4096);
    codec->OutputData(silence_flush_, 4096);
}

void Esp32Music::ResetCodecAndDecoderState(bool clear_stream_buffer, bool flush_i2s)
{
    if (clear_stream_buffer) {
        ClearAudioBuffer();
    }

    if (mp3_input_buffer_) {
        memset(mp3_input_buffer_, 0, 8192);
    }
    if (pcm_decode_buffer_) {
        memset(pcm_decode_buffer_, 0, 2304 * sizeof(int16_t));
    }
    if (!mono_buffer_.empty()) {
        memset(mono_buffer_.data(), 0, mono_buffer_.size() * sizeof(int16_t));
        mono_buffer_.clear();
    }
    memset(&mp3_frame_info_, 0, sizeof(mp3_frame_info_));

    current_play_time_ms_ = 0;
    played_pcm_samples_ = 0;
    last_frame_time_ms_ = 0;
    total_frames_decoded_ = 0;
    song_name_displayed_ = false;
    current_lyric_index_ = -1;

    if (final_pcm_data_fft) {
        memset(final_pcm_data_fft, 0, FFT_PCM_SAMPLES * sizeof(int16_t));
        fft_pcm_samples_ = 0;
        fft_pcm_seq_ = 0;
    }
    {
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            display->ResetFftVisual();
        }
    }

    CleanupMp3Decoder();
    if (!InitializeMp3Decoder()) {
        ESP_LOGE(TAG, "Failed to reinitialize MP3 decoder after reset");
    }

    Application::GetInstance().GetAudioService().ResetDecoder();
    if (flush_i2s) {
        FlushCodecSilence();
    }
    ESP_LOGI(TAG, "Playback pipeline reset (clear_stream=%d flush_i2s=%d)",
             (int)clear_stream_buffer, (int)flush_i2s);
}

void Esp32Music::SignalPlaybackAbort()
{
    is_downloading_ = false;
    is_playing_ = false;
    buffer_cv_.notify_all();
}

uint8_t* Esp32Music::AllocStreamChunk()
{
    {
        std::lock_guard<std::mutex> lock(chunk_pool_mutex_);
        if (!chunk_pool_.empty()) {
            uint8_t* p = chunk_pool_.back();
            chunk_pool_.pop_back();
            return p;
        }
    }
    return (uint8_t*)heap_caps_malloc(STREAM_CHUNK_SIZE, MALLOC_CAP_SPIRAM);
}

void Esp32Music::FreeStreamChunk(uint8_t* p)
{
    if (p == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(chunk_pool_mutex_);
    if (chunk_pool_.size() < CHUNK_POOL_MAX) {
        chunk_pool_.push_back(p);
        return;
    }
    heap_caps_free(p);
}

void Esp32Music::TrimChunkPool()
{
    std::lock_guard<std::mutex> lock(chunk_pool_mutex_);
    while (chunk_pool_.size() > CHUNK_POOL_KEEP) {
        uint8_t* p = chunk_pool_.back();
        chunk_pool_.pop_back();
        heap_caps_free(p);
    }
}

// 初始化MP3解码器
bool Esp32Music::InitializeMp3Decoder()
{
    mp3_decoder_ = MP3InitDecoder();
    if (mp3_decoder_ == nullptr)
    {
        ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
        mp3_decoder_initialized_ = false;
        return false;
    }

    mp3_decoder_initialized_ = true;
    ESP_LOGI(TAG, "MP3 decoder initialized successfully");
    return true;
}

// 清理MP3解码器
void Esp32Music::CleanupMp3Decoder()
{
    if (mp3_decoder_ != nullptr)
    {
        MP3FreeDecoder(mp3_decoder_);
        mp3_decoder_ = nullptr;
    }
    mp3_decoder_initialized_ = false;
    ESP_LOGI(TAG, "MP3 decoder cleaned up");
}

// 重置采样率到原始值
void Esp32Music::ResetSampleRate()
{
    auto &board = Board::GetInstance();
    auto codec = board.GetAudioCodec();
    if (codec && codec->original_output_sample_rate() > 0 &&
        codec->output_sample_rate() != codec->original_output_sample_rate())
    {
        ESP_LOGI(TAG, "重置采样率：从 %d Hz 重置到原始值 %d Hz",
                 codec->output_sample_rate(), codec->original_output_sample_rate());
        if (codec->SetOutputSampleRate(-1))
        { // -1 表示重置到原始值
            ESP_LOGI(TAG, "成功重置采样率到原始值: %d Hz", codec->output_sample_rate());
        }
        else
        {
            ESP_LOGW(TAG, "无法重置采样率到原始值");
        }
    }
}

// 跳过MP3文件开头的ID3标签
size_t Esp32Music::SkipId3Tag(uint8_t *data, size_t size)
{
    if (!data || size < 10)
    {
        return 0;
    }

    // 检查ID3v2标签头 "ID3"
    if (memcmp(data, "ID3", 3) != 0)
    {
        return 0;
    }

    // 计算标签大小（synchsafe integer格式）
    uint32_t tag_size = ((uint32_t)(data[6] & 0x7F) << 21) |
                        ((uint32_t)(data[7] & 0x7F) << 14) |
                        ((uint32_t)(data[8] & 0x7F) << 7) |
                        ((uint32_t)(data[9] & 0x7F));

    // ID3v2头部(10字节) + 标签内容
    size_t total_skip = 10 + tag_size;

    // 确保不超过可用数据大小
    if (total_skip > size)
    {
        total_skip = size;
    }

    ESP_LOGI(TAG, "Found ID3v2 tag, skipping %u bytes", (unsigned int)total_skip);
    return total_skip;
}

// 下载歌词
bool Esp32Music::DownloadLyrics(const std::string &lyric_url)
{
    //ESP_LOGI(TAG, "Downloading lyrics from: %s", lyric_url.c_str());

    // 检查URL是否为空
    if (lyric_url.empty())
    {
        ESP_LOGE(TAG, "Lyric URL is empty!");
        return false;
    }

    // 添加重试逻辑
    const int max_retries = 3;
    int retry_count = 0;
    bool success = false;
    std::string lyric_content;
    std::string current_url = lyric_url;
    int redirect_count = 0;
    const int max_redirects = 5; // 最多允许5次重定向

    while (retry_count < max_retries && !success && redirect_count < max_redirects)
    {
        if (retry_count > 0)
        {
            ESP_LOGI(TAG, "Retrying lyric download (attempt %d of %d)", retry_count + 1, max_retries);
            // 重试前暂停一下
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        // 使用Board提供的HTTP客户端
        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(0);
        if (!http)
        {
            ESP_LOGE(TAG, "Failed to create HTTP client for lyric download");
            retry_count++;
            continue;
        }

        // 设置基本请求头
        http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
        http->SetHeader("Accept", "text/plain");

        // 添加ESP32认证头
        //add_auth_headers(http.get());

        // 打开GET连接
        if (!http->Open("GET", current_url))
        {
            ESP_LOGE(TAG, "Failed to open HTTP connection for lyrics");
            // 移除delete http; 因为unique_ptr会自动管理内存
            retry_count++;
            continue;
        }

        // 检查HTTP状态码
        int status_code = http->GetStatusCode();
        ESP_LOGI(TAG, "Lyric download HTTP status code: %d", status_code);

        // 处理重定向 - 由于Http类没有GetHeader方法，我们只能根据状态码判断
        if (status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 || status_code == 308)
        {
            // 由于无法获取Location头，只能报告重定向但无法继续
            ESP_LOGW(TAG, "Received redirect status %d but cannot follow redirect (no GetHeader method)", status_code);
            http->Close();
            retry_count++;
            continue;
        }

        // 非200系列状态码视为错误
        if (status_code < 200 || status_code >= 300)
        {
            ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
            http->Close();
            retry_count++;
            continue;
        }

        // 读取响应
        lyric_content.clear();
        char buffer[1024];
        int bytes_read;
        bool read_error = false;
        int total_read = 0;

        // 由于无法获取Content-Length和Content-Type头，我们不知道预期大小和内容类型
        ESP_LOGD(TAG, "Starting to read lyric content");

        while (true)
        {
            bytes_read = http->Read(buffer, sizeof(buffer) - 1);
            // ESP_LOGD(TAG, "Lyric HTTP read returned %d bytes", bytes_read); // 注释掉以减少日志输出

            if (bytes_read > 0)
            {
                buffer[bytes_read] = '\0';
                lyric_content += buffer;
                total_read += bytes_read;

                // 定期打印下载进度 - 改为DEBUG级别减少输出
                if (total_read % 4096 == 0)
                {
                    ESP_LOGD(TAG, "Downloaded %d bytes so far", total_read);
                }
            }
            else if (bytes_read == 0)
            {
                // 正常结束，没有更多数据
                ESP_LOGD(TAG, "Lyric download completed, total bytes: %d", total_read);
                success = true;
                break;
            }
            else
            {
                // bytes_read < 0，可能是ESP-IDF的已知问题
                // 如果已经读取到了一些数据，则认为下载成功
                if (!lyric_content.empty())
                {
                    ESP_LOGW(TAG, "HTTP read returned %d, but we have data (%d bytes), continuing", bytes_read, lyric_content.length());
                    success = true;
                    break;
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to read lyric data: error code %d", bytes_read);
                    read_error = true;
                    break;
                }
            }
        }

        http->Close();

        if (read_error)
        {
            retry_count++;
            continue;
        }

        // 如果成功读取数据，跳出重试循环
        if (success)
        {
            break;
        }
    }

    // 检查是否超过了最大重试次数
    if (retry_count >= max_retries)
    {
        ESP_LOGE(TAG, "Failed to download lyrics after %d attempts", max_retries);
        return false;
    }

    // 记录前几个字节的数据，帮助调试
    if (!lyric_content.empty())
    {
        size_t preview_size = std::min(lyric_content.size(), size_t(50));
        std::string preview = lyric_content.substr(0, preview_size);
        ESP_LOGD(TAG, "Lyric content preview (%d bytes): %s", lyric_content.length(), preview.c_str());
    }
    else
    {
        ESP_LOGE(TAG, "Failed to download lyrics or lyrics are empty");
        return false;
    }

    ESP_LOGI(TAG, "Lyrics downloaded successfully, size: %d bytes", lyric_content.length());
    return ParseLyrics(lyric_content);
}

// 解析歌词
bool Esp32Music::ParseLyrics(const std::string &lyric_content)
{
    ESP_LOGI(TAG, "Parsing lyrics content");

    // 使用锁保护lyrics_数组访问
    std::lock_guard<std::mutex> lock(lyrics_mutex_);

    lyrics_.clear();

    // 按行分割歌词内容
    std::istringstream stream(lyric_content);
    std::string line;

    while (std::getline(stream, line))
    {
        // 去除行尾的回车符
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        // 跳过空行
        if (line.empty())
        {
            continue;
        }

        // 解析LRC格式: [mm:ss.xx]歌词文本
        if (line.length() > 10 && line[0] == '[')
        {
            size_t close_bracket = line.find(']');
            if (close_bracket != std::string::npos)
            {
                std::string tag_or_time = line.substr(1, close_bracket - 1);
                std::string content = line.substr(close_bracket + 1);

                // 检查是否是元数据标签而不是时间戳
                // 元数据标签通常是 [ti:标题], [ar:艺术家], [al:专辑] 等
                size_t colon_pos = tag_or_time.find(':');
                if (colon_pos != std::string::npos)
                {
                    std::string left_part = tag_or_time.substr(0, colon_pos);

                    // 检查冒号左边是否是时间（数字）
                    bool is_time_format = true;
                    for (char c : left_part)
                    {
                        if (!isdigit(c))
                        {
                            is_time_format = false;
                            break;
                        }
                    }

                    // 如果不是时间格式，跳过这一行（元数据标签）
                    if (!is_time_format)
                    {
                        // 可以在这里处理元数据，例如提取标题、艺术家等信息
                        ESP_LOGD(TAG, "Skipping metadata tag: [%s]", tag_or_time.c_str());
                        continue;
                    }

                    // 是时间格式，解析时间戳
                    try
                    {
                        int minutes = std::stoi(tag_or_time.substr(0, colon_pos));
                        float seconds = std::stof(tag_or_time.substr(colon_pos + 1));
                        int timestamp_ms = minutes * 60 * 1000 + (int)(seconds * 1000);

                        // 安全处理歌词文本，确保UTF-8编码正确
                        std::string safe_lyric_text;
                        if (!content.empty())
                        {
                            // 创建安全副本并验证字符串
                            safe_lyric_text = content;
                            // 确保字符串以null结尾
                            safe_lyric_text.shrink_to_fit();
                        }

                        lyrics_.push_back(std::make_pair(timestamp_ms, safe_lyric_text));

                        if (!safe_lyric_text.empty())
                        {
                            // 限制日志输出长度，避免中文字符截断问题
                            size_t log_len = std::min(safe_lyric_text.length(), size_t(50));
                            std::string log_text = safe_lyric_text.substr(0, log_len);
                            ESP_LOGD(TAG, "Parsed lyric: [%d ms] %s", timestamp_ms, log_text.c_str());
                        }
                        else
                        {
                            ESP_LOGD(TAG, "Parsed lyric: [%d ms] (empty)", timestamp_ms);
                        }
                    }
                    catch (const std::exception &e)
                    {
                        ESP_LOGW(TAG, "Failed to parse time: %s", tag_or_time.c_str());
                    }
                }
            }
        }
    }

    // 按时间戳排序
    std::sort(lyrics_.begin(), lyrics_.end());

    ESP_LOGI(TAG, "Parsed %d lyric lines", lyrics_.size());
    return !lyrics_.empty();
}

// 歌词显示线程
void Esp32Music::LyricDisplayThread()
{
    ESP_LOGI(TAG, "Lyric display thread started");

    // 优先使用 getMusicDetails 直接返回的 lrctxt（与 lyric_url 内容相同，避免重复 HTTP 下载）。
    // 只有 lrctxt 为空时才退回到下载 lyric_url。
    bool ok = false;
    if (!current_lyric_text_.empty()) {
        ESP_LOGI(TAG, "Using inline lyric text (%d bytes), skip URL download",
                 current_lyric_text_.length());
        ok = ParseLyrics(current_lyric_text_);
    } else if (!current_lyric_url_.empty()) {
        ok = DownloadLyrics(current_lyric_url_);
    } else {
        ESP_LOGE(TAG, "No lyric data source available");
    }

    if (!ok) {
        ESP_LOGE(TAG, "Failed to load lyrics");
        is_lyric_running_ = false;
        return;
    }

    current_lyric_text_.clear();
    current_lyric_text_.shrink_to_fit();

    // 定期检查是否需要更新显示(频率可以降低)
    while (is_lyric_running_ && is_playing_)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ESP_LOGI(TAG, "Lyric display thread finished");
}

void Esp32Music::ReleaseFftPcm()
{
    fft_pcm_seq_.fetch_add(1, std::memory_order_relaxed);
    int16_t* p = final_pcm_data_fft;
    final_pcm_data_fft = nullptr;
    fft_pcm_samples_.store(0, std::memory_order_relaxed);
    fft_pcm_seq_.fetch_add(1, std::memory_order_release);
    if (p != nullptr) {
        heap_caps_free(p);
    }
}

void Esp32Music::PublishPcmForFft(const int16_t* pcm, int sample_count)
{
    if (pcm == nullptr || sample_count <= 0) {
        return;
    }
    if (final_pcm_data_fft == nullptr) {
        final_pcm_data_fft = (int16_t*)heap_caps_malloc(
            FFT_PCM_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (final_pcm_data_fft == nullptr) {
            return;
        }
    }

    int n = sample_count;
    if (n > FFT_PCM_SAMPLES) {
        n = FFT_PCM_SAMPLES;
    }

    fft_pcm_seq_.fetch_add(1, std::memory_order_relaxed);
    memcpy(final_pcm_data_fft, pcm, (size_t)n * sizeof(int16_t));
    if (n < FFT_PCM_SAMPLES) {
        memset(final_pcm_data_fft + n, 0, (size_t)(FFT_PCM_SAMPLES - n) * sizeof(int16_t));
    }
    fft_pcm_samples_.store(FFT_PCM_SAMPLES, std::memory_order_relaxed);
    fft_pcm_seq_.fetch_add(1, std::memory_order_release);
}

bool Esp32Music::CopyPcmForFft(int16_t* dst, size_t max_samples)
{
    if (dst == nullptr || max_samples == 0) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        uint32_t s1 = fft_pcm_seq_.load(std::memory_order_acquire);
        if (s1 & 1u) {
            continue;
        }
        int16_t* src = final_pcm_data_fft;
        int n = fft_pcm_samples_.load(std::memory_order_relaxed);
        if (src == nullptr || n <= 0) {
            return false;
        }
        size_t copy_n = std::min(max_samples, (size_t)n);
        memcpy(dst, src, copy_n * sizeof(int16_t));
        uint32_t s2 = fft_pcm_seq_.load(std::memory_order_acquire);
        if (s1 == s2) {
            return true;
        }
    }
    return false;
}

void Esp32Music::TickPlaybackUi()
{
    if (!is_playing_.load()) {
        return;
    }
    // I2S DMA 里还有已解码未播出的 PCM；歌词轴要比解码时钟略提前。
    constexpr int buffer_latency_ms = 800;
    UpdateLyricDisplay(current_play_time_ms_.load() + buffer_latency_ms);
}

void Esp32Music::UpdateLyricDisplay(int64_t current_time_ms)
{
    std::lock_guard<std::mutex> lock(lyrics_mutex_);

    if (lyrics_.empty())
    {
        auto &board = Board::GetInstance();
        auto display = board.GetDisplay();
        if (display) {
            display->SetMusicProgress(static_cast<int>(current_time_ms), nullptr, nullptr);
        }
        return;
    }

    // 查找当前应该显示的歌词
    int new_lyric_index = -1;

    // 从当前歌词索引开始查找，提高效率
    int start_index = (current_lyric_index_.load() >= 0) ? current_lyric_index_.load() : 0;

    // 正向查找：找到最后一个时间戳小于等于当前时间的歌词
    for (int i = start_index; i < (int)lyrics_.size(); i++)
    {
        if (lyrics_[i].first <= current_time_ms)
        {
            new_lyric_index = i;
        }
        else
        {
            break; // 时间戳已超过当前时间
        }
    }

    // 如果没有找到(可能当前时间比第一句歌词还早)，显示空
    if (new_lyric_index == -1)
    {
        new_lyric_index = -1;
    }

    // 如果歌词索引发生变化，更新显示
    std::string lyric_text;
    std::string lyric_next_text;
    if (new_lyric_index >= 0 && new_lyric_index < (int)lyrics_.size())
    {
        lyric_text = lyrics_[new_lyric_index].second;
        /* 下一句：给"接下来要唱"的预览行用。
         * 索引 +1 越界或当前是最后一句时给空串，display 那边按 "" 清掉预览。 */
        int next_idx = new_lyric_index + 1;
        if (next_idx < (int)lyrics_.size()) {
            lyric_next_text = lyrics_[next_idx].second;
        }
    }

    if (new_lyric_index != current_lyric_index_)
    {
        current_lyric_index_ = new_lyric_index;

        auto &board = Board::GetInstance();
        auto display = board.GetDisplay();
        if (display)
        {
            /* 把"当前播放时间 + 当前歌词 + 下一句歌词"一起发到 display：
             *  - SetMusicProgress 内部会更新进度条与两个歌词 label
             *  - 这里只在新一句歌词触发时才调用，避免每帧重复写 label */
            display->SetMusicProgress(static_cast<int>(current_time_ms),
                                      lyric_text.c_str(),
                                      lyric_next_text.c_str());
            ESP_LOGD(TAG, "Lyric update at %lldms: cur=%s next=%s",
                     current_time_ms,
                     lyric_text.empty() ? "(none)" : lyric_text.c_str(),
                     lyric_next_text.empty() ? "(none)" : lyric_next_text.c_str());
        }
    } else {
        /* 歌词没换，但播放时间在走。每隔若干帧刷一次进度条/时间标签即可，
         * 太频繁反而抢 LVGL。50ms 一次对用户而言刷新率足够。
         * lyric/lyric_next 都传 nullptr，让 display 沿用上次显示。 */
        auto &board = Board::GetInstance();
        auto display = board.GetDisplay();
        if (display)
        {
            display->SetMusicProgress(static_cast<int>(current_time_ms),
                                      nullptr,
                                      nullptr);
        }
    }
}

// 删除复杂的认证初始化方法，使用简单的静态函数

// 删除复杂的类方法，使用简单的静态函数

/**
 * @brief 添加认证头到HTTP请求
 * @param http_client HTTP客户端指针
 *
 * 添加的认证头包括：
 * - X-MAC-Address: 设备MAC地址
 * - X-Chip-ID: 设备芯片ID
 * - X-Timestamp: 当前时间戳
 * - X-Dynamic-Key: 动态生成的密钥
 */
// 删除复杂的AddAuthHeaders方法，使用简单的静态函数

// 删除复杂的认证验证和配置方法，使用简单的静态函数

// 显示模式控制方法实现
void Esp32Music::SetDisplayMode(DisplayMode mode)
{
    /**DisplayMode old_mode = display_mode_.load();
    display_mode_ = mode;

    ESP_LOGI(TAG, "Display mode changed from %s to %s",
             (old_mode == DISPLAY_MODE_SPECTRUM) ? "SPECTRUM" : "LYRICS",
             (mode == DISPLAY_MODE_SPECTRUM) ? "SPECTRUM" : "LYRICS");**/
}
