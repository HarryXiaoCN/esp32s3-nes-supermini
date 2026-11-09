"""生成中文字点阵字库（C 头文件），供 ST7789 显示使用。

字符集取 GB2312 一级汉字（3755 字，覆盖所有常用字）+ ASCII 可见字符，
这样任何游戏名都能显示出来，不会出现"方块"。
每字 16x16 单色点阵 = 32 字节，全库约 120KB，放 flash（.rodata）不占 RAM。

用法：
    python gen_cn_font.py --font /path/to/chinese-font.ttf
    python gen_cn_font.py                      # 自动在系统里找中文字体

字体选择建议：
    优先用 **16px 像素字体**（字形对齐格点，最干净）。
    矢量字体（微软雅黑、黑体）在 16x16 下笔画容易粘连，能认但不精致。
    本项目开发时用的是 Minecraft AE Pixel 风格字体。

字体文件有各自的许可，请不要把字体文件提交进仓库。
"""
import argparse
import os
import sys

from PIL import Image, ImageDraw, ImageFont

SIZE = 16
HERE = os.path.dirname(os.path.abspath(__file__))
DEST = os.path.normpath(os.path.join(HERE, "..", "src", "cn_font.h"))

AUTO_CANDIDATES = [
    r"C:\Windows\Fonts\msyh.ttc",
    r"C:\Windows\Fonts\simhei.ttf",
    r"C:\Windows\Fonts\simsun.ttc",
    "/System/Library/Fonts/PingFang.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
]


def auto_font():
    for p in AUTO_CANDIDATES:
        if os.path.exists(p):
            return p
    return None


def gb2312_level1():
    """GB2312 一级汉字表（0xB0A1-0xD7F9），共 3755 字。"""
    out = []
    for hi in range(0xB0, 0xD8):
        for lo in range(0xA1, 0xFF):
            try:
                out.append(bytes([hi, lo]).decode("gb2312"))
            except Exception:
                pass
    return out


def render(ch, font, img, draw):
    """渲染单个字符到 16x16 单色位图，返回 32 字节（每行 2 字节，高位在左）。"""
    draw.rectangle((0, 0, SIZE - 1, SIZE - 1), fill=0)
    draw.text((SIZE / 2, SIZE / 2), ch, font=font, fill=255, anchor="mm")

    bits = []
    for y in range(SIZE):
        row = 0
        for x in range(SIZE):
            if img.getpixel((x, y)) >= 110:
                row |= 1 << (SIZE - 1 - x)
        bits.append((row >> 8) & 0xFF)
        bits.append(row & 0xFF)
    return bits


def main():
    ap = argparse.ArgumentParser(description="生成 16x16 中文点阵字库")
    ap.add_argument("--font", default=None, help="中文字体路径（.ttf/.ttc/.otf）")
    ap.add_argument("--out", default=DEST, help="输出头文件路径")
    args = ap.parse_args()

    font_path = args.font or auto_font()
    if not font_path or not os.path.exists(font_path):
        print("找不到中文字体，请用 --font 指定：", file=sys.stderr)
        print(r"  python gen_cn_font.py --font C:\Windows\Fonts\msyh.ttc", file=sys.stderr)
        sys.exit(1)

    print(f"字体: {font_path}")
    font = ImageFont.truetype(font_path, SIZE)
    img = Image.new("L", (SIZE, SIZE), 0)
    draw = ImageDraw.Draw(img)

    chars = set(gb2312_level1())
    for c in range(0x20, 0x7F):
        chars.add(chr(c))

    chars = sorted(chars, key=ord)   # 二分查找要求按码点升序

    glyphs = []
    skipped = []
    for ch in chars:
        bits = render(ch, font, img, draw)
        if not any(bits):
            skipped.append(ch)
            continue
        glyphs.append((ord(ch), bits))

    print(f"字符 {len(chars)} 个，有效字形 {len(glyphs)}，空白跳过 {len(skipped)}")
    print(f"字模体积 {len(glyphs) * 32 / 1024:.1f} KB")

    if len(glyphs) < 1000:
        print("警告：有效字形太少，这个字体可能不含中文！", file=sys.stderr)
        print(f"  缺失样例: {''.join(skipped[:30])}", file=sys.stderr)

    out = []
    out.append("// 自动生成，请勿手改。生成脚本：tools/gen_cn_font.py")
    out.append("// 16x16 单色点阵，每字 32 字节，按 Unicode 码点升序排列（供二分查找）")
    out.append("// 字符集：GB2312 一级汉字 + ASCII")
    out.append("#ifndef CN_FONT_H")
    out.append("#define CN_FONT_H")
    out.append("")
    out.append("#include <stdint.h>")
    out.append("")
    out.append(f"#define CN_GLYPH_W {SIZE}")
    out.append(f"#define CN_GLYPH_H {SIZE}")
    out.append(f"#define CN_GLYPH_BYTES_PER_ROW {SIZE // 8}")
    out.append("#define CN_GLYPH_SIZE 32")
    out.append("")
    out.append("typedef struct {")
    out.append("    uint16_t code;")
    out.append("    uint8_t  bits[CN_GLYPH_SIZE];")
    out.append("} cn_glyph_t;")
    out.append("")
    out.append(f"#define CN_GLYPH_COUNT {len(glyphs)}")
    out.append("")
    out.append("static const cn_glyph_t cn_glyphs[CN_GLYPH_COUNT] = {")
    for code, bits in glyphs:
        # 不能用字符串字面量：C 会在末尾补 '\0'，32 字节数组就超了
        hexb = ",".join(f"0x{b:02X}" for b in bits)
        out.append(f"    {{0x{code:04X}, {{{hexb}}}}},")
    out.append("};")
    out.append("")
    out.append("#endif  // CN_FONT_H")

    out_dir = os.path.dirname(args.out)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    print(f"已生成: {args.out}")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    main()
