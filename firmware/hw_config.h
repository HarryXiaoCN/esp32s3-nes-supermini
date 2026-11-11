#ifndef HW_CONFIG_H
#define HW_CONFIG_H

// ============================================================================
// ESP32-S3 SuperMini 引脚分配（按实物丝印确认，2026-11）
//
// 实物边缘只得 15 个 GPIO（丝印：一侧 tx rx 1..7，另一侧 5v gnd 3v3 13..8），
// 而本工程需要 18 个，所以 3 个不常用的按键必须焊到底部裸露焊盘。
//
// 底部焊盘实测排布（贴片面朝上，USB 口朝上）：
//   左列：34 33 21 19 17 16 15 14
//   右列：38 37 36 35 45 42 41 40 39
//   中间：48 47 46
//
// 禁用清单：
//   19,20                USB D-/D+
//   26,27,28,29,30,31,32 ESP32-S3 片内 flash（本板未引出，仅作提醒）
//   33,34,35,36,37,38    PSRAM / 二次 flash 相关
//   45,46                strapping（flash 电压 / boot 模式）
//   47,48                48 接了板载 WS2812 RGB LED，47 谨慎
//   0,3                  strapping（boot 模式 / JTAG 选择）
// ============================================================================

// 1 = 打开声音（MAX98357A I2S 功放），0 = 静音编译
#define ENABLE_SOUND 1

// ---------------- ST7789 240x240（SPI，只写不读）----------------
// 注意：DC 用 13 而不是 15 —— 本板边缘没有引出 GPIO14/15
#define HW_TFT_SCK   12    // 丝印 12
#define HW_TFT_MOSI  11    // 丝印 11
#define HW_TFT_CS    10    // 丝印 10
#define HW_TFT_DC    13    // 丝印 13
#define HW_TFT_RST   9     // 丝印 9

// ---------------- SD 卡（SPI，与屏幕共享 SCK/MOSI，独立 CS）----------------
#define SD_SCK       12    // = HW_TFT_SCK，共享
#define SD_MOSI      11    // = HW_TFT_MOSI，共享
#define SD_MISO      8     // 丝印 8，只有 SD 卡读数据用
#define SD_CS        7     // 丝印 7

// SD 卡 SPI 时钟。非 UHS 卡上限 25MHz；个别卡/转接板不稳时降到 10000000
#define SD_SPI_FREQ  20000000

#if SD_SCK != HW_TFT_SCK
#error "SD_SCK 必须和 HW_TFT_SCK 相同（屏幕与 SD 卡共享 SPI 总线）"
#endif
#if SD_MOSI != HW_TFT_MOSI
#error "SD_MOSI 必须和 HW_TFT_MOSI 相同（屏幕与 SD 卡共享 SPI 总线）"
#endif

// ---------------- MAX98357A（I2S，单声道）----------------
#define I2S_BCK      6     // 丝印 6，接 BCLK
#define I2S_WS       5     // 丝印 5，接 LRC
#define I2S_DO       4     // 丝印 4，接 DIN

// ---------------- 板载 LED ----------------
// 丝印 "LED"，GP48。原理图（官方 wiki）确认：
//   GP48 → R1(1K) → 红色 LED1(0402) → GND
//   同一根线还接 LE1(WS2812) 的 DI（DO 悬空），所以是"红灯直驱 + RGB 数据脚"共用。
// 高电平点亮：HIGH 最亮 / 浮空 微亮 / LOW 灭。wiki 示例注释的 HIGH=off 是反的。
// 蓝灯不在这根线上 —— 它是 TP4054 的充电状态灯（VBUS→1K→蓝灯→CHRG），固件无解。
// 固件默认不驱动，设置页留了手动测试档（浮空 / 拉低 / 拉高）。
#define HW_LED_PIN   48

// ---------------- 按键（INPUT_PULLUP，另一端接 GND，按下为低）----------------
// 前 5 个在板子边缘，后 3 个需要焊到底部焊盘
#define BTN_UP       1     // 丝印 1
#define BTN_DOWN     2     // 丝印 2
#define BTN_LEFT     3     // 丝印 3
#define BTN_RIGHT    43    // 丝印 TX（UART0 TX，走 USB-CDC 时不占用）
#define BTN_A        44    // 丝印 RX（UART0 RX）
#define BTN_B        14    // 底部焊盘（左列最下）
#define BTN_START    16    // 底部焊盘（左列，15 上方）
#define BTN_SELECT   42    // 底部焊盘（右列，45 下方、41 上方）
                           // 注：17 和 39 两个焊盘都焊坏/扯坏了，改用 42。
                           // 它离 39 中间隔着 41、40 两格，受损坏影响最小。

// ---------------- 编译期护栏 ----------------
#if (BTN_UP >= 26 && BTN_UP <= 32) || (BTN_DOWN >= 26 && BTN_DOWN <= 32) || \
    (BTN_LEFT >= 26 && BTN_LEFT <= 32) || (BTN_RIGHT >= 26 && BTN_RIGHT <= 32) || \
    (BTN_A >= 26 && BTN_A <= 32) || (BTN_B >= 26 && BTN_B <= 32) || \
    (BTN_START >= 26 && BTN_START <= 32) || (BTN_SELECT >= 26 && BTN_SELECT <= 32)
#error "按键不能接 GPIO26~32：ESP32-S3 片内 flash 引脚"
#endif
#if (BTN_UP >= 33 && BTN_UP <= 38) || (BTN_DOWN >= 33 && BTN_DOWN <= 38) || \
    (BTN_LEFT >= 33 && BTN_LEFT <= 38) || (BTN_RIGHT >= 33 && BTN_RIGHT <= 38) || \
    (BTN_A >= 33 && BTN_A <= 38) || (BTN_B >= 33 && BTN_B <= 38) || \
    (BTN_START >= 33 && BTN_START <= 38) || (BTN_SELECT >= 33 && BTN_SELECT <= 38)
#error "按键不能接 GPIO33~38：PSRAM / 二次 flash 相关"
#endif
#if (BTN_UP == 19 || BTN_UP == 20 || BTN_DOWN == 19 || BTN_DOWN == 20 || \
     BTN_LEFT == 19 || BTN_LEFT == 20 || BTN_RIGHT == 19 || BTN_RIGHT == 20 || \
     BTN_A == 19 || BTN_A == 20 || BTN_B == 19 || BTN_B == 20 || \
     BTN_START == 19 || BTN_START == 20 || BTN_SELECT == 19 || BTN_SELECT == 20)
#error "按键不能接 GPIO19/20：USB D-/D+"
#endif
#if (BTN_UP == 45 || BTN_UP == 46 || BTN_DOWN == 45 || BTN_DOWN == 46 || \
     BTN_LEFT == 45 || BTN_LEFT == 46 || BTN_RIGHT == 45 || BTN_RIGHT == 46 || \
     BTN_A == 45 || BTN_A == 46 || BTN_B == 45 || BTN_B == 46 || \
     BTN_START == 45 || BTN_START == 46 || BTN_SELECT == 45 || BTN_SELECT == 46)
#error "按键不能接 GPIO45/46：strapping 引脚，会影响启动"
#endif

#endif
