#!/usr/bin/env python3
"""
将 32x32 PNG 表情转换成 LVGL C 源码（LV_COLOR_FORMAT_RGB565A8），
直接替换 xiaozhi-fonts 中的 emoji_xxxx_32.c 文件。

格式说明（与原始 twemoji_32 完全一致）：
- 每个像素 = 2 字节 RGB565 颜色 + 1 字节 Alpha
- 数据布局：先所有像素的 RGB565（32*32*2=2048字节），后所有 Alpha（32*32=1024字节）
- 总大小：3072 字节

使用方法：
    python convert_to_lvgl.py

输入：output/emoji_32/ 下的 21 张 PNG
输出：output/emoji_c/ 目录下每个表情一个 .c 文件
      直接复制到 xiaozhi-fonts/src/emoji/ 目录替换即可
"""

from pathlib import Path
from PIL import Image

EMOJI_DIR = Path("output/emoji_32")
OUTPUT_DIR = Path("output/emoji_c")
OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

# emoji 名称对应 Unicode 码点（与小智 twemoji_32 一致）
EMOJI_MAP = [
    ("neutral",     "1f636"),
    ("happy",       "1f642"),
    ("laughing",    "1f606"),
    ("funny",       "1f602"),
    ("sad",         "1f614"),
    ("angry",       "1f620"),
    ("crying",      "1f62d"),
    ("loving",      "1f60d"),
    ("embarrassed", "1f633"),
    ("surprised",   "1f62f"),
    ("shocked",     "1f631"),
    ("thinking",    "1f914"),
    ("winking",     "1f609"),
    ("cool",        "1f60e"),
    ("relaxed",     "1f60c"),
    ("delicious",   "1f924"),
    ("kissy",       "1f618"),
    ("confident",   "1f60f"),
    ("sleepy",      "1f634"),
    ("silly",       "1f61c"),
    ("confused",    "1f644"),
]


def png_to_rgb565a8(png_path: Path) -> bytes:
    """
    将 PNG 转为 RGB565A8 格式字节数据。
    布局：[所有像素的RGB565 2字节小端] + [所有像素的Alpha 1字节]
    """
    img = Image.open(png_path).convert("RGBA")
    assert img.size == (32, 32), f"尺寸错误: {img.size}，需要 32x32"

    rgb565_data = bytearray()
    alpha_data = bytearray()

    for y in range(32):
        for x in range(32):
            r, g, b, a = img.getpixel((x, y))
            # RGB565 小端序
            rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            rgb565_data.append(rgb565 & 0xFF)
            rgb565_data.append((rgb565 >> 8) & 0xFF)
            # Alpha
            alpha_data.append(a)

    return bytes(rgb565_data + alpha_data)


def generate_c_file(emotion_name: str, unicode_cp: str, data: bytes) -> str:
    """生成单个 emoji 的 C 源码（与原始 twemoji_32 格式完全一致）"""
    var_name = f"emoji_{unicode_cp}_32"
    attr_name = f"LV_ATTRIBUTE_EMOJI_{unicode_cp.upper()}_32"

    lines = []
    lines.append("")
    lines.append("#if defined(LV_LVGL_H_INCLUDE_SIMPLE)")
    lines.append('#include "lvgl.h"')
    lines.append("#elif defined(LV_BUILD_TEST)")
    lines.append('#include "../lvgl.h"')
    lines.append("#else")
    lines.append('#include "lvgl/lvgl.h"')
    lines.append("#endif")
    lines.append("")
    lines.append("")
    lines.append("#ifndef LV_ATTRIBUTE_MEM_ALIGN")
    lines.append("#define LV_ATTRIBUTE_MEM_ALIGN")
    lines.append("#endif")
    lines.append("")
    lines.append(f"#ifndef {attr_name}")
    lines.append(f"#define {attr_name}")
    lines.append("#endif")
    lines.append("")
    lines.append("static const")
    lines.append(f"LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST {attr_name}")
    lines.append(f"uint8_t {var_name}_map[] = {{")
    lines.append("")

    # 每行 64 字节（与原始文件一致）
    for row in range(32):
        # RGB565 部分：每行 64 字节（32 像素 * 2 字节）
        start = row * 64
        end = start + 64
        chunk = data[start:end]
        hex_str = ",".join(f"0x{b:02x}" for b in chunk)
        lines.append(f"    {hex_str},")

    # Alpha 部分在 RGB565 数据之后
    alpha_offset = 32 * 32 * 2  # 2048
    for row in range(32):
        start = alpha_offset + row * 32
        end = start + 32
        chunk = data[start:end]
        hex_str = ",".join(f"0x{b:02x}" for b in chunk)
        lines.append(f"    {hex_str},")

    lines.append("")
    lines.append("};")
    lines.append("")
    lines.append(f"const lv_image_dsc_t {var_name} = {{")
    lines.append("  .header.magic = LV_IMAGE_HEADER_MAGIC,")
    lines.append("  .header.cf = LV_COLOR_FORMAT_RGB565A8,")
    lines.append("  .header.flags = 0,")
    lines.append("  .header.w = 32,")
    lines.append("  .header.h = 32,")
    lines.append("  .header.stride = 64,")
    lines.append(f"  .data_size = {len(data)},")
    lines.append(f"  .data = {var_name}_map,")
    lines.append("};")
    lines.append("")

    return "\n".join(lines)


def main():
    print("=" * 60)
    print("  PNG → LVGL emoji C 源码转换")
    print("  格式: RGB565A8, 32x32, 兼容 xiaozhi-fonts")
    print("=" * 60)
    print()

    converted = 0
    for emotion_name, unicode_cp in EMOJI_MAP:
        png_path = EMOJI_DIR / f"{emotion_name}.png"
        if not png_path.exists():
            print(f"  ⚠ 缺失: {emotion_name}.png")
            continue

        print(f"  {emotion_name} → emoji_{unicode_cp}_32.c ...", end="")

        data = png_to_rgb565a8(png_path)
        c_code = generate_c_file(emotion_name, unicode_cp, data)

        out_path = OUTPUT_DIR / f"emoji_{unicode_cp}_32.c"
        out_path.write_text(c_code, encoding="utf-8")
        print(f" ✓ ({len(data)} bytes)")
        converted += 1

    print()
    print(f"  转换完成: {converted}/{len(EMOJI_MAP)} 个文件")
    print(f"  输出目录: {OUTPUT_DIR}/")
    print()
    print("  ===== 替换步骤 =====")
    print()
    target = "xiaozhi-esp32/managed_components/78__xiaozhi-fonts/src/emoji/"
    print(f"  直接复制替换:")
    print(f"    cp {OUTPUT_DIR}/*.c {target}")
    print()
    print("  然后重新编译固件即可！")


if __name__ == "__main__":
    main()
