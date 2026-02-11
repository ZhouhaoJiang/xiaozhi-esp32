#pragma once
#include <string>
#include "esp_http_client.h"

// 天气数据结构
struct WeatherData {
    std::string city;     // 城市名
    std::string temp;     // 温度（字符串）
    std::string text;     // 天气描述（如"晴"、"多云"）
    std::string update_time;
    bool valid = false;
};

// 天气管理器：通过和风天气 API 获取天气数据
// 流程：IP 定位城市 → 获取实时天气
class WeatherManager {
public:
    static WeatherManager& getInstance();
    
    // 更新天气数据（包含定位+天气请求，耗时较长，应在后台任务中调用）
    // 返回 true 表示更新成功，false 表示失败（网络错误等）
    bool update();
    
    // 获取最新天气数据（线程安全，直接读取缓存）
    // 即使网络断开，也能返回上次成功获取的数据
    WeatherData getLatestData() { return latest_data_; }

    // 设置 API 密钥和主机（从配置中读取）
    void setApiConfig(const char* key, const char* host);

    // 通过外部工具（如 MCP）直接写入天气数据
    // 适用于不走板载 HTTP 天气接口，而由 AI 侧先查好天气再下发到设备
    bool updateFromExternal(const std::string& city,
                            const std::string& weather_text,
                            const std::string& temperature,
                            const std::string& update_time = "");

private:
    WeatherManager();
    WeatherData latest_data_;
    
    // API 配置
    std::string api_key_;
    std::string api_host_;
    
    static esp_err_t http_event_handler(esp_http_client_event_t *evt);
    void parseWeatherJson(const char* json_data);
};
