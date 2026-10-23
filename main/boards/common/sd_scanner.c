#include "sd_scanner.h"
#include "mjpeg_player.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_attr.h"
#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

static const char *SD_TAG = "SD扫描器";

/* RTC 在欠压复位后仍保留，用来识别“没电了还在反复重启”。 */
static RTC_NOINIT_ATTR uint32_t s_sd_boot_magic;
static RTC_NOINIT_ATTR uint32_t s_sd_unstable_boots;
static const uint32_t kSdBootMagic = 0x53444231u;

static volatile int s_sd_writers = 0;
static volatile bool s_writes_allowed = true;
static bool s_shutdown_registered = false;
/* 卡有应答，但 FAT 挂不上。只有这时才允许手动格式化。 */
static bool s_fs_mount_failed = false;
static bool s_formatting = false;

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

    /*
     * 绝不在挂载失败时格式化。allocation_unit_size 只在格式化时生效，
     * FatFS 上限是 128 个扇区（64KB），之前的 512KB 一旦格式化会直接失败或写坏参数。
     * disk_status_check 让总线出错后停止继续写，而不是把脏 FAT 再刷一遍。
     */
    scanner->mount_config = (esp_vfs_fat_sdmmc_mount_config_t) {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
        .use_one_fat = false,
        .disk_status_check_enable = true
    };

    ESP_LOGI(SD_TAG, "🔧 SD扫描器初始化完成");
    return ESP_OK;
}

static void sd_release_ldo(sd_scanner_t *scanner)
{
#if SOC_SDMMC_IO_POWER_EXTERNAL
    if (scanner->pwr_ctrl_handle) {
        sd_pwr_ctrl_del_on_chip_ldo(scanner->pwr_ctrl_handle);
        scanner->pwr_ctrl_handle = NULL;
    }
#else
    (void)scanner;
#endif
}

static void sd_note_boot_stability(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    if (s_sd_boot_magic != kSdBootMagic) {
        s_sd_boot_magic = kSdBootMagic;
        s_sd_unstable_boots = 0;
    }

    /* 欠压复位说明上一轮电源已经塌了，这一轮再写 FAT 很容易把卡写坏。 */
    if (reason == ESP_RST_BROWNOUT) {
        if (s_sd_unstable_boots < 100) {
            s_sd_unstable_boots++;
        }
        s_writes_allowed = false;
        ESP_LOGW(SD_TAG, "欠压复位，本轮禁止写 SD（连续 %u 次）", (unsigned)s_sd_unstable_boots);
        return;
    }

    /* 反复冷启动多半是没电顶着重启。单次正常开机不会到 3 次。 */
    if (reason == ESP_RST_POWERON || reason == ESP_RST_UNKNOWN ||
        reason == ESP_RST_WDT || reason == ESP_RST_PANIC ||
        reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT) {
        if (s_sd_unstable_boots < 100) {
            s_sd_unstable_boots++;
        }
        s_writes_allowed = (s_sd_unstable_boots < 3);
        if (!s_writes_allowed) {
            ESP_LOGW(SD_TAG, "连续不稳定启动 %u 次，本轮禁止写 SD", (unsigned)s_sd_unstable_boots);
        }
        return;
    }

    s_sd_unstable_boots = 0;
    s_writes_allowed = true;
}

static void sd_shutdown_handler(void)
{
    sd_card_quiesce_and_unmount();
}

static esp_err_t sd_mount_once(sd_scanner_t *scanner)
{
    esp_err_t ret;

    ESP_LOGI(SD_TAG, "🔧 正在初始化SD卡...");

    /* 20MHz 4-bit。40MHz 在电压下跌时 CRC 失败，重试写会把 FAT 写坏。 */
    ESP_LOGI(SD_TAG, "💾 使用SDMMC外设");
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    host.slot = SDMMC_HOST_SLOT_0;
    host.command_timeout_ms = 2000;
    host.flags |= SDMMC_HOST_FLAG_DEINIT_ARG | SDMMC_HOST_FLAG_4BIT;
    host.flags &= ~SDMMC_HOST_FLAG_1BIT;
    ESP_LOGI(SD_TAG, "💾 SD卡配置: 频率=%d kHz, 超时=%d ms", host.max_freq_khz, host.command_timeout_ms);

    // 电源控制配置（ESP32-P4专用）
#if SOC_SDMMC_IO_POWER_EXTERNAL
    ESP_LOGI(SD_TAG, "⚡ 启用ESP32-P4片上LDO电源控制，LDO通道ID: 4");
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,
    };

    ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &scanner->pwr_ctrl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(SD_TAG, "❌ 创建片上LDO电源控制驱动失败: %s", esp_err_to_name(ret));
        scanner->pwr_ctrl_handle = NULL;
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
            ESP_LOGE(SD_TAG, "❌ 挂载文件系统失败，不会格式化。");
        } else {
            ESP_LOGE(SD_TAG, "❌ 初始化SD卡失败 (%s)。", esp_err_to_name(ret));
        }
        scanner->card = NULL;
        /* 拉掉卡电，让卡控制器从异常状态里出来，下次重试才有意义。 */
        sd_release_ldo(scanner);
        return ret;
    }

    scanner->is_mounted = true;
    ESP_LOGI(SD_TAG, "✅ 文件系统挂载成功");

    sdmmc_card_print_info(stdout, scanner->card);

    return ESP_OK;
}

esp_err_t sd_scanner_mount(sd_scanner_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    sd_scanner_t *scanner = handle;

    if (scanner->is_mounted) {
        ESP_LOGW(SD_TAG, "⚠️ SD卡已经挂载");
        return ESP_OK;
    }

    static bool boot_noted = false;
    if (!boot_noted) {
        sd_note_boot_stability();
        boot_noted = true;
    }
    if (!s_writes_allowed) {
        /* 电源刚塌过，先等轨恢复再碰卡。 */
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    if (!s_shutdown_registered) {
        if (esp_register_shutdown_handler(sd_shutdown_handler) == ESP_OK) {
            s_shutdown_registered = true;
        }
    }

    esp_err_t ret = ESP_FAIL;
    bool saw_fs_fail = false;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        ret = sd_mount_once(scanner);
        if (ret == ESP_OK) {
            s_fs_mount_failed = false;
            return ESP_OK;
        }
        if (ret == ESP_FAIL) {
            saw_fs_fail = true;
        }
        ESP_LOGW(SD_TAG, "挂载第 %d 次失败，断电 200ms 后重试", attempt);
        scanner->is_mounted = false;
        scanner->card = NULL;
        sd_release_ldo(scanner);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    s_fs_mount_failed = saw_fs_fail;
    if (s_fs_mount_failed) {
        ESP_LOGW(SD_TAG, "卡有应答但文件系统挂不上，长按音量键可格式化为 FAT 16KB");
    }
    return ret;
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

    /*
     * 先空等一会儿，让卡把最后一次 fsync 的 NAND 编程做完，再卸载。
     * 卸载会关掉 SD 电源；电断在卡内部编程中途，卡就会被写坏。
     */
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, scanner->card);
    scanner->is_mounted = false;
    scanner->card = NULL;
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(SD_TAG, "💾 SD卡已卸载");

    sd_release_ldo(scanner);
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

bool sd_card_fs_mount_failed(void) {
    return s_fs_mount_failed;
}

esp_err_t sd_card_format_fat(void) {
    if (!s_fs_mount_failed || s_formatting) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_writes_allowed) {
        ESP_LOGW(SD_TAG, "电源不稳，拒绝格式化");
        return ESP_ERR_INVALID_STATE;
    }
    s_formatting = true;

    if (g_scanner == NULL) {
        esp_err_t err = sd_scanner_init(&g_scanner);
        if (err != ESP_OK) {
            s_formatting = false;
            return err;
        }
    }
    if (g_scanner->is_mounted) {
        s_fs_mount_failed = false;
        s_formatting = false;
        return ESP_ERR_INVALID_STATE;
    }

    /* 只在这一次挂载里允许格式化。大卡会落成 FAT32，分配单元 16KB。 */
    g_scanner->mount_config.format_if_mount_failed = true;
    g_scanner->mount_config.allocation_unit_size = 16 * 1024;
    g_scanner->mount_config.use_one_fat = false;
    ESP_LOGW(SD_TAG, "开始格式化 SD 卡为 FAT，分配单元 16KB");
    esp_err_t ret = sd_scanner_mount(g_scanner);
    if (g_scanner != NULL) {
        g_scanner->mount_config.format_if_mount_failed = false;
    }
    if (ret == ESP_OK) {
        s_fs_mount_failed = false;
        sd_scanner_scan(g_scanner);
        ESP_LOGI(SD_TAG, "SD卡已格式化为 FAT，分配单元 16KB");
    } else {
        ESP_LOGE(SD_TAG, "格式化失败: %s", esp_err_to_name(ret));
    }
    s_formatting = false;
    return ret;
}

bool sd_card_writes_allowed(void) {
    return s_writes_allowed && sd_scanner_is_mounted();
}

void sd_card_writer_enter(void) {
    __atomic_add_fetch(&s_sd_writers, 1, __ATOMIC_RELAXED);
}

void sd_card_writer_leave(void) {
    __atomic_sub_fetch(&s_sd_writers, 1, __ATOMIC_RELAXED);
}

esp_err_t sd_card_flush_file(FILE* fp) {
    if (fp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (fflush(fp) != 0) {
        return ESP_FAIL;
    }
    if (fsync(fileno(fp)) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void sd_card_close_file(FILE* fp) {
    if (fp == NULL) {
        return;
    }
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
}

void sd_card_quiesce_and_unmount(void) {
    s_writes_allowed = false;
    /* 读任务退出时会 fclose。禁止在 FatFS 调用中途 vTaskDelete。 */
    mjpeg_player_stop();

    for (int i = 0; i < 80 && __atomic_load_n(&s_sd_writers, __ATOMIC_RELAXED) > 0; ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (__atomic_load_n(&s_sd_writers, __ATOMIC_RELAXED) > 0) {
        ESP_LOGW(SD_TAG, "仍有写任务未退出，继续卸载");
    }
    if (g_scanner != NULL && g_scanner->is_mounted) {
        sd_scanner_unmount(g_scanner);
    }
}

// 初始化并扫描SD卡（保持挂载状态）
esp_err_t sd_scanner_init_and_scan(void)
{
    ESP_LOGI(SD_TAG, "🔍 初始化SD卡并保持挂载...");

    /* 已经挂着就只重新扫描。以前这里 free 掉句柄再 mount 一次，等于两套 FAT 同时写同一张卡。 */
    if (g_scanner != NULL) {
        if (g_scanner->is_mounted) {
            ESP_LOGI(SD_TAG, "💾 SD卡已挂载，跳过重复挂载");
            return sd_scanner_scan(g_scanner);
        }
        sd_scanner_deinit(g_scanner);
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
