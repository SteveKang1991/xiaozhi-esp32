#include "sd_scanner.h"
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

static const char *SD_TAG = "SD扫描器";

// 全局文件夹信息缓存
static sd_folder_info_t g_folder_cache[10];
static int g_folder_count = 0;

// ==================== 私有函数声明 ====================

// 获取文件大小的字符串表示
static void format_file_size(size_t size, char *buffer, size_t buffer_size)
{
    if (size < 1024) {
        snprintf(buffer, buffer_size, "%zu B", size);
    } else if (size < 1024 * 1024) {
        snprintf(buffer, buffer_size, "%.1f KB", size / 1024.0);
    } else {
        snprintf(buffer, buffer_size, "%.1f MB", size / (1024.0 * 1024.0));
    }
}

// 获取文件类型和emoji
static const char* get_file_type_with_emoji(const char *filename)
{
    const char *ext = strrchr(filename, '.');
    if (!ext) return "未知";
    
    if (strcasecmp(ext, ".mp3") == 0) return "音频";
    if (strcasecmp(ext, ".wav") == 0) return "音频";
    if (strcasecmp(ext, ".mjpeg") == 0) return "视频";
    if (strcasecmp(ext, ".mp4") == 0) return "视频";
    if (strcasecmp(ext, ".avi") == 0) return "视频";
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) return "图片";
    if (strcasecmp(ext, ".png") == 0) return "图片";
    if (strcasecmp(ext, ".bmp") == 0) return "图片";
    if (strcasecmp(ext, ".gif") == 0) return "图片";
    if (strcasecmp(ext, ".txt") == 0) return "文本";
    if (strcasecmp(ext, ".log") == 0) return "日志";
    if (strcasecmp(ext, ".bin") == 0) return "二进制";
    if (strcasecmp(ext, ".dat") == 0) return "数据";
    if (strcasecmp(ext, ".json") == 0) return "配置";
    if (strcasecmp(ext, ".xml") == 0) return "配置";
    
    return "其他";
}

// 检查是否应该跳过的目录
static bool should_skip_directory(const char *dirname)
{
    // 跳过系统目录
    if (strcmp(dirname, "System Volume Information") == 0) return true;
    if (strcmp(dirname, "$RECYCLE.BIN") == 0) return true;
    if (strncmp(dirname, ".", 1) == 0) return true; // 跳过隐藏目录
    
    return false;
}

// 打印目录标题
static void print_directory_header(const char *dir_name)
{
    ESP_LOGI(SD_TAG, "");
    ESP_LOGI(SD_TAG, "📄 正在扫描目录: %s", dir_name);
    ESP_LOGI(SD_TAG, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
}

// 打印文件信息行（简化版）
static void print_file_info(const char *filename, const char *type_emoji, const char *size)
{
    ESP_LOGI(SD_TAG, "📝   %-12s %s (%s)", type_emoji, filename, size);
}

// 打印目录信息行
static void print_dir_info(const char *dirname)
{
    ESP_LOGI(SD_TAG, "  📄 目录      %s", dirname);
}

// 获取文件夹类型
static int get_folder_type(const char *folder_name) {
    if (strcasecmp(folder_name, "mjpeg") == 0 || 
        strstr(folder_name, "video") != NULL ||
        strstr(folder_name, "mp4") != NULL) {
        return 2; // 视频
    } else if (strcasecmp(folder_name, "mp3") == 0 || 
               strstr(folder_name, "music") != NULL ||
               strstr(folder_name, "audio") != NULL) {
        return 3; // 音乐
    } else if (strcasecmp(folder_name, "imgdiet") == 0 || 
               strstr(folder_name, "photo") != NULL ||
               strstr(folder_name, "image") != NULL ||
               strstr(folder_name, "picture") != NULL) {
        return 1; // 图片
    }
    return 0; // 其他
}

// 扫描单个目录中的文件
static void scan_directory_files(const char *dir_path, const char *display_name, int *total_files, int *total_dirs)
{
    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGW(SD_TAG, "❌ 无法打开目录: %s", dir_path);
        return;
    }

    struct dirent *entry;
    struct stat file_stat;
    char full_path[512];
    char size_str[32];
    int dir_file_count = 0;
    int dir_folder_count = 0;
    bool is_root_dir = (strcmp(display_name, "SD卡根目录") == 0);
    
    print_directory_header(display_name);
    
    while ((entry = readdir(dir)) != NULL) {
        // 跳过当前目录和父目录
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        
        if (stat(full_path, &file_stat) == 0) {
            if (S_ISDIR(file_stat.st_mode)) {
                // 跳过系统目录
                if (should_skip_directory(entry->d_name)) {
                    continue;
                }
                // 这是一个目录
                print_dir_info(entry->d_name);
                (*total_dirs)++;
                dir_folder_count++;
                
                // 如果是根目录下的文件夹，计算其中的文件数量并保存信息
                if (is_root_dir && g_folder_count < 10) {
                    DIR *subdir = opendir(full_path);
                    int file_count = 0;
                    if (subdir) {
                        struct dirent *subentry;
                        while ((subentry = readdir(subdir)) != NULL) {
                            if (strcmp(subentry->d_name, ".") != 0 && strcmp(subentry->d_name, "..") != 0) {
                                file_count++;
                            }
                        }
                        closedir(subdir);
                    }
                    
                    // 保存文件夹信息
                    snprintf(g_folder_cache[g_folder_count].name, sizeof(g_folder_cache[g_folder_count].name), "%.63s", entry->d_name);
                    g_folder_cache[g_folder_count].file_count = file_count;
                    g_folder_cache[g_folder_count].type = get_folder_type(entry->d_name);
                    g_folder_count++;
                }
            } else {
                // 这是一个文件
                format_file_size(file_stat.st_size, size_str, sizeof(size_str));
                print_file_info(entry->d_name, get_file_type_with_emoji(entry->d_name), size_str);
                (*total_files)++;
                dir_file_count++;
            }
        }
    }
    
    ESP_LOGI(SD_TAG, "  📊 统计: %d个文件, %d个文件夹", dir_file_count, dir_folder_count);
    closedir(dir);
}

// 递归扫描所有目录
static void scan_all_directories(const char *base_path, int *total_files, int *total_dirs)
{
    DIR *dir = opendir(base_path);
    if (!dir) {
        ESP_LOGW(SD_TAG, "❌ 无法打开根目录: %s", base_path);
        return;
    }

    struct dirent *entry;
    struct stat file_stat;
    char full_path[512];
    
    // 首先扫描根目录
    scan_directory_files(base_path, "SD卡根目录", total_files, total_dirs);
    
    // 重新打开目录以扫描子目录
    rewinddir(dir);
    
    while ((entry = readdir(dir)) != NULL) {
        // 跳过当前目录和父目录
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        // 跳过系统目录
        if (should_skip_directory(entry->d_name)) {
            continue;
        }
        
        snprintf(full_path, sizeof(full_path), "%s/%s", base_path, entry->d_name);
        
        if (stat(full_path, &file_stat) == 0 && S_ISDIR(file_stat.st_mode)) {
            // 扫描子目录
            scan_directory_files(full_path, entry->d_name, total_files, total_dirs);
        }
    }
    
    closedir(dir);
}

// ==================== 公有函数实现 ====================

esp_err_t sd_scanner_init(sd_scanner_handle_t *handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    *handle = malloc(sizeof(sd_scanner_t));
    if (!*handle) {
        ESP_LOGE(SD_TAG, "❌ 内存分配失败");
        return ESP_ERR_NO_MEM;
    }

    sd_scanner_t *scanner = *handle;
    scanner->is_mounted = false;
    scanner->card = NULL;
#if SOC_SDMMC_IO_POWER_EXTERNAL
    scanner->pwr_ctrl_handle = NULL;
#endif

    // SD卡挂载配置（高性能模式）
    scanner->mount_config = (esp_vfs_fat_sdmmc_mount_config_t) {
        .format_if_mount_failed = false,
        .max_files = 20,  // 增加最大文件数，支持更多并发
        .allocation_unit_size = 512 * 1024,  // 🚀 大幅提升allocation_unit_size至512KB，加速大文件顺序读取
        .use_one_fat = false,
        .disk_status_check_enable = false
    };

    ESP_LOGI(SD_TAG, "🔧 SD扫描器初始化完成");
    return ESP_OK;
}

esp_err_t sd_scanner_mount(sd_scanner_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    sd_scanner_t *scanner = handle;
    esp_err_t ret;

    if (scanner->is_mounted) {
        ESP_LOGW(SD_TAG, "⚠️ SD卡已经挂载");
        return ESP_OK;
    }

    ESP_LOGI(SD_TAG, "🔧 正在初始化SD卡...");

    // 使用SDMMC外设（尽可能高的安全频率）
    ESP_LOGI(SD_TAG, "💾 使用SDMMC外设");
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    // 高速模式（40MHz），若卡或布线不支持会在初始化协商时回退
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.slot = SDMMC_HOST_SLOT_0;
    // 移除DDR模式，保持标准速度
    // 设置合适的读取超时
    host.command_timeout_ms = 5000;
    // 启用DMA提高传输效率
    host.flags |= SDMMC_HOST_FLAG_DEINIT_ARG | SDMMC_HOST_FLAG_4BIT;
    // 移除1位模式标志，强制4线模式
    host.flags &= ~SDMMC_HOST_FLAG_1BIT;
    ESP_LOGI(SD_TAG, "💾 SD卡标准速度模式配置: 频率=%d kHz, 超时=%d ms", host.max_freq_khz, host.command_timeout_ms);

    // 电源控制配置（ESP32-P4专用）
#if SOC_SDMMC_IO_POWER_EXTERNAL
    ESP_LOGI(SD_TAG, "⚡ 启用ESP32-P4片上LDO电源控制，LDO通道ID: 4");
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,
    };

    ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &scanner->pwr_ctrl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(SD_TAG, "❌ 创建片上LDO电源控制驱动失败: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(SD_TAG, "🔧 片上LDO电源控制驱动创建成功");
    host.pwr_ctrl_handle = scanner->pwr_ctrl_handle;
#else
    ESP_LOGW(SD_TAG, "⚠️ 未启用ESP32-P4片上LDO电源控制");
#endif

    // 配置SDMMC插槽和引脚（高速模式）
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;  // 4线模式
    
    // 使用预定义的引脚配置
    slot_config.clk = SDMMC_CLK_PIN;
    slot_config.cmd = SDMMC_CMD_PIN;
    slot_config.d0 = SDMMC_D0_PIN;
    slot_config.d1 = SDMMC_D1_PIN;
    slot_config.d2 = SDMMC_D2_PIN;
    slot_config.d3 = SDMMC_D3_PIN;
    
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    // 如硬件满足UHS-I（1.8V）与走线条件，可启用以下选项
    // slot_config.flags |= SDMMC_SLOT_FLAG_UHS1;  // 谨慎启用

    ESP_LOGI(SD_TAG, "💾 正在挂载文件系统...");
    ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &scanner->mount_config, &scanner->card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(SD_TAG, "❌ 挂载文件系统失败。如果需要格式化SD卡，请设置相应的配置选项。");
        } else {
            ESP_LOGE(SD_TAG, "❌ 初始化SD卡失败 (%s)。请确保SD卡线路有上拉电阻。", esp_err_to_name(ret));
        }
        return ret;
    }

    scanner->is_mounted = true;
    ESP_LOGI(SD_TAG, "✅ 文件系统挂载成功");

    // 打印SD卡信息
    sdmmc_card_print_info(stdout, scanner->card);

    return ESP_OK;
}

esp_err_t sd_scanner_scan(sd_scanner_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    sd_scanner_t *scanner = handle;
    
    if (!scanner->is_mounted) {
        ESP_LOGE(SD_TAG, "❌ SD卡未挂载，请先调用sd_scanner_mount");
        return ESP_ERR_INVALID_STATE;
    }

    // 开始扫描文件
    ESP_LOGI(SD_TAG, "🔍 开始扫描SD卡文件...");
    
    // 清零文件夹缓存
    g_folder_count = 0;
    memset(g_folder_cache, 0, sizeof(g_folder_cache));
    
    ESP_LOGI(SD_TAG, "═══════════════════════════════════════════════════════════════════════════════");
    
    int total_files = 0;
    int total_dirs = 0;
    
    scan_all_directories(MOUNT_POINT, &total_files, &total_dirs);
    
    ESP_LOGI(SD_TAG, "");
    ESP_LOGI(SD_TAG, "═══════════════════════════════════════════════════════════════════════════════");
    ESP_LOGI(SD_TAG, "✅ 扫描完成！共发现 %d 个文件，%d 个目录", total_files, total_dirs);
    ESP_LOGI(SD_TAG, "✅ 收集到 %d 个文件夹信息", g_folder_count);
    ESP_LOGI(SD_TAG, "═══════════════════════════════════════════════════════════════════════════════");

    return ESP_OK;
}

esp_err_t sd_scanner_unmount(sd_scanner_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    sd_scanner_t *scanner = handle;
    
    if (!scanner->is_mounted) {
        ESP_LOGW(SD_TAG, "⚠️ SD卡未挂载");
        return ESP_OK;
    }

    // 卸载SD卡文件系统
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, scanner->card);
    scanner->is_mounted = false;
    scanner->card = NULL;
    ESP_LOGI(SD_TAG, "💾 SD卡已卸载");

    // 清理电源控制驱动
#if SOC_SDMMC_IO_POWER_EXTERNAL
    if (scanner->pwr_ctrl_handle) {
        esp_err_t ret = sd_pwr_ctrl_del_on_chip_ldo(scanner->pwr_ctrl_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(SD_TAG, "❌ 删除片上LDO电源控制驱动失败");
            return ret;
        }
        scanner->pwr_ctrl_handle = NULL;
    }
#endif

    return ESP_OK;
}

esp_err_t sd_scanner_deinit(sd_scanner_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    sd_scanner_t *scanner = handle;
    
    // 如果还在挂载状态，先卸载
    if (scanner->is_mounted) {
        sd_scanner_unmount(handle);
    }

    free(scanner);
    ESP_LOGI(SD_TAG, "💾 SD扫描器已释放");
    return ESP_OK;
}

int sd_scanner_get_folder_info(sd_folder_info_t *folders, int max_folders)
{
    if (!folders || max_folders <= 0) {
        return 0;
    }
    
    int count = (g_folder_count < max_folders) ? g_folder_count : max_folders;
    for (int i = 0; i < count; i++) {
        folders[i] = g_folder_cache[i];
    }
    
    ESP_LOGI(SD_TAG, "💾 返回 %d 个文件夹信息", count);
    return count;
}

// 全局SD卡扫描器，保持挂载状态
sd_scanner_handle_t g_scanner = NULL;

// 获取全局SD卡扫描器
sd_scanner_handle_t sd_scanner_get_global(void) {
    return g_scanner;
}

// 检查SD卡是否已挂载
bool sd_scanner_is_mounted(void) {
    return (g_scanner != NULL && g_scanner->is_mounted);
}

// 初始化并扫描SD卡（保持挂载状态）
esp_err_t sd_scanner_init_and_scan(void)
{
    ESP_LOGI(SD_TAG, "🔍 初始化SD卡并保持挂载...");

    // 如果已经初始化过，仅释放结构体内存，不卸载SD卡（保持挂载状态供视频播放器使用）
    if (g_scanner != NULL) {
        ESP_LOGI(SD_TAG, "💾 SD卡扫描器已存在，仅释放结构体内存，保持SD卡挂载");
        free(g_scanner);
        g_scanner = NULL;
    }

    // 初始化扫描器
    esp_err_t ret = sd_scanner_init(&g_scanner);
    if (ret != ESP_OK) {
        ESP_LOGE(SD_TAG, "❌ SD扫描器初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 尝试挂载SD卡
    ret = sd_scanner_mount(g_scanner);
    if (ret != ESP_OK) {
        ESP_LOGW(SD_TAG, "⚠️ SD卡挂载失败，可能没有插入SD卡: %s", esp_err_to_name(ret));
        sd_scanner_deinit(g_scanner);
        g_scanner = NULL;
        return ret;
    }

    // 扫描SD卡内容
    ret = sd_scanner_scan(g_scanner);
    if (ret != ESP_OK) {
        ESP_LOGE(SD_TAG, "❌ SD卡扫描失败: %s", esp_err_to_name(ret));
        // 即使扫描失败，也不要卸载SD卡，保持挂载状态供可能的后续使用
        ESP_LOGW(SD_TAG, "⚠️ 扫描失败但保持SD卡挂载状态");
        // 不在这里卸载SD卡
        // sd_scanner_unmount(g_scanner);
        // sd_scanner_deinit(g_scanner);
        // g_scanner = NULL;
        return ret; // 返回错误但不释放资源
    }

    ESP_LOGI(SD_TAG, "✅ SD卡扫描完成，保持挂载状态: %s", g_scanner->is_mounted ? "已挂载" : "未挂载");

    // 确保挂载状态
    if (g_scanner && !g_scanner->is_mounted) {
        ESP_LOGW(SD_TAG, "⚠️ SD卡状态异常，重新挂载");
        ret = sd_scanner_mount(g_scanner);
        if (ret != ESP_OK) {
            ESP_LOGE(SD_TAG, "❌ 重新挂载SD卡失败: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    return ESP_OK;
}
