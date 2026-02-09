#include <vector>
#include <string>
#include <cstring>
#include <cmath>
#include <cJSON.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_lcd_panel_io.h>
#include <esp_log.h>
#include <esp_err.h>
#include "custom_lcd_display.h"
#include "lcd_display.h"
#include "esp_lvgl_port.h"
#include "assets/lang_config.h"
#include "settings.h"
#include "config.h"
#include "board.h"
#include "application.h"
#include "lvgl_theme.h"
#include "managers/sensor_manager.h"
#include "managers/weather_manager.h"
#include "secret_config.h"

// 声明天气站专用字体（从 MyWeatherStation 移植，字符集有限但够天气站用）
LV_FONT_DECLARE(alibaba_puhui_16);
LV_FONT_DECLARE(alibaba_puhui_24);
LV_FONT_DECLARE(alibaba_puhui_48);
LV_FONT_DECLARE(alibaba_black_64);

// 声明小智自带字体（7415 个常用汉字，用于 AI 对话区域）
LV_FONT_DECLARE(font_puhui_16_4);

// 声明状态栏图标（从 MyWeatherStation 移植）
LV_IMAGE_DECLARE(ui_img_wifi);
LV_IMAGE_DECLARE(ui_img_wifi_low);
LV_IMAGE_DECLARE(ui_img_wifi_off);
LV_IMAGE_DECLARE(ui_img_battery_full);
LV_IMAGE_DECLARE(ui_img_battery_medium);
LV_IMAGE_DECLARE(ui_img_battery_low);
LV_IMAGE_DECLARE(ui_img_battery_charging);

// ===== RLCD 硬件层 =====

void CustomLcdDisplay::Lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p)
{
    assert(disp != NULL);
    CustomLcdDisplay *Disp = (CustomLcdDisplay *)lv_display_get_user_data(disp);
    uint16_t *buffer = (uint16_t *)color_p;
    for(int y = area->y1; y <= area->y2; y++)
    {
        for(int x = area->x1; x <= area->x2; x++) 
        {
            uint8_t color = (*buffer < 0x7fff) ? ColorBlack : ColorWhite;
            Disp->RLCD_SetPixel(x, y, color);
            buffer++;
        }
    }
    Disp->RLCD_Display();
    lv_disp_flush_ready(disp);
}

CustomLcdDisplay::CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_handle_t panel,
    int width, int height, int offset_x, int offset_y,
    bool mirror_x, bool mirror_y, bool swap_xy,
    spi_display_config_t spiconfig,
    spi_host_device_t spi_host) : LcdDisplay(panel_io, panel, width, height),
    mosi_(spiconfig.mosi), scl_(spiconfig.scl), 
    dc_(spiconfig.dc), cs_(spiconfig.cs), rst_(spiconfig.rst), 
    width_(width), height_(height)
{
    ESP_LOGI(TAG, "初始化 SPI 总线");
    esp_err_t ret;
    spi_bus_config_t buscfg = {};
    int transfer = width_ * height_;
    buscfg.miso_io_num = -1;
    buscfg.mosi_io_num = mosi_;
    buscfg.sclk_io_num = scl_;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = transfer;
    ret = spi_bus_initialize(spi_host, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);
    
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.dc_gpio_num = dc_;
    io_config.cs_gpio_num = cs_;
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.spi_mode = 0;
    io_config.trans_queue_depth = 7;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)spi_host, &io_config, &io_handle));
    
    gpio_config_t gpio_conf = {};
    gpio_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_conf.mode = GPIO_MODE_OUTPUT;
    gpio_conf.pin_bit_mask = (0x1ULL << rst_);
    gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
    Set_ResetIOLevel(1);

    // 分配 1-bit 显示缓冲区
    DisplayLen = transfer >> 3;
    DispBuffer = (uint8_t *)heap_caps_malloc(DisplayLen, MALLOC_CAP_SPIRAM);
    assert(DispBuffer);
    
    // 分配像素映射 LUT（加速 RGB565 → 1-bit 转换）
    PixelIndexLUT = (uint16_t (*)[300])heap_caps_malloc(transfer * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    PixelBitLUT   = (uint8_t (*)[300])heap_caps_malloc(transfer * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    assert(PixelIndexLUT);
    assert(PixelBitLUT);
    if(width_ == 400) {
        InitLandscapeLUT();
    } else {
        InitPortraitLUT();
    }

    ESP_LOGI(TAG, "初始化 LVGL");
    lv_init();
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 2;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);
    lvgl_port_lock(0);

    display_ = lv_display_create(width, height);
    lv_display_set_flush_cb(display_, Lvgl_flush_cb);
    lv_display_set_user_data(display_, this);
    size_t lvgl_buffer_size = LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565) * transfer;
    uint8_t *lvgl_buffer1 = (uint8_t *)heap_caps_malloc(lvgl_buffer_size, MALLOC_CAP_SPIRAM);
    assert(lvgl_buffer1);
    lv_display_set_buffers(display_, lvgl_buffer1, NULL, lvgl_buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    ESP_LOGI(TAG, "初始化 RLCD 屏幕");
    RLCD_Init();

    lvgl_port_unlock();
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "显示初始化失败");
        return;
    }

    ESP_LOGI(TAG, "创建天气站 + AI 混合 UI");
    SetupWeatherUI();
}

CustomLcdDisplay::~CustomLcdDisplay() {
    if (update_task_handle_) {
        vTaskDelete(update_task_handle_);
    }
}

// ===== 天气站 2x2 卡片布局 UI =====

void CustomLcdDisplay::SetupWeatherUI() {
    DisplayLockGuard lock(this);
    
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

    const lv_font_t *font_small  = &alibaba_puhui_16;
    const lv_font_t *font_normal = &alibaba_puhui_24;
    const lv_font_t *font_large  = &alibaba_puhui_48;
    const lv_font_t *font_clock  = &alibaba_black_64;
    // 小智完整字库（7415 常用汉字，AI 对话区域专用）
    const lv_font_t *font_ai     = &font_puhui_16_4;

    // ===== 状态栏（右上角白底胶囊）=====
    lv_obj_t *status_bar = lv_obj_create(screen);
    lv_obj_set_size(status_bar, 115, 28);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_white(), 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_radius(status_bar, 14, 0);
    lv_obj_align(status_bar, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_set_style_pad_all(status_bar, 0, 0);
    lv_obj_set_style_pad_left(status_bar, 8, 0);
    lv_obj_set_style_pad_right(status_bar, 8, 0);
    lv_obj_remove_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_bar, 5, 0);

    // WiFi 图标（我们自己的图片图标，不给基类用）
    wifi_icon_img_ = lv_image_create(status_bar);
    lv_image_set_src(wifi_icon_img_, &ui_img_wifi_off);

    // 电池图标
    battery_icon_img_ = lv_image_create(status_bar);
    lv_image_set_src(battery_icon_img_, &ui_img_battery_full);

    // 电量百分比文字
    battery_pct_label_ = lv_label_create(status_bar);
    lv_obj_set_style_text_font(battery_pct_label_, font_small, 0);
    lv_obj_set_style_text_color(battery_pct_label_, lv_color_black(), 0);
    lv_label_set_text(battery_pct_label_, "---%");

    // ===== 左上角温湿度（白字，直接在黑底上）=====
    sensor_label_ = lv_label_create(screen);
    lv_obj_set_style_text_font(sensor_label_, font_small, 0);
    lv_obj_set_style_text_color(sensor_label_, lv_color_white(), 0);
    lv_obj_align(sensor_label_, LV_ALIGN_TOP_LEFT, 10, 8);
    lv_label_set_text(sensor_label_, "--.-°C  --.-%");

    // ===== 2×2 卡片网格布局 =====
    const int pad = 8;
    const int gap = 6;
    const int top_y = 36;
    const int top_row_h = 128;
    const int bot_row_h = 300 - top_y - top_row_h - gap - pad;  // = 122
    const int left_w = 248;
    const int right_w = 400 - pad * 2 - left_w - gap;  // = 130
    const int bot_y = top_y + top_row_h + gap;
    const int bot_total_w = 400 - pad * 2 - gap;
    const int bot_card_w = bot_total_w * 2 / 3;  // AI 区域 = 252
    const int music_card_w = bot_total_w - bot_card_w;  // 音乐区域 = 126

    // --- 左上：时钟卡片 ---
    lv_obj_t *time_card = lv_obj_create(screen);
    lv_obj_set_pos(time_card, pad, top_y);
    lv_obj_set_size(time_card, left_w, top_row_h);
    lv_obj_set_style_border_width(time_card, 2, 0);
    lv_obj_set_style_border_color(time_card, lv_color_black(), 0);
    lv_obj_set_style_radius(time_card, 15, 0);
    lv_obj_set_style_bg_color(time_card, lv_color_white(), 0);
    lv_obj_set_style_pad_all(time_card, 0, 0);
    lv_obj_remove_flag(time_card, LV_OBJ_FLAG_SCROLLABLE);

    time_label_ = lv_label_create(time_card);
    lv_obj_set_style_text_color(time_label_, lv_color_black(), 0);
    lv_obj_set_style_text_font(time_label_, font_clock, 0);
    lv_obj_set_style_text_letter_space(time_label_, 2, 0);
    lv_obj_center(time_label_);
    lv_label_set_text(time_label_, "00:00");

    // 时钟卡片内边框装饰
    lv_obj_t *time_inner = lv_obj_create(time_card);
    lv_obj_set_size(time_inner, left_w - 14, top_row_h - 14);
    lv_obj_center(time_inner);
    lv_obj_set_style_bg_opa(time_inner, 0, 0);
    lv_obj_set_style_border_width(time_inner, 2, 0);
    lv_obj_set_style_border_color(time_inner, lv_color_black(), 0);
    lv_obj_set_style_radius(time_inner, 10, 0);
    lv_obj_remove_flag(time_inner, LV_OBJ_FLAG_SCROLLABLE);

    // --- 右上：日历卡片 ---
    int right_x = pad + left_w + gap;
    int day_header_h = 40;

    lv_obj_t *calendar_card = lv_obj_create(screen);
    lv_obj_set_pos(calendar_card, right_x, top_y);
    lv_obj_set_size(calendar_card, right_w, top_row_h);
    lv_obj_set_style_border_width(calendar_card, 3, 0);
    lv_obj_set_style_border_color(calendar_card, lv_color_white(), 0);
    lv_obj_set_style_radius(calendar_card, 15, 0);
    lv_obj_set_style_bg_color(calendar_card, lv_color_black(), 0);
    lv_obj_set_style_pad_all(calendar_card, 0, 0);
    lv_obj_remove_flag(calendar_card, LV_OBJ_FLAG_SCROLLABLE);

    // "TUE" 星期标签
    day_label_ = lv_label_create(calendar_card);
    lv_obj_set_style_text_font(day_label_, font_normal, 0);
    lv_obj_set_style_text_color(day_label_, lv_color_white(), 0);
    lv_obj_align(day_label_, LV_ALIGN_TOP_MID, 0, 8);
    lv_label_set_text(day_label_, "---");

    // 日期数字白色区域
    int date_area_h = 55;
    lv_obj_t *date_area = lv_obj_create(calendar_card);
    lv_obj_set_pos(date_area, 6, day_header_h);
    lv_obj_set_size(date_area, right_w - 18, date_area_h);
    lv_obj_set_style_bg_color(date_area, lv_color_white(), 0);
    lv_obj_set_style_border_width(date_area, 0, 0);
    lv_obj_set_style_radius(date_area, 10, 0);
    lv_obj_set_style_pad_all(date_area, 0, 0);
    lv_obj_remove_flag(date_area, LV_OBJ_FLAG_SCROLLABLE);

    date_num_label_ = lv_label_create(date_area);
    lv_obj_set_style_text_font(date_num_label_, font_large, 0);
    lv_obj_set_style_text_color(date_num_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(date_num_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(date_num_label_);
    lv_label_set_text(date_num_label_, "--");

    // 天气标签
    weather_label_ = lv_label_create(calendar_card);
    lv_obj_set_style_text_font(weather_label_, font_small, 0);
    lv_obj_set_style_text_color(weather_label_, lv_color_white(), 0);
    lv_obj_set_style_text_align(weather_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(weather_label_, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_label_set_text(weather_label_, "-- --°C");

    // --- 左下：AI 对话卡片（小智接管这里）---
    // 布局：[左侧表情区 64px | 分隔线 | 右侧对话文字区]
    const int emotion_w = 64;  // 表情区域宽度

    chat_card_ = lv_obj_create(screen);
    lv_obj_set_pos(chat_card_, pad, bot_y);
    lv_obj_set_size(chat_card_, bot_card_w, bot_row_h);
    lv_obj_set_style_border_width(chat_card_, 2, 0);
    lv_obj_set_style_border_color(chat_card_, lv_color_black(), 0);
    lv_obj_set_style_radius(chat_card_, 15, 0);
    lv_obj_set_style_bg_color(chat_card_, lv_color_white(), 0);
    lv_obj_set_style_pad_all(chat_card_, 0, 0);
    lv_obj_remove_flag(chat_card_, LV_OBJ_FLAG_SCROLLABLE);

    // AI 卡片内边框装饰
    lv_obj_t *chat_inner = lv_obj_create(chat_card_);
    lv_obj_set_size(chat_inner, bot_card_w - 14, bot_row_h - 14);
    lv_obj_center(chat_inner);
    lv_obj_set_style_bg_opa(chat_inner, 0, 0);
    lv_obj_set_style_border_width(chat_inner, 2, 0);
    lv_obj_set_style_border_color(chat_inner, lv_color_black(), 0);
    lv_obj_set_style_radius(chat_inner, 10, 0);
    lv_obj_remove_flag(chat_inner, LV_OBJ_FLAG_SCROLLABLE);

    // 左侧表情区域（上方 emoji 图片 + 下方文字标签）
    emotion_img_ = lv_image_create(chat_card_);
    lv_obj_set_size(emotion_img_, 48, 48);
    lv_image_set_inner_align(emotion_img_, LV_IMAGE_ALIGN_CENTER);
    lv_obj_align(emotion_img_, LV_ALIGN_LEFT_MID, 16, -16);
    lv_obj_add_flag(emotion_img_, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏，等 SetEmotion 设置图片

    emotion_label_ = lv_label_create(chat_card_);
    lv_obj_set_style_text_font(emotion_label_, font_ai, 0);  // 用小智完整字库，确保所有中文都能渲染
    lv_obj_set_style_text_color(emotion_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(emotion_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(emotion_label_, emotion_w);
    lv_label_set_long_mode(emotion_label_, LV_LABEL_LONG_WRAP);
    lv_obj_align(emotion_label_, LV_ALIGN_LEFT_MID, 8, 28);
    lv_label_set_text(emotion_label_, "待命");

    // 竖分隔线
    lv_obj_t *divider = lv_obj_create(chat_card_);
    lv_obj_set_size(divider, 2, bot_row_h - 30);
    lv_obj_set_style_bg_color(divider, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_radius(divider, 1, 0);
    lv_obj_align(divider, LV_ALIGN_LEFT_MID, emotion_w + 14, 0);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);

    // 右侧对话文字区
    int text_area_w = bot_card_w - emotion_w - 14 - 2 - 20;  // 减去表情区+间距+分隔线+右边距
    chat_status_label_ = lv_label_create(chat_card_);
    lv_obj_set_style_text_font(chat_status_label_, font_ai, 0);
    lv_obj_set_style_text_color(chat_status_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(chat_status_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(chat_status_label_, text_area_w);
    lv_obj_set_style_text_line_space(chat_status_label_, 3, 0);
    lv_label_set_long_mode(chat_status_label_, LV_LABEL_LONG_WRAP);
    lv_obj_align(chat_status_label_, LV_ALIGN_LEFT_MID, emotion_w + 20, 0);
    lv_label_set_text(chat_status_label_, "AI 待命");

    // --- 右下：备忘录/待办卡片 ---
    lv_obj_t *memo_card = lv_obj_create(screen);
    lv_obj_set_pos(memo_card, pad + bot_card_w + gap, bot_y);
    lv_obj_set_size(memo_card, music_card_w, bot_row_h);
    lv_obj_set_style_border_width(memo_card, 2, 0);
    lv_obj_set_style_border_color(memo_card, lv_color_black(), 0);
    lv_obj_set_style_radius(memo_card, 15, 0);
    lv_obj_set_style_bg_color(memo_card, lv_color_white(), 0);
    lv_obj_set_style_pad_all(memo_card, 6, 0);
    lv_obj_remove_flag(memo_card, LV_OBJ_FLAG_SCROLLABLE);

    // 顶部标题 "MEMO"
    lv_obj_t *memo_title = lv_label_create(memo_card);
    lv_obj_set_style_text_font(memo_title, font_small, 0);
    lv_obj_set_style_text_color(memo_title, lv_color_black(), 0);
    lv_obj_align(memo_title, LV_ALIGN_TOP_LEFT, 2, 0);
    lv_label_set_text(memo_title, "MEMO");

    // 标题下分隔线
    lv_obj_t *memo_sep = lv_obj_create(memo_card);
    lv_obj_set_size(memo_sep, music_card_w - 24, 1);
    lv_obj_set_style_bg_color(memo_sep, lv_color_black(), 0);
    lv_obj_set_style_border_width(memo_sep, 0, 0);
    lv_obj_set_style_radius(memo_sep, 0, 0);
    lv_obj_align(memo_sep, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_remove_flag(memo_sep, LV_OBJ_FLAG_SCROLLABLE);

    // 备忘列表（多行文字，每行一条：时间 + 内容）
    memo_list_label_ = lv_label_create(memo_card);
    lv_obj_set_style_text_font(memo_list_label_, font_ai, 0);  // 小智完整字库
    lv_obj_set_style_text_color(memo_list_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(memo_list_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(memo_list_label_, music_card_w - 20);
    lv_label_set_long_mode(memo_list_label_, LV_LABEL_LONG_CLIP);  // 超出裁剪，不滚动
    lv_obj_set_height(memo_list_label_, bot_row_h - 32);  // 标题+分隔线占约 26px，留余量
    lv_obj_align(memo_list_label_, LV_ALIGN_TOP_LEFT, 2, 26);
    lv_label_set_text(memo_list_label_, "暂无待办");

    // 给基类创建隐藏的占位控件（防止基类方法空指针崩溃）
    // SetTheme / UpdateStatusBar 等方法会操作这些成员变量，
    // 必须给它们赋值合法的 LVGL 对象
    
    // container_ 是 SetTheme 必须操作的（设置背景图/颜色），
    // 创建一个隐藏的全屏容器
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, 1, 1);
    lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);

    network_label_ = lv_label_create(screen);
    lv_label_set_text(network_label_, "");
    lv_obj_add_flag(network_label_, LV_OBJ_FLAG_HIDDEN);
    
    battery_label_ = lv_label_create(screen);
    lv_label_set_text(battery_label_, "");
    lv_obj_add_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);
    
    status_label_ = lv_label_create(screen);
    lv_label_set_text(status_label_, "");
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);

    notification_label_ = lv_label_create(screen);
    lv_label_set_text(notification_label_, "");
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    
    mute_label_ = lv_label_create(screen);
    lv_label_set_text(mute_label_, "");
    lv_obj_add_flag(mute_label_, LV_OBJ_FLAG_HIDDEN);
    
    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    
    // emoji 相关（SetEmotion 基类方法会用到）
    emoji_label_ = lv_label_create(screen);
    lv_label_set_text(emoji_label_, "");
    lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    
    emoji_image_ = lv_img_create(screen);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    // chat_message_label_ 指向我们的 AI 状态标签（让基类方法能更新它）
    chat_message_label_ = chat_status_label_;

    ESP_LOGI(TAG, "天气站 UI 创建完成");

    // 启动时从 NVS 加载上次保存的备忘录
    LoadMemoFromNvs();
}

// ===== 备忘录功能 =====

void CustomLcdDisplay::LoadMemoFromNvs() {
    // 直接调用 RefreshMemoDisplay 从 NVS 读取并更新 UI
    RefreshMemoDisplay();
}

void CustomLcdDisplay::RefreshMemoDisplay() {
    DisplayLockGuard lock(this);
    if (!memo_list_label_) return;

    // 从 NVS 读取 JSON 数组
    Settings settings("memo", false);
    std::string json_str = settings.GetString("items", "");

    if (json_str.empty()) {
        lv_label_set_text(memo_list_label_, "暂无待办");
        return;
    }

    cJSON *arr = cJSON_Parse(json_str.c_str());
    if (!arr || !cJSON_IsArray(arr)) {
        lv_label_set_text(memo_list_label_, "暂无待办");
        if (arr) cJSON_Delete(arr);
        return;
    }

    // 格式化每条备忘为一行: "时间 内容"
    // 卡片高度约 90px，16px 字体每行约 18px，最多显示约 5 行
    std::string display_text;
    int count = cJSON_GetArraySize(arr);
    for (int i = 0; i < count && i < 5; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        cJSON *t = cJSON_GetObjectItem(item, "t");
        cJSON *c = cJSON_GetObjectItem(item, "c");

        if (i > 0) display_text += "\n";

        // 格式：[时间] 内容  或  · 内容（无时间时）
        if (t && cJSON_IsString(t) && strlen(t->valuestring) > 0) {
            display_text += t->valuestring;
            display_text += " ";
        } else {
            display_text += "· ";
        }
        if (c && cJSON_IsString(c)) {
            display_text += c->valuestring;
        }
    }

    // 如果超过 5 条，提示还有更多
    if (count > 5) {
        display_text += "\n...还有" + std::to_string(count - 5) + "条";
    }

    cJSON_Delete(arr);
    lv_label_set_text(memo_list_label_, display_text.c_str());
    ESP_LOGI(TAG, "备忘列表已刷新，共 %d 条", count);
}

// ===== AI 消息适配（重写小智的方法，只更新左下角卡片）=====

void CustomLcdDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_status_label_ == nullptr) return;
    if (!content || strlen(content) == 0) return;

    // 在 RLCD 上只显示最新消息文字（空间有限，不做聊天气泡）
    lv_label_set_text(chat_status_label_, content);
}

void CustomLcdDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    
    // 1. 更新左侧文字（完整映射小智所有 21 种表情 + 额外状态）
    const char* text = "待命";
    if (strcmp(emotion, "neutral") == 0)         text = "待命";
    else if (strcmp(emotion, "happy") == 0)      text = "开心";
    else if (strcmp(emotion, "laughing") == 0)   text = "大笑";
    else if (strcmp(emotion, "funny") == 0)      text = "搞笑";
    else if (strcmp(emotion, "sad") == 0)        text = "难过";
    else if (strcmp(emotion, "angry") == 0)      text = "生气";
    else if (strcmp(emotion, "crying") == 0)     text = "哭泣";
    else if (strcmp(emotion, "loving") == 0)     text = "喜爱";
    else if (strcmp(emotion, "embarrassed") == 0) text = "害羞";
    else if (strcmp(emotion, "surprised") == 0)  text = "惊讶";
    else if (strcmp(emotion, "shocked") == 0)    text = "震惊";
    else if (strcmp(emotion, "thinking") == 0)   text = "思考";
    else if (strcmp(emotion, "winking") == 0)    text = "眨眼";
    else if (strcmp(emotion, "cool") == 0)       text = "耍酷";
    else if (strcmp(emotion, "relaxed") == 0)    text = "放松";
    else if (strcmp(emotion, "delicious") == 0)  text = "好吃";
    else if (strcmp(emotion, "kissy") == 0)      text = "亲亲";
    else if (strcmp(emotion, "confident") == 0)  text = "自信";
    else if (strcmp(emotion, "sleepy") == 0)     text = "犯困";
    else if (strcmp(emotion, "silly") == 0)      text = "调皮";
    else if (strcmp(emotion, "confused") == 0)   text = "困惑";
    // 额外状态
    else if (strcmp(emotion, "fear") == 0)       text = "害怕";
    else if (strcmp(emotion, "disgusted") == 0)  text = "嫌弃";
    else if (strcmp(emotion, "microchip_ai") == 0) text = "就绪";
    // 未知情绪也显示中文，不显示英文原文
    else                                         text = "待命";
    
    if (emotion_label_) {
        lv_label_set_text(emotion_label_, text);
    }
    
    // 2. 尝试加载小智自带的 emoji 图片
    if (emotion_img_ && current_theme_) {
        auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
        auto image = emoji_collection ? emoji_collection->GetEmojiImage(emotion) : nullptr;
        if (image && !image->IsGif()) {
            // 有对应的静态图片，显示出来（GIF 在单色屏上意义不大，跳过）
            lv_image_set_src(emotion_img_, image->image_dsc());
            lv_obj_remove_flag(emotion_img_, LV_OBJ_FLAG_HIDDEN);
        } else {
            // 没有图片，隐藏图片控件，只显示文字
            lv_obj_add_flag(emotion_img_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void CustomLcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (chat_status_label_) lv_label_set_text(chat_status_label_, "");
    // 表情不清除，保持常驻
}

// ===== 重写状态栏更新（禁用基类的 Font Awesome 文字更新）=====

void CustomLcdDisplay::UpdateStatusBar(bool update_all) {
    // 不调用基类实现！
    // 基类会尝试用 lv_label_set_text 更新 network_label_ 和 battery_label_，
    // 但那些是隐藏的占位标签。我们自己的图片图标由 DataUpdateTask 管理。
}

// ===== 重写主题切换 =====

void CustomLcdDisplay::SetTheme(Theme* theme) {
    // RLCD 是 1-bit 单色屏，只有黑白两色，不需要主题切换。
    // 基类的 SetTheme 会操作 container_、content_、top_bar_ 等控件，
    // 我们的天气站 UI 没有创建这些，直接跳过避免崩溃。
    ESP_LOGI("CustomDisplay", "RLCD 单色屏，跳过主题切换");
}

// ===== 数据更新后台任务 =====

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
    
    while (1) {
        auto& app = Application::GetInstance();
        DeviceState ds = app.GetDeviceState();
        
        // 判断网络是否已连接（必须不是 starting 和 activating 前期状态）
        bool network_connected = (ds != kDeviceStateStarting && 
                                  ds != kDeviceStateWifiConfiguring &&
                                  ds != kDeviceStateUnknown);
        
        // NTP 时间同步（网络连接后执行，失败可重试最多 3 次）
        if (network_connected && !time_synced && ntp_retry_count < 3) {
            ESP_LOGI("DataUpdate", "网络已连接，同步 NTP 时间 (第 %d 次)...", ntp_retry_count + 1);
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
                ESP_LOGI("DataUpdate", "NTP 同步确认成功，当前时间: %04d-%02d-%02d %02d:%02d",
                         check_info.tm_year + 1900, check_info.tm_mon + 1, check_info.tm_mday,
                         check_info.tm_hour, check_info.tm_min);
            } else {
                ntp_retry_count++;
                ESP_LOGW("DataUpdate", "NTP 同步后时间不合理（年份=%d），将重试", check_info.tm_year + 1900);
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
        {
            DisplayLockGuard lock(self);
            
            // 1. 时间和日期更新（即使 NTP 没同步也用 RTC 时间）
            // 确保时区正确（小智的 ota.cc 会用 settimeofday 覆盖系统时间）
            setenv("TZ", TIMEZONE_STRING, 1);
            tzset();
            
            time_t now;
            struct tm timeinfo;
            time(&now);
            localtime_r(&now, &timeinfo);
            
            // 时间跳变保护：NTP 同步后，如果系统 epoch 被外部改了（偏差>2小时），
            // 从硬件 RTC 恢复正确时间
            if (time_synced && self->last_valid_epoch_ > 0) {
                long drift = (long)(now - self->last_valid_epoch_);
                // 正常每秒循环 drift ≈ 1s，如果绝对值 > 7200s（2小时），肯定异常
                if (drift < -7200 || drift > 7200) {
                    ESP_LOGW("DataUpdate", "系统时间被篡改（偏差 %ld 秒），从 RTC 恢复", drift);
                    struct tm rtc_tm;
                    SensorManager::getInstance().getRtcTime(&rtc_tm);
                    time_t rtc_epoch = mktime(&rtc_tm);
                    if (rtc_epoch > 1700000000) {
                        struct timeval tv = { .tv_sec = rtc_epoch, .tv_usec = 0 };
                        settimeofday(&tv, NULL);
                        time(&now);
                        localtime_r(&now, &timeinfo);
                        self->last_min_ = -1;
                        ESP_LOGI("DataUpdate", "已从 RTC 恢复: %02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
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
                ESP_LOGI("DataUpdate", "时间已更新: %s, %s, %d日", time_buf, weeks_en[timeinfo.tm_wday], timeinfo.tm_mday);

                // ===== 备忘闹钟：检查是否有备忘的时间匹配当前 HH:MM =====
                if (time_synced) {
                    Settings memo_settings("memo", false);
                    std::string memo_json = memo_settings.GetString("items", "");
                    if (!memo_json.empty()) {
                        cJSON *memo_arr = cJSON_Parse(memo_json.c_str());
                        if (memo_arr && cJSON_IsArray(memo_arr)) {
                            int memo_count = cJSON_GetArraySize(memo_arr);
                            for (int mi = 0; mi < memo_count; mi++) {
                                cJSON *memo_item = cJSON_GetArrayItem(memo_arr, mi);
                                cJSON *mt = cJSON_GetObjectItem(memo_item, "t");
                                cJSON *mc = cJSON_GetObjectItem(memo_item, "c");
                                // 只匹配 "HH:MM" 格式（5个字符，中间是冒号）
                                if (mt && mt->valuestring && strlen(mt->valuestring) == 5 
                                    && mt->valuestring[2] == ':') {
                                    if (strcmp(mt->valuestring, time_buf) == 0) {
                                        // 时间到了！语音提醒
                                        const char *memo_text = (mc && mc->valuestring) ? mc->valuestring : "备忘提醒";
                                        char alert_msg[128];
                                        snprintf(alert_msg, sizeof(alert_msg), "备忘提醒: %s %s", mt->valuestring, memo_text);
                                        ESP_LOGI("DataUpdate", "触发备忘闹钟: %s", alert_msg);
                                        app.Alert("提醒", alert_msg, "happy", "");
                                    }
                                }
                            }
                            cJSON_Delete(memo_arr);
                        }
                    }
                }
            }

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

// ===== RLCD 硬件底层方法 =====

void CustomLcdDisplay::InitPortraitLUT() {
    uint16_t W4 = width_ >> 2;
    for (uint16_t y = 0; y < height_; y++) {
        uint16_t byte_y = y >> 1;
        uint8_t  local_y = y & 1;
        for (uint16_t x = 0; x < width_; x++) {
            uint16_t byte_x = x >> 2;
            uint8_t  local_x = x & 3;
            uint32_t index = byte_y * W4 + byte_x;
            uint8_t bit = 7 - ((local_x << 1) | local_y);
            PixelIndexLUT[x][y] = index;
            PixelBitLUT  [x][y] = (1 << bit);
        }
    }
}

void CustomLcdDisplay::InitLandscapeLUT() {
    uint16_t H4 = height_ >> 2;
    for (uint16_t y = 0; y < height_; y++) {
        uint16_t inv_y = height_ - 1 - y;
        uint16_t block_y = inv_y >> 2;
        uint8_t  local_y  = inv_y & 3;
        for (uint16_t x = 0; x < width_; x++) {
            uint16_t byte_x = x >> 1;
            uint8_t  local_x = x & 1;
            uint32_t index = byte_x * H4 + block_y;
            uint8_t bit = 7 - ((local_y << 1) | local_x);
            PixelIndexLUT[x][y] = index;
            PixelBitLUT  [x][y] = (1 << bit);
        }
    }
}

void CustomLcdDisplay::Set_ResetIOLevel(uint8_t level) {
    gpio_set_level((gpio_num_t)rst_, level ? 1 : 0);
}

void CustomLcdDisplay::RLCD_SendCommand(uint8_t Reg) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, Reg, NULL, 0));
}

void CustomLcdDisplay::RLCD_SendData(uint8_t Data) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, -1, &Data, 1));
}

void CustomLcdDisplay::RLCD_Sendbuffera(uint8_t *Data, int len) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(io_handle, -1, Data, len));
}

void CustomLcdDisplay::RLCD_Reset(void) {
    Set_ResetIOLevel(1);
    vTaskDelay(pdMS_TO_TICKS(50));
    Set_ResetIOLevel(0);
    vTaskDelay(pdMS_TO_TICKS(20));
    Set_ResetIOLevel(1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

void CustomLcdDisplay::RLCD_ColorClear(uint8_t color) {
    memset(DispBuffer, color, DisplayLen);
}

void CustomLcdDisplay::RLCD_Init() {
    RLCD_Reset();

    RLCD_SendCommand(0xD6); RLCD_SendData(0x17); RLCD_SendData(0x02);
    RLCD_SendCommand(0xD1); RLCD_SendData(0x01);
    RLCD_SendCommand(0xC0); RLCD_SendData(0x11); RLCD_SendData(0x04);
    RLCD_SendCommand(0xC1); RLCD_SendData(0x69); RLCD_SendData(0x69); RLCD_SendData(0x69); RLCD_SendData(0x69);
    RLCD_SendCommand(0xC2); RLCD_SendData(0x19); RLCD_SendData(0x19); RLCD_SendData(0x19); RLCD_SendData(0x19);
    RLCD_SendCommand(0xC4); RLCD_SendData(0x4B); RLCD_SendData(0x4B); RLCD_SendData(0x4B); RLCD_SendData(0x4B);
    RLCD_SendCommand(0xC5); RLCD_SendData(0x19); RLCD_SendData(0x19); RLCD_SendData(0x19); RLCD_SendData(0x19);
    RLCD_SendCommand(0xD8); RLCD_SendData(0x80); RLCD_SendData(0xE9);
    RLCD_SendCommand(0xB2); RLCD_SendData(0x02);
    RLCD_SendCommand(0xB3); RLCD_SendData(0xE5); RLCD_SendData(0xF6); RLCD_SendData(0x05); RLCD_SendData(0x46);
    RLCD_SendData(0x77); RLCD_SendData(0x77); RLCD_SendData(0x77); RLCD_SendData(0x77); RLCD_SendData(0x76); RLCD_SendData(0x45);
    RLCD_SendCommand(0xB4); RLCD_SendData(0x05); RLCD_SendData(0x46); RLCD_SendData(0x77); RLCD_SendData(0x77);
    RLCD_SendData(0x77); RLCD_SendData(0x77); RLCD_SendData(0x76); RLCD_SendData(0x45);
    RLCD_SendCommand(0x62); RLCD_SendData(0x32); RLCD_SendData(0x03); RLCD_SendData(0x1F);
    RLCD_SendCommand(0xB7); RLCD_SendData(0x13);
    RLCD_SendCommand(0xB0); RLCD_SendData(0x64);
    RLCD_SendCommand(0x11); vTaskDelay(pdMS_TO_TICKS(200));
    RLCD_SendCommand(0xC9); RLCD_SendData(0x00);
    RLCD_SendCommand(0x36); RLCD_SendData(0x48);
    RLCD_SendCommand(0x3A); RLCD_SendData(0x11);
    RLCD_SendCommand(0xB9); RLCD_SendData(0x20);
    RLCD_SendCommand(0xB8); RLCD_SendData(0x29);
    RLCD_SendCommand(0x21);
    RLCD_SendCommand(0x2A); RLCD_SendData(0x12); RLCD_SendData(0x2A);
    RLCD_SendCommand(0x2B); RLCD_SendData(0x00); RLCD_SendData(0xC7);
    RLCD_SendCommand(0x35); RLCD_SendData(0x00);
    RLCD_SendCommand(0xD0); RLCD_SendData(0xFF);
    RLCD_SendCommand(0x38);
    RLCD_SendCommand(0x29);
    RLCD_ColorClear(ColorWhite);
}

void CustomLcdDisplay::RLCD_SetPixel(uint16_t x, uint16_t y, uint8_t color) {
    uint32_t idx = PixelIndexLUT[x][y];
    uint8_t  mask = PixelBitLUT[x][y];
    uint8_t *p = &DispBuffer[idx];
    if (color)
        *p |= mask;
    else
        *p &= ~mask;
}

void CustomLcdDisplay::RLCD_Display() {
    RLCD_SendCommand(0x2A);
    RLCD_SendData(0x12);
    RLCD_SendData(0x2A);
    RLCD_SendCommand(0x2B);
    RLCD_SendData(0x00);
    RLCD_SendData(0xC7);
    RLCD_SendCommand(0x2c);
    RLCD_Sendbuffera(DispBuffer, DisplayLen);
}
