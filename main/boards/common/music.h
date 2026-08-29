#ifndef MUSIC_H
#define MUSIC_H

#include <string>
#include <cstddef>
#include <cstdint>

class Music {
public:
    virtual ~Music() = default;  // 添加虚析构函数
    
    virtual bool Download(const std::string& song_name, const std::string& artist_name = "") = 0;
    virtual std::string GetDownloadResult() = 0;
    virtual std::string GetPictureUrl() const { return ""; }  // 获取专辑封面 URL
    virtual bool StartStreaming(const std::string& music_url) = 0;
    virtual bool StopStreaming() = 0;  // 停止流式播放
    virtual size_t GetBufferSize() const = 0;
    virtual bool IsDownloading() const = 0;
    virtual int16_t* GetAudioData() = 0;
    virtual bool IsPlaying() const = 0;  // 是否正在播放音乐

    /** Seqlock-safe copy of the latest PCM frame for FFT. Returns false if none. */
    virtual bool CopyPcmForFft(int16_t* dst, size_t max_samples) { (void)dst; (void)max_samples; return false; }

    /** UI tick from the FFT task (lyrics + progress). play_thread must not call LVGL. */
    virtual void TickPlaybackUi() {}
};

#endif // MUSIC_H 