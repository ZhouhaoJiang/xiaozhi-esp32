#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_log.h>
#include <esp_system.h>
#include <soc/rtc.h>
#include "custom_lcd_display.h"
#include "wifi_board.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "codecs/box_audio_codec.h"
#include "wifi_station.h"
#include "mcp_server.h"
#include "settings.h"
#include <cJSON.h>
#include "lvgl.h"
#include "managers/sensor_manager.h"

// 声明小智字体（用于系统信息显示时临时切换字体大小）
LV_FONT_DECLARE(font_puhui_14_1);
LV_FONT_DECLARE(font_puhui_16_4);
#include "managers/weather_manager.h"

#define TAG "waveshare_rlcd_4_2"

class CustomBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Button user_button_;  // GPIO18 用户按键
    CustomLcdDisplay *display_;
    adc_oneshot_unit_handle_t adc1_handle;
    adc_cali_handle_t cali_handle;

    void InitializeI2c() {
        // I2C 总线初始化
        // 这条 I2C 总线被多个设备共享：
        // - ES8311 音频解码器
        // - ES7210 音频编码器
        // - SHTC3 温湿度传感器
        // - PCF85063 RTC 时钟
        i2c_master_bus_config_t i2c_bus_cfg = {};
        i2c_bus_cfg.i2c_port = ESP32_I2C_HOST;
        i2c_bus_cfg.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
        i2c_bus_cfg.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
        i2c_bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_bus_cfg.glitch_ignore_cnt = 7;
        i2c_bus_cfg.intr_priority = 0;
        i2c_bus_cfg.trans_queue_depth = 0;
        i2c_bus_cfg.flags.enable_internal_pullup = 1;
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeSensors() {
        // 初始化传感器（使用同一条 I2C 总线）
        SensorManager::getInstance().init(i2c_bus_);
        ESP_LOGI(TAG, "传感器初始化完成");
    }

    void InitializeButtons() { 
        // BOOT 按钮（GPIO0）- 主要交互按键
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        // USER 按钮（GPIO18）- 辅助功能按键
        user_button_.OnClick([this]() {
            // 单击：切换屏幕显示模式（预留给多布局切换功能）
            // TODO: 实现多屏幕模式切换
            if (display_) {
                display_->SetChatMessage("system", "屏幕模式切换\n功能开发中...");
            }
            ESP_LOGI(TAG, "USER 按钮单击：屏幕模式切换（待实现）");
        });

        user_button_.OnDoubleClick([this]() {
            // 双击：刷新所有数据（天气、传感器、时间）
            RefreshAllData();
        });

        user_button_.OnLongPress([this]() {
            // 长按：显示系统信息
            ShowSystemInfo();
        });
    }

    // USER 按钮功能实现
    void ShowSystemInfo() {
        // 显示详细系统信息到 AI 对话区（启用多行滚动）
        char info[512];
        
        // 内存信息
        size_t free_heap = esp_get_free_heap_size();
        size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        
        // CPU 信息
        rtc_cpu_freq_config_t cpu_freq_conf;
        rtc_clk_cpu_freq_get_config(&cpu_freq_conf);
        uint32_t cpu_freq_mhz = cpu_freq_conf.freq_mhz;
        
        // 电池信息
        int battery_level = 0;
        bool charging = false, discharging = false;
        GetBatteryLevel(battery_level, charging, discharging);
        
        // WiFi 信息
        auto& app = Application::GetInstance();
        const char* wifi_status = "未连接";
        if (app.GetDeviceState() != kDeviceStateStarting && 
            app.GetDeviceState() != kDeviceStateWifiConfiguring) {
            wifi_status = "已连接";
        }
        
        // 运行时间
        uint64_t uptime_sec = esp_timer_get_time() / 1000000;
        uint32_t uptime_hours = uptime_sec / 3600;
        uint32_t uptime_mins = (uptime_sec % 3600) / 60;
        
        // 计算百分比
        int heap_percent = (int)(((total_heap - free_heap) * 100.0f) / total_heap);
        int psram_percent = total_psram > 0 ? 
                           (int)(((total_psram - free_psram) * 100.0f) / total_psram) : 0;
        
        // 详细格式（单份内容，用于鱼咬尾拼接）
        snprintf(info, sizeof(info), 
                 "=== 系统信息 ===\n"
                 "CPU: %luMHz\n"
                 "运行: %luh%lumin\n"
                 "SRAM: \n %dKB/%dKB (%d%%)\n"
                 "PSRAM: \n %dMB/%dMB (%d%%)\n"
                 "电池: %d%% %s\n"
                 "WiFi: %s\n"
                 "==============\n"
                 "\n",  // 分隔符
                 cpu_freq_mhz,
                 uptime_hours, uptime_mins,
                 (total_heap - free_heap) / 1024, total_heap / 1024, heap_percent,
                 (total_psram - free_psram) / 1024 / 1024, total_psram / 1024 / 1024, psram_percent,
                 battery_level, charging ? "充电中" : "放电中",
                 wifi_status);
        
        if (display_) {
            lv_obj_t* chat_label = display_->GetChatStatusLabel();
            if (chat_label) {
                // 暂停 DataUpdateTask 对 UI 的更新（避免锁竞争导致 watchdog 超时）
                display_->SetShowingSystemInfo(true);
                
                {
                    DisplayLockGuard lock(display_);
                    
                    // 先删除旧动画（防止冲突）
                    lv_anim_delete(chat_label, nullptr);
                    
                    // 🐟 鱼咬尾：拼接两份相同内容
                    std::string info_double = std::string(info) + std::string(info);
                    
                    // 🔑 关键修复：切换到 TOP_LEFT 绝对定位
                    // 原因：label 初始化时用的是 LV_ALIGN_LEFT_MID（居中对齐），
                    // LVGL 内部会存储这个对齐方式，布局刷新时会重新计算位置，
                    // 导致动画里 set_y 设的值被覆盖。
                    // 切换到 TOP_LEFT 后，Y=0 就是父容器顶部，动画不会被干扰。
                    const int text_x = 64 + 20;  // emotion_w + 间距，保持文字在分隔线右侧
                    lv_obj_align(chat_label, LV_ALIGN_TOP_LEFT, text_x, 0);
                    
                    lv_label_set_text(chat_label, info_double.c_str());
                    lv_label_set_long_mode(chat_label, LV_LABEL_LONG_WRAP);
                    
                    // 强制计算布局，获取实际高度
                    lv_obj_update_layout(chat_label);
                    int label_h = lv_obj_get_height(chat_label);  // 双份内容的总高度
                    int single_h = label_h / 2;  // 单份内容高度
                    
                    // 🐟 鱼咬尾动画原理：
                    // 内容 = [A][A]（两份完全相同的文字首尾相接）
                    // Y=0 时显示第一个 A 的开头
                    // 向上滚动到 Y=-single_h 时，显示第二个 A 的开头
                    // 因为两个 A 完全一样，动画重复跳回 Y=0 时视觉上无缝衔接！
                    lv_anim_t a;
                    lv_anim_init(&a);
                    lv_anim_set_var(&a, chat_label);
                    lv_anim_set_values(&a, 0, -single_h);
                    lv_anim_set_delay(&a, 1500);  // 开始前停顿 1.5 秒，让用户先看到开头
                    lv_anim_set_duration(&a, single_h * 30);  // 速度：每像素 30ms
                    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
                    lv_anim_set_repeat_delay(&a, 0);  // 无缝重复，不停顿
                    lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
                        lv_obj_set_y((lv_obj_t *)obj, v);
                    });
                    lv_anim_start(&a);
                }  // ← DisplayLockGuard 在这里自动释放
            }
        }
        
        ESP_LOGI(TAG, "系统信息: CPU=%luMHz, 运行=%luh%lumin, SRAM=%d%%, PSRAM=%d%%, 电量=%d%%", 
                 cpu_freq_mhz, uptime_hours, uptime_mins, heap_percent, psram_percent, battery_level);
    }

    void RefreshAllData() {
        ESP_LOGI(TAG, "手动刷新所有数据...");
        
        // 立即更新天气数据
        WeatherManager::getInstance().update();
        
        // 重新同步 NTP 时间
        SensorManager::getInstance().syncNtpTime();
        
        // 强制刷新屏幕显示
        if (display_) {
            display_->SetChatMessage("system", "正在刷新数据...\n天气、时间已更新");
        }
        
        ESP_LOGI(TAG, "数据刷新完成");
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        
        // ===== 系统信息工具 =====
        mcp_server.AddTool("self.system.info",
            "Get device system information (CPU, memory, battery, WiFi status).\n"
            "Use when user asks: '系统信息', 'CPU频率', '内存使用情况', '电量多少', 'system status', 'how much RAM'",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                // 收集系统信息
                size_t free_heap = esp_get_free_heap_size();
                size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
                size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
                
                rtc_cpu_freq_config_t cpu_freq_conf;
                rtc_clk_cpu_freq_get_config(&cpu_freq_conf);
                uint32_t cpu_freq_mhz = cpu_freq_conf.freq_mhz;
                
                int battery_level = 0;
                bool charging = false, discharging = false;
                GetBatteryLevel(battery_level, charging, discharging);
                
                auto& app = Application::GetInstance();
                const char* wifi_status = "未连接";
                if (app.GetDeviceState() != kDeviceStateStarting && 
                    app.GetDeviceState() != kDeviceStateWifiConfiguring) {
                    wifi_status = "已连接";
                }
                
                uint64_t uptime_sec = esp_timer_get_time() / 1000000;
                uint32_t uptime_hours = uptime_sec / 3600;
                uint32_t uptime_mins = (uptime_sec % 3600) / 60;
                
                int heap_percent = (int)(((total_heap - free_heap) * 100.0f) / total_heap);
                int psram_percent = total_psram > 0 ? 
                                   (int)(((total_psram - free_psram) * 100.0f) / total_psram) : 0;
                
                // 格式化为自然语言（AI 容易读出来）
                char info[512];
                snprintf(info, sizeof(info),
                         "系统运行正常。CPU频率%luMHz，已运行%lu小时%lu分钟。"
                         "内存方面，SRAM使用了%dKB，占总量%dKB的%d%%；"
                         "PSRAM使用了%dMB，占总量%dMB的%d%%。"
                         "电池电量%d%%，当前%s。WiFi%s。",
                         cpu_freq_mhz, uptime_hours, uptime_mins,
                         (total_heap - free_heap) / 1024, total_heap / 1024, heap_percent,
                         (total_psram - free_psram) / 1024 / 1024, total_psram / 1024 / 1024, psram_percent,
                         battery_level, charging ? "正在充电" : "使用电池供电",
                         wifi_status);
                
                ESP_LOGI(TAG, "AI查询系统信息");
                return std::string(info);
            });
        
        // ===== 配网工具 =====
        mcp_server.AddTool("self.disp.network", "重新配网", PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            EnterWifiConfigMode();
            return true;
        });

        // ===== 备忘录工具（多条列表模式）=====
        // NVS key "items" 存储 JSON 数组: [{"t":"15:00","c":"开会"}, ...]

        // 添加一条备忘
        mcp_server.AddTool("self.memo.add",
            "Add a memo / reminder / todo item. It will be persistently displayed on the device screen and survives reboot.\n"
            "Use when user says: '提醒我下午3点开会', '记住买牛奶', '待办写周报'\n"
            "Args:\n"
            "  `content`: Short memo text (max ~8 Chinese chars for best display on the small screen)\n"
            "  `time`: Time label (e.g. '15:00', '明天', '周五'). Empty string if no specific time.",
            PropertyList({
                Property("content", kPropertyTypeString),
                Property("time", kPropertyTypeString, std::string(""))
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto content = properties["content"].value<std::string>();
                auto time_str = properties["time"].value<std::string>();

                // 读取现有列表
                std::string json_str;
                {
                    Settings settings("memo", false);
                    json_str = settings.GetString("items", "[]");
                }

                cJSON *arr = cJSON_Parse(json_str.c_str());
                if (!arr) arr = cJSON_CreateArray();

                // 限制最多 10 条
                if (cJSON_GetArraySize(arr) >= 10) {
                    cJSON_Delete(arr);
                    return std::string("备忘已满（最多10条），请先完成或清除一些");
                }

                // 追加新条目
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "t", time_str.c_str());
                cJSON_AddStringToObject(item, "c", content.c_str());
                cJSON_AddItemToArray(arr, item);

                // 写回 NVS
                char *new_json = cJSON_PrintUnformatted(arr);
                {
                    Settings settings("memo", true);
                    settings.SetString("items", new_json);
                }
                int count = cJSON_GetArraySize(arr);
                cJSON_free(new_json);
                cJSON_Delete(arr);

                // 刷新屏幕
                if (display_) display_->RefreshMemoDisplay();
                ESP_LOGI(TAG, "备忘已添加: %s", content.c_str());
                return std::string("已添加备忘: ") + content + "（共" + std::to_string(count) + "条）";
            });

        // 查看所有备忘
        mcp_server.AddTool("self.memo.list",
            "List all memos / reminders / todos on the device.\n"
            "Use when user asks: '我有什么待办', '看看备忘', 'what do I need to do'",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                Settings settings("memo", false);
                std::string json_str = settings.GetString("items", "[]");

                cJSON *arr = cJSON_Parse(json_str.c_str());
                if (!arr || cJSON_GetArraySize(arr) == 0) {
                    if (arr) cJSON_Delete(arr);
                    return std::string("当前没有备忘");
                }

                std::string result = "当前备忘列表:\n";
                int count = cJSON_GetArraySize(arr);
                for (int i = 0; i < count; i++) {
                    cJSON *item = cJSON_GetArrayItem(arr, i);
                    cJSON *t = cJSON_GetObjectItem(item, "t");
                    cJSON *c = cJSON_GetObjectItem(item, "c");
                    result += std::to_string(i + 1) + ". ";
                    if (t && strlen(t->valuestring) > 0) {
                        result += "[";
                        result += t->valuestring;
                        result += "] ";
                    }
                    if (c) result += c->valuestring;
                    result += "\n";
                }
                cJSON_Delete(arr);
                return result;
            });

        // 完成/删除某条备忘（按序号）
        mcp_server.AddTool("self.memo.done",
            "Mark a memo as done and remove it from the list.\n"
            "Use when user says: '第一条做完了', '删掉买牛奶那条', '完成了开会'\n"
            "Args:\n"
            "  `index`: 1-based index of the memo to remove. If unsure, call self.memo.list first.",
            PropertyList({
                Property("index", kPropertyTypeInteger, 1, 10)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int idx = properties["index"].value<int>();

                std::string json_str;
                {
                    Settings settings("memo", false);
                    json_str = settings.GetString("items", "[]");
                }

                cJSON *arr = cJSON_Parse(json_str.c_str());
                if (!arr) return std::string("备忘列表为空");

                int count = cJSON_GetArraySize(arr);
                if (idx < 1 || idx > count) {
                    cJSON_Delete(arr);
                    return std::string("序号无效，当前共") + std::to_string(count) + "条备忘";
                }

                // 获取被删除条目的内容用于反馈
                cJSON *removed = cJSON_GetArrayItem(arr, idx - 1);
                cJSON *c = cJSON_GetObjectItem(removed, "c");
                std::string removed_text = (c && c->valuestring) ? c->valuestring : "";

                cJSON_DeleteItemFromArray(arr, idx - 1);

                // 写回 NVS
                char *new_json = cJSON_PrintUnformatted(arr);
                {
                    Settings settings("memo", true);
                    settings.SetString("items", new_json);
                }
                cJSON_free(new_json);
                cJSON_Delete(arr);

                // 刷新屏幕
                if (display_) display_->RefreshMemoDisplay();
                ESP_LOGI(TAG, "备忘已完成: %s", removed_text.c_str());
                return std::string("已完成: ") + removed_text;
            });

        // 清空所有备忘
        mcp_server.AddTool("self.memo.clear",
            "Clear ALL memos / reminders / todos.\n"
            "Use when user says: '清空备忘', '全部删掉', 'clear all memos'",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                {
                    Settings settings("memo", true);
                    settings.EraseKey("items");
                }
                if (display_) display_->RefreshMemoDisplay();
                ESP_LOGI(TAG, "所有备忘已清除");
                return std::string("所有备忘已清除");
            });
    }

    void InitializeLcdDisplay() {
        spi_display_config_t spi_config = {};
        spi_config.mosi = RLCD_MOSI_PIN;
        spi_config.scl = RLCD_SCK_PIN;
        spi_config.dc = RLCD_DC_PIN;
        spi_config.cs = RLCD_CS_PIN;
        spi_config.rst = RLCD_RST_PIN;
        display_ = new CustomLcdDisplay(NULL, NULL, RLCD_WIDTH, RLCD_HEIGHT,
            DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY, spi_config);
        
        // 启动天气站数据更新任务
        display_->StartDataUpdateTask();
    }

    uint16_t BatterygetVoltage(void) {
        static bool initialized = false;
        static adc_oneshot_unit_handle_t adc_handle;
        static adc_cali_handle_t cali_handle = NULL;
        if (!initialized) {
            adc_oneshot_unit_init_cfg_t init_config = {
                .unit_id = ADC_UNIT_1,
            };
            adc_oneshot_new_unit(&init_config, &adc_handle);
    
            adc_oneshot_chan_cfg_t ch_config = {
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_3, &ch_config);
    
            adc_cali_curve_fitting_config_t cali_config = {
                .unit_id = ADC_UNIT_1,
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            if (adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle) == ESP_OK) {
                initialized = true;
            }
        }

        if (initialized) {
            int raw_value = 0;
            int raw_voltage = 0;
            int voltage = 0;
            adc_oneshot_read(adc_handle, ADC_CHANNEL_3, &raw_value);
            adc_cali_raw_to_voltage(cali_handle, raw_value, &raw_voltage);
            voltage = raw_voltage * 3;  // 分压电阻比例
            return (uint16_t)voltage;
        }
        return 0;
    }

    uint8_t BatterygetPercent() {
        // 静态变量用于指数移动平均（EMA）滤波，消除 ADC 噪声导致的电量漂移
        static float ema_voltage = 0.0f;    // 平滑后的电压值
        static bool ema_initialized = false;
        const float alpha = 0.1f;           // 平滑系数：越小越平滑（0.1 ≈ 约 10 次采样才能跟上真实变化）

        // 采样 10 次取平均（减少瞬时噪声）
        int voltage = 0;
        for (uint8_t i = 0; i < 10; i++) {
            voltage += BatterygetVoltage();
        }
        voltage /= 10;

        // EMA 滤波：new_value = alpha * 当前值 + (1-alpha) * 历史值
        if (!ema_initialized) {
            ema_voltage = (float)voltage;
            ema_initialized = true;
        } else {
            ema_voltage = alpha * (float)voltage + (1.0f - alpha) * ema_voltage;
        }

        int smoothed = (int)(ema_voltage + 0.5f);  // 四舍五入
        // 电压→百分比映射（抛物线拟合）
        int percent = (-1 * smoothed * smoothed + 9016 * smoothed - 19189000) / 10000;
        percent = (percent > 100) ? 100 : (percent < 0) ? 0 : percent;
        return (uint8_t)percent;
    }

public:
    CustomBoard() : boot_button_(BOOT_BUTTON_GPIO), user_button_(USER_BUTTON_GPIO) {    
        InitializeI2c();
        InitializeSensors();  // 在 I2C 初始化后立即初始化传感器
        InitializeButtons();     
        InitializeTools();
        InitializeLcdDisplay();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        charging = false;
        discharging = !charging;
        level = (int)BatterygetPercent();
        return true;
    }
};

DECLARE_BOARD(CustomBoard);
