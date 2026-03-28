// 番茄钟 UI —— 专注倒计时页面
//
// 400x300 黑白单色 RLCD，全屏黑底白字。
//
// 布局：
// ┌──────────────────────────────────────────┐
// │ 14:30 24.5°C 68%      [WiFi][电池][85%] │  顶部信息层
// │                                          │
// │              🍅 专注中                    │  状态文字
// │                                          │
// │              20:35                       │  大号倒计时（居中）
// │                                          │
// │  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  │  进度条
// │              25分钟 专注 / 5分钟 休息      │  设定信息
// │                                          │
// │  ┌──────────────────────────────────┐    │
// │  │[emoji]     │                     │    │  底部 AI 状态卡
// │  │ 待命       │ AI 待命              │    │
// │  └──────────────────────────────────┘    │
// └──────────────────────────────────────────┘

#include "custom_lcd_display.h"
#include <esp_log.h>

// 字体声明
LV_FONT_DECLARE(alibaba_puhui_16);
LV_FONT_DECLARE(alibaba_puhui_24);
LV_FONT_DECLARE(alibaba_black_64);
LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_14_1);

// 状态栏图标
LV_IMAGE_DECLARE(ui_img_wifi);
LV_IMAGE_DECLARE(ui_img_wifi_off);
LV_IMAGE_DECLARE(ui_img_battery_full);

static const char *TAG = "PomodoroUI";

void CustomLcdDisplay::SetupPomodoroUI() {
    DisplayLockGuard lock(this);

    lv_obj_t *root = lv_screen_active();
    const lv_font_t *font_num    = &alibaba_puhui_16;
    const lv_font_t *font_time   = &alibaba_puhui_24;
    const lv_font_t *font_big    = &alibaba_black_64;
    const lv_font_t *font_cn     = &font_puhui_16_4;
    const lv_font_t *font_sm     = &font_puhui_14_1;

    const int SCR_W = 400;
    const int SCR_H = 300;
    const int PAD = 12;

    // ===== 番茄钟页面容器（全屏黑底，初始隐藏）=====
    pomodoro_page_ = lv_obj_create(root);
    lv_obj_set_size(pomodoro_page_, SCR_W, SCR_H);
    lv_obj_set_pos(pomodoro_page_, 0, 0);
    lv_obj_set_style_bg_color(pomodoro_page_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(pomodoro_page_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pomodoro_page_, 0, 0);
    lv_obj_set_style_pad_all(pomodoro_page_, 0, 0);
    lv_obj_set_style_radius(pomodoro_page_, 0, 0);
    lv_obj_remove_flag(pomodoro_page_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pomodoro_page_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *page = pomodoro_page_;

    // ============================================================
    // 第 1 层：顶部信息（时钟 + 温湿度 + 状态栏胶囊）
    // ============================================================

    // 左上角时钟
    pomo_time_label_ = lv_label_create(page);
    lv_obj_set_style_text_font(pomo_time_label_, font_time, 0);
    lv_obj_set_style_text_color(pomo_time_label_, lv_color_white(), 0);
    lv_obj_align(pomo_time_label_, LV_ALIGN_TOP_LEFT, 10, 5);
    lv_label_set_text(pomo_time_label_, "00:00");

    // 温湿度
    pomo_sensor_label_ = lv_label_create(page);
    lv_obj_set_style_text_font(pomo_sensor_label_, font_sm, 0);
    lv_obj_set_style_text_color(pomo_sensor_label_, lv_color_white(), 0);
    lv_obj_set_style_text_opa(pomo_sensor_label_, LV_OPA_60, 0);
    lv_obj_align(pomo_sensor_label_, LV_ALIGN_TOP_LEFT, 80, 11);
    lv_label_set_text(pomo_sensor_label_, "--.-°C --.-%");

    // 右上角状态栏胶囊
    lv_obj_t *status_bar = lv_obj_create(page);
    lv_obj_set_size(status_bar, 130, 28);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_white(), 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_radius(status_bar, 14, 0);
    lv_obj_align(status_bar, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_set_style_pad_left(status_bar, 8, 0);
    lv_obj_set_style_pad_right(status_bar, 8, 0);
    lv_obj_set_style_pad_column(status_bar, 5, 0);
    lv_obj_remove_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    pomo_wifi_icon_img_ = lv_image_create(status_bar);
    lv_image_set_src(pomo_wifi_icon_img_, &ui_img_wifi_off);
    pomo_battery_icon_img_ = lv_image_create(status_bar);
    lv_image_set_src(pomo_battery_icon_img_, &ui_img_battery_full);
    pomo_battery_pct_label_ = lv_label_create(status_bar);
    lv_obj_set_style_text_font(pomo_battery_pct_label_, font_num, 0);
    lv_obj_set_style_text_color(pomo_battery_pct_label_, lv_color_black(), 0);
    lv_label_set_text(pomo_battery_pct_label_, "---%");

    // ============================================================
    // 第 2 层：番茄钟状态文字
    // ============================================================

    pomo_state_label_ = lv_label_create(page);
    lv_obj_set_style_text_font(pomo_state_label_, font_cn, 0);
    lv_obj_set_style_text_color(pomo_state_label_, lv_color_white(), 0);
    lv_obj_set_style_text_align(pomo_state_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(pomo_state_label_, SCR_W);
    lv_label_set_text(pomo_state_label_, "番茄钟 · 准备就绪");
    lv_obj_align(pomo_state_label_, LV_ALIGN_TOP_MID, 0, 60);

    // ============================================================
    // 第 3 层：大号倒计时数字（居中）
    // ============================================================

    pomo_countdown_label_ = lv_label_create(page);
    lv_obj_set_style_text_font(pomo_countdown_label_, font_big, 0);
    lv_obj_set_style_text_color(pomo_countdown_label_, lv_color_white(), 0);
    lv_obj_set_style_text_align(pomo_countdown_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(pomo_countdown_label_, "25:00");
    lv_obj_align(pomo_countdown_label_, LV_ALIGN_CENTER, 0, -30);

    // ============================================================
    // 第 4 层：进度条
    // ============================================================

    const int bar_y = 165;
    const int bar_w = SCR_W - PAD * 2 - 40;

    pomo_progress_bar_ = lv_bar_create(page);
    lv_obj_set_size(pomo_progress_bar_, bar_w, 12);
    lv_obj_set_pos(pomo_progress_bar_, (SCR_W - bar_w) / 2, bar_y);
    lv_bar_set_range(pomo_progress_bar_, 0, 1000);
    lv_bar_set_value(pomo_progress_bar_, 0, LV_ANIM_OFF);

    // 轨道（白色背景 + 白色边框）
    lv_obj_set_style_bg_color(pomo_progress_bar_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(pomo_progress_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pomo_progress_bar_, 1, 0);
    lv_obj_set_style_border_color(pomo_progress_bar_, lv_color_white(), 0);
    lv_obj_set_style_radius(pomo_progress_bar_, 6, 0);
    lv_obj_set_style_pad_top(pomo_progress_bar_, 2, 0);
    lv_obj_set_style_pad_bottom(pomo_progress_bar_, 2, 0);
    lv_obj_set_style_pad_left(pomo_progress_bar_, 2, 0);
    lv_obj_set_style_pad_right(pomo_progress_bar_, 2, 0);

    // 指示器（纯黑填充）
    lv_obj_set_style_bg_color(pomo_progress_bar_, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(pomo_progress_bar_, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(pomo_progress_bar_, 4, LV_PART_INDICATOR);

    // 设定信息文字（进度条下方）
    pomo_info_label_ = lv_label_create(page);
    lv_obj_set_style_text_font(pomo_info_label_, font_sm, 0);
    lv_obj_set_style_text_color(pomo_info_label_, lv_color_white(), 0);
    lv_obj_set_style_text_opa(pomo_info_label_, LV_OPA_60, 0);
    lv_obj_set_style_text_align(pomo_info_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(pomo_info_label_, SCR_W);
    lv_label_set_text(pomo_info_label_, "25分钟 专注 / 5分钟 休息");
    lv_obj_align(pomo_info_label_, LV_ALIGN_TOP_MID, 0, bar_y + 20);

    // ============================================================
    // 第 5 层：底部 AI 状态卡（和天气页/音乐页一致的布局）
    // ============================================================

    const int ai_h = 72;
    const int ai_w = SCR_W - PAD * 2;
    const int ai_y = SCR_H - ai_h - 6;
    const int emotion_w = 56;

    lv_obj_t *ai_card = lv_obj_create(page);
    lv_obj_set_size(ai_card, ai_w, ai_h);
    lv_obj_set_pos(ai_card, PAD, ai_y);
    lv_obj_set_style_bg_color(ai_card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(ai_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ai_card, 2, 0);
    lv_obj_set_style_border_color(ai_card, lv_color_black(), 0);
    lv_obj_set_style_radius(ai_card, 16, 0);
    lv_obj_set_style_pad_all(ai_card, 0, 0);
    lv_obj_remove_flag(ai_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_clip_corner(ai_card, true, 0);

    // 左侧：emoji 图片
    pomo_emotion_img_ = lv_image_create(ai_card);
    lv_obj_set_size(pomo_emotion_img_, 40, 40);
    lv_image_set_inner_align(pomo_emotion_img_, LV_IMAGE_ALIGN_CENTER);
    lv_obj_align(pomo_emotion_img_, LV_ALIGN_LEFT_MID, 10, -10);
    lv_obj_add_flag(pomo_emotion_img_, LV_OBJ_FLAG_HIDDEN);

    // 左侧：情绪文字标签
    pomo_emotion_label_ = lv_label_create(ai_card);
    lv_obj_set_style_text_font(pomo_emotion_label_, font_cn, 0);
    lv_obj_set_style_text_color(pomo_emotion_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(pomo_emotion_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(pomo_emotion_label_, emotion_w);
    lv_label_set_long_mode(pomo_emotion_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(pomo_emotion_label_, "待命");
    lv_obj_align(pomo_emotion_label_, LV_ALIGN_LEFT_MID, 4, 20);

    // 竖分隔线
    lv_obj_t *divider = lv_obj_create(ai_card);
    lv_obj_set_size(divider, 2, ai_h - 20);
    lv_obj_set_style_bg_color(divider, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_radius(divider, 1, 0);
    lv_obj_align(divider, LV_ALIGN_LEFT_MID, emotion_w + 10, 0);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);

    // 右侧：AI 对话文字
    const int text_x = emotion_w + 18;
    const int text_w = ai_w - text_x - 12;
    pomo_chat_status_label_ = lv_label_create(ai_card);
    lv_obj_set_style_text_font(pomo_chat_status_label_, font_cn, 0);
    lv_obj_set_style_text_color(pomo_chat_status_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(pomo_chat_status_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(pomo_chat_status_label_, text_w);
    lv_obj_set_style_text_line_space(pomo_chat_status_label_, 3, 0);
    lv_label_set_long_mode(pomo_chat_status_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(pomo_chat_status_label_, "说「开始番茄钟」启动");
    lv_obj_align(pomo_chat_status_label_, LV_ALIGN_LEFT_MID, text_x, 0);

    ESP_LOGI(TAG, "番茄钟页面 UI 创建完成");
}
