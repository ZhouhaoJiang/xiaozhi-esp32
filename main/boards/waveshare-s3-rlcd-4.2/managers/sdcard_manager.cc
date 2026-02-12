#include "sdcard_manager.h"
#include <esp_log.h>
#include <dirent.h>
#include <cstring>
#include <algorithm>

bool SdcardManager::init(int clk, int cmd, int d0) {
    if (mounted_) {
        ESP_LOGW(TAG, "SD 卡已挂载，跳过重复初始化");
        return true;
    }

    // 挂载配置
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;  // 挂载失败不格式化（保护数据）
    mount_config.max_files = 5;
    mount_config.allocation_unit_size = 16 * 1024;

    // SDMMC 主机配置（1-bit 模式，兼容性最好）
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;  // 1-bit 模式
    slot_config.clk = (gpio_num_t)clk;
    slot_config.cmd = (gpio_num_t)cmd;
    slot_config.d0 = (gpio_num_t)d0;

    ESP_LOGI(TAG, "正在挂载 SD 卡... (CLK=%d, CMD=%d, D0=%d)", clk, cmd, d0);

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(mount_point_, &host, &slot_config, &mount_config, &card_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD 卡挂载失败: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "请检查：1) SD 卡是否插入  2) 格式是否为 FAT32  3) 接线是否正确");
        card_ = nullptr;
        mounted_ = false;
        return false;
    }

    mounted_ = true;

    // 打印 SD 卡信息
    sdmmc_card_print_info(stdout, card_);
    ESP_LOGI(TAG, "SD 卡挂载成功，挂载点: %s", mount_point_);

    return true;
}

SdcardManager::~SdcardManager() {
    if (mounted_ && card_) {
        esp_vfs_fat_sdcard_unmount(mount_point_, card_);
        ESP_LOGI(TAG, "SD 卡已卸载");
    }
}

std::vector<std::string> SdcardManager::listFiles(const char* dir_path, const char* extension) {
    std::vector<std::string> files;

    if (!mounted_) {
        ESP_LOGW(TAG, "SD 卡未挂载，无法列出文件");
        return files;
    }

    DIR* dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGW(TAG, "无法打开目录: %s", dir_path);
        // 目录打不开时，输出 SD 根目录内容做自检，方便排查目录名/层级问题
        DIR* root_dir = opendir(mount_point_);
        if (root_dir) {
            ESP_LOGW(TAG, "开始列出 SD 根目录内容: %s", mount_point_);
            struct dirent* root_entry;
            while ((root_entry = readdir(root_dir)) != nullptr) {
                if (strcmp(root_entry->d_name, ".") == 0 || strcmp(root_entry->d_name, "..") == 0) {
                    continue;
                }
                const char* type_text = (root_entry->d_type == DT_DIR) ? "DIR" : "FILE";
                ESP_LOGW(TAG, "  - [%s] %s", type_text, root_entry->d_name);
            }
            closedir(root_dir);
        } else {
            ESP_LOGW(TAG, "无法打开 SD 根目录进行自检: %s", mount_point_);
        }
        return files;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        // 跳过目录和隐藏文件
        if (entry->d_type == DT_DIR) continue;
        if (entry->d_name[0] == '.') continue;

        std::string filename(entry->d_name);

        // 如果指定了扩展名过滤
        if (extension && extension[0] != '\0') {
            size_t ext_len = strlen(extension);
            if (filename.size() < ext_len) continue;
            
            // 不区分大小写比较扩展名
            std::string file_ext = filename.substr(filename.size() - ext_len);
            std::string target_ext(extension);
            std::transform(file_ext.begin(), file_ext.end(), file_ext.begin(), ::tolower);
            std::transform(target_ext.begin(), target_ext.end(), target_ext.begin(), ::tolower);
            if (file_ext != target_ext) continue;
        }

        // 拼接完整路径
        std::string full_path = std::string(dir_path) + "/" + filename;
        files.push_back(full_path);
    }

    closedir(dir);

    // 按文件名排序
    std::sort(files.begin(), files.end());

    ESP_LOGI(TAG, "目录 %s 下找到 %d 个文件", dir_path, (int)files.size());
    return files;
}
