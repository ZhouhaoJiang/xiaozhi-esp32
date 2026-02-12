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
#include "settings.h"
#include "assets/lang_config.h"
#include "managers/sensor_manager.h"
#include "managers/weather_manager.h"
#include "managers/pomodoro_manager.h"
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
    // 暂时停用板载和风天气 API 配置，改为由 MCP 工具写入天气缓存
    // WeatherManager::getInstance().setApiConfig(
    //     WEATHER_API_KEY,
    //     WEATHER_API_HOST
    // );
    
    // 栈从 16KB 下调到 8KB，给音频/MQTT 留更多 SRAM 余量
    // 优先级保持较低，避免与语音收发实时链路抢占 CPU
    xTaskCreate(DataUpdateTask, "weather_ui_update", 8192, this, 2, &update_task_handle_);
}

void CustomLcdDisplay::DataUpdateTask(void *arg) {
    CustomLcdDisplay *self = (CustomLcdDisplay *)arg;
    bool time_synced = false;
    
    // NTP 指数退避重试参数
    int ntp_retry_count = 0;
    const int NTP_MAX_RETRIES = 5;            // 最多重试 5 次后放弃
    uint32_t ntp_retry_delay_ms = 1000;       // 初始延迟 1 秒，每次翻倍（1s, 2s, 4s, 8s, 16s）
    uint32_t ntp_last_sync_ms = 0;            // 上次 NTP 同步成功的时间（用于 24 小时校准）
    const uint32_t NTP_RESYNC_INTERVAL = 24 * 60 * 60 * 1000;  // 24 小时（毫秒）
    
    // 电池电量变化很慢，降频采样可显著减轻 ADC 和 UI 刷新压力
    const uint32_t BATTERY_POLL_INTERVAL = 10 * 1000;           // 每 10 秒采样一次
    uint32_t last_battery_poll_ms = 0;
    int cached_battery_level = 0;
    bool cached_charging = false, cached_discharging = false;
    bool battery_cached = false;
    
    // 等待一会让系统启动完成
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    // 记录进入 idle 的时刻，用于"连续 idle 足够久才发网络请求"的保护
    uint32_t idle_since_ms = 0;
    
    // 初始化活动时间（系统启动算一次活动）
    self->last_activity_ms_ = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // 用于备忘闹钟检查的时间信息（在锁外使用）
    struct tm timeinfo;
    
    // 时区设置只需初始化一次，避免每秒 setenv/tzset 带来的系统开销
    setenv("TZ", TIMEZONE_STRING, 1);
    tzset();
    
    while (1) {
        auto& app = Application::GetInstance();
        DeviceState ds = app.GetDeviceState();
        
        // 判断网络是否已连接（必须不是 starting、配网、未知等前期状态）
        bool network_connected = (ds != kDeviceStateStarting && 
                                  ds != kDeviceStateWifiConfiguring &&
                                  ds != kDeviceStateUnknown);
        
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        const bool in_audio_session = (ds == kDeviceStateConnecting ||
                                       ds == kDeviceStateListening ||
                                       ds == kDeviceStateSpeaking);

        // ===== 连续 idle 计时 =====
        // NTP / 天气等网络请求必须在"连续 idle 超过 N 秒"后才发，
        // 防止刚开机或刚结束对话就发 HTTPS 请求，和下一次唤醒撞车导致首句卡顿
        const uint32_t IDLE_GUARD_MS = 5000;  // 连续 idle 至少 5 秒才允许网络请求
        if (ds == kDeviceStateIdle) {
            if (idle_since_ms == 0) {
                idle_since_ms = now_ms;
            }
        } else {
            idle_since_ms = 0;  // 非 idle 立刻重置
        }
        bool idle_long_enough = (idle_since_ms > 0 && (now_ms - idle_since_ms >= IDLE_GUARD_MS));
        
        // ===== NTP 时间同步 =====
        // 仅在连续 idle 足够久后同步，避免与 AI 对话抢网络/内存
        if (network_connected && idle_long_enough) {
            bool should_sync = false;
            
            if (!time_synced && ntp_retry_count < NTP_MAX_RETRIES) {
                // 首次同步：未同步且未超过最大重试次数
                should_sync = true;
            } else if (time_synced && ntp_last_sync_ms > 0 && 
                       (now_ms - ntp_last_sync_ms > NTP_RESYNC_INTERVAL)) {
                // 定期校准：已同步但超过 24 小时，重新同步一次
                ESP_LOGI(TAG, "距上次 NTP 同步已超过 24 小时，重新校准...");
                should_sync = true;
            }
            
            if (should_sync) {
                ESP_LOGI(TAG, "同步 NTP 时间 (第 %d 次)...", ntp_retry_count + 1);
                SensorManager::getInstance().syncNtpTime();
                
                // 检查时间是否合理（年份 > 2024 说明同步成功了）
                time_t now_check;
                struct tm check_info;
                time(&now_check);
                localtime_r(&now_check, &check_info);
                if (check_info.tm_year + 1900 >= 2024) {
                    time_synced = true;
                    ntp_retry_count = 0;           // 重置重试计数
                    ntp_retry_delay_ms = 1000;     // 重置退避延迟
                    ntp_last_sync_ms = now_ms;     // 记录同步成功时间
                    self->last_min_ = -1;          // 强制刷新 UI
                    time(&self->last_valid_epoch_);
                    ESP_LOGI(TAG, "NTP 同步成功: %04d-%02d-%02d %02d:%02d",
                             check_info.tm_year + 1900, check_info.tm_mon + 1, check_info.tm_mday,
                             check_info.tm_hour, check_info.tm_min);
                } else {
                    ntp_retry_count++;
                    ESP_LOGW(TAG, "NTP 同步失败（年份=%d），第 %d/%d 次，%d 秒后重试", 
                             check_info.tm_year + 1900, ntp_retry_count, NTP_MAX_RETRIES,
                             (int)(ntp_retry_delay_ms / 1000));
                    // 指数退避等待后再继续循环
                    vTaskDelay(pdMS_TO_TICKS(ntp_retry_delay_ms));
                    ntp_retry_delay_ms *= 2;  // 下次翻倍：1s → 2s → 4s → 8s → 16s
                    if (ntp_retry_delay_ms > 16000) ntp_retry_delay_ms = 16000;
                    
                    if (ntp_retry_count >= NTP_MAX_RETRIES) {
                        ESP_LOGE(TAG, "NTP 同步已失败 %d 次，放弃重试（使用 RTC 时间）", NTP_MAX_RETRIES);
                    }
                }
            }
        }
        
        // ===== 天气更新 =====
        // 暂时停用板载和风天气自动拉取，天气由 AI 通过 MCP 主动写入
        
        // ===== 时间获取（在锁外也需要用，所以先获取）=====
        time_t now;
        time(&now);
        localtime_r(&now, &timeinfo);
        
        // 记录本轮是否分钟变了（备忘闹钟检查也要用）
        bool minute_changed = (timeinfo.tm_min != self->last_min_);
        
        // ===== UI 更新（每秒）=====
        // 🔑 如果正在显示系统信息滚动，跳过整个 UI 更新块（避免锁竞争）
        if (!self->showing_system_info_) {
            DisplayLockGuard lock(self);
            
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
                        minute_changed = true;  // 时间恢复后强制触发
                        ESP_LOGI(TAG, "已从 RTC 恢复: %02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
                    }
                }
                self->last_valid_epoch_ = now;
            }
            
            // 每分钟或强制刷新时更新 UI
            if (minute_changed) {
                char time_buf[16];
                strftime(time_buf, sizeof(time_buf), "%H:%M", &timeinfo);
                if (self->time_label_) lv_label_set_text(self->time_label_, time_buf);
                if (self->music_time_label_) lv_label_set_text(self->music_time_label_, time_buf);
                if (self->pomo_time_label_) lv_label_set_text(self->pomo_time_label_, time_buf);

                const char *weeks_en[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
                if (self->day_label_) lv_label_set_text(self->day_label_, weeks_en[timeinfo.tm_wday]);

                char date_buf[8];
                snprintf(date_buf, sizeof(date_buf), "%d", timeinfo.tm_mday);
                if (self->date_num_label_) lv_label_set_text(self->date_num_label_, date_buf);

                self->last_min_ = timeinfo.tm_min;
                ESP_LOGI(TAG, "时间已更新: %s, %s, %d日", time_buf, weeks_en[timeinfo.tm_wday], timeinfo.tm_mday);
            }
        }  // DisplayLockGuard 自动释放

        // ===== 备忘闹钟检查（在锁外执行，避免长时间持锁）=====
        if (minute_changed) {
            if (time_synced) {
                Settings memo_rd("memo", false);
                std::string memo_json = memo_rd.GetString("items", "");
                
                // 🔍 调试：打印当前时间和备忘录 JSON
                char time_buf[16];
                strftime(time_buf, sizeof(time_buf), "%H:%M", &timeinfo);
                ESP_LOGI(TAG, "⏰ 备忘闹钟检查: 当前时间=%s, NVS数据=%s", 
                         time_buf, memo_json.empty() ? "(空)" : memo_json.c_str());
                
                if (!memo_json.empty()) {
                    cJSON *memo_arr = cJSON_Parse(memo_json.c_str());
                    if (memo_arr && cJSON_IsArray(memo_arr)) {
                        bool memo_changed = false;
                        int total_items = cJSON_GetArraySize(memo_arr);
                        ESP_LOGI(TAG, "⏰ 备忘列表共 %d 条", total_items);
                        
                        // 倒序遍历，这样删除不会打乱前面的索引
                        for (int mi = total_items - 1; mi >= 0; mi--) {
                            cJSON *memo_item = cJSON_GetArrayItem(memo_arr, mi);
                            cJSON *mt = cJSON_GetObjectItem(memo_item, "t");
                            cJSON *mc = cJSON_GetObjectItem(memo_item, "c");
                            
                            // 🔍 调试：打印每条备忘的时间
                            if (mt && mt->valuestring) {
                                ESP_LOGI(TAG, "⏰ 检查备忘[%d]: 时间=%s, 内容=%s", 
                                         mi, mt->valuestring, 
                                         (mc && mc->valuestring) ? mc->valuestring : "(空)");
                            }
                            
                            // 只匹配 "HH:MM" 格式（5个字符，中间是冒号）
                            if (mt && mt->valuestring && strlen(mt->valuestring) == 5 
                                && mt->valuestring[2] == ':') {
                                if (strcmp(mt->valuestring, time_buf) == 0) {
                                    const char *memo_text = (mc && mc->valuestring) ? mc->valuestring : "备忘提醒";
                                    char alert_buf[128];
                                    snprintf(alert_buf, sizeof(alert_buf), "备忘提醒: %s %s", mt->valuestring, memo_text);
                                    ESP_LOGI(TAG, "🔔 触发备忘闹钟: %s", alert_buf);
                                    
                                    // 播放提示音 + 屏幕显示提醒
                                    app.Alert("提醒", alert_buf, "happy", Lang::Sounds::OGG_POPUP);

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
                            ESP_LOGI(TAG, "✅ 已过期备忘已自动删除");
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
            static uint32_t last_noncritical_ui_update_ms = 0;
            const bool allow_noncritical_update =
                !in_audio_session ||
                last_noncritical_ui_update_ms == 0 ||
                (now_ms - last_noncritical_ui_update_ms >= 5000);

            // 对话期间将非关键 UI/传感器刷新降频到 5 秒一次，避免与语音链路抢占 CPU
            if (allow_noncritical_update) {
                last_noncritical_ui_update_ms = now_ms;

                // 2. 温湿度更新
                SensorData sd = SensorManager::getInstance().getTempHumidity();
                if (sd.valid) {
                    if (fabs(sd.temperature - self->last_temp_) > 0.2f || fabs(sd.humidity - self->last_humi_) > 1.0f) {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%.1f°C  %.0f%%", sd.temperature, sd.humidity);
                        if (self->sensor_label_) lv_label_set_text(self->sensor_label_, buf);
                        if (self->music_sensor_label_) lv_label_set_text(self->music_sensor_label_, buf);
                        if (self->pomo_sensor_label_) lv_label_set_text(self->pomo_sensor_label_, buf);
                        self->last_temp_ = sd.temperature;
                        self->last_humi_ = sd.humidity;
                    }
                }

                // 3. 天气更新（内容变化时才刷新，避免无效重绘）
                WeatherData wd = WeatherManager::getInstance().getLatestData();
                if (wd.valid && self->weather_label_) {
                    char weather_buf[48];
                    snprintf(weather_buf, sizeof(weather_buf), "%s %s %s°C",
                             wd.city.c_str(), wd.text.c_str(), wd.temp.c_str());
                    static std::string last_weather_text;
                    if (last_weather_text != weather_buf) {
                        lv_label_set_text(self->weather_label_, weather_buf);
                        last_weather_text = weather_buf;
                    }
                }

                // 4. 电池状态更新（采样降频 + 变化更新）
                auto& board = Board::GetInstance();
                if (!battery_cached || last_battery_poll_ms == 0 ||
                    (now_ms - last_battery_poll_ms >= BATTERY_POLL_INTERVAL)) {
                    int level = 0;
                    bool charging = false, discharging = false;
                    if (board.GetBatteryLevel(level, charging, discharging)) {
                        cached_battery_level = level;
                        cached_charging = charging;
                        cached_discharging = discharging;
                        battery_cached = true;
                        last_battery_poll_ms = now_ms;
                    }
                }

                if (battery_cached) {
                    static int last_icon_mode = -1;       // 0=low,1=medium,2=full,3=charging
                    static int last_battery_level = -1;   // 上次显示的电量百分比
                    int icon_mode = 2;
                    if (cached_charging) {
                        icon_mode = 3;
                    } else if (cached_battery_level < 20) {
                        icon_mode = 0;
                    } else if (cached_battery_level < 60) {
                        icon_mode = 1;
                    }

                    if (icon_mode != last_icon_mode) {
                        const void* icon_src = &ui_img_battery_full;
                        if (icon_mode == 3) icon_src = &ui_img_battery_charging;
                        else if (icon_mode == 0) icon_src = &ui_img_battery_low;
                        else if (icon_mode == 1) icon_src = &ui_img_battery_medium;

                        if (self->battery_icon_img_) {
                            lv_image_set_src(self->battery_icon_img_, icon_src);
                        }
                        if (self->music_battery_icon_img_) {
                            lv_image_set_src(self->music_battery_icon_img_, icon_src);
                        }
                        if (self->pomo_battery_icon_img_) {
                            lv_image_set_src(self->pomo_battery_icon_img_, icon_src);
                        }
                        last_icon_mode = icon_mode;
                    }

                    if (self->battery_pct_label_ && cached_battery_level != last_battery_level) {
                        char bat_buf[16];
                        snprintf(bat_buf, sizeof(bat_buf), "%d%%", cached_battery_level);
                        lv_label_set_text(self->battery_pct_label_, bat_buf);
                        if (self->music_battery_pct_label_) lv_label_set_text(self->music_battery_pct_label_, bat_buf);
                        if (self->pomo_battery_pct_label_) lv_label_set_text(self->pomo_battery_pct_label_, bat_buf);
                        last_battery_level = cached_battery_level;
                    }

                    // 低电量提醒（对齐原版行为）：
                    // - 放电且低于 20% 时显示弹窗并播一次提示音
                    // - 回升到 25% 及以上（或进入充电）后隐藏，避免 19/20% 抖动反复闪烁
                    static bool low_battery_popup_visible = false;
                    const bool should_show_low_battery = (!cached_charging && cached_discharging && cached_battery_level < 20);
                    const bool should_hide_low_battery = (cached_charging || !cached_discharging || cached_battery_level >= 25);
                    if (self->low_battery_popup_) {
                        if (!low_battery_popup_visible && should_show_low_battery) {
                            lv_obj_remove_flag(self->low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                            app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY);
                            low_battery_popup_visible = true;
                        } else if (low_battery_popup_visible && should_hide_low_battery) {
                            lv_obj_add_flag(self->low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                            low_battery_popup_visible = false;
                        }
                    }
                }

                // 5. WiFi 图标更新（状态变化时才更新）
                static DeviceState last_wifi_state = kDeviceStateUnknown;
                if (ds != last_wifi_state) {
                    const void* wifi_src = &ui_img_wifi_off;
                    if (ds != kDeviceStateStarting && ds != kDeviceStateWifiConfiguring) {
                        wifi_src = &ui_img_wifi;
                    } else if (ds == kDeviceStateWifiConfiguring) {
                        wifi_src = &ui_img_wifi_low;
                    }
                    if (self->wifi_icon_img_) {
                        lv_image_set_src(self->wifi_icon_img_, wifi_src);
                    }
                    if (self->music_wifi_icon_img_) {
                        lv_image_set_src(self->music_wifi_icon_img_, wifi_src);
                    }
                    if (self->pomo_wifi_icon_img_) {
                        lv_image_set_src(self->pomo_wifi_icon_img_, wifi_src);
                    }
                    last_wifi_state = ds;
                }
            }

            // 6. AI 状态更新
            static DeviceState last_ds = kDeviceStateUnknown;
            if (ds != last_ds) {
                // AI 状态发生变化（对话、聆听等），视为用户活动
                if (ds == kDeviceStateListening || ds == kDeviceStateSpeaking || 
                    ds == kDeviceStateConnecting) {
                    self->NotifyUserActivity();
                }

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
                if (self->music_emotion_label_) {
                    lv_label_set_text(self->music_emotion_label_, emotion_text);
                }
                // 番茄钟运行中时，番茄钟页面的 AI 卡片不被普通状态变化覆盖
                // 只在空闲时才让番茄钟页面跟随设备状态
                auto& pomo_inst = PomodoroManager::getInstance();
                bool pomo_running = (pomo_inst.getState() != PomodoroManager::IDLE);

                if (!pomo_running && self->pomo_emotion_label_) {
                    lv_label_set_text(self->pomo_emotion_label_, emotion_text);
                }
                // 非说话/配网/激活状态时更新右侧文字（这些状态由 Alert/SetChatMessage 管理详细信息）
                if (ds != kDeviceStateSpeaking && ds != kDeviceStateWifiConfiguring &&
                    ds != kDeviceStateActivating && self->chat_status_label_ && strlen(status_text) > 0) {
                    // 状态短文案（如“聆听中...”）必须强制不滚动，避免继承上一条长文本动画
                    self->SetShowingSystemInfo(false);
                    lv_anim_delete(self->chat_status_label_, nullptr);
                    lv_label_set_long_mode(self->chat_status_label_, LV_LABEL_LONG_WRAP);
                    lv_obj_align(self->chat_status_label_, LV_ALIGN_LEFT_MID, 64 + 20, 0);
                    lv_label_set_text(self->chat_status_label_, status_text);
                }
                if (ds != kDeviceStateSpeaking && ds != kDeviceStateWifiConfiguring &&
                    ds != kDeviceStateActivating && self->music_chat_status_label_ && strlen(status_text) > 0) {
                    lv_label_set_long_mode(self->music_chat_status_label_, LV_LABEL_LONG_WRAP);
                    lv_label_set_text(self->music_chat_status_label_, status_text);
                }
                if (!pomo_running && ds != kDeviceStateSpeaking && ds != kDeviceStateWifiConfiguring &&
                    ds != kDeviceStateActivating && self->pomo_chat_status_label_ && strlen(status_text) > 0) {
                    lv_label_set_long_mode(self->pomo_chat_status_label_, LV_LABEL_LONG_WRAP);
                    lv_label_set_text(self->pomo_chat_status_label_, status_text);
                }
                last_ds = ds;
            }
        }  // DisplayLockGuard 自动释放

        // ===== 番茄钟 UI 刷新 =====
        // 番茄钟运行时，每秒更新倒计时显示和进度条
        {
            auto& pomo = PomodoroManager::getInstance();
            auto pomo_state = pomo.getState();
            if (pomo_state != PomodoroManager::IDLE && self->pomo_countdown_label_) {
                // 计算进度（千分比）
                int total = pomo.getTotalSeconds();
                int remaining = pomo.getRemainingSeconds();
                int progress = 0;
                if (total > 0) {
                    progress = ((total - remaining) * 1000) / total;
                }

                // 状态文字
                const char* state_text = "倒计时中";
                if (pomo_state == PomodoroManager::PAUSED) {
                    state_text = "已暂停";
                }

                // 设定信息
                char info_buf[64];
                snprintf(info_buf, sizeof(info_buf), "共 %d 分钟", pomo.getMinutes());

                // 更新 UI
                self->UpdatePomodoroDisplay(
                    state_text,
                    pomo.getRemainingTimeStr().c_str(),
                    progress,
                    info_buf
                );

                // 番茄钟运行中时，覆盖底部 AI 卡的显示
                // 只在 AI 不说话时更新（说话时由 SetChatMessage 管理）
                if (ds != kDeviceStateSpeaking) {
                    DisplayLockGuard pomo_lock(self);
                    if (self->pomo_emotion_label_) {
                        const char* pomo_emoji = (pomo_state == PomodoroManager::PAUSED) ? "暂停" : "专注";
                        lv_label_set_text(self->pomo_emotion_label_, pomo_emoji);
                    }
                    if (self->pomo_chat_status_label_) {
                        char pomo_status_buf[64];
                        if (pomo_state == PomodoroManager::PAUSED) {
                            snprintf(pomo_status_buf, sizeof(pomo_status_buf), "已暂停 · 剩余 %s", pomo.getRemainingTimeStr().c_str());
                        } else {
                            snprintf(pomo_status_buf, sizeof(pomo_status_buf), "专注中 · 剩余 %s", pomo.getRemainingTimeStr().c_str());
                        }
                        lv_label_set_long_mode(self->pomo_chat_status_label_, LV_LABEL_LONG_WRAP);
                        lv_label_set_text(self->pomo_chat_status_label_, pomo_status_buf);
                    }
                }
            }
        }

        // ===== 省电模式检测 =====
        // 5 分钟无活动（无按钮、无 AI 对话）时进入省电模式，降低刷新频率
        // 注意：必须重新取当前时间，因为 NotifyUserActivity() 可能在本轮循环中
        // 被 AI 状态变化触发过，如果用循环开头的 now_ms 会导致 uint32 溢出
        {
            uint32_t check_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            uint32_t activity_ms = self->last_activity_ms_;
            if (!self->power_saving_ && activity_ms > 0 && check_ms >= activity_ms) {
                if (check_ms - activity_ms > self->IDLE_TIMEOUT_MS) {
                    self->power_saving_ = true;
                    ESP_LOGI(TAG, "⚡ 5 分钟无活动，进入省电模式（刷新间隔 %d 秒 → %d 秒）",
                             self->NORMAL_REFRESH_MS / 1000, self->SAVING_REFRESH_MS / 1000);
                }
            }
        }
        
        // 动态刷新间隔：正常 1 秒，省电 5 秒
        int delay_ms = self->power_saving_ ? self->SAVING_REFRESH_MS : self->NORMAL_REFRESH_MS;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}
