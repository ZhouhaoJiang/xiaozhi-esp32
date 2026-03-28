// 天气站 2x2 卡片布局 UI
// 
// 负责创建天气站的所有 LVGL 控件：
// - 状态栏（右上角白底胶囊：WiFi + 电池）
// - 左上角温湿度标签
// - 时钟卡片（左上）
// - 日历卡片（右上）
// - AI 对话卡片（左下）
// - 备忘录卡片（右下）
// - 基类占位控件（防止空指针崩溃）

#include "custom_lcd_display.h"
#include <esp_log.h>

// 声明天气站专用字体（从 MyWeatherStation 移植，字符集有限但够天气站用）
LV_FONT_DECLARE(alibaba_puhui_16);
LV_FONT_DECLARE(alibaba_puhui_24);
LV_FONT_DECLARE(alibaba_puhui_48);
LV_FONT_DECLARE(alibaba_black_64);
#ifdef CONFIG_LV_FONT_MONTSERRAT_36
LV_FONT_DECLARE(lv_font_montserrat_36);
#endif

// 声明小智自带字体（7415 个常用汉字，用于 AI 对话区域）
LV_FONT_DECLARE(font_puhui_16_4);  // 16px 标准字体
LV_FONT_DECLARE(font_puhui_14_1);  // 14px 小字体（用于系统信息显示）

// 声明状态栏图标（从 MyWeatherStation 移植）
LV_IMAGE_DECLARE(ui_img_wifi);
LV_IMAGE_DECLARE(ui_img_wifi_low);
LV_IMAGE_DECLARE(ui_img_wifi_off);
LV_IMAGE_DECLARE(ui_img_battery_full);
LV_IMAGE_DECLARE(ui_img_battery_medium);
LV_IMAGE_DECLARE(ui_img_battery_low);
LV_IMAGE_DECLARE(ui_img_battery_charging);

static const char *TAG = "WeatherUI";

void CustomLcdDisplay::SetupWeatherUI() {
    DisplayLockGuard lock(this);
    
    lv_obj_t *root = lv_screen_active();
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    weather_page_ = lv_obj_create(root);
    lv_obj_set_size(weather_page_, 400, 300);
    lv_obj_set_pos(weather_page_, 0, 0);
    lv_obj_set_style_bg_opa(weather_page_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(weather_page_, 0, 0);
    lv_obj_set_style_pad_all(weather_page_, 0, 0);
    lv_obj_set_style_radius(weather_page_, 0, 0);
    lv_obj_remove_flag(weather_page_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *screen = weather_page_;

    const lv_font_t *font_small  = &alibaba_puhui_16;
    const lv_font_t *font_normal = &alibaba_puhui_24;
    const lv_font_t *font_large  = &alibaba_puhui_48;
    const lv_font_t *font_clock  = &alibaba_black_64;
    // 小智完整字库（7415 常用汉字，AI 对话区域专用）
    const lv_font_t *font_ai     = &font_puhui_16_4;
#ifdef CONFIG_LV_FONT_MONTSERRAT_36
    const lv_font_t *font_date = &lv_font_montserrat_36;
#else
    const lv_font_t *font_date = font_normal;
#endif

    // ===== 2×2 卡片网格布局（同时用于右上胶囊与日历卡片对齐）=====
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

    // ===== 状态栏（右上角白底胶囊）=====
    lv_obj_t *status_bar = lv_obj_create(screen);
    lv_obj_set_size(status_bar, right_w, 28);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_white(), 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_radius(status_bar, 14, 0);
    lv_obj_align(status_bar, LV_ALIGN_TOP_RIGHT, -pad, 4);
    lv_obj_set_style_pad_all(status_bar, 0, 0);
    lv_obj_set_style_pad_left(status_bar, 8, 0);
    lv_obj_set_style_pad_right(status_bar, 8, 0);
    lv_obj_remove_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
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
    lv_label_set_text(sensor_label_, "--.-°C  --.-%");
    lv_obj_update_layout(sensor_label_);
    int sensor_h = lv_obj_get_height(sensor_label_);
    int sensor_y = 4 + (28 - sensor_h) / 2;
    if (sensor_y < 0) sensor_y = 0;
    const int sensor_x = pad + 2;
    lv_obj_align(sensor_label_, LV_ALIGN_TOP_LEFT, sensor_x, sensor_y);

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
    lv_obj_set_style_text_font(day_label_, font_ai, 0);
    lv_obj_set_style_text_color(day_label_, lv_color_white(), 0);
    lv_label_set_text(day_label_, "---");
    lv_obj_update_layout(day_label_);
    int day_h = lv_obj_get_height(day_label_);
    int day_y = (day_header_h - day_h) / 2;
    if (day_y < 0) day_y = 0;
    lv_obj_align(day_label_, LV_ALIGN_TOP_MID, 0, day_y);

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
    lv_obj_set_style_text_font(date_num_label_, font_date, 0);
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
    lv_label_set_text(weather_label_, "天气未更新");

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
    lv_obj_set_style_clip_corner(chat_card_, true, 0);  // 裁剪圆角边界（防止子元素溢出）

    // AI 卡片内边框装饰（同时作为文字滚动的裁剪容器）
    lv_obj_t *chat_inner = lv_obj_create(chat_card_);
    lv_obj_set_size(chat_inner, bot_card_w - 14, bot_row_h - 14);
    lv_obj_center(chat_inner);
    lv_obj_set_style_bg_opa(chat_inner, 0, 0);
    lv_obj_set_style_border_width(chat_inner, 2, 0);
    lv_obj_set_style_border_color(chat_inner, lv_color_black(), 0);
    lv_obj_set_style_radius(chat_inner, 10, 0);
    lv_obj_set_style_pad_all(chat_inner, 0, 0);  // 重要：清除内边距，让子元素可以精确定位
    lv_obj_remove_flag(chat_inner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_clip_corner(chat_inner, true, 0);  // 🔑 裁剪内边框，防止文字溢出

    // 左侧表情区域（上方 emoji 图片 + 下方文字标签）
    // 注意：现在所有元素都是 chat_inner 的子元素，位置需要相对 chat_inner 调整
    emotion_img_ = lv_image_create(chat_inner);  // 父容器改为 chat_inner
    lv_obj_set_size(emotion_img_, 48, 48);
    lv_image_set_inner_align(emotion_img_, LV_IMAGE_ALIGN_CENTER);
    lv_obj_align(emotion_img_, LV_ALIGN_LEFT_MID, 16, -16);
    lv_obj_add_flag(emotion_img_, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏，等 SetEmotion 设置图片

    emotion_label_ = lv_label_create(chat_inner);  // 父容器改为 chat_inner
    lv_obj_set_style_text_font(emotion_label_, font_ai, 0);  // 用小智完整字库，确保所有中文都能渲染
    lv_obj_set_style_text_color(emotion_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(emotion_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(emotion_label_, emotion_w);
    lv_label_set_long_mode(emotion_label_, LV_LABEL_LONG_WRAP);
    lv_obj_align(emotion_label_, LV_ALIGN_LEFT_MID, 8, 28);
    lv_label_set_text(emotion_label_, "待命");

    // 竖分隔线
    lv_obj_t *divider = lv_obj_create(chat_inner);  // 父容器改为 chat_inner
    lv_obj_set_size(divider, 2, bot_row_h - 30);
    lv_obj_set_style_bg_color(divider, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_radius(divider, 1, 0);
    lv_obj_align(divider, LV_ALIGN_LEFT_MID, emotion_w + 14, 0);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);

    // 右侧对话文字区
    int text_area_w = bot_card_w - emotion_w - 14 - 2 - 20;  // 减去表情区+间距+分隔线+右边距
    chat_status_label_ = lv_label_create(chat_inner);  // 🔑 父容器改为 chat_inner（关键修改）
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
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, 320, 42);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(low_battery_popup_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(low_battery_popup_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(low_battery_popup_, 2, 0);
    lv_obj_set_style_border_color(low_battery_popup_, lv_color_black(), 0);
    lv_obj_set_style_radius(low_battery_popup_, 12, 0);
    lv_obj_set_style_pad_all(low_battery_popup_, 6, 0);
    lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_obj_set_style_text_font(low_battery_label_, font_ai, 0);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(low_battery_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(low_battery_label_, 300);
    lv_obj_center(low_battery_label_);
    lv_label_set_text(low_battery_label_, "电量低，请尽快充电");
    
    // emoji 相关（SetEmotion 基类方法会用到）
    emoji_label_ = lv_label_create(screen);
    lv_label_set_text(emoji_label_, "");
    lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    
    emoji_image_ = lv_img_create(screen);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    // chat_message_label_ 指向我们的 AI 状态标签（让基类方法能更新它）
    chat_message_label_ = chat_status_label_;

    ESP_LOGI(TAG, "天气站 UI 创建完成");
}
