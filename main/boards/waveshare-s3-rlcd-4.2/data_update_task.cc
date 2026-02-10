// 数据更新后台任务
//
// 负责周期性更新屏幕上的动态数据：
// - NTP 时间同步 + 时间跳变保护
// - 时钟/日历更新（每分钟）
// - 天气数据更新（每 10 分钟）
// - 温湿度传感器读取
// - 电池状态 / WiFi 图标更新
// - AI 状态文字更新
// - 备忘闹钟检查

#include "custom_lcd_display.h"

#include <cmath>
#include <cstring>
#include <cJSON.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

#include "application.h"
#include "board.h"
#include "config.h"
#include "settings.h"
#include "assets/lang_config.h"
#include "managers/sensor_manager.h"
#include "managers/weather_manager.h"
#include "secret_config.h"

// 声明状态栏图标（DataUpdateTask 需要更新图标）
LV_IMAGE_DECLARE(ui_img_wifi);
LV_IMAGE_DECLARE(ui_img_wifi_low);
LV_IMAGE_DECLARE(ui_img_wifi_off);
LV_IMAGE_DECLARE(ui_img_battery_full);
LV_IMAGE_DECLARE(ui_img_battery_medium);
LV_IMAGE_DECLARE(ui_img_battery_low);
LV_IMAGE_DECLARE(ui_img_battery_charging);

static const char *TAG = "DataUpdate";

void CustomLcdDisplay::StartDataUpdateTask() {
    // 配置天气 API（密钥在 secret_config.h 中，不提交到 Git）
    WeatherManager::getInstance().setApiConfig(
        WEATHER_API_KEY,
        WEATHER_API_HOST
    );
    
    xTaskCreate(DataUpdateTask, "weather_ui_update", 16384, this, 3, &update_task_handle_);
}

void CustomLcdDisplay::DataUpdateTask(void *arg) {
    CustomLcdDisplay *self = (CustomLcdDisplay *)arg;
    bool time_synced = false;
    uint32_t last_weather_update = 0;
    int ntp_retry_count = 0;
    
    // 等待一会让系统启动完成
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    // 用于备忘闹钟检查的时间信息（在锁外使用）
    struct tm timeinfo;
    
    while (1) {
        auto& app = Application::GetInstance();
        DeviceState ds = app.GetDeviceState();
        
        // 判断网络是否已连接（必须不是 starting 和 activating 前期状态）
        bool network_connected = (ds != kDeviceStateStarting && 
                                  ds != kDeviceStateWifiConfiguring &&
                                  ds != kDeviceStateUnknown);
        
        // NTP 时间同步（网络连接后执行，失败可重试最多 3 次）
        if (network_connected && !time_synced && ntp_retry_count < 3) {
            ESP_LOGI(TAG, "网络已连接，同步 NTP 时间 (第 %d 次)...", ntp_retry_count + 1);
            SensorManager::getInstance().syncNtpTime();
            
            // 检查时间是否合理（年份 > 2024 说明同步成功了）
            time_t now_check;
            struct tm check_info;
            time(&now_check);
            localtime_r(&now_check, &check_info);
            if (check_info.tm_year + 1900 >= 2024) {
                time_synced = true;
                self->last_min_ = -1; // 强制刷新 UI
                time(&self->last_valid_epoch_);  // 记录正确的 epoch
                ESP_LOGI(TAG, "NTP 同步确认成功，当前时间: %04d-%02d-%02d %02d:%02d",
                         check_info.tm_year + 1900, check_info.tm_mon + 1, check_info.tm_mday,
                         check_info.tm_hour, check_info.tm_min);
            } else {
                ntp_retry_count++;
                ESP_LOGW(TAG, "NTP 同步后时间不合理（年份=%d），将重试", check_info.tm_year + 1900);
            }
        }
        
        // 天气更新（每 10 分钟，仅在 idle 状态）
        if (network_connected) {
            uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (last_weather_update == 0 || (now_ms - last_weather_update > 10 * 60 * 1000)) {
                if (ds == kDeviceStateIdle) {
                    WeatherManager::getInstance().update();
                    last_weather_update = now_ms;
                }
            }
        }
        
        // ===== UI 更新（每秒）=====
        // 🔑 如果正在显示系统信息滚动，跳过整个 UI 更新块（避免锁竞争）
        if (!self->showing_system_info_) {
            DisplayLockGuard lock(self);
            
            // 1. 时间和日期更新（即使 NTP 没同步也用 RTC 时间）
            // 确保时区正确（小智的 ota.cc 会用 settimeofday 覆盖系统时间）
            setenv("TZ", TIMEZONE_STRING, 1);
            tzset();
            
            time_t now;
            time(&now);
            localtime_r(&now, &timeinfo);
            
            // 时间跳变保护：NTP 同步后，如果系统 epoch 被外部改了（偏差>2小时），
            // 从硬件 RTC 恢复正确时间
            if (time_synced && self->last_valid_epoch_ > 0) {
                long drift = (long)(now - self->last_valid_epoch_);
                // 正常每秒循环 drift ≈ 1s，如果绝对值 > 7200s（2小时），肯定异常
                if (drift < -7200 || drift > 7200) {
                    ESP_LOGW(TAG, "系统时间被篡改（偏差 %ld 秒），从 RTC 恢复", drift);
                    struct tm rtc_tm;
                    SensorManager::getInstance().getRtcTime(&rtc_tm);
                    time_t rtc_epoch = mktime(&rtc_tm);
                    if (rtc_epoch > 1700000000) {
                        struct timeval tv = { .tv_sec = rtc_epoch, .tv_usec = 0 };
                        settimeofday(&tv, NULL);
                        time(&now);
                        localtime_r(&now, &timeinfo);
                        self->last_min_ = -1;
                        ESP_LOGI(TAG, "已从 RTC 恢复: %02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
                    }
                }
                self->last_valid_epoch_ = now;
            }
            
            // 每分钟或强制刷新时更新
            if (timeinfo.tm_min != self->last_min_) {
                char time_buf[16];
                strftime(time_buf, sizeof(time_buf), "%H:%M", &timeinfo);
                if (self->time_label_) lv_label_set_text(self->time_label_, time_buf);

                const char *weeks_en[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
                if (self->day_label_) lv_label_set_text(self->day_label_, weeks_en[timeinfo.tm_wday]);

                char date_buf[8];
                snprintf(date_buf, sizeof(date_buf), "%d", timeinfo.tm_mday);
                if (self->date_num_label_) lv_label_set_text(self->date_num_label_, date_buf);

                self->last_min_ = timeinfo.tm_min;
                ESP_LOGI(TAG, "时间已更新: %s, %s, %d日", time_buf, weeks_en[timeinfo.tm_wday], timeinfo.tm_mday);
            }
        }  // DisplayLockGuard 自动释放 ← 在这里释放锁！

        // ===== 备忘闹钟检查（移到锁外，避免长时间持锁）=====
        if (timeinfo.tm_min != self->last_min_) {
            if (time_synced) {
                Settings memo_rd("memo", false);
                std::string memo_json = memo_rd.GetString("items", "");
                if (!memo_json.empty()) {
                    cJSON *memo_arr = cJSON_Parse(memo_json.c_str());
                    if (memo_arr && cJSON_IsArray(memo_arr)) {
                        bool memo_changed = false;
                        char time_buf[16];
                        strftime(time_buf, sizeof(time_buf), "%H:%M", &timeinfo);
                        
                        // 倒序遍历，这样删除不会打乱前面的索引
                        for (int mi = cJSON_GetArraySize(memo_arr) - 1; mi >= 0; mi--) {
                            cJSON *memo_item = cJSON_GetArrayItem(memo_arr, mi);
                            cJSON *mt = cJSON_GetObjectItem(memo_item, "t");
                            cJSON *mc = cJSON_GetObjectItem(memo_item, "c");
                            // 只匹配 "HH:MM" 格式（5个字符，中间是冒号）
                            if (mt && mt->valuestring && strlen(mt->valuestring) == 5 
                                && mt->valuestring[2] == ':') {
                                if (strcmp(mt->valuestring, time_buf) == 0) {
                                    const char *memo_text = (mc && mc->valuestring) ? mc->valuestring : "备忘提醒";
                                    char alert_buf[128];
                                    snprintf(alert_buf, sizeof(alert_buf), "备忘提醒: %s %s", mt->valuestring, memo_text);
                                    ESP_LOGI(TAG, "触发备忘闹钟: %s", alert_buf);
                                    
                                    // 🔔 先播放提示音（短促提醒，不会打断 AI）
                                    app.Alert("提醒", alert_buf, "happy", Lang::Sounds::OGG_POPUP);
                                    
                                    // 🎙️ 主动触发 AI 语音提醒（让小智用语音说出备忘内容）
                                    // 构造自然语言提醒文本，让 LLM 更人性化地提醒
                                    char ai_prompt[256];
                                    snprintf(ai_prompt, sizeof(ai_prompt), 
                                             "现在是 %s，该 %s 了", 
                                             mt->valuestring, memo_text);
                                    app.TriggerAiReminder(ai_prompt);

                                    // 触发后从列表中删除这条
                                    cJSON_DeleteItemFromArray(memo_arr, mi);
                                    memo_changed = true;
                                }
                            }
                        }
                        // 如果有条目被删除，写回 NVS 并刷新屏幕
                        if (memo_changed) {
                            char *new_json = cJSON_PrintUnformatted(memo_arr);
                            {
                                Settings memo_wr("memo", true);
                                memo_wr.SetString("items", new_json);
                            }
                            cJSON_free(new_json);
                            self->RefreshMemoDisplay();  // 这个函数会自动获取锁
                            ESP_LOGI(TAG, "已过期备忘已自动删除");
                        }
                        cJSON_Delete(memo_arr);
                    }
                }
            }
        }

        // ===== 其他 UI 更新（需要重新获取锁）=====
        // 🔑 如果正在显示系统信息滚动，跳过 UI 更新（避免锁竞争）
        if (!self->showing_system_info_) {
            DisplayLockGuard lock(self);
            
            // 2. 温湿度更新
            SensorData sd = SensorManager::getInstance().getTempHumidity();
            if (sd.valid) {
                if (fabs(sd.temperature - self->last_temp_) > 0.2f || fabs(sd.humidity - self->last_humi_) > 1.0f) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%.1f°C  %.0f%%", sd.temperature, sd.humidity);
                    if (self->sensor_label_) lv_label_set_text(self->sensor_label_, buf);
                    self->last_temp_ = sd.temperature;
                    self->last_humi_ = sd.humidity;
                }
            }

            // 3. 天气更新
            WeatherData wd = WeatherManager::getInstance().getLatestData();
            if (wd.valid && self->weather_label_) {
                char weather_buf[48];
                snprintf(weather_buf, sizeof(weather_buf), "%s %s %s°C", 
                         wd.city.c_str(), wd.text.c_str(), wd.temp.c_str());
                lv_label_set_text(self->weather_label_, weather_buf);
            }

            // 4. 电池状态更新
            int level = 0;
            bool charging = false, discharging = false;
            auto& board = Board::GetInstance();
            if (board.GetBatteryLevel(level, charging, discharging)) {
                if (self->battery_icon_img_) {
                    if (charging) {
                        lv_image_set_src(self->battery_icon_img_, &ui_img_battery_charging);
                    } else {
                        if (level < 20) lv_image_set_src(self->battery_icon_img_, &ui_img_battery_low);
                        else if (level < 60) lv_image_set_src(self->battery_icon_img_, &ui_img_battery_medium);
                        else lv_image_set_src(self->battery_icon_img_, &ui_img_battery_full);
                    }
                }
                if (self->battery_pct_label_) {
                    char bat_buf[16];
                    snprintf(bat_buf, sizeof(bat_buf), "%d%%", level);
                    lv_label_set_text(self->battery_pct_label_, bat_buf);
                }
            }

            // 5. WiFi 图标更新
            if (self->wifi_icon_img_) {
                if (ds != kDeviceStateStarting && ds != kDeviceStateWifiConfiguring) {
                    lv_image_set_src(self->wifi_icon_img_, &ui_img_wifi);
                } else if (ds == kDeviceStateWifiConfiguring) {
                    lv_image_set_src(self->wifi_icon_img_, &ui_img_wifi_low);
                } else {
                    lv_image_set_src(self->wifi_icon_img_, &ui_img_wifi_off);
                }
            }

            // 6. AI 状态更新
            static DeviceState last_ds = kDeviceStateUnknown;
            if (ds != last_ds) {
                // 更新左侧表情区域（显示当前状态简称）
                const char* emotion_text = "待命";
                const char* status_text = "";
                switch (ds) {
                    case kDeviceStateConnecting:      emotion_text = "连接"; status_text = "连接中..."; break;
                    case kDeviceStateListening:       emotion_text = "聆听"; status_text = "聆听中..."; break;
                    case kDeviceStateSpeaking:        emotion_text = "说话"; break;  // 对话文字由 SetChatMessage 更新
                    case kDeviceStateStarting:        emotion_text = "启动"; status_text = "启动中..."; break;
                    case kDeviceStateWifiConfiguring: emotion_text = "配网"; break;   // 详细文案由 Alert() -> SetChatMessage 设置
                    case kDeviceStateUpgrading:       emotion_text = "升级"; status_text = "升级中..."; break;
                    case kDeviceStateActivating:      emotion_text = "激活"; break;   // 详细文案由 Alert() -> SetChatMessage 设置
                    case kDeviceStateFatalError:      emotion_text = "错误"; status_text = "发生错误"; break;
                    case kDeviceStateIdle:            emotion_text = "待命"; break;   // 空闲时表情由 SetEmotion 管理
                    default: break;
                }
                if (self->emotion_label_) {
                    lv_label_set_text(self->emotion_label_, emotion_text);
                }
                // 非说话/配网/激活状态时更新右侧文字（这些状态由 Alert/SetChatMessage 管理详细信息）
                if (ds != kDeviceStateSpeaking && ds != kDeviceStateWifiConfiguring && 
                    ds != kDeviceStateActivating && self->chat_status_label_ && strlen(status_text) > 0) {
                    lv_label_set_text(self->chat_status_label_, status_text);
                }
                last_ds = ds;
            }
        }  // DisplayLockGuard 自动释放

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
