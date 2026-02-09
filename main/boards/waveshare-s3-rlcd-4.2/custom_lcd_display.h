#ifndef __CUSTOM_LCD_DISPLAY_H__
#define __CUSTOM_LCD_DISPLAY_H__

#include <driver/gpio.h>
#include "lcd_display.h"
#include "managers/sensor_manager.h"
#include "managers/weather_manager.h"

enum ColorSelection {
    ColorBlack = 0,    
    ColorWhite = 0xff
};

typedef struct {
    uint8_t mosi;
    uint8_t scl;
    uint8_t dc;
    uint8_t cs;
    uint8_t rst;
} spi_display_config_t;

// 天气站 + AI 混合显示
// 
// 屏幕布局 (400x300, 1-bit 单色 RLCD)：
// ┌──────────────────┬──────────────────┐
// │   时钟卡片(248x128) │  日历卡片(130x128) │
// │    "14:30"        │   TUE / 15      │
// │                   │   晴 25°C       │
// ├──────────────────┼──────────────────┤
// │   AI 对话(252x122) │  音乐卡片(126x122) │
// │  "聆听中..."      │   🎵 黑胶唱片    │
// └──────────────────┴──────────────────┘
// 状态栏浮在右上角（WiFi + 电池 + 温湿度）
class CustomLcdDisplay : public LcdDisplay {
private:
    esp_lcd_panel_io_handle_t io_handle = NULL;
    const char         *TAG                 = "CustomDisplay";
    int                 mosi_;
    int                 scl_;
    int                 dc_;
    int                 cs_;
    int                 rst_;
    int                 width_;
    int                 height_;
    uint8_t            *DispBuffer = NULL;
    int                 DisplayLen;
    uint16_t (*PixelIndexLUT)[300];
    uint8_t  (*PixelBitLUT  )[300];
    void InitPortraitLUT();
    void InitLandscapeLUT();
    void Set_ResetIOLevel(uint8_t level);
    void RLCD_SendCommand(uint8_t Reg);
    void RLCD_SendData(uint8_t Data);
    void RLCD_Sendbuffera(uint8_t *Data, int len);
    void RLCD_Reset(void);
    static void Lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p);

    // ===== 天气站 UI 组件 =====
    // 状态栏（右上角浮动胶囊）
    lv_obj_t *sensor_label_ = nullptr;      // 左上角温湿度标签
    
    // 时钟卡片（左上）
    lv_obj_t *time_label_ = nullptr;        // 大字时钟 "14:30"
    
    // 日历卡片（右上）
    lv_obj_t *day_label_ = nullptr;         // 星期 "TUE"
    lv_obj_t *date_num_label_ = nullptr;    // 日期 "15"
    lv_obj_t *weather_label_ = nullptr;     // 天气 "晴 25°C"
    
    // AI 对话卡片（左下）
    lv_obj_t *chat_card_ = nullptr;         // AI 卡片容器
    lv_obj_t *chat_status_label_ = nullptr; // AI 对话文字（右侧）
    lv_obj_t *emotion_label_ = nullptr;     // 表情文字（左侧下方）
    lv_obj_t *emotion_img_ = nullptr;       // 表情图片（左侧上方，小智自带 emoji）

    // 备忘录卡片（右下）
    lv_obj_t *memo_list_label_ = nullptr;     // 多行备忘列表文字

    // 图片图标（不能用基类的 label，因为我们用 lv_image 而不是 Font Awesome 文字）
    lv_obj_t *wifi_icon_img_ = nullptr;
    lv_obj_t *battery_icon_img_ = nullptr;
    lv_obj_t *battery_pct_label_ = nullptr;  // 电池百分比文字

    // 数据更新任务句柄
    TaskHandle_t update_task_handle_ = nullptr;
    
    // 上次更新的值（用于避免不必要的 UI 刷新）
    int last_min_ = -1;
    time_t last_valid_epoch_ = 0;  // NTP 同步后记录正确的 epoch，用于检测时间被外部篡改
    float last_temp_ = -99.0f;
    float last_humi_ = -99.0f;

    void SetupWeatherUI();
    void LoadMemoFromNvs();   // 从 NVS 加载备忘录到 UI
    static void DataUpdateTask(void *arg);

public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy, spi_display_config_t spiconfig, spi_host_device_t spi_host = SPI3_HOST);
    ~CustomLcdDisplay();
    void RLCD_Init();
    void RLCD_ColorClear(uint8_t color);
    void RLCD_Display();
    void RLCD_SetPixel(uint16_t x, uint16_t y, uint8_t color);
    
    // 重写小智的 AI 显示方法，适配到左下角卡片
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetEmotion(const char* emotion) override;
    virtual void ClearChatMessages() override;
    
    // 重写状态栏更新（我们用图片图标，不用 Font Awesome 文字）
    virtual void UpdateStatusBar(bool update_all = false) override;
    
    // 重写主题切换（RLCD 单色屏不需要主题切换，避免基类操作不存在的控件导致崩溃）
    virtual void SetTheme(Theme* theme) override;
    
    // 启动数据更新任务（需要在网络连接后调用）
    void StartDataUpdateTask();
    
    // 刷新右下角备忘录列表显示（从 NVS 读取后格式化显示）
    void RefreshMemoDisplay();
};

#endif
