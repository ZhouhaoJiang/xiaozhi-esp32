#ifndef __SDCARD_MANAGER_H__
#define __SDCARD_MANAGER_H__

#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <driver/sdmmc_host.h>
#include <string>
#include <vector>

// SD 卡管理器（单例）
// 
// 基于 SDMMC 接口驱动 SD 卡，挂载为 FAT 文件系统。
// 参考 Example/ESP-IDF/06_SD_Card 的 sdcard_bsp 实现。
//
// 默认引脚（Waveshare ESP32-S3-RLCD-4.2 板载 SD 卡槽）：
//   CLK = GPIO38, CMD = GPIO21, D0 = GPIO39
//
// 挂载点：/sdcard
class SdcardManager {
public:
    static SdcardManager& getInstance() {
        static SdcardManager instance;
        return instance;
    }

    // 初始化并挂载 SD 卡（返回 true 表示成功）
    bool init(int clk = 38, int cmd = 21, int d0 = 39);

    // SD 卡是否已成功挂载
    bool isMounted() const { return mounted_; }

    // 扫描指定目录下的文件列表（返回完整路径）
    // 例如：listFiles("/sdcard/white-noise", ".mp3") 
    std::vector<std::string> listFiles(const char* dir_path, const char* extension = nullptr);

    // 获取挂载点路径
    const char* getMountPoint() const { return mount_point_; }

private:
    SdcardManager() = default;
    ~SdcardManager();

    // 禁止拷贝
    SdcardManager(const SdcardManager&) = delete;
    SdcardManager& operator=(const SdcardManager&) = delete;

    static constexpr const char* TAG = "SdcardManager";
    static constexpr const char* mount_point_ = "/sdcard";
    
    bool mounted_ = false;
    sdmmc_card_t* card_ = nullptr;
};

#endif
