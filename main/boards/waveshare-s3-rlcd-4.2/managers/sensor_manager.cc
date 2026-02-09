#include "sensor_manager.h"
#include "../secret_config.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_netif_sntp.h"
#include <string.h>
#include <sys/time.h>

static const char *TAG = "SensorManager";

// I2C 传感器地址
#define SHTC3_ADDR      0x70
#define PCF85063_ADDR   0x51

SensorManager& SensorManager::getInstance() {
    static SensorManager instance;
    return instance;
}

SensorManager::SensorManager() {}

void SensorManager::init(i2c_master_bus_handle_t bus_handle) {
    if (initialized_) return;
    bus_handle_ = bus_handle;
    
    // 添加 SHTC3 温湿度传感器到 I2C 总线
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = SHTC3_ADDR;
    dev_cfg.scl_speed_hz = 100000;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle_, &dev_cfg, &shtc3_handle_));

    // 添加 PCF85063 RTC 时钟到 I2C 总线
    dev_cfg.device_address = PCF85063_ADDR;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle_, &dev_cfg, &pcf85063_handle_));

    initSHTC3();
    initRTC();
    
    // 先设置时区！必须在任何 mktime/localtime 之前
    setenv("TZ", TIMEZONE_STRING, 1);
    tzset();
    ESP_LOGI(TAG, "时区已设置: %s", TIMEZONE_STRING);
    
    // 启动时从硬件 RTC 恢复系统时间
    // 这样即使没有网络，系统也能显示上次同步的时间
    // 注意：RTC 存的是本地时间（CST+8），mktime 会根据已设置的时区转成 epoch
    struct tm ti;
    getRtcTime(&ti);
    time_t t = mktime(&ti);
    if (t > 1700000000) { // 检查 RTC 是否有有效时间（2023年以后）
        struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "从 RTC 恢复时间成功");
    }
    
    initialized_ = true;
    ESP_LOGI(TAG, "传感器初始化完成");
}

void SensorManager::initSHTC3() {
    // SHTC3 软件复位命令
    uint8_t cmd[] = {0x35, 0x17}; 
    i2c_master_transmit(shtc3_handle_, cmd, sizeof(cmd), pdMS_TO_TICKS(100));
}

void SensorManager::initRTC() {
    // PCF85063 控制寄存器初始化
    uint8_t cmd[] = {0x00, 0x00}; 
    i2c_master_transmit(pcf85063_handle_, cmd, sizeof(cmd), pdMS_TO_TICKS(100));
}

void SensorManager::syncNtpTime() {
    // 同步前确保时区设置正确（防止被 ota.cc 的 settimeofday 等操作干扰）
    setenv("TZ", TIMEZONE_STRING, 1);
    tzset();
    
    ESP_LOGI(TAG, "正在同步 NTP 时间...");
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    esp_netif_sntp_init(&config);

    int retry = 0;
    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000)) != ESP_OK && ++retry < 5) {
        ESP_LOGI(TAG, "等待 NTP 响应... (%d/5)", retry);
    }

    if (retry < 5) {
        // NTP 同步成功后再次确认时区（以防其他模块覆盖）
        setenv("TZ", TIMEZONE_STRING, 1);
        tzset();
        
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);
        setRtcTime(&timeinfo); // 同步到硬件 RTC，断电后也能保持时间
        ESP_LOGI(TAG, "NTP 同步成功: %04d-%02d-%02d %02d:%02d:%02d", 
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
        ESP_LOGW(TAG, "NTP 同步失败");
    }
    
    esp_netif_sntp_deinit();
}

SensorData SensorManager::getTempHumidity() {
    SensorData data = {0, 0, false};
    if (!shtc3_handle_) return data;
    
    // 发送读取命令（正常模式，温度优先）
    uint8_t cmd[] = {0x7C, 0xA2};
    if (i2c_master_transmit(shtc3_handle_, cmd, sizeof(cmd), pdMS_TO_TICKS(100)) == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20)); // 等待测量完成
        uint8_t buf[6];
        if (i2c_master_receive(shtc3_handle_, buf, sizeof(buf), pdMS_TO_TICKS(100)) == ESP_OK) {
            uint16_t t_raw = (buf[0] << 8) | buf[1];
            uint16_t h_raw = (buf[3] << 8) | buf[4];
            data.temperature = -45 + 175 * ((float)t_raw / 65536.0);
            data.humidity = 100 * ((float)h_raw / 65536.0);
            data.valid = true;
        }
    }
    return data;
}

static uint8_t bcd2dec(uint8_t val) { return (val >> 4) * 10 + (val & 0x0f); }
static uint8_t dec2bcd(uint8_t val) { return ((val / 10) << 4) + (val % 10); }

void SensorManager::getRtcTime(struct tm *timeinfo) {
    if (!pcf85063_handle_) return;
    uint8_t buf[7];
    uint8_t reg = 0x04;
    if (i2c_master_transmit_receive(pcf85063_handle_, &reg, 1, buf, 7, pdMS_TO_TICKS(100)) == ESP_OK) {
        timeinfo->tm_sec  = bcd2dec(buf[0] & 0x7F);
        timeinfo->tm_min  = bcd2dec(buf[1] & 0x7F);
        timeinfo->tm_hour = bcd2dec(buf[2] & 0x3F);
        timeinfo->tm_mday = bcd2dec(buf[3] & 0x3F);
        timeinfo->tm_wday = bcd2dec(buf[4] & 0x07);
        timeinfo->tm_mon  = bcd2dec(buf[5] & 0x1F) - 1;
        timeinfo->tm_year = bcd2dec(buf[6]) + 100;
    }
}

void SensorManager::setRtcTime(struct tm *timeinfo) {
    if (!pcf85063_handle_) return;
    uint8_t buf[8];
    buf[0] = 0x04; // 起始寄存器地址
    buf[1] = dec2bcd(timeinfo->tm_sec);
    buf[2] = dec2bcd(timeinfo->tm_min);
    buf[3] = dec2bcd(timeinfo->tm_hour);
    buf[4] = dec2bcd(timeinfo->tm_mday);
    buf[5] = dec2bcd(timeinfo->tm_wday);
    buf[6] = dec2bcd(timeinfo->tm_mon + 1);
    buf[7] = dec2bcd(timeinfo->tm_year - 100);
    i2c_master_transmit(pcf85063_handle_, buf, 8, pdMS_TO_TICKS(100));
}
