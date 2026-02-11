// 音乐页 UI —— 沉浸式播放器布局（左右排布版）
//
// 400x300 黑白单色 RLCD，全屏黑底白字。
// 所有中文文字统一使用小智自带的 font_puhui_16_4（7415 常用汉字），避免缺字。
//
// 布局：
// ┌──────────────────────────────────────────┐
// │ 14:30 24.5°C 68%      [WiFi][电池][85%] │  顶部信息层
// │                                          │
// │  ┌──────────┐   晴天                     │
// │  │          │   周杰伦                   │
// │  │  ◉ 唱片  │                           │
// │  │ (120x120)│   故事的小黄花             │
// │  │          │   从出生那年就飘着          │
// │  └──────────┘                            │
// │                                          │
// │  ━━━━━━━━━━━━━━━━━━━━○──────────  02:31  │  进度条 + 时间
// │                                          │
// │  ┌──────────────────────────────────┐    │
// │  │[emoji]     │                     │    │  底部 AI 状态卡
// │  │ 待命       │ AI 待命              │    │  （和天气页一致的 emoji+文字 布局）
// │  └──────────────────────────────────┘    │
// └──────────────────────────────────────────┘

#include "custom_lcd_display.h"
#include <esp_log.h>

// 字体：只用小智自带完整字库 + 天气站字体（仅用于非中文场景如时间数字）
LV_FONT_DECLARE(alibaba_puhui_16);   // 16px（用于时间数字、电量等纯 ASCII 场景）
LV_FONT_DECLARE(alibaba_puhui_24);   // 24px（用于时钟数字）
LV_FONT_DECLARE(font_puhui_16_4);    // 16px 小智完整字库（所有中文文字用这个）
LV_FONT_DECLARE(font_puhui_14_1);    // 14px 小字体

// 状态栏图标
LV_IMAGE_DECLARE(ui_img_wifi);
LV_IMAGE_DECLARE(ui_img_wifi_off);
LV_IMAGE_DECLARE(ui_img_battery_full);

static const char *TAG = "MusicUI";

void CustomLcdDisplay::SetupMusicUI() {
    DisplayLockGuard lock(this);

    lv_obj_t *root = lv_screen_active();
    const lv_font_t *font_num    = &alibaba_puhui_16;   // 纯数字/ASCII 用
    const lv_font_t *font_time   = &alibaba_puhui_24;   // 时钟数字
    const lv_font_t *font_cn     = &font_puhui_16_4;    // 所有中文文字统一用这个
    const lv_font_t *font_sm     = &font_puhui_14_1;    // 14px 小字

    // 全局布局常量
    const int SCR_W = 400;
    const int SCR_H = 300;
    const int PAD = 12;               // 左右安全边距

    // ===== 音乐页面容器（全屏黑底，初始隐藏）=====
    music_page_ = lv_obj_create(root);
    lv_obj_set_size(music_page_, SCR_W, SCR_H);
    lv_obj_set_pos(music_page_, 0, 0);
    lv_obj_set_style_bg_color(music_page_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(music_page_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(music_page_, 0, 0);
    lv_obj_set_style_pad_all(music_page_, 0, 0);
    lv_obj_set_style_radius(music_page_, 0, 0);
    lv_obj_remove_flag(music_page_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(music_page_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *page = music_page_;

    // ============================================================
    // 第 1 层：顶部信息（时钟 + 温湿度 + 状态栏胶囊）
    // ============================================================

    // 左上角时钟
    music_time_label_ = lv_label_create(page);
    lv_obj_set_style_text_font(music_time_label_, font_time, 0);
    lv_obj_set_style_text_color(music_time_label_, lv_color_white(), 0);
    lv_obj_align(music_time_label_, LV_ALIGN_TOP_LEFT, 10, 5);
    lv_label_set_text(music_time_label_, "00:00");

    // 温湿度（跟在时钟右边，小字 + 低透明度，区分主次）
    music_sensor_label_ = lv_label_create(page);
    lv_obj_set_style_text_font(music_sensor_label_, font_sm, 0);
    lv_obj_set_style_text_color(music_sensor_label_, lv_color_white(), 0);
    lv_obj_set_style_text_opa(music_sensor_label_, LV_OPA_60, 0);
    lv_obj_align(music_sensor_label_, LV_ALIGN_TOP_LEFT, 80, 11);
    lv_label_set_text(music_sensor_label_, "--.-°C --.-%");

    // 右上角状态栏胶囊
    lv_obj_t *status_bar = lv_obj_create(page);
    lv_obj_set_size(status_bar, 115, 28);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_white(), 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_radius(status_bar, 14, 0);
    lv_obj_align(status_bar, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_set_style_pad_left(status_bar, 8, 0);
    lv_obj_set_style_pad_right(status_bar, 8, 0);
    lv_obj_set_style_pad_column(status_bar, 5, 0);
    lv_obj_set_style_pad_row(status_bar, 0, 0);
    lv_obj_remove_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    music_wifi_icon_img_ = lv_image_create(status_bar);
    lv_image_set_src(music_wifi_icon_img_, &ui_img_wifi_off);
    music_battery_icon_img_ = lv_image_create(status_bar);
    lv_image_set_src(music_battery_icon_img_, &ui_img_battery_full);
    music_battery_pct_label_ = lv_label_create(status_bar);
    lv_obj_set_style_text_font(music_battery_pct_label_, font_num, 0);
    lv_obj_set_style_text_color(music_battery_pct_label_, lv_color_black(), 0);
    lv_label_set_text(music_battery_pct_label_, "---%");

    // ============================================================
    // 第 2 层：主内容区 —— 左侧唱片 + 右侧歌曲信息（左右排布）
    // ============================================================

    const int content_y = 36;         // 顶部信息下方
    const int vinyl_card_size = 150;  // 唱片方卡尺寸
    const int vinyl_size = 130;       // 唱片圆盘直径
    const int vinyl_x = PAD;          // 唱片左边距
    const int info_x = vinyl_x + vinyl_card_size + 12;  // 右侧信息区起始 x
    const int info_w = SCR_W - info_x - PAD;             // 右侧信息区宽度

    // --- 左侧：唱片方卡 ---
    lv_obj_t *vinyl_card = lv_obj_create(page);
    lv_obj_set_size(vinyl_card, vinyl_card_size, vinyl_card_size);
    lv_obj_set_pos(vinyl_card, vinyl_x, content_y);
    lv_obj_set_style_bg_color(vinyl_card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(vinyl_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(vinyl_card, 2, 0);
    lv_obj_set_style_border_color(vinyl_card, lv_color_black(), 0);
    lv_obj_set_style_radius(vinyl_card, 16, 0);
    lv_obj_set_style_pad_all(vinyl_card, 0, 0);
    lv_obj_remove_flag(vinyl_card, LV_OBJ_FLAG_SCROLLABLE);

    // 唱片圆盘（黑色）
    lv_obj_t *vinyl_disc = lv_obj_create(vinyl_card);
    lv_obj_set_size(vinyl_disc, vinyl_size, vinyl_size);
    lv_obj_center(vinyl_disc);
    lv_obj_set_style_radius(vinyl_disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(vinyl_disc, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(vinyl_disc, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(vinyl_disc, 2, 0);
    lv_obj_set_style_border_color(vinyl_disc, lv_color_white(), 0);
    lv_obj_set_style_pad_all(vinyl_disc, 0, 0);
    lv_obj_remove_flag(vinyl_disc, LV_OBJ_FLAG_SCROLLABLE);

    // 唱片纹路（3 层同心圆）
    const int ring_sizes[] = {104, 84, 64};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *ring = lv_obj_create(vinyl_disc);
        lv_obj_set_size(ring, ring_sizes[i], ring_sizes[i]);
        lv_obj_center(ring);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ring, 1, 0);
        lv_obj_set_style_border_color(ring, lv_color_white(), 0);
        lv_obj_set_style_border_opa(ring, LV_OPA_40, 0);
        lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    }

    // 中心标签区（白色圆形）
    const int center_size = 40;
    lv_obj_t *vinyl_center = lv_obj_create(vinyl_disc);
    lv_obj_set_size(vinyl_center, center_size, center_size);
    lv_obj_center(vinyl_center);
    lv_obj_set_style_radius(vinyl_center, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(vinyl_center, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(vinyl_center, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(vinyl_center, 2, 0);
    lv_obj_set_style_border_color(vinyl_center, lv_color_black(), 0);
    lv_obj_set_style_pad_all(vinyl_center, 0, 0);
    lv_obj_remove_flag(vinyl_center, LV_OBJ_FLAG_SCROLLABLE);

    // 中心小孔
    lv_obj_t *vinyl_hole = lv_obj_create(vinyl_center);
    lv_obj_set_size(vinyl_hole, 10, 10);
    lv_obj_center(vinyl_hole);
    lv_obj_set_style_radius(vinyl_hole, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(vinyl_hole, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(vinyl_hole, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(vinyl_hole, 0, 0);
    lv_obj_remove_flag(vinyl_hole, LV_OBJ_FLAG_SCROLLABLE);

    // --- 右侧：歌曲信息（白色卡片，和唱片等高）---
    lv_obj_t *info_card = lv_obj_create(page);
    lv_obj_set_size(info_card, info_w, vinyl_card_size);
    lv_obj_set_pos(info_card, info_x, content_y);
    lv_obj_set_style_bg_color(info_card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(info_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(info_card, 2, 0);
    lv_obj_set_style_border_color(info_card, lv_color_black(), 0);
    lv_obj_set_style_radius(info_card, 16, 0);
    lv_obj_set_style_pad_all(info_card, 10, 0);
    lv_obj_remove_flag(info_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_clip_corner(info_card, true, 0);

    // 歌名（小智完整字库，黑字，不会缺字）
    music_title_label_ = lv_label_create(info_card);
    lv_obj_set_style_text_font(music_title_label_, font_cn, 0);
    lv_obj_set_style_text_color(music_title_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(music_title_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(music_title_label_, info_w - 24);
    lv_label_set_long_mode(music_title_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(music_title_label_, "未播放");
    lv_obj_align(music_title_label_, LV_ALIGN_TOP_LEFT, 0, 4);

    // 歌手（小字，透明度低一些，形成层级）
    music_artist_label_ = lv_label_create(info_card);
    lv_obj_set_style_text_font(music_artist_label_, font_sm, 0);
    lv_obj_set_style_text_color(music_artist_label_, lv_color_black(), 0);
    lv_obj_set_style_text_opa(music_artist_label_, LV_OPA_60, 0);
    lv_obj_set_style_text_align(music_artist_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(music_artist_label_, info_w - 24);
    lv_label_set_long_mode(music_artist_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(music_artist_label_, "未知歌手");
    lv_obj_align(music_artist_label_, LV_ALIGN_TOP_LEFT, 0, 28);

    // 分隔线
    lv_obj_t *info_sep = lv_obj_create(info_card);
    lv_obj_set_size(info_sep, info_w - 30, 1);
    lv_obj_set_style_bg_color(info_sep, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(info_sep, LV_OPA_20, 0);
    lv_obj_set_style_border_width(info_sep, 0, 0);
    lv_obj_set_style_radius(info_sep, 0, 0);
    lv_obj_align(info_sep, LV_ALIGN_TOP_LEFT, 0, 50);
    lv_obj_remove_flag(info_sep, LV_OBJ_FLAG_SCROLLABLE);

    // 歌词区域（三行卡拉 OK 样式：上一句淡色、当前句醒目、下一句淡色）
    const int lyric_start_y = 58;       // 分隔线下方起始 y
    const int lyric_line_h = 24;        // 每行歌词高度
    const int lyric_gap = 3;            // 行间距
    const int lyric_w = info_w - 24;

    // 上一句歌词（小字，完全不透明 —— 1-bit 单色屏没有灰度，低透明度会直接消失）
    music_lyric_prev_label_ = lv_label_create(info_card);
    lv_obj_set_style_text_font(music_lyric_prev_label_, font_sm, 0);
    lv_obj_set_style_text_color(music_lyric_prev_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(music_lyric_prev_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(music_lyric_prev_label_, lyric_w);
    lv_label_set_long_mode(music_lyric_prev_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(music_lyric_prev_label_, "");
    lv_obj_align(music_lyric_prev_label_, LV_ALIGN_TOP_LEFT, 0, lyric_start_y);

    // 当前歌词（完整字库，醒目黑色，加大字号）
    music_lyric_label_ = lv_label_create(info_card);
    lv_obj_set_style_text_font(music_lyric_label_, font_cn, 0);
    lv_obj_set_style_text_color(music_lyric_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(music_lyric_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(music_lyric_label_, lyric_w);
    lv_label_set_long_mode(music_lyric_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(music_lyric_label_, "等待播放...");
    lv_obj_align(music_lyric_label_, LV_ALIGN_TOP_LEFT, 0, lyric_start_y + lyric_line_h + lyric_gap);

    // 下一句歌词（小字，完全不透明 —— 1-bit 单色屏没有灰度，低透明度会直接消失）
    music_lyric_next_label_ = lv_label_create(info_card);
    lv_obj_set_style_text_font(music_lyric_next_label_, font_sm, 0);
    lv_obj_set_style_text_color(music_lyric_next_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(music_lyric_next_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(music_lyric_next_label_, lyric_w);
    lv_label_set_long_mode(music_lyric_next_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(music_lyric_next_label_, "");
    lv_obj_align(music_lyric_next_label_, LV_ALIGN_TOP_LEFT, 0, lyric_start_y + (lyric_line_h + lyric_gap) * 2);

    // ============================================================
    // 第 3 层：进度条 + 时间
    // ============================================================

    const int bar_y = content_y + vinyl_card_size + 10;
    const int bar_w = SCR_W - PAD * 2 - 120;  // 留出右侧时间文字的空间

    music_progress_bar_ = lv_bar_create(page);
    // 高度加到 12px，内部留 2px padding 给白边
    lv_obj_set_size(music_progress_bar_, bar_w, 12);
    lv_obj_set_pos(music_progress_bar_, PAD, bar_y + 4);
    lv_bar_set_range(music_progress_bar_, 0, 1000);
    lv_bar_set_value(music_progress_bar_, 0, LV_ANIM_OFF);

    // ── 轨道（白色背景 + 白色边框）──
    // 在黑底屏上形成醒目的白色外壳
    lv_obj_set_style_bg_color(music_progress_bar_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(music_progress_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(music_progress_bar_, 1, 0);
    lv_obj_set_style_border_color(music_progress_bar_, lv_color_white(), 0);
    lv_obj_set_style_border_opa(music_progress_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(music_progress_bar_, 6, 0);
    // 关键：给轨道加 padding，让黑色指示器内缩，四周露出白色底
    lv_obj_set_style_pad_top(music_progress_bar_, 2, 0);
    lv_obj_set_style_pad_bottom(music_progress_bar_, 2, 0);
    lv_obj_set_style_pad_left(music_progress_bar_, 2, 0);
    lv_obj_set_style_pad_right(music_progress_bar_, 2, 0);

    // ── 指示器（纯黑填充，无描边）──
    // 因为轨道有 padding，黑色指示器被白色包裹，自然形成白边
    lv_obj_set_style_bg_color(music_progress_bar_, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(music_progress_bar_, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(music_progress_bar_, 4, LV_PART_INDICATOR);

    // 进度时间文字（进度条右侧）
    music_progress_label_ = lv_label_create(page);
    lv_obj_set_style_text_font(music_progress_label_, font_sm, 0);
    lv_obj_set_style_text_color(music_progress_label_, lv_color_white(), 0);
    lv_obj_set_style_text_opa(music_progress_label_, LV_OPA_70, 0);
    lv_label_set_text(music_progress_label_, "00:00 / 00:00");
    lv_obj_set_pos(music_progress_label_, PAD + bar_w + 8, bar_y + 2);

    // ============================================================
    // 第 4 层：底部 AI 状态卡（和天气页 AI 卡片一致的布局）
    // [emoji 图片] [表情文字] | [AI 对话文字]
    // ============================================================

    const int ai_h = 72;              // 比之前的 32px 加大，给 emoji 留空间
    const int ai_w = SCR_W - PAD * 2;
    const int ai_y = SCR_H - ai_h - 6;
    const int emotion_w = 56;         // 左侧表情区域宽度

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

    // 左侧：emoji 图片（上方，48x48）
    music_emotion_img_ = lv_image_create(ai_card);
    lv_obj_set_size(music_emotion_img_, 40, 40);
    lv_image_set_inner_align(music_emotion_img_, LV_IMAGE_ALIGN_CENTER);
    lv_obj_align(music_emotion_img_, LV_ALIGN_LEFT_MID, 10, -10);
    lv_obj_add_flag(music_emotion_img_, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏，等 SetEmotion 设置

    // 左侧：情绪文字标签（下方）
    music_emotion_label_ = lv_label_create(ai_card);
    lv_obj_set_style_text_font(music_emotion_label_, font_cn, 0);
    lv_obj_set_style_text_color(music_emotion_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(music_emotion_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(music_emotion_label_, emotion_w);
    lv_label_set_long_mode(music_emotion_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(music_emotion_label_, "待命");
    lv_obj_align(music_emotion_label_, LV_ALIGN_LEFT_MID, 4, 20);

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
    music_chat_status_label_ = lv_label_create(ai_card);
    lv_obj_set_style_text_font(music_chat_status_label_, font_cn, 0);
    lv_obj_set_style_text_color(music_chat_status_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(music_chat_status_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(music_chat_status_label_, text_w);
    lv_obj_set_style_text_line_space(music_chat_status_label_, 3, 0);
    lv_label_set_long_mode(music_chat_status_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(music_chat_status_label_, "AI 待命");
    lv_obj_align(music_chat_status_label_, LV_ALIGN_LEFT_MID, text_x, 0);

    ESP_LOGI(TAG, "音乐页面 UI 创建完成（左右排布 + AI 表情卡）");
}
