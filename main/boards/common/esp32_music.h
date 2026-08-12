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

// 音频数据块结构
struct AudioChunk {
    uint8_t* data;
    size_t size;
    
    AudioChunk() : data(nullptr), size(0) {}
    AudioChunk(uint8_t* d, size_t s) : data(d), size(s) {}
};

class Esp32Music : public Music {
public:
    enum DisplayMode {
        DISPLAY_MODE_SPECTRUM = 0,
        DISPLAY_MODE_LYRICS = 1
    };

private:
    std::string last_downloaded_data_;
    std::string current_music_url_;
    std::string current_song_name_;
    bool song_name_displayed_;
    
    std::string current_lyric_url_;
    std::string current_lyric_text_;
    std::vector<std::pair<int, std::string>> lyrics_;
    std::mutex lyrics_mutex_;
    std::atomic<int> current_lyric_index_;
    std::thread lyric_thread_;
    std::atomic<bool> is_lyric_running_;

    std::string current_picture_url_;
    
    std::atomic<DisplayMode> display_mode_;
    std::atomic<bool> is_playing_;
    std::atomic<bool> is_downloading_;
    std::thread play_thread_;
    std::thread download_thread_;
    int64_t current_play_time_ms_;
    int64_t last_frame_time_ms_;
    int total_frames_decoded_;

    std::queue<AudioChunk> audio_buffer_;
    std::mutex buffer_mutex_;
    std::condition_variable buffer_cv_;
    size_t buffer_size_;
    static constexpr size_t MAX_BUFFER_SIZE = 256 * 1024;
    static constexpr size_t MIN_BUFFER_SIZE = 32 * 1024;
    /* ★ skip_enqueue_：StopStreaming 设置后，download 线程不再 enqueue，直接 free chunk 并退出。
     * 解决 cv.wait() 期间 download 持有 buffer_mutex_ 导致 ClearAudioBuffer 阻塞 300ms 的问题。*/
    std::atomic<bool> skip_enqueue_;

    HMP3Decoder mp3_decoder_;
    MP3FrameInfo mp3_frame_info_;
    bool mp3_decoder_initialized_;
    
    void DownloadAudioStream(const std::string& music_url);
    void PlayAudioStream();
    void ClearAudioBuffer();
    bool InitializeMp3Decoder();
    void CleanupMp3Decoder();
    void ResetSampleRate();
    
    bool DownloadLyrics(const std::string& lyric_url);
    bool ParseLyrics(const std::string& lyric_content);
    void LyricDisplayThread();
    void UpdateLyricDisplay(int64_t current_time_ms);
    
    size_t SkipId3Tag(uint8_t* data, size_t size);

    std::vector<int16_t> mono_buffer_;

public:
    Esp32Music();
    ~Esp32Music();

    virtual bool Download(const std::string& song_name, const std::string& artist_name) override;
    virtual std::string GetDownloadResult() override;
    virtual std::string GetPictureUrl() const override { return current_picture_url_; }
    
    virtual bool StartStreaming(const std::string& music_url) override;
    virtual bool StopStreaming() override;
    virtual size_t GetBufferSize() const override { return buffer_size_; }
    virtual bool IsDownloading() const override { return is_downloading_; }
    virtual int16_t* GetAudioData() override { return nullptr; }
    virtual bool IsPlaying() const override { return is_playing_; }
    
    void SetDisplayMode(DisplayMode mode);
    DisplayMode GetDisplayMode() const { return display_mode_.load(); }
};

#endif // ESP32_MUSIC_H
