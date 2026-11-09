"""在现有字库基础上补充缺字。

背景：`cn_font.h` 原本只覆盖 GB2312 **一级**汉字（3755）+ ASCII。实测 SD 卡上
42 首歌名里有 13 首显示成方块 —— 日文假名、二级/繁体汉字（奕 靓 輪 遠 澤 愛
織 紀 風 磯 戸 見 琪 蜢）、全角标点（（）·～）、还有西里尔字母（Звезда）。

策略：**原有字形原样保留**（一级汉字用的是像素字体，观感好），只把缺的字符用
系统字体渲染后追加，最后按码点重排（二分查找要求升序）。重复运行是幂等的。

用法：
    python tools/extend_cn_font.py                        # 补 GB2312 全表
    python tools/extend_cn_font.py --ref-dir D:\\Music     # 顺带把某个目录里
                                                          # 文件名用到的字符也补上

注意：本脚本只写 src/cn_font.h（已 gitignore，字体不可再分发）。
"""
import argparse
import io
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

SIZE = 16
HERE = os.path.dirname(os.path.abspath(__file__))
DEST = os.path.normpath(os.path.join(HERE, "..", "src", "cn_font.h"))

FONT_CANDIDATES = [
    r"C:\Windows\Fonts\simhei.ttf",
    r"C:\Windows\Fonts\msyh.ttc",
    r"C:\Windows\Fonts\simsun.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
]


def parse_font(path):
    """读出已有的 {码点: 32 字节字模}。"""
    text = io.open(path, encoding="utf-8").read()
    glyphs = {}
    for m in re.finditer(r"\{0x([0-9A-Fa-f]{4}),\s*\{([^}]*)\}\}", text):
        code = int(m.group(1), 16)
        bits = bytes(int(b, 16) for b in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(2)))
        if len(bits) == 32:
            glyphs[code] = bits
    return glyphs


def gb2312_all():
    """GB2312 全表：符号区（含日文假名/希腊/西里尔）+ 一级汉字 + 二级汉字。"""
    out = set()
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                out.add(bytes([hi, lo]).decode("gb2312"))
            except Exception:
                pass
    return out


def render(ch, font, img, draw):
    """渲染到 16x16 单色位图，返回 32 字节（每行 2 字节，高位在左）。"""
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
    ap = argparse.ArgumentParser(description="补充字库缺字")
    ap.add_argument("--font", default=None, help="补充用的字体（默认自动找系统黑体）")
    ap.add_argument("--ref-dir", default=None,
                    help="参考目录：把这个目录下文件名用到的字符也补上（如音乐库）")
    ap.add_argument("--out", default=DEST)
    args = ap.parse_args()

    if not os.path.exists(args.out):
        print("找不到现有字库: %s" % args.out, file=sys.stderr)
        return 1

    old = parse_font(args.out)
    print("现有字库: %d 字" % len(old))

    want = set(gb2312_all())
    nref = 0
    if args.ref_dir and os.path.isdir(args.ref_dir):
        for f in os.listdir(args.ref_dir):
            for ch in f:
                if ch != " ":
                    want.add(ch)
            nref += 1
        print("参考目录: %s（%d 个文件）" % (args.ref_dir, nref))
    print("目标字符集: %d 个" % len(want))

    need = [c for c in want if ord(c) not in set(old.keys())]
    print("需要新增: %d 个" % len(need))
    if not need:
        print("没有缺字，无需改动")
        return 0

    font_path = args.font or next((p for p in FONT_CANDIDATES if os.path.exists(p)), None)
    if not font_path:
        print("找不到可用的系统字体，请用 --font 指定", file=sys.stderr)
        return 1
    print("补充字形用的字体: %s" % font_path)

    font = ImageFont.truetype(font_path, SIZE)
    img = Image.new("L", (SIZE, SIZE), 0)
    draw = ImageDraw.Draw(img)

    added, blank = 0, []
    for ch in sorted(need):
        bits = render(ch, font, img, draw)
        if not any(bits):
            blank.append(ch)          # 这个字体里也没有
            continue
        old[ord(ch)] = bytes(bits)
        added += 1

    print("新增字形: %d 个" % added)
    if blank:
        print("字体里也没有的 %d 个: %s" % (len(blank), "".join(blank[:30])))

    items = sorted(old.items(), key=lambda kv: kv[0])
    print("合并后: %d 字，字模 %.1f KB" % (len(items), len(items) * 32 / 1024))

    out = []
    out.append("// 自动生成，请勿手改。生成脚本：tools/gen_cn_font.py + tools/extend_cn_font.py")
    out.append("// 16x16 单色点阵，每字 32 字节，按 Unicode 码点升序排列（供二分查找）")
    out.append("// 字符集：GB2312 全表（符号 + 一级/二级汉字）+ 日文假名 + ASCII")
    out.append("// 注：一级汉字沿用原像素字体字形；新增字符用系统黑体补的")
    out.append("#ifndef CN_FONT_H")
    out.append("#define CN_FONT_H")
    out.append("")
    out.append("#include <stdint.h>")
    out.append("")
    out.append("#define CN_GLYPH_W %d" % SIZE)
    out.append("#define CN_GLYPH_H %d" % SIZE)
    out.append("#define CN_GLYPH_BYTES_PER_ROW %d" % (SIZE // 8))
    out.append("#define CN_GLYPH_SIZE 32")
    out.append("")
    out.append("typedef struct {")
    out.append("    uint16_t code;")
    out.append("    uint8_t  bits[CN_GLYPH_SIZE];")
    out.append("} cn_glyph_t;")
    out.append("")
    out.append("#define CN_GLYPH_COUNT %d" % len(items))
    out.append("")
    out.append("static const cn_glyph_t cn_glyphs[CN_GLYPH_COUNT] = {")
    for code, bits in items:
        hexb = ",".join("0x%02X" % b for b in bits)
        out.append("    {0x%04X, {%s}}," % (code, hexb))
    out.append("};")
    out.append("")
    out.append("#endif  // CN_FONT_H")

    io.open(args.out, "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")
    print("已写入 %s (%.1f KB)" % (args.out, os.path.getsize(args.out) / 1024))
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main())
