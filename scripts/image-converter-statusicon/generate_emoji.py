#!/usr/bin/env python3
"""
使用 Google Gemini API 生成 21 张单色表情图片，
然后统一处理成 32x32 的 1-bit 黑白 PNG，用于 RLCD 单色屏显示。

使用方法：
1. 设置环境变量 GEMINI_API_KEY（或在脚本中直接修改）
2. pip install google-genai Pillow
3. python generate_emoji.py

生成结果在 output/emoji_32/ 目录下。
"""

import os
import sys
import time
import base64
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed
import threading

try:
    from google import genai
    from google.genai import types
except ImportError:
    print("请先安装依赖: pip install google-genai")
    sys.exit(1)

try:
    from PIL import Image
except ImportError:
    print("请先安装依赖: pip install Pillow")
    sys.exit(1)

# ===== 配置 =====
# GEMINI_API_KEY = os.environ.get("GEMINI_API_KEY", "")
GEMINI_API_KEY = "AIzaSyCTDVzSZotB8dd-M2ao3FAwtWIKc8-fIbM"
if not GEMINI_API_KEY:
    print("错误：请设置 GEMINI_API_KEY 环境变量")
    print("  export GEMINI_API_KEY='你的API密钥'")
    sys.exit(1)

# 输出目录
RAW_DIR = Path("output/emoji_raw")      # Gemini 生成的原始图片
FINAL_DIR = Path("output/emoji_32")     # 处理后的 32x32 单色图片
RAW_DIR.mkdir(parents=True, exist_ok=True)
FINAL_DIR.mkdir(parents=True, exist_ok=True)

# 21 种表情定义：(英文名, emoji, 表情动作描述 - 针对日系长发美女角色)
EMOTIONS = [
    ("neutral",     "😶", "calm, serene expression with gentle closed lips, peaceful eyes looking forward"),
    ("happy",       "🙂", "warm gentle smile, slightly upturned lips, soft happy eyes"),
    ("laughing",    "😆", "eyes squeezed shut from laughing hard, wide open mouth, hair slightly messy from movement"),
    ("funny",       "😂", "laughing so hard with a tear at the corner of one eye, mouth wide open in joy"),
    ("sad",         "😔", "downcast eyes looking down, slightly furrowed brows, small frown, melancholic mood"),
    ("angry",       "😠", "sharp furrowed eyebrows, intense glaring eyes, tight frown, fierce expression"),
    ("crying",      "😭", "streams of tears flowing down both cheeks, mouth open crying, distressed expression"),
    ("loving",      "😍", "heart-shaped sparkles in eyes, dreamy adoring expression, blissful smile"),
    ("embarrassed", "😳", "wide surprised eyes, small dots on cheeks for blush effect, lips slightly parted in shyness"),
    ("surprised",   "😯", "wide round eyes, small 'o' shaped open mouth, raised eyebrows, hair slightly blown back"),
    ("shocked",     "😱", "extremely wide eyes with tiny pupils, mouth wide open in shock, hands near face"),
    ("thinking",    "🤔", "one hand touching chin, eyes looking upward to the side, slight pursed lips, contemplative"),
    ("winking",     "😉", "one eye closed in a playful wink, slight smirk, confident and flirty"),
    ("cool",        "😎", "wearing small stylish sunglasses, confident slight smile, relaxed composed look"),
    ("relaxed",     "😌", "eyes gently closed, peaceful serene smile, head slightly tilted, content expression"),
    ("delicious",   "🤤", "eyes half-closed in bliss, small drool drop at corner of mouth, savoring expression"),
    ("kissy",       "😘", "puckered lips blowing a kiss, one eye winking, small heart floating near lips"),
    ("confident",   "😏", "one eyebrow slightly raised, knowing smirk, half-lidded confident eyes"),
    ("sleepy",      "😴", "eyes closed, head slightly drooping, small 'zzz' marks near head, drowsy peaceful face"),
    ("silly",       "😜", "tongue sticking out to one side, one eye winking, playful mischievous expression"),
    ("confused",    "🙄", "eyes rolled upward, slight pout, exasperated 'really?' expression"),
]

# 参考图路径（传给 Gemini 保持角色风格一致）
REFERENCE_IMAGE_PATH = Path("output/image.png")

# 生成提示词模板
# 角色设定：参考图中的可爱女孩，但生成黑白线稿版本（适合单色屏）
PROMPT_TEMPLATE = """Look at the reference image. This is the character I want you to draw.

Now draw the SAME cute girl character showing the emotion: "{emotion_name}" {emoji}
Expression: {description}

CRITICAL ART STYLE - Must follow exactly:
- Draw as a BLACK AND WHITE LINE ART illustration (like a coloring book page)
- ONLY pure black lines on pure white background
- THICK bold outlines (3-4px thick) so details survive at tiny 32x32 pixel size
- Keep the cute anime/chibi style from the reference: big round eyes, fluffy hair, ribbons
- Face and head only, filling most of the frame, centered
- SIMPLE features: big expressive eyes, small dot nose, simple mouth
- Hair should be drawn with just the outline and a few bold inner lines
- NO shading, NO gray, NO fill, NO screentone, NO hatching
- NO background at all, pure white
- The expression must be very exaggerated and obvious even at tiny size
- Think: how would this character look as a rubber stamp or simple icon

Keep it SIMPLE and BOLD. Less detail is better. The image will be shrunk to 32x32 pixels.

Generate a 256x256 pixel image."""


def load_reference_image() -> types.Part | None:
    """加载参考图片，传给 Gemini 保持角色风格一致"""
    if not REFERENCE_IMAGE_PATH.exists():
        print(f"  ⚠ 参考图片不存在: {REFERENCE_IMAGE_PATH}")
        print(f"    请将参考图片放到 {REFERENCE_IMAGE_PATH}")
        return None

    image_bytes = REFERENCE_IMAGE_PATH.read_bytes()
    print(f"  已加载参考图片: {REFERENCE_IMAGE_PATH} ({len(image_bytes)} bytes)")
    return types.Part.from_bytes(
        data=image_bytes,
        mime_type="image/png",
    )


def generate_single_emoji(client, emotion_name: str, emoji: str, description: str,
                          ref_image_part: types.Part | None = None) -> bytes | None:
    """使用 Gemini 生成单个表情图片，传入参考图保持风格一致"""
    prompt = PROMPT_TEMPLATE.format(
        emotion_name=emotion_name,
        emoji=emoji,
        description=description,
    )

    print(f"  正在生成 {emotion_name} {emoji} ...", end="", flush=True)

    try:
        # 构建请求内容：参考图 + 文字提示
        contents = []
        if ref_image_part:
            contents.append(ref_image_part)
        contents.append(prompt)

        response = client.models.generate_content(
            model="gemini-2.5-flash-image",
            contents=contents,
            config=types.GenerateContentConfig(
                response_modalities=["IMAGE", "TEXT"],
            ),
        )

        # 从响应中提取图片
        if response.candidates:
            for part in response.candidates[0].content.parts:
                if part.inline_data and part.inline_data.mime_type.startswith("image/"):
                    print(" ✓")
                    return part.inline_data.data

        print(" ✗ (没有返回图片)")
        return None

    except Exception as e:
        print(f" ✗ 错误: {e}")
        return None


def process_to_monochrome_32(input_path: Path, output_path: Path):
    """将图片处理成 32x32 的 1-bit 黑白 PNG"""
    from PIL import ImageFilter, ImageOps
    
    img = Image.open(input_path).convert("RGBA")
    
    # 将透明背景填充为白色
    bg = Image.new("RGBA", img.size, (255, 255, 255, 255))
    bg.paste(img, mask=img.split()[3])
    img = bg.convert("L")  # 转灰度
    
    # 先在大尺寸上做处理，保留更多细节
    # 增强对比度
    img = ImageOps.autocontrast(img, cutoff=5)
    
    # 缩放到 64x64（中间尺寸，比直接缩到 32 保留更多细节）
    img = img.resize((64, 64), Image.LANCZOS)
    
    # 再次增强对比度
    img = ImageOps.autocontrast(img, cutoff=10)
    
    # 缩放到最终 32x32
    img = img.resize((32, 32), Image.LANCZOS)
    
    # 二值化：用稍低的阈值（180），让更多细节保留为黑色
    img = img.point(lambda x: 255 if x > 180 else 0, mode="1")

    # 保存为 PNG
    img.save(output_path, "PNG")
    
    # 同时保存一份 4x 放大预览（128x128，方便查看效果）
    preview_dir = Path("output/emoji_preview")
    preview_dir.mkdir(parents=True, exist_ok=True)
    preview = img.resize((128, 128), Image.NEAREST)  # 最近邻缩放，保持像素锐利
    preview.save(preview_dir / output_path.name, "PNG")


def main():
    print("=" * 60)
    print("  小智 RLCD 单色表情生成器")
    print("  使用 Google Gemini API 生成 21 张表情")
    print("=" * 60)
    print()

    # 初始化 Gemini 客户端
    client = genai.Client(api_key=GEMINI_API_KEY)

    # 加载参考图片
    print("[准备] 加载参考图片...")
    ref_image = load_reference_image()
    if ref_image:
        print("  参考图将用于保持所有表情的角色和风格一致")
    else:
        print("  ⚠ 没有参考图，将仅依赖文字提示生成（风格可能不一致）")
    print()

    # 第一步：生成原始图片（5 并发）
    print("[第 1 步] 生成原始表情图片（5 并发）...")
    print()

    # 筛选出需要生成的
    todo = []
    skipped = 0
    for emotion_name, emoji, description in EMOTIONS:
        raw_path = RAW_DIR / f"{emotion_name}.png"
        if raw_path.exists():
            print(f"  {emotion_name} {emoji} 已存在，跳过")
            skipped += 1
        else:
            todo.append((emotion_name, emoji, description))

    generated = 0
    failed = 0
    print_lock = threading.Lock()

    def gen_task(emotion_name, emoji, description):
        """单个生成任务（线程内执行）"""
        image_data = generate_single_emoji(client, emotion_name, emoji, description, ref_image)
        if image_data:
            raw_path = RAW_DIR / f"{emotion_name}.png"
            raw_path.write_bytes(image_data)
            return (emotion_name, True)
        else:
            with print_lock:
                print(f"  ⚠ {emotion_name} 生成失败")
            return (emotion_name, False)

    if todo:
        print(f"  需要生成 {len(todo)} 张，启动 5 并发...")
        print()
        with ThreadPoolExecutor(max_workers=5) as executor:
            futures = {
                executor.submit(gen_task, name, emo, desc): name
                for name, emo, desc in todo
            }
            for future in as_completed(futures):
                name, success = future.result()
                if success:
                    generated += 1
                else:
                    failed += 1

    print()
    print(f"  完成: {generated} 张新生成, {skipped} 张已存在, {failed} 张失败")
    print()

    # 检查是否有缺失的
    missing = []
    for emotion_name, emoji, _ in EMOTIONS:
        if not (RAW_DIR / f"{emotion_name}.png").exists():
            missing.append(f"{emotion_name} {emoji}")

    if missing:
        print(f"  ⚠ 缺失 {len(missing)} 张表情: {', '.join(missing)}")
        print("  可以重新运行脚本来重试缺失的表情")
        print()

    # 第二步：统一处理成 32x32 单色 PNG
    print("[第 2 步] 处理成 32x32 单色 PNG...")
    print()

    processed = 0
    for emotion_name, emoji, _ in EMOTIONS:
        raw_path = RAW_DIR / f"{emotion_name}.png"
        final_path = FINAL_DIR / f"{emotion_name}.png"

        if not raw_path.exists():
            print(f"  {emotion_name} {emoji} 原始图片缺失，跳过")
            continue

        try:
            process_to_monochrome_32(raw_path, final_path)
            print(f"  {emotion_name} {emoji} → 32x32 ✓")
            processed += 1
        except Exception as e:
            print(f"  {emotion_name} {emoji} 处理失败: {e}")

    print()
    print("=" * 60)
    print(f"  完成！共处理 {processed}/{len(EMOTIONS)} 张表情")
    print(f"  原始图片: {RAW_DIR}/")
    print(f"  最终图片: {FINAL_DIR}/")
    print("=" * 60)

    # 生成预览 HTML
    generate_preview_html()


def generate_preview_html():
    """生成一个 HTML 预览页面，方便查看效果"""
    html_path = Path("output/preview.html")

    rows = []
    for emotion_name, emoji, description in EMOTIONS:
        raw_exists = (RAW_DIR / f"{emotion_name}.png").exists()
        final_exists = (FINAL_DIR / f"{emotion_name}.png").exists()

        rows.append(f"""
        <tr>
            <td>{emoji}</td>
            <td><strong>{emotion_name}</strong></td>
            <td>{description}</td>
            <td>{'<img src="emoji_raw/' + emotion_name + '.png" width="64">' if raw_exists else '❌'}</td>
            <td style="background:#000; text-align:center">
                {'<img src="emoji_32/' + emotion_name + '.png" width="32" style="image-rendering:pixelated">' if final_exists else '❌'}
            </td>
            <td style="background:#000; text-align:center">
                {'<img src="emoji_32/' + emotion_name + '.png" width="64" style="image-rendering:pixelated">' if final_exists else '❌'}
            </td>
        </tr>""")

    html = f"""<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <title>RLCD 单色表情预览</title>
    <style>
        body {{ font-family: -apple-system, sans-serif; max-width: 900px; margin: 40px auto; }}
        table {{ border-collapse: collapse; width: 100%; }}
        th, td {{ border: 1px solid #ddd; padding: 8px; text-align: center; }}
        th {{ background: #f5f5f5; }}
        h1 {{ text-align: center; }}
        .note {{ color: #666; text-align: center; margin: 20px 0; }}
    </style>
</head>
<body>
    <h1>🖥 RLCD 单色表情预览</h1>
    <p class="note">最右列是 2x 放大效果，中间列是实际 32x32 像素尺寸</p>
    <table>
        <tr>
            <th>Emoji</th>
            <th>名称</th>
            <th>描述</th>
            <th>原始图</th>
            <th>32x32 实际</th>
            <th>32x32 放大2x</th>
        </tr>
        {''.join(rows)}
    </table>
</body>
</html>"""

    html_path.write_text(html, encoding="utf-8")
    print(f"\n  预览页面: {html_path}")


if __name__ == "__main__":
    main()
