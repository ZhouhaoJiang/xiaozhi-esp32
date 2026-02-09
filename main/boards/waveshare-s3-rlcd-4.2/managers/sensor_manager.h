#pragma once
#include "driver/i2c_master.h"
#include <time.h>

// 温湿度传感器数据
struct SensorData {
    float temperature;
    float humidity;
    bool valid;
};

// 传感器管理器：管理 SHTC3 温湿度传感器和 PCF85063 RTC 时钟
// 
// 硬件说明：
// - SHTC3：温湿度传感器，I2C 地址 0x70
// - PCF85063：实时时钟芯片，I2C 地址 0x51
// - 两个芯片共享 I2C 总线（SDA=GPIO13, SCL=GPIO14）
class SensorManager {
public:
    static SensorManager& getInstance();
    
    // 初始化（需要传入已创建的 I2C 总线句柄）
    void init(i2c_master_bus_handle_t bus_handle);
    
    // 读取温湿度（每次调用都会发起 I2C 通信）
    SensorData getTempHumidity();
    
    // RTC 时间操作
    void getRtcTime(struct tm *timeinfo);
    void setRtcTime(struct tm *timeinfo);
    
    // NTP 网络时间同步（需要网络连接）
    void syncNtpTime();

private:
    SensorManager();
    void initSHTC3();
    void initRTC();
    
    i2c_master_bus_handle_t bus_handle_ = NULL;
    i2c_master_dev_handle_t shtc3_handle_ = NULL;
    i2c_master_dev_handle_t pcf85063_handle_ = NULL;
    bool initialized_ = false;
};
