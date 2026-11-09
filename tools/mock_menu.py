"""固件像素级 UI 模拟器。

**这不是"参考固件画的示意图"，而是逐像素重放实机画面。**

和旧版（用 TrueType 近似画中文、坐标手抄）的区别：本脚本直接解析固件源码里的
四样东西，再按 osd.cpp 组件层的绘制顺序重放一遍：

    src/cn_font.h      汉字 16x16 点阵（3849 字）
    src/tft_driver.h   ASCII 5x7 点阵（font_5x7，PROGMEM）
    src/ui_theme.h     颜色 / 行高 / 内边距 / 圆角 token
    src/osd.cpp        组件层调用顺序

连 fill_round_rect 的四角圆弧算法、draw_cn_text 的整格填充、draw_chevron 的
5 个像素都是照抄固件，所以输出图和实机应当一致（包括 RGB565 量化后的色偏
—— 这里刻意保留，那才是屏幕上真实的颜色）。

三个顶层页面是并列的（游戏 / 音乐 / 设置），SELECT 循环向后切、B 向前切。

用法：
    python tools/mock_menu.py --mode all       # 全部页面 + 3x3 对比图
    python tools/mock_menu.py --mode menu
    python tools/mock_menu.py --mode music
    python tools/mock_menu.py --mode settings

输出在 docs/ 下，放大 2 倍（最近邻，保持像素感）。
"""
import argparse
import os
import re
import sys

from PIL import Image

W = H = 240
SCALE = 2
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
SRC = os.path.join(ROOT, "src")
DOCS = os.path.join(ROOT, "docs")

CN_GLYPH_W = CN_GLYPH_H = 16
VISIBLE_ROWS = 5

# 三个顶层页签（顺序必须和固件 ui_page_t 一致：游戏 / 音乐 / 设置）
TABS = {
    0: ["游戏", "音乐", "设置"],
    1: ["GAMES", "MUSIC", "SETUP"],
}


# ---------------------------------------------------------------- 资源加载
def load_theme(path):
    """解析 ui_theme.h 里的数值型 #define（UI_BAR_H 是宏函数，单独处理）。"""
    t = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"#define\s+UI_(\w+)\s+(0x[0-9A-Fa-f]+|\d+)\s*(?://.*)?$", line.strip())
            if m:
                t[m.group(1)] = int(m.group(2), 0)
    if len(t) < 15:
        raise SystemExit("ui_theme.h 解析异常，只拿到 %d 个 token" % len(t))
    return t


def load_cn_font(path):
    """解析 cn_font.h：{0xXXXX, {32 字节}} -> {码点: bytes[32]}"""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    glyphs = {}
    for m in re.finditer(r"\{0x([0-9A-Fa-f]{4}),\s*\{([^}]*)\}\}", text):
        code = int(m.group(1), 16)
        bits = bytes(int(b, 16) for b in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(2)))
        if len(bits) == 32:
            glyphs[code] = bits
    return glyphs


def load_ascii_font(path):
    """解析 tft_driver.h 的 font_5x7：每字符 5 字节，每字节是一列，bit0..6 是行。"""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    m = re.search(r"font_5x7\[\]\s*PROGMEM\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        raise SystemExit("tft_driver.h 里没找到 font_5x7")
    # 每行尾部的 `// 0x20 ' '` 注释里也有 0xNN 形式的十六进制，
    # 必须先剥掉注释再提取，否则字模会整体错位。
    body = re.sub(r"//[^\n]*", "", m.group(1))
    raw = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
    if len(raw) % 5 != 0:
        raise SystemExit("font_5x7 字节数 %d 不是 5 的倍数" % len(raw))
    font = {}
    for i in range(0, len(raw), 5):
        font[0x20 + i // 5] = raw[i:i + 5]
    return font


# ---------------------------------------------------------------- 画布
class Canvas:
    """240x240 RGB565 画布。所有绘制方法都复现 tft_driver.h / osd.cpp 的实现。"""

    def __init__(self):
        self.px = [0x0000] * (W * H)

    def fill(self, color):
        self.px = [color] * (W * H)

    def rect(self, x, y, w, h, color):
        """复现 tft.drawFilledRect（含裁剪）。"""
        if w <= 0 or h <= 0:
            return
        x0, y0 = max(0, x), max(0, y)
        x1, y1 = min(W - 1, x + w - 1), min(H - 1, y + h - 1)
        if x1 < x0 or y1 < y0:
            return
        for yy in range(y0, y1 + 1):
            base = yy * W
            for xx in range(x0, x1 + 1):
                self.px[base + xx] = color

    def round_rect(self, x, y, w, h, r, color):
        """复现 osd.cpp 的 fill_round_rect（四角圆弧逐行计算，右侧不能漏）。"""
        if w <= 0 or h <= 0:
            return
        if r * 2 > w:
            r = w // 2
        if r * 2 > h:
            r = h // 2
        if r <= 0:
            self.rect(x, y, w, h, color)
            return

        self.rect(x + r, y, w - 2 * r, h, color)              # 中间竖条
        self.rect(x, y + r, r, h - 2 * r, color)              # 左条
        self.rect(x + w - r, y + r, r, h - 2 * r, color)      # 右条

        for i in range(r):
            dy = r - i
            dx = 0
            while (dx + 1) * (dx + 1) + dy * dy <= r * r:
                dx += 1
            cw = dx * 2
            self.rect(x + r - dx, y + i, cw, 1, color)                 # 左上
            self.rect(x + w - r - dx, y + i, cw, 1, color)             # 右上
            self.rect(x + r - dx, y + h - 1 - i, cw, 1, color)         # 左下
            self.rect(x + w - r - dx, y + h - 1 - i, cw, 1, color)     # 右下

    def text(self, x, y, s, fg, bg):
        """复现 draw_cn_text：整行拼条带，ASCII 整格填（含右侧 2px 间距）。"""
        total_w = text_width(s)
        if total_w <= 0 or x < 0 or x >= W or y < 0 or y + CN_GLYPH_H > H:
            return total_w

        w = total_w
        if x + w > W:
            w = W - x
        if w <= 0:
            return total_w

        ascii_top = (CN_GLYPH_H - 7) // 2

        for row in range(CN_GLYPH_H):
            band = [bg] * w
            cx = 0
            for ch in s:
                if cx >= w:
                    break
                code = ord(ch)
                if code < 0x80:
                    cw = 10 if ch == " " else 7
                    glyph = ASCII.get(code)
                    gy = row - ascii_top
                    visible = glyph is not None and 0 <= gy < 7
                    for c in range(cw):
                        px = cx + c
                        if px >= w:
                            break
                        on = visible and c < 5 and ((glyph[c] >> gy) & 1)
                        band[px] = fg if on else bg
                    cx += cw
                else:
                    g = CN.get(code)
                    v = (g[row * 2] << 8) | g[row * 2 + 1] if g else 0
                    for c in range(CN_GLYPH_W):
                        px = cx + c
                        if px >= w:
                            break
                        if g:
                            on = (v >> (CN_GLYPH_W - 1 - c)) & 1
                        else:
                            # 缺字时固件画实心方块占位
                            on = 2 <= row < CN_GLYPH_H - 2 and 2 <= c < CN_GLYPH_W - 2
                        band[px] = fg if on else bg
                    cx += CN_GLYPH_W

            for i in range(cx, w):
                band[i] = bg
            base = (y + row) * W
            for i in range(w):
                self.px[base + x + i] = band[i]
        return total_w

    def to_image(self):
        img = Image.new("RGB", (W, H))
        px = img.load()
        for yy in range(H):
            base = yy * W
            for xx in range(W):
                px[xx, yy] = c565_to_rgb888(self.px[base + xx])
        return img


def text_width(s):
    """复现 cn_text_width：汉字 16，ASCII 7，空格 10。"""
    return sum(10 if ch == " " else (7 if ord(ch) < 0x80 else 16) for ch in s)


def c565_to_rgb888(c):
    """RGB565 -> RGB888。刻意保留量化损失，那正是屏幕上真实的颜色。"""
    r = (c >> 11) & 0x1F
    g = (c >> 5) & 0x3F
    b = c & 0x1F
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def draw_center(cv, y, s, fg, bg):
    """复现 draw_cn_center：按整屏宽度居中。"""
    cv.text((W - text_width(s)) // 2, y, s, fg, bg)


def draw_chevron(cv, x, y, color):
    """复现 draw_chevron：5 个像素的对角线。"""
    for i in range(5):
        cv.rect(x + i, y + i, 1, 1, color)
        cv.rect(x + i, y + 8 - i, 1, 1, color)


# ---------------------------------------------------------------- 组件层
def ui_page(cv, title, trailing, centered):
    cv.fill(TM["BG"])
    if centered:
        draw_center(cv, TM["TITLE_CTR"], title, TM["TXT"], TM["BG"])
    else:
        cv.text(TM["TITLE_X"], TM["TITLE_Y"], title, TM["TXT"], TM["BG"])
    if trailing:
        tw = text_width(trailing)
        cv.text(W - TM["TITLE_X"] - tw, TM["TITLE_Y"] + 2, trailing, TM["TXT2"], TM["BG"])


def ui_page_tabs(cv, active, trailing, lang=0):
    """复现 ui_page_tabs：顶部页签，当前页下方 2px 系统蓝指示条。"""
    cv.fill(TM["BG"])

    x = TM["TITLE_X"]
    for i, label in enumerate(TABS[lang]):
        w = text_width(label)
        on = (i == active)
        cv.text(x, TM["TITLE_Y"], label, TM["TXT"] if on else TM["TXT2"], TM["BG"])
        if on:
            cv.rect(x, TM["TITLE_Y"] + CN_GLYPH_H + TM["TAB_BAR_DY"],
                    w, TM["TAB_BAR_H"], TM["ACCENT"])
        x += w + TM["TAB_GAP"]

    if trailing:
        tw = text_width(trailing)
        cv.text(W - TM["TITLE_X"] - tw, TM["TITLE_Y"] + 2, trailing, TM["TXT2"], TM["BG"])


NAV = {0: "SELECT 切页", 1: "SELECT: PAGE"}


def ui_page_footer(cv, hint, nav=None):
    """复现 ui_page_footer：左=本页操作，右=全局导航（SELECT 切页）。"""
    clear_y = H - TM["FOOTER_CLEAR"]
    cv.rect(0, clear_y, W, H - clear_y, TM["BG"])

    y = H - 30
    if nav:
        nw = text_width(nav)
        cv.text(W - TM["PAGE_PAD"] - 4 - nw, y, nav, TM["TXT3"], TM["BG"])
    if hint:
        cv.text(TM["PAGE_PAD"] + 4, y, hint, TM["TXT2"], TM["BG"])

    cv.round_rect((W - TM["HOME_W"]) // 2, H - 8, TM["HOME_W"], TM["HOME_H"], 2, TM["HOMEIND"])


def ui_group(cv, y, rows, row_h):
    cv.round_rect(TM["PAGE_PAD"], y, W - 2 * TM["PAGE_PAD"],
                  row_h * rows + TM["ROW_GAP"], TM["CARD_R"], TM["CARD"])


def ui_row(cv, y, h, sel, label, value, chevron, sep):
    x = TM["PAGE_PAD"]
    w = W - 2 * TM["PAGE_PAD"]
    ty = y + (h - CN_GLYPH_H) // 2

    if sel:
        cv.round_rect(x + TM["ROW_INSET"], y, w - 2 * TM["ROW_INSET"], h, TM["ROW_R"], TM["CARD_SEL"])
        bar_h = h * 9 // 16                      # UI_BAR_H(h)
        cv.round_rect(x + TM["BAR_INSET"], y + (h - bar_h) // 2, TM["BAR_W"], bar_h, 2, TM["ACCENT"])
    else:
        cv.rect(x + TM["ROW_INSET"], y, w - 2 * TM["ROW_INSET"], h, TM["CARD"])

    bg = TM["CARD_SEL"] if sel else TM["CARD"]
    cv.text(x + TM["ROW_PAD"], ty, label, TM["TXT"] if sel else TM["TXT2"], bg)

    if value:
        vw = text_width(value)
        cv.text(x + w - TM["ROW_END"] - vw, ty, value, TM["ACCENT"] if sel else TM["TXT2"], bg)
    if chevron:
        draw_chevron(cv, x + w - TM["ROW_END"] - 4, y + (h - 9) // 2, TM["TXT2"])
    if sep and not sel:
        cv.rect(x + TM["ROW_PAD"], y + h - 1, w - TM["ROW_PAD"] - TM["ROW_END"], 1, TM["SEP"])


def ui_scrollbar(cv, top, h, total, visible, first):
    if total <= visible or visible <= 0:
        return
    x = W - TM["SCROLL_W"] - 6
    bar_h = max(12, h * visible // total)
    span = total - visible
    bar_y = top + (h - bar_h) * (first if span > 0 else 0) // (span if span > 0 else 1)
    cv.round_rect(x, top, TM["SCROLL_W"], h, 1, TM["CARD_SEL"])
    cv.round_rect(x, bar_y, TM["SCROLL_W"], bar_h, 1, TM["TXT3"])


# ---------------------------------------------------------------- 页面
def page_menu(games, sel, lang=0, total=None):
    cv = Canvas()
    if total is None:
        total = len(games)
    hint = ["A开始", "A: PLAY"][lang]
    ui_page_tabs(cv, 0, str(total), lang)
    ui_page_footer(cv, hint, NAV[lang])

    n = min(VISIBLE_ROWS, total)
    ui_group(cv, TM["GROUP_Y"], n, TM["ROW_H"])
    for i in range(n):
        y = TM["GROUP_Y"] + TM["ROW_INSET"] + i * TM["ROW_H"]
        sep = (i < n - 1) and not (i + 1 == sel)
        name = games[i] if i < len(games) else ""
        ui_row(cv, y, TM["ROW_H"], i == sel, name, None, True, sep)
    ui_scrollbar(cv, TM["GROUP_Y"], TM["ROW_H"] * VISIBLE_ROWS + TM["ROW_GAP"],
                 total, VISIBLE_ROWS, 0)
    return cv


def page_music(lang=0, count=None, sel=0, cur=0, playing="idle", pct=42):
    cv = Canvas()
    total = MUSIC_TOTAL if count is None else count

    # 空状态
    if total == 0:
        ui_page_tabs(cv, 1, None, lang)
        ey = 84
        ui_group(cv, ey, 1, 56)
        draw_center(cv, ey + 12, ["音乐播放", "MUSIC PLAYER"][lang], TM["TXT"], TM["CARD"])
        draw_center(cv, ey + 34, ["SD 根目录没有 .mp3", "NO .mp3 IN SD ROOT"][lang],
                    TM["TXT2"], TM["CARD"])
        ui_page_footer(cv, ["把 .mp3 放进 SD 根目录", "PUT .mp3 IN SD ROOT"][lang], NAV[lang])
        return cv

    ui_page_tabs(cv, 1, str(total), lang)

    # 播放中 / 暂停 / 空闲 三种状态行
    if playing == "play":
        hint = ["播放中 %d%%", "PLAYING %d%%"][lang] % pct
    elif playing == "pause":
        hint = ["已暂停 %d%%", "PAUSED %d%%"][lang] % pct
    else:
        hint = ["A播放 左右切歌", "A PLAY L/R SKIP"][lang]
    ui_page_footer(cv, hint, NAV[lang])

    # 5 行曲目列表
    top = max(0, sel - (VISIBLE_ROWS - 1))
    if top > total - VISIBLE_ROWS:
        top = max(0, total - VISIBLE_ROWS)
    ui_group(cv, TM["GROUP_Y"], VISIBLE_ROWS, TM["ROW_H"])
    for i in range(VISIBLE_ROWS):
        gi = top + i
        if gi >= total:
            break
        y = TM["GROUP_Y"] + TM["ROW_INSET"] + i * TM["ROW_H"]
        is_sel = (gi == sel)
        sep = (i < VISIBLE_ROWS - 1) and (gi < total - 1)
        ui_row(cv, y, TM["ROW_H"], is_sel, "", None, False, sep)

        # 文字自己画：选中=白，正在播放=强调色，其余=次要色
        if is_sel:
            fg = TM["TXT"]
        elif gi == cur:
            fg = TM["ACCENT"]
        else:
            fg = TM["TXT2"]
        bg = TM["CARD_SEL"] if is_sel else TM["CARD"]
        ty = y + (TM["ROW_H"] - CN_GLYPH_H) // 2
        max_px = W - 2 * TM["PAGE_PAD"] - TM["ROW_PAD"] - TM["ROW_END"]
        name = DEMO_TRACKS[gi] if gi < len(DEMO_TRACKS) else ("TRACK %02d" % (gi + 1))
        # 按像素宽度截断（和固件 draw_text_fit 一致）
        if text_width(name) > max_px:
            acc, w = "", 0
            for ch in name:
                cw = 10 if ch == " " else (7 if ord(ch) < 128 else 16)
                if w + cw + 14 > max_px:
                    break
                acc += ch
                w += cw
            name = acc + ".."
        cv.text(TM["PAGE_PAD"] + TM["ROW_PAD"], ty, name, fg, bg)

    ui_scrollbar(cv, TM["GROUP_Y"], TM["ROW_H"] * VISIBLE_ROWS + TM["ROW_GAP"],
                 total, VISIBLE_ROWS, top)
    return cv


def page_settings(sel=0, lang=0, vol=80):
    cv = Canvas()
    labels = ["语言", "音量"] if lang == 0 else ["LANGUAGE", "VOLUME"]
    hint = ["上下选 左右调", "L/R ADJUST"][lang]
    note = ["下次进入游戏时生效", "TAKES EFFECT NEXT LAUNCH"][lang]

    ui_page_tabs(cv, 2, None, lang)
    ui_group(cv, TM["GROUP_Y"], 2, TM["ROW_TALL"])
    ui_page_footer(cv, hint, NAV[lang])

    cv.text(TM["PAGE_PAD"] + TM["ROW_PAD"],
            TM["GROUP_Y"] + 2 * TM["ROW_TALL"] + TM["ROW_GAP"] + 12,
            note, TM["TXT2"], TM["BG"])

    y0 = TM["GROUP_Y"] + TM["ROW_INSET"]
    ui_row(cv, y0, TM["ROW_TALL"], sel == 0, labels[0],
           "中文" if lang == 0 else "English", False, sel != 1)

    y1 = y0 + TM["ROW_TALL"]
    ui_row(cv, y1, TM["ROW_TALL"], sel == 1, labels[1], "%d%%" % vol, False, False)

    # 音量 10 格离散条
    seg, seg_gap, bar_h = 8, 2, 8
    bar_x = TM["PAGE_PAD"] + TM["ROW_PAD"] + 44 + 8
    by = y1 + (TM["ROW_TALL"] - bar_h) // 2
    filled = (vol + 5) // 10
    for s in range(10):
        cv.round_rect(bar_x + s * (seg + seg_gap), by, seg, bar_h, 2,
                      TM["ACCENT"] if s < filled else TM["TXT3"])
    return cv


def page_lang(sel):
    cv = Canvas()
    ui_page(cv, ["选择语言", "SELECT LANGUAGE"][sel], None, True)
    ui_group(cv, 74, 2, TM["ROW_TALL"])
    for i, name in enumerate(["中文", "English"]):
        y = 74 + TM["ROW_INSET"] + i * TM["ROW_TALL"]
        sep = (i < 1) and not (i + 1 == sel)
        ui_row(cv, y, TM["ROW_TALL"], i == sel, name, None, False, sep)
    ui_page_footer(cv, ["上下选择  A确认", "UP/DN: MOVE   A: OK"][sel])
    return cv


def page_empty(lang=0):
    cv = Canvas()
    ui_page_tabs(cv, 0, None, lang)
    ey = 84
    ui_group(cv, ey, 1, 56)
    draw_center(cv, ey + 12, ["未找到游戏", "NO GAMES FOUND"][lang], TM["TXT"], TM["CARD"])
    draw_center(cv, ey + 34, ["把 .nes 放进 SD 根目录", "PUT .nes IN SD ROOT"][lang],
                TM["TXT2"], TM["CARD"])
    ui_page_footer(cv, "", NAV[lang])   # 没有游戏时按 A 什么也不发生
    return cv


# ---------------------------------------------------------------- main
TM = None
CN = None
ASCII = None

DEMO_GAMES = ["赤影战士", "魂斗罗", "超级玛丽", "坦克大战", "忍者龙剑传"]
# 音乐页演示用曲名（取自 SD 卡上的真实文件，够铺满一屏 + 触发滚动条）
DEMO_TRACKS = [
    "06 - 老街",
    "阿兰 - BALLAD 名もなき恋のうた",
    "陈柏宇 - 你瞒我瞒",
    "陈粒 - 小半",
    "陈奕迅 - 约定",
    "方大同 - 每个人都会",
    "花澤香菜 - 恋愛サーキュレーション",
    "过节",
]
MUSIC_TOTAL = 42
DEMO_GAMES_EN = ["CONTRA", "METROID", "MARIO BROS", "TETRIS", "BATTLE CITY"]

MODES = {
    # 第一行：三个顶层页面（中文）
    "menu":        lambda: page_menu(DEMO_GAMES, sel=0, total=391),
    "music":       lambda: page_music(sel=0, cur=2, playing="play", pct=42),
    "settings":    lambda: page_settings(sel=1, lang=0, vol=80),
    # 第二行：同样三个页面（英文）
    "menu-en":     lambda: page_menu(DEMO_GAMES_EN, sel=2, lang=1, total=391),
    "music-en":    lambda: page_music(lang=1, sel=2, cur=0, playing="idle"),
    "settings-en": lambda: page_settings(sel=0, lang=1, vol=80),
    # 第三行：模态页与空状态
    "lang-zh":     lambda: page_lang(0),
    "lang-en":     lambda: page_lang(1),
    "empty":       page_empty,
}

SHEET_ORDER = ["menu", "music", "settings",
               "menu-en", "music-en", "settings-en",
               "lang-zh", "lang-en", "empty"]


def main():
    global TM, CN, ASCII

    ap = argparse.ArgumentParser(description="按固件源码逐像素重放 UI 画面")
    ap.add_argument("--mode", default="all", choices=["all"] + list(MODES))
    ap.add_argument("--out-dir", default=DOCS)
    ap.add_argument("--scale", type=int, default=SCALE)
    args = ap.parse_args()

    TM = load_theme(os.path.join(SRC, "ui_theme.h"))
    CN = load_cn_font(os.path.join(SRC, "cn_font.h"))
    ASCII = load_ascii_font(os.path.join(SRC, "tft_driver.h"))
    print("字库: 汉字 %d 个, ASCII %d 个, token %d 项" % (len(CN), len(ASCII), len(TM)))

    os.makedirs(args.out_dir, exist_ok=True)
    names = list(MODES) if args.mode == "all" else [args.mode]

    for name in names:
        img = MODES[name]().to_image()
        img = img.resize((W * args.scale, H * args.scale), Image.NEAREST)
        path = os.path.join(args.out_dir, "ui-%s.png" % name)
        img.save(path)
        print("已生成:", path)

    # all 模式额外拼一张 3x3 对比图：三行 = 中文三页 / 英文三页 / 模态与空状态
    if args.mode == "all":
        cols, rows = 3, 3
        sheet = Image.new("RGB", (W * cols, H * rows), (18, 18, 18))
        for idx, name in enumerate(SHEET_ORDER):
            sheet.paste(MODES[name]().to_image(), ((idx % cols) * W, (idx // cols) * H))
        sheet = sheet.resize((W * cols * args.scale, H * rows * args.scale), Image.NEAREST)
        p = os.path.join(args.out_dir, "ui-sheet.png")
        sheet.save(p)
        print("已生成:", p)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    main()
