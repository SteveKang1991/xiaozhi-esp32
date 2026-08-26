#ifndef ESP32_MUSIC_H
#define ESP32_MUSIC_H

#include <string>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>

#include "music.h"

// MP3解码器支持
extern "C" {
#include "mp3dec.h"
}

// 前向声明 cJSON，避免在头文件中引入 cJSON.h
struct cJSON;

// 音频数据块结构
struct AudioChunk {
    uint8_t* data;
    size_t size;
    
    AudioChunk() : data(nullptr), size(0) {}
    AudioChunk(uint8_t* d, size_t s) : data(d), size(s) {}
};

class Esp32Music : public Music {
public:
    // 显示模式控制 - 移动到public区域
    enum DisplayMode {
        DISPLAY_MODE_SPECTRUM = 0,  // 默认显示频谱
        DISPLAY_MODE_LYRICS = 1     // 显示歌词
    };

private:
    std::string last_downloaded_data_;
    std::string current_music_url_;
    std::string current_song_name_;
    bool song_name_displayed_;
    
    // 歌词相关
    std::string current_lyric_url_;
    std::string current_lyric_text_;  // 首次 getMusicDetails 返回的 lrctxt（避免重复下载）
    std::vector<std::pair<int, std::string>> lyrics_;  // 时间戳和歌词文本
    std::mutex lyrics_mutex_;  // 保护lyrics_数组的互斥锁
    std::atomic<int> current_lyric_index_;
    std::thread lyric_thread_;
    std::atomic<bool> is_lyric_running_;

    // 专辑封面相关
    std::string current_picture_url_;  // 专辑封面 URL
    
    std::atomic<DisplayMode> display_mode_;
    std::atomic<bool> is_playing_;
    std::atomic<bool> is_downloading_;
    std::thread play_thread_;
    std::thread download_thread_;
    int64_t current_play_time_ms_;  // 当前播放时间(毫秒)
    int64_t last_frame_time_ms_;    // 上一帧的时间戳
    int total_frames_decoded_;      // 已解码的帧数

    // 音频缓冲区
    std::queue<AudioChunk> audio_buffer_;
    std::mutex buffer_mutex_;
    std::condition_variable buffer_cv_;
    size_t buffer_size_;
    int64_t download_start_time_ms_;  // 记录下载开始时间，用于超时检测
    static constexpr size_t MAX_BUFFER_SIZE = 256 * 1024;  // 256KB缓冲区（降低以减少brownout风险）
    static constexpr size_t MIN_BUFFER_SIZE = 32 * 1024;   // 32KB最小播放缓冲（降低以减少brownout风险）
    static constexpr int64_t DOWNLOAD_TIMEOUT_MS = 3000;   // 3秒下载超时，防止死等
    
    // MP3解码器相关
    HMP3Decoder mp3_decoder_;
    MP3FrameInfo mp3_frame_info_;
    bool mp3_decoder_initialized_;
    
    // 私有方法
    void DownloadAudioStream(const std::string& music_url);
    void PlayAudioStream();
    void ClearAudioBuffer();
    bool InitializeMp3Decoder();
    void CleanupMp3Decoder();
    void ResetSampleRate();  // 重置采样率到原始值
    
    // 歌词相关私有方法
    bool DownloadLyrics(const std::string& lyric_url);
    bool ParseLyrics(const std::string& lyric_content);
    void LyricDisplayThread();
    void UpdateLyricDisplay(int64_t current_time_ms);

    // 启动歌词线程（歌词模式开启时调用），供 HandleMusicDetailsJson 共用
    void StartLyricThreadIfNeeded(const std::string& song_name);

    // 解析音乐详情 JSON 并启动播放与歌词。
    //   is_backup_api = false  -> 主接口 qq_plus，字段: data.musicurl/singer/interval/viplrc/lrctxt
    //   is_backup_api = true   -> 备用接口 wyvip，  字段: data.url/songname/vipmusic.duration/music.lrc/lrcurl
    // 成功返回 true；audio_url 缺失或为空返回 false（让调用方决定是否继续尝试备用 URL）。
    bool HandleMusicDetailsJson(cJSON* response_json, const std::string& song_name, bool is_backup_api);
    
    // ID3标签处理
    size_t SkipId3Tag(uint8_t* data, size_t size);

    std::vector<int16_t> mono_buffer_;  // 双→单声道转换复用 buffer，构造函数预分配

public:
    Esp32Music();
    ~Esp32Music();

    virtual bool Download(const std::string& song_name, const std::string& artist_name) override;
  
    virtual std::string GetDownloadResult() override;
    
    // 新增方法
    virtual bool StartStreaming(const std::string& music_url) override;
    virtual bool StopStreaming() override;  // 停止流式播放
    virtual size_t GetBufferSize() const override { return buffer_size_; }
    virtual bool IsDownloading() const override { return is_downloading_; }
    virtual int16_t* GetAudioData() override { return nullptr; }  // 不再使用 FFT，保留接口满足 music.h 抽象
    virtual bool IsPlaying() const override { return is_playing_; }
    
    // 专辑封面相关
    std::string GetPictureUrl() const override { return current_picture_url_; }
    
    // 显示模式控制方法
    void SetDisplayMode(DisplayMode mode);
    DisplayMode GetDisplayMode() const { return display_mode_.load(); }
};

#endif // ESP32_MUSIC_H
