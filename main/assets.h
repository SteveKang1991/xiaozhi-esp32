#ifndef ASSETS_H
#define ASSETS_H

#include <string>
#include <functional>
#include <memory>

#include <cJSON.h>
#include <esp_partition.h>
#include <model_path.h>
#include <map>
#include <string>

#if HAVE_LVGL
#include <spi_flash_mmap.h>
#endif

struct Asset {
    size_t size;
    size_t offset;
};

// Always forward-declare LvglImage so the getter signature compiles even on
// boards where HAVE_LVGL is 0 (EmoteStrategy path). The actual member is only
// populated on LVGL boards (see LvglStrategy::Apply()).
class LvglImage;

class Assets {
public:
    static Assets& GetInstance() {
        static Assets instance;
        return instance;
    }
    ~Assets();

    bool Download(std::string url, std::function<void(int progress, size_t speed)> progress_callback);
    bool Apply(bool refresh_display_theme = true);
    bool GetAssetData(const std::string& name, void*& ptr, size_t& size);

    inline bool partition_valid() const { return partition_valid_; }
    inline std::string default_assets_url() const { return default_assets_url_; }

// Music playback background image, populated from index.json's
    // "music_bg_image" entry by LvglStrategy::Apply() on boards with
    // HAVE_LVGL=1. Always available (cheap shared_ptr member); just returns
    // nullptr on EmoteStrategy-only boards or before Apply() runs. The
    // signature is unconditional so display code can include the getter
    // without having to repeat the HAVE_LVGL guard at every call site.
    inline std::shared_ptr<LvglImage> music_background_image() const { return music_background_image_; }

private:
    Assets();
    Assets(const Assets&) = delete;
    Assets& operator=(const Assets&) = delete;

    bool InitializePartition();
    void UnApplyPartition();
    static bool FindPartition(Assets* assets);
    static bool LoadSrmodelsFromIndex(Assets* assets, cJSON* root = nullptr);
  
    class AssetStrategy {
    public:
        virtual ~AssetStrategy() = default;
        virtual bool Apply(Assets* assets, bool refresh_display_theme = true) = 0;
        virtual bool InitializePartition(Assets* assets) = 0;
        virtual void UnApplyPartition(Assets* assets) = 0;
        virtual bool GetAssetData(Assets* assets, const std::string& name, void*& ptr, size_t& size) = 0;
    };
    
    class LvglStrategy : public AssetStrategy {
    public:
        bool Apply(Assets* assets, bool refresh_display_theme = true) override;
        bool InitializePartition(Assets* assets) override;
        void UnApplyPartition(Assets* assets) override;
        bool GetAssetData(Assets* assets, const std::string& name, void*& ptr, size_t& size) override;
    private:
        static uint32_t CalculateChecksum(const char* data, uint32_t length);
        std::map<std::string, Asset> assets_;
        esp_partition_mmap_handle_t mmap_handle_ = 0;
        const char* mmap_root_ = nullptr;
        bool checksum_valid_ = false;
    };
    
    class EmoteStrategy : public AssetStrategy {
    public:
        bool Apply(Assets* assets, bool refresh_display_theme = true) override;
        bool InitializePartition(Assets* assets) override;
        void UnApplyPartition(Assets* assets) override;
        bool GetAssetData(Assets* assets, const std::string& name, void*& ptr, size_t& size) override;
    };
    
    // Strategy instance
    std::unique_ptr<AssetStrategy> strategy_;

// Lazily populated in LvglStrategy::Apply() from index.json. The shared_ptr
    // is declared unconditionally (so the getter always compiles); on boards
    // without LVGL it stays nullptr forever.
    std::shared_ptr<LvglImage> music_background_image_ = nullptr;

protected:
    const esp_partition_t* partition_ = nullptr;
    bool partition_valid_ = false;
    std::string default_assets_url_;
    srmodel_list_t* models_list_ = nullptr;
};

#endif
