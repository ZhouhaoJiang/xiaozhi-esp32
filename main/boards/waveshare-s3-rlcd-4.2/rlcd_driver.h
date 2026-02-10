#ifndef __RLCD_DRIVER_H__
#define __RLCD_DRIVER_H__

#include <cstdint>
#include <driver/gpio.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>

// RLCD 像素颜色（1-bit 单色屏，只有黑白两色）
enum ColorSelection {
    ColorBlack = 0,    
    ColorWhite = 0xff
};

// SPI 显示屏引脚配置
typedef struct {
    uint8_t mosi;
    uint8_t scl;
    uint8_t dc;
    uint8_t cs;
    uint8_t rst;
} spi_display_config_t;

// RLCD 硬件驱动层
// 
// 负责底层 SPI 通信、像素映射、屏幕初始化等硬件操作。
// 这个类不涉及任何 UI 逻辑，只提供像素级别的绘制能力。
class RlcdDriver {
public:
    RlcdDriver(spi_display_config_t spiconfig, int width, int height, 
               spi_host_device_t spi_host = SPI3_HOST);
    ~RlcdDriver();

    // 屏幕初始化（发送初始化命令序列）
    void RLCD_Init();
    // 清屏（填充指定颜色）
    void RLCD_ColorClear(uint8_t color);
    // 设置单个像素
    void RLCD_SetPixel(uint16_t x, uint16_t y, uint8_t color);
    // 将缓冲区内容刷新到屏幕
    void RLCD_Display();
    // 设置屏幕对比度 (0x00-0x1F)
    void SetContrast(uint8_t level);

    // 获取屏幕尺寸
    int width() const { return width_; }
    int height() const { return height_; }

    // 获取 SPI IO 句柄（LVGL 初始化可能需要）
    esp_lcd_panel_io_handle_t io_handle() const { return io_handle_; }

private:
    // SPI 引脚配置
    int mosi_;
    int scl_;
    int dc_;
    int cs_;
    int rst_;
    int width_;
    int height_;

    // SPI IO 句柄
    esp_lcd_panel_io_handle_t io_handle_ = nullptr;

    // 1-bit 显示缓冲区
    uint8_t *DispBuffer = nullptr;
    int DisplayLen = 0;

    // 像素映射查找表（加速 RGB565 → 1-bit 转换）
    uint16_t (*PixelIndexLUT)[300] = nullptr;
    uint8_t  (*PixelBitLUT)[300] = nullptr;

    // 初始化像素映射 LUT（竖屏/横屏两种模式）
    void InitPortraitLUT();
    void InitLandscapeLUT();

    // 硬件控制
    void Set_ResetIOLevel(uint8_t level);
    void RLCD_SendCommand(uint8_t Reg);
    void RLCD_SendData(uint8_t Data);
    void RLCD_Sendbuffera(uint8_t *Data, int len);
    void RLCD_Reset();
};

#endif
