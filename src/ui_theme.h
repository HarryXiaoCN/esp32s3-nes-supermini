// UI 设计 token —— 所有页面的颜色、尺寸、间距的唯一来源。
//
// 【为什么要单独一个头文件】
//   改造前，游戏列表 / 设置页 / 语言选择页各自硬编码几何参数，
//   同一个「选中行」在三个页面画出了三种比例：
//     accent 侧条占行高：56% / 36% / 45%
//     文字垂直偏移：    y+8 / y+9 / y+14
//     行内左内边距：    16  / 18  / 20
//   统一到这套 token 之后，任何页面上的任何元素都能在同一个地方找到唯一数值。
//
// 【颜色】
//   取 iOS 深色模式的系统语义色（不是随手挑的黑灰蓝），RGB565 转换已逐一验算：
//     #1C1C1E -> 0x18E3   #2C2C2E -> 0x2965   #38383A -> 0x39C7
//     #8E8E93 -> 0x8C72   #0A84FF -> 0x0C3F   #636366 -> 0x630C
#ifndef UI_THEME_H
#define UI_THEME_H

#include <stdint.h>

// ---------------- 颜色 ----------------
// 命名沿用 iOS 的系统语义色，这样加新页面时能直接对应到 iOS 的哪个控件。
#define UI_BG        0x0000   // systemBackground                   #000000
#define UI_CARD      0x18E3   // secondarySystemGroupedBackground  #1C1C1E
#define UI_CARD_SEL  0x2965   // tertiarySystemGroupedBackground   #2C2C2E
#define UI_SEP       0x39C7   // separator                         #38383A
#define UI_TXT       0xFFFF   // label                             #FFFFFF
#define UI_TXT2      0x8C72   // secondaryLabel                    #8E8E93
#define UI_TXT3      0x630C   // tertiaryLabel                     #636366
#define UI_ACCENT    0x0C3F   // systemBlue                        #0A84FF
#define UI_DANGER    0xFA27   // systemRed                         #FF453A
#define UI_HOMEIND   0x39E7   // home indicator                    #3A3A3C

// ---------------- 页面骨架 ----------------
#define UI_PAGE_PAD   10      // 卡片左右外边距（卡片宽 = 屏宽 - 2*本值）
#define UI_TITLE_X    12      // 标题左对齐起点
#define UI_TITLE_Y    10      // 标题顶部（16px 字高 -> 10..26）
#define UI_TITLE_CTR  18      // 居中标题顶部（首屏模态页用）
#define UI_GROUP_Y    40      // 卡片顶部（标题底 26 + 14 留白）

// 顶部页签（相当于 iOS 的 tab bar，占用标题那一行以省下 240 屏的垂直空间）。
// 游戏 / 音乐 / 设置三个顶层页面，SELECT 循环向后切、B 循环向前切。
#define UI_TAB_GAP     16     // 相邻页签的间距
#define UI_TAB_BAR_H   2      // 当前页签下方的蓝色指示条高度
#define UI_TAB_BAR_DY  2      // 指示条距文字底部的间隙

// ---------------- 分组卡片 / 行 ----------------
#define UI_ROW_PAD    16      // 行内文字左内边距
#define UI_ROW_END    14      // 行内右侧 value / chevron 内边距
#define UI_CARD_R     10      // 卡片圆角
#define UI_ROW_R      8       // 选中行圆角
#define UI_ROW_INSET  2       // 行相对卡片的内缩
#define UI_ROW_H      32      // 紧凑行（游戏列表：一屏 5 行）
#define UI_ROW_TALL   40      // 标准行（设置页：iOS 的 44pt 在这个屏上要压一档）
#define UI_ROW_GAP    4       // 卡片上下额外留白（整卡高度 = 行高*行数 + 本值）

// 选中行的蓝色侧条（相当于 iOS 侧边栏的选中指示）。高度按行高比例算，
// 这样紧凑行和标准行的蓝条视觉重量一致 —— 这正是改造前最大的不一致点。
#define UI_BAR_W      4
#define UI_BAR_INSET  4       // 距卡片左边
#define UI_BAR_H(h)   ((h) * 9 / 16)

// ---------------- 其它 ----------------
#define UI_SCROLL_W   2       // 滚动条宽
#define UI_FOOTER_CLEAR 36    // 单行 footer 需要清掉的底部高度
#define UI_HOME_W     60      // Home 指示条宽
#define UI_HOME_H     3

#endif
