#include "rlcd_driver.h"

#include <cstring>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "RlcdDriver";

// ===== 构造 / 析构 =====

RlcdDriver::RlcdDriver(spi_display_config_t spiconfig, int width, int height,
                        spi_host_device_t spi_host)
    : mosi_(spiconfig.mosi), scl_(spiconfig.scl),
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
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)spi_host, &io_config, &io_handle_));

    // 配置复位引脚
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
    if (width_ == 400) {
        InitLandscapeLUT();
    } else {
        InitPortraitLUT();
    }

    ESP_LOGI(TAG, "RLCD 硬件驱动初始化完成 (%dx%d)", width_, height_);
}

RlcdDriver::~RlcdDriver() {
    if (DispBuffer) heap_caps_free(DispBuffer);
    if (PixelIndexLUT) heap_caps_free(PixelIndexLUT);
    if (PixelBitLUT) heap_caps_free(PixelBitLUT);
}

// ===== 像素映射 LUT 初始化 =====

void RlcdDriver::InitPortraitLUT() {
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

void RlcdDriver::InitLandscapeLUT() {
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

// ===== 硬件控制 =====

void RlcdDriver::Set_ResetIOLevel(uint8_t level) {
    gpio_set_level((gpio_num_t)rst_, level ? 1 : 0);
}

void RlcdDriver::RLCD_SendCommand(uint8_t Reg) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle_, Reg, NULL, 0));
}

void RlcdDriver::RLCD_SendData(uint8_t Data) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle_, -1, &Data, 1));
}

void RlcdDriver::RLCD_Sendbuffera(uint8_t *Data, int len) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(io_handle_, -1, Data, len));
}

void RlcdDriver::RLCD_Reset() {
    Set_ResetIOLevel(1);
    vTaskDelay(pdMS_TO_TICKS(50));
    Set_ResetIOLevel(0);
    vTaskDelay(pdMS_TO_TICKS(20));
    Set_ResetIOLevel(1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

// ===== 屏幕操作 =====

void RlcdDriver::RLCD_ColorClear(uint8_t color) {
    memset(DispBuffer, color, DisplayLen);
}

void RlcdDriver::RLCD_Init() {
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

void RlcdDriver::RLCD_SetPixel(uint16_t x, uint16_t y, uint8_t color) {
    uint32_t idx = PixelIndexLUT[x][y];
    uint8_t  mask = PixelBitLUT[x][y];
    uint8_t *p = &DispBuffer[idx];
    if (color)
        *p |= mask;
    else
        *p &= ~mask;
}

void RlcdDriver::RLCD_Display() {
    RLCD_SendCommand(0x2A);
    RLCD_SendData(0x12);
    RLCD_SendData(0x2A);
    RLCD_SendCommand(0x2B);
    RLCD_SendData(0x00);
    RLCD_SendData(0xC7);
    RLCD_SendCommand(0x2c);
    RLCD_Sendbuffera(DispBuffer, DisplayLen);
}

void RlcdDriver::SetContrast(uint8_t level) {
    // 调节 RLCD 对比度（通过修改 VCOM 电压）
    // level 范围: 0x00-0x1F (理论上越大对比度越高)
    // 
    // 实测结果（Waveshare ESP32-S3-RLCD-4.2）：
    //   0x11 = 最佳默认值（出厂初始化值）
    //   过小（<0x0C）或过大（>0x16）都会导致显示过淡
    //   推荐范围：0x0D-0x15（在 0x11 附近微调）
    
    if (level > 0x1F) level = 0x1F;
    
    RLCD_SendCommand(0xC0);  // VCOM 控制寄存器
    RLCD_SendData(level);    // 设置 VCOM 高电压
    RLCD_SendData(0x04);     // 设置 VCOM 低电压（保持默认）
    
    ESP_LOGI(TAG, "屏幕对比度已设置为: 0x%02X", level);
}
