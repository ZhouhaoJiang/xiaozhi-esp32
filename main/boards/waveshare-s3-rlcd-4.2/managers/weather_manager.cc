#include "weather_manager.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "zlib.h"
#include <string.h>
#include <algorithm>
#include <cctype>

static const char *TAG = "WeatherManager";

// HTTP 响应缓冲区（分配在 SPIRAM 上，避免占用宝贵的内部 RAM）
static char* response_buffer = NULL;
static int response_len = 0;
static const int RESPONSE_BUFFER_SIZE = 8192;

// GZIP 解压缓冲区（和风天气 API 默认返回 gzip 压缩数据）
static char* decompressed_buffer = NULL;
static const int DECOMPRESSED_BUFFER_SIZE = 8192;

esp_err_t WeatherManager::http_event_handler(esp_http_client_event_t *evt) {
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (response_buffer && response_len + evt->data_len < RESPONSE_BUFFER_SIZE - 1) {
                memcpy(response_buffer + response_len, evt->data, evt->data_len);
                response_len += evt->data_len;
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

WeatherManager::WeatherManager() {
    response_buffer = (char*)heap_caps_malloc(RESPONSE_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    decompressed_buffer = (char*)heap_caps_malloc(DECOMPRESSED_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
}

WeatherManager& WeatherManager::getInstance() {
    static WeatherManager instance;
    return instance;
}

void WeatherManager::setApiConfig(const char* key, const char* host) {
    api_key_ = key;
    api_host_ = host;
}

// GZIP 安全解压（和风天气 API 返回 gzip 格式）
static bool decompress_gzip_safe(const uint8_t* src, int src_len, char* dst, int dst_max_len, int* out_len) {
    if (src_len < 18 || src[0] != 0x1f || src[1] != 0x8b) return false;
    z_stream strm = {};
    strm.next_in = (Bytef*)src;
    strm.avail_in = src_len;
    strm.next_out = (Bytef*)dst;
    strm.avail_out = dst_max_len - 1;
    if (inflateInit2(&strm, 15 + 16) != Z_OK) return false;
    int ret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);
    if (ret != Z_STREAM_END && ret != Z_OK) return false;
    *out_len = dst_max_len - 1 - strm.avail_out;
    dst[*out_len] = '\0';
    return true;
}

// 判断城市名是否可用于 UI 展示（排除 "Ip" 这类占位值）
static bool is_valid_display_city(const char* city) {
    if (city == nullptr || city[0] == '\0') {
        return false;
    }
    std::string normalized(city);
    normalized.erase(std::remove_if(normalized.begin(), normalized.end(), ::isspace), normalized.end());
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return normalized != "ip" && normalized != "auto_ip" && normalized != "unknown";
}

// 优先使用更像“真实地名”的字段，避免把 "Ip" 显示到页面上
static std::string pick_city_name_for_display(cJSON* first_city, const std::string& fallback_city) {
    const char* keys[] = {"adm2", "adm1", "name"};
    for (const char* key : keys) {
        cJSON* item = cJSON_GetObjectItem(first_city, key);
        if (item && cJSON_IsString(item) && is_valid_display_city(item->valuestring)) {
            return item->valuestring;
        }
    }
    return fallback_city;
}

bool WeatherManager::update() {
    if (!response_buffer || api_key_.empty() || api_host_.empty()) {
        ESP_LOGW(TAG, "天气 API 未配置或缓冲区未分配");
        return false;
    }

    // 第一步：通过和风天气 GeoAPI 进行 IP 定位
    response_len = 0;
    memset(response_buffer, 0, RESPONSE_BUFFER_SIZE);
    char geo_url[256];
    // 使用 auto_ip 让服务端按公网出口 IP 识别城市
    snprintf(geo_url, sizeof(geo_url), "https://%s/geo/v2/city/lookup?location=auto_ip&key=%s",
             api_host_.c_str(), api_key_.c_str());

    ESP_LOGI(TAG, "正在进行 IP 定位...");
    esp_http_client_config_t geo_config = {};
    geo_config.url = geo_url;
    geo_config.event_handler = http_event_handler;
    geo_config.timeout_ms = 8000;
    geo_config.crt_bundle_attach = esp_crt_bundle_attach;
    
    esp_http_client_handle_t geo_client = esp_http_client_init(&geo_config);
    esp_http_client_set_header(geo_client, "Host", api_host_.c_str());
    esp_err_t geo_err = esp_http_client_perform(geo_client);
    int geo_status = esp_http_client_get_status_code(geo_client);
    
    // 默认位置（苏州）
    double lat = 31.23, lon = 120.62; 
    std::string city_name = "苏州";

    if (geo_err == ESP_OK && geo_status == 200 && response_len > 0) {
        // geo 响应也可能是 gzip 压缩的，先尝试解压
        const char* geo_json = NULL;
        int geo_d_len = 0;
        if (decompressed_buffer &&
            decompress_gzip_safe((uint8_t*)response_buffer, response_len,
                                 decompressed_buffer, DECOMPRESSED_BUFFER_SIZE, &geo_d_len)) {
            geo_json = decompressed_buffer;
            ESP_LOGI(TAG, "IP 定位响应已 gzip 解压 (%d -> %d bytes)", response_len, geo_d_len);
        } else {
            response_buffer[response_len] = '\0';
            geo_json = response_buffer;
        }
        cJSON *root = cJSON_Parse(geo_json);
        if (root) {
            cJSON *code = cJSON_GetObjectItem(root, "code");
            if (code && cJSON_IsString(code) && strcmp(code->valuestring, "200") == 0) {
                cJSON *location_array = cJSON_GetObjectItem(root, "location");
                if (location_array && cJSON_GetArraySize(location_array) > 0) {
                    cJSON *first_city = cJSON_GetArrayItem(location_array, 0);
                    cJSON *lat_item = cJSON_GetObjectItem(first_city, "lat");
                    cJSON *lon_item = cJSON_GetObjectItem(first_city, "lon");
                    if (lat_item && lon_item &&
                        cJSON_IsString(lat_item) && cJSON_IsString(lon_item)) {
                        lat = atof(lat_item->valuestring);
                        lon = atof(lon_item->valuestring);
                        city_name = pick_city_name_for_display(first_city, city_name);
                        ESP_LOGI(TAG, "定位成功: %s (%.2f, %.2f)", city_name.c_str(), lat, lon);
                    } else {
                        ESP_LOGW(TAG, "定位响应缺少必要字段（lat/lon），使用默认城市");
                    }
                }
            } else {
                const char *api_code = (code && cJSON_IsString(code)) ? code->valuestring : "null";
                ESP_LOGW(TAG, "IP 定位接口返回 code=%s，使用默认城市", api_code);
            }
            cJSON_Delete(root);
        } else {
            // 打印响应前 80 字节帮助诊断（可能是 HTML 网关页、乱码等）
            ESP_LOGW(TAG, "IP 定位响应 JSON 解析失败 (len=%d, head=%.80s)，使用默认城市",
                     response_len, geo_json);
        }
    } else {
        // 打印失败细节，便于区分网络错误 / HTTP 错误 / 参数错误
        if (response_len > 0) {
            response_buffer[response_len] = '\0';
            ESP_LOGW(TAG, "IP 定位请求失败 (err=%d, status=%d, body=%.120s)，使用默认城市",
                     geo_err, geo_status, response_buffer);
        } else {
            ESP_LOGW(TAG, "IP 定位请求失败 (err=%d, status=%d, empty body)，使用默认城市",
                     geo_err, geo_status);
        }
    }
    esp_http_client_cleanup(geo_client);

    // 第二步：获取实时天气数据
    response_len = 0;
    memset(response_buffer, 0, RESPONSE_BUFFER_SIZE);
    char weather_url[512];
    snprintf(weather_url, sizeof(weather_url), 
             "https://%s/v7/weather/now?location=%.2f,%.2f&key=%s&lang=zh", 
             api_host_.c_str(), lon, lat, api_key_.c_str());

    ESP_LOGI(TAG, "获取天气数据...");
    esp_http_client_config_t weather_config = {};
    weather_config.url = weather_url;
    weather_config.event_handler = http_event_handler;
    weather_config.timeout_ms = 15000;
    weather_config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&weather_config);
    
    esp_http_client_set_header(client, "Host", api_host_.c_str());
    esp_http_client_set_header(client, "User-Agent", "ESP32-Weather-Station");
    esp_http_client_set_header(client, "Accept-Encoding", "gzip");
    
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    bool success = false;

    if (err == ESP_OK && status_code == 200 && response_len > 0) {
        int d_len = 0;
        const char* final_json = NULL;
        if (decompress_gzip_safe((uint8_t*)response_buffer, response_len, 
                                  decompressed_buffer, DECOMPRESSED_BUFFER_SIZE, &d_len)) {
            final_json = decompressed_buffer;
        } else {
            response_buffer[response_len] = '\0';
            final_json = response_buffer;
        }

        if (final_json) {
            parseWeatherJson(final_json);
            latest_data_.city = city_name;
            success = latest_data_.valid;
        }
    } else {
        ESP_LOGE(TAG, "天气请求失败 (err=%d, status=%d)", err, status_code);
    }
    esp_http_client_cleanup(client);
    return success;
}

void WeatherManager::parseWeatherJson(const char* json_data) {
    cJSON *root = cJSON_Parse(json_data);
    if (!root) return;
    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (code && strcmp(code->valuestring, "200") == 0) {
        cJSON *now = cJSON_GetObjectItem(root, "now");
        if (now) {
            latest_data_.temp = cJSON_GetObjectItem(now, "temp")->valuestring;
            latest_data_.text = cJSON_GetObjectItem(now, "text")->valuestring;
            latest_data_.valid = true;
            ESP_LOGI(TAG, "天气更新成功: %s°C, %s", latest_data_.temp.c_str(), latest_data_.text.c_str());
        }
    }
    cJSON_Delete(root);
}
