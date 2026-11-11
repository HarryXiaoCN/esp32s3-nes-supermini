#include <Arduino.h>
#include <SPI.h>
#include <Preferences.h>
#include <FS.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include "hw_config.h"
#include "tft_driver.h"
#include "turning.h"
#include "cn_font.h"
#include "ui_theme.h"
#include "mp3_player.h"

#if ENABLE_SOUND
// ESP32 Arduino core 3.x 的新 I2S 驱动（封装 IDF 的 i2s_std）。
// 旧的 <driver/i2s.h>（i2s_driver_install / i2s_write）在 core 3.x 里虽然还能编译，
// 但已经 deprecated，而且这里要按单声道配置，新 API 更直接。
#include <ESP_I2S.h>
#endif

#include <string.h>
#include <stdarg.h>
#include <math.h>

extern "C" {
#include "noftypes.h"
#include "bitmap.h"
#include "osd.h"
#include "nofrendo.h"
#include "nesinput.h"
#include "event.h"
#include "nofconfig.h"
}

#define NES_SCREEN_WIDTH 256
#define NES_SCREEN_HEIGHT 240

// DISPLAY_WIDTH / DISPLAY_HEIGHT 由 tft_driver.h 提供（240x240）。
// 240 宽的面板放不下 NES 的 256 列画面，因此左右各裁 8 列（NES 的 overscan 边缘，通常本来就是黑边）。
#define NES_X_CROP        ((NES_SCREEN_WIDTH - DISPLAY_WIDTH) / 2)
#define NES_VISIBLE_WIDTH (NES_SCREEN_WIDTH - 2 * NES_X_CROP)
 
// 音频采样率：.ino 启动参数里传的 -sample 会覆盖这个默认值；
// 必须在 osd_init_sound()（真正配置 I2S）之前就确定下来。
#define AUDIO_SAMPLE_RATE_DEFAULT 22050
#define AUDIO_VOLUME_DEFAULT 100
#define AUDIO_GAIN 100  
#define TARGET_FRAME_MICROS 16666   
 
static int audio_sample_rate = AUDIO_SAMPLE_RATE_DEFAULT;

int master_volume = AUDIO_VOLUME_DEFAULT;
bool show_fps = false;
bool select_pressed = false;
static bool runtime_sound_enabled = false;

extern TFTDriver tft;
extern "C" int nes_get_gamepad_state();

static uint16_t scale_color_565(uint16_t color, uint8_t alpha) {
  uint8_t r = (color >> 11) & 0x1F;
  uint8_t g = (color >> 5) & 0x3F;
  uint8_t b = color & 0x1F;

  r = (r * alpha) / 255;
  g = (g * alpha) / 255;
  b = (b * alpha) / 255;

  return (uint16_t)((r << 11) | (g << 5) | b);
}

static uint16_t swap_rb_565(uint16_t color) {
  uint16_t r = (color >> 11) & 0x1F;
  uint16_t g = (color >> 5) & 0x3F;
  uint16_t b = color & 0x1F;
  return (uint16_t)((b << 11) | (g << 5) | r);
}

// ---------------- 中文显示支持 ----------------
// 字库由 tools/gen_cn_font.py 从系统字体生成（见 cn_font.h），24x24 点阵。
// ASCII 仍然用 tft_driver.h 里的 5x7 字库，垂直居中到大字行里，这样中英可以混排。

static uint16_t utf8_next(const char **sp) {
  const unsigned char *p = (const unsigned char *)*sp;
  uint16_t c = *p;
  if (c == 0) {
    return 0;
  }
  if (c < 0x80) {
    *sp += 1;
    return c;
  }
  if ((c & 0xE0) == 0xC0) {
    c = (uint16_t)(((c & 0x1F) << 6) | (p[1] & 0x3F));
    *sp += 2;
    return c;
  }
  if ((c & 0xF0) == 0xE0) {
    c = (uint16_t)(((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F));
    *sp += 3;
    return c;
  }
  *sp += 1;
  return 0xFFFD;
}

static const cn_glyph_t *find_cn_glyph(uint16_t code) {
  // cn_glyphs 按 Unicode 码点升序生成，用二分查找；
  // 3849 个字形线性扫描会让菜单绘制明显变慢。
  int lo = 0;
  int hi = CN_GLYPH_COUNT - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    uint16_t c = cn_glyphs[mid].code;
    if (c == code) {
      return &cn_glyphs[mid];
    }
    if (c < code) {
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return NULL;
}

// 宽度计算（中文 CN_GLYPH_W 像素，ASCII 7 像素，空格 10 像素）
static int cn_text_width(const char *utf8) {
  int w = 0;
  const char *s = utf8;
  while (*s) {
    uint16_t c = utf8_next(&s);
    if (c == 0) break;
    if (c < 0x80) w += (c == ' ') ? 10 : 7;
    else w += CN_GLYPH_W;
  }
  return w;
}

// 中英混排绘制，返回整行宽度（布局用，可能大于实际画出宽度）。
//
// 【为什么要整行拼条带再推屏】
//   改造前：ASCII 走 tft.drawChar，而 drawChar 内部是逐像素 drawFilledRect，
//           5x7 字模 = 35 次完整的 SPI 窗口事务/字符；
//           汉字走 draw_cn_glyph，每个字 16 次 pushImage。
//           菜单里一行 8 个汉字 = 128 次 SPI 事务，5 行 = 640 次 ——
//           这正是切页/换选项必须 delay() 遮丑的根本原因。
//   第一轮：整行先渲染进一条 16px 高的内部 RAM 条带，再逐行 pushImage，
//           无论这行有多少字，固定只要 16 次事务（实测约 8 倍提速）。
//   第二轮：16 行一次拼完、一次推屏。原来 16 次 CASET/RASET/RAMWR 事务
//           压成 1 次，数据仍然是 w×16×2 字节（3840 像素以内，一次发完）。
//   ASCII 仍然用 5x7 小字模（7px 宽），这样一行能塞下更长的英文提示；
//   字模在 16px 行高里垂直居中，和汉字基线对齐。
static int draw_cn_text(int x, int y, const char *utf8, uint16_t fg, uint16_t bg) {
  int total_w = cn_text_width(utf8);
  if (total_w <= 0) {
    return 0;
  }

  // 绝不写到屏外：横向按可用宽度裁掉，纵向放不下就整行不画。
  if (x < 0 || x >= DISPLAY_WIDTH) return total_w;
  if (y < 0 || y + CN_GLYPH_H > DISPLAY_HEIGHT) return total_w;

  int w = total_w;
  if (x + w > DISPLAY_WIDTH) {
    w = DISPLAY_WIDTH - x;
  }
  if (w <= 0) return total_w;

  // 紧凑布局：第 row 行从 band[row * w * 2] 开始。直接按 SPI 字节序（高字节在前）
  // 写，推屏时 pushImageBytes() 零转换直发。16*240*2 = 7680 字节内部 RAM。
  static uint8_t band[CN_GLYPH_H * DISPLAY_WIDTH * 2];
  const int ascii_top = (CN_GLYPH_H - 7) / 2;
  const uint8_t fg_hi = (uint8_t)(fg >> 8), fg_lo = (uint8_t)(fg & 0xFF);
  const uint8_t bg_hi = (uint8_t)(bg >> 8), bg_lo = (uint8_t)(bg & 0xFF);

  for (int row = 0; row < CN_GLYPH_H; row++) {
    uint8_t *line = &band[row * w * 2];
    int cx = 0;
    const char *s = utf8;

    while (*s && cx < w) {
      uint16_t code = utf8_next(&s);
      if (code == 0) break;

      if (code < 0x80) {
        // 每个 ASCII 占 7px 格宽（空格 10px），但 5x7 字模只占左边 5px。
        // 整格都要填，否则字符右侧那 2px 会残留上一帧的内容。
        int cw = (code == ' ') ? 10 : 7;
        const uint8_t *glyph = (code == ' ') ? NULL : &font_5x7[(code - 0x20) * 5];
        int gy = row - ascii_top;
        bool visible = (glyph != NULL) && (gy >= 0 && gy < 7);
        for (int c = 0; c < cw; c++) {
          int px = cx + c;
          if (px >= w) break;
          bool on = visible && (c < 5) && (pgm_read_byte(&glyph[c]) & (1 << gy));
          line[px * 2]     = on ? fg_hi : bg_hi;
          line[px * 2 + 1] = on ? fg_lo : bg_lo;
        }
        cx += cw;
      } else {
        const cn_glyph_t *g = find_cn_glyph(code);
        uint32_t v = 0;
        if (g) {
          for (int k = 0; k < CN_GLYPH_BYTES_PER_ROW; k++) {
            v = (v << 8) | g->bits[row * CN_GLYPH_BYTES_PER_ROW + k];
          }
        }
        for (int c = 0; c < CN_GLYPH_W; c++) {
          int px = cx + c;
          if (px >= w) break;
          // 字库缺字时画实心方块占位，方便一眼发现漏字
          bool on = g ? ((v & (1u << (CN_GLYPH_W - 1 - c))) != 0)
                      : (row >= 2 && row < CN_GLYPH_H - 2 && c >= 2 && c < CN_GLYPH_W - 2);
          line[px * 2]     = on ? fg_hi : bg_hi;
          line[px * 2 + 1] = on ? fg_lo : bg_lo;
        }
        cx += CN_GLYPH_W;
      }
    }

    for (int i = cx; i < w; i++) {          // 行尾空出来的部分补背景
      line[i * 2]     = bg_hi;
      line[i * 2 + 1] = bg_lo;
    }
  }

  tft.pushImageBytes(x, y, w, CN_GLYPH_H, band);

  return total_w;
}

// 水平居中绘制
static void draw_cn_center(int y, const char *utf8, uint16_t fg, uint16_t bg) {
  int w = cn_text_width(utf8);
  draw_cn_text((DISPLAY_WIDTH - w) / 2, y, utf8, fg, bg);
}

// 按像素宽度截断 UTF-8 文本，超出时末尾补 ".."。
//
// 【为什么必须有】draw_cn_text 只画到"当前这段文字有多宽"。列表里长名字换成
// 短名字时，旧文字会留在右边；而列表重绘只覆盖卡片范围（10..230），
// 溢出到 231..239 那几列永远清不掉 —— 表现就是"短名字右边还挂着上一首的尾巴"。
// 所以凡是变长内容（曲名、ROM 名）都必须先按卡片可用宽度截断。
static void draw_text_fit(int x, int y, const char *utf8, int max_px,
                          uint16_t fg, uint16_t bg) {
  if (max_px <= 0) return;
  if (cn_text_width(utf8) <= max_px) {
    draw_cn_text(x, y, utf8, fg, bg);
    return;
  }

  static char buf[128];
  const int dot_w = 7;              // ".." 两个点的宽度
  int w = 0;
  size_t n = 0;
  const char *s = utf8;

  while (*s) {
    const char *prev = s;
    uint16_t code = utf8_next(&s);
    if (code == 0) break;
    int cw = (code < 0x80) ? ((code == ' ') ? 10 : 7) : CN_GLYPH_W;
    if (w + cw + dot_w * 2 > max_px) break;
    size_t len = (size_t)(s - prev);
    if (n + len >= sizeof(buf) - 3) break;
    memcpy(buf + n, prev, len);
    n += len;
    w += cw;
  }

  buf[n++] = '.';
  buf[n++] = '.';
  buf[n] = '\0';
  draw_cn_text(x, y, buf, fg, bg);
}

// 按键位掩码（nes_get_gamepad_state 的返回值）
#define HW_MASK_A 0x01
#define HW_MASK_B 0x02
#define HW_MASK_SELECT 0x04
#define HW_MASK_START 0x08
#define HW_MASK_UP 0x10
#define HW_MASK_DOWN 0x20
#define HW_MASK_LEFT 0x40
#define HW_MASK_RIGHT 0x80

// 界面配色与几何尺寸定义在 ui_theme.h（单一来源）。
// 改造前这里有 8 个颜色宏，但每个页面又各自硬编码行高/内边距/蓝条比例，
// 导致同一个「选中行」在三个页面画成三种样子。现在颜色和尺寸都在 token 里。

// ---------------- 多语言 ----------------
// 中英文各自用最合适的字库：汉字走 16x16 点阵，英文走 5x7 小字库（7px 宽），
// 这样英文能在一行里塞下更长的提示，不用为了对齐牺牲内容。
typedef enum { LANG_ZH = 0, LANG_EN = 1, LANG_COUNT = 2 } ui_lang_t;

static ui_lang_t ui_lang = LANG_ZH;

// ---------------- 顶层页面 ----------------
// 三个并列的顶层页面，不是"从列表进设置"的子页面关系。
// SELECT 循环向后切、B 循环向前切，页签（顶部那一行）本身就承担导航提示，
// 所以页脚不用再写"SELECT 切换页面"。以后加页面只要在 PAGE_COUNT 前插一项。
typedef enum { PAGE_GAMES = 0, PAGE_MUSIC, PAGE_SETTINGS, PAGE_COUNT } ui_page_t;

static ui_page_t g_page = PAGE_GAMES;

static const char *TXT_TABS[PAGE_COUNT][LANG_COUNT] = {
    { "游戏", "GAMES" },
    { "音乐", "MUSIC" },
    { "设置", "SETUP" },
};

static const char *TXT_HINT[LANG_COUNT]      = { "A开始",          "A: PLAY" };
// 页脚右侧的全局导航提示：每个顶层页面都一样（首次开机的模态页没有）
static const char *TXT_NAV_SELECT[LANG_COUNT] = { "SELECT 切页",   "SELECT: PAGE" };
static const char *TXT_NO_GAMES[LANG_COUNT]  = { "未找到游戏",     "NO GAMES FOUND" };
static const char *TXT_NO_GAMES_TIP[LANG_COUNT] = { "把 .nes 放进 SD 根目录", "PUT .nes IN SD ROOT" };
static const char *TXT_BACK[LANG_COUNT]      = { "返回菜单",       "BACK TO MENU" };
static const char *TXT_GAME[LANG_COUNT]      = { "游戏",           "GAME" };
static const char *TXT_LANG_TITLE[LANG_COUNT] = { "选择语言",      "SELECT LANGUAGE" };
static const char *TXT_LANG_HINT[LANG_COUNT] = { "上下选择  A确认", "UP/DN: MOVE   A: OK" };
// 音乐页
static const char *TXT_MUSIC_TITLE[LANG_COUNT]   = { "音乐播放",      "MUSIC PLAYER" };
static const char *TXT_MUSIC_NONE[LANG_COUNT]    = { "SD 根目录没有 .mp3", "NO .mp3 IN SD ROOT" };
static const char *TXT_MUSIC_EMPTY_HINT[LANG_COUNT] = { "把 .mp3 放进 SD 根目录", "PUT .mp3 IN SD ROOT" };
static const char *TXT_MUSIC_HINT[LANG_COUNT]    = { "A播放 左右切歌", "A PLAY L/R SKIP" };
static const char *TXT_MUSIC_PLAYING[LANG_COUNT] = { "播放中 %d%%",   "PLAYING %d%%" };
static const char *TXT_MUSIC_PAUSED[LANG_COUNT]  = { "已暂停 %d%%",   "PAUSED %d%%" };
// 设置页
static const char *TXT_LANG_ITEM[LANG_COUNT] = { "语言",           "LANGUAGE" };
static const char *TXT_VOL_ITEM[LANG_COUNT]  = { "音量",           "VOLUME" };
static const char *TXT_TONE_ITEM[LANG_COUNT] = { "音色",           "TONE" };
// 音色三档，顺序必须和 mp3_player.h 的 MP3_TONE_FLAT/DEFAULT/SOFT 一致
static const char *TXT_TONE_VAL[LANG_COUNT][MP3_TONE_COUNT] = {
  { "原声", "默认", "柔和" },
  { "FLAT", "DEFAULT", "SOFT" },
};
static const char *TXT_LED_ITEM[LANG_COUNT] = { "LED 脚",         "LED PIN" };
static const char *TXT_LED_VAL[LANG_COUNT][3] = {
  { "浮空", "拉低", "拉高" },
  { "HI-Z", "LOW",  "HIGH" },
};
// 屏幕 SPI 时钟档位。数值本身是 MHz，两种语言都看得懂，只翻标签。
// 80M 是"快档"：整屏 23ms → 11.5ms，菜单翻页几乎无感；花屏就切回 40M。
static const char *TXT_SCR_ITEM[LANG_COUNT] = { "屏幕",           "LCD SPI" };
static const char *TXT_SCR_VAL[2] = { "40M", "80M" };
static const char *TXT_HINT_CFG[LANG_COUNT]  = { "上下选 左右调", "L/R ADJUST" };

// NVS 里存的键名与命名空间
#define PREFS_NAMESPACE "nes"
#define PREFS_KEY_LANG  "lang"
#define PREFS_KEY_VOL   "vol"
#define PREFS_KEY_TONE  "tone"
#define PREFS_KEY_LED   "led"
#define PREFS_KEY_SPI   "spi"

// 读取已保存的语言。返回 -1 表示还没选过（首次开机）。
static int load_lang_from_nvs() {
  Preferences prefs;
  if (!prefs.begin(PREFS_NAMESPACE, true)) {
    return -1;
  }
  int v = prefs.getInt(PREFS_KEY_LANG, -1);
  prefs.end();
  if (v < 0 || v >= LANG_COUNT) {
    return -1;
  }
  return v;
}

static void save_lang_to_nvs(ui_lang_t l) {
  Preferences prefs;
  if (!prefs.begin(PREFS_NAMESPACE, false)) {
    return;
  }
  prefs.putInt(PREFS_KEY_LANG, (int)l);
  prefs.end();
}

// 音量持久化（设置页里调，进游戏也用它）。返回 -1 表示还没保存过。
static int load_volume_from_nvs() {
  Preferences prefs;
  if (!prefs.begin(PREFS_NAMESPACE, true)) {
    return -1;
  }
  int v = prefs.getInt(PREFS_KEY_VOL, -1);
  prefs.end();
  if (v < 0 || v > 100) return -1;
  return v;
}

static void save_volume_to_nvs(int v) {
  Preferences prefs;
  if (!prefs.begin(PREFS_NAMESPACE, false)) {
    return;
  }
  prefs.putInt(PREFS_KEY_VOL, v);
  prefs.end();
}

// 音色档位持久化。返回 -1 表示还没保存过（用 mp3_player 里的默认值）。
static int load_tone_from_nvs() {
  Preferences prefs;
  if (!prefs.begin(PREFS_NAMESPACE, true)) {
    return -1;
  }
  int v = prefs.getInt(PREFS_KEY_TONE, -1);
  prefs.end();
  if (v < 0 || v >= MP3_TONE_COUNT) return -1;
  return v;
}

static void save_tone_to_nvs(int v) {
  Preferences prefs;
  if (!prefs.begin(PREFS_NAMESPACE, false)) {
    return;
  }
  prefs.putInt(PREFS_KEY_TONE, v);
  prefs.end();
}

// 板载 LED 手动测试档：0 浮空（默认，不驱动）/ 1 拉低 / 2 拉高。
//
// 官方 wiki 写的是"板载 RGB 和蓝色 LED 共用 GPIO48"，示例注释里 HIGH=灭、LOW=亮
// （低电平点亮）。但实测把 48 拉低时那颗红灯灭了、充电指示的蓝灯照闪，和 wiki
// 对不上——所以留这一档在设置页里，你可以改一下看哪颗灯真的跟着变，
// 顺便确认闪烁的蓝灯到底归不归它管。
int menu_led = 0;

static void led_pin_apply(int mode) {
  if (mode < 0 || mode > 2) mode = 0;
  menu_led = mode;
  if (mode == 0) {
    pinMode(HW_LED_PIN, INPUT);                  // 高阻 = 恢复上电常态，不驱动
  } else {
    pinMode(HW_LED_PIN, OUTPUT);
    digitalWrite(HW_LED_PIN, mode == 1 ? LOW : HIGH);
  }
  Serial.printf("[CFG] LED pin %d -> %s\n", HW_LED_PIN,
                mode == 0 ? "HI-Z" : (mode == 1 ? "LOW" : "HIGH"));
}

static int load_led_from_nvs() {
  Preferences prefs;
  if (!prefs.begin(PREFS_NAMESPACE, true)) {
    return -1;
  }
  int v = prefs.getInt(PREFS_KEY_LED, -1);
  prefs.end();
  if (v < 0 || v > 2) return -1;
  return v;
}

static void save_led_to_nvs(int v) {
  Preferences prefs;
  if (!prefs.begin(PREFS_NAMESPACE, false)) {
    return;
  }
  prefs.putInt(PREFS_KEY_LED, v);
  prefs.end();
}

// 屏幕 SPI 时钟档位：0 = 40MHz（保守）/ 1 = 80MHz（默认，快）。
//
// 【为什么做成设置项而不是写死】80MHz 在焊接短线上通常没问题，但屏幕和 SD 卡
// 共享 SCK/MOSI，能不能稳跑取决于实际走线、卡座和屏排线的寄生参数。给用户一个
// 运行时开关，比在固件里猜安全：40MHz 一定稳，80MHz 快一倍（整屏 23ms → 11.5ms）。
// 80MHz 是 ESP32-S3 SPI 主机的硬件上限（APB 80MHz），请求更高频率驱动会自动
// 钳到 80MHz，不会更快，所以设置页只给这两档。
//
// 【防花屏兜底】开机时按住 B 会强制回 40MHz 并写回 NVS —— 80MHz 万一花屏到
// 看不清菜单，这是不看屏幕也能恢复的唯一办法。
int menu_spi = 1;                  // 默认 80MHz；NVS 里存过就以 NVS 为准

static int load_spi_from_nvs() {
  Preferences prefs;
  if (!prefs.begin(PREFS_NAMESPACE, true)) {
    return -1;
  }
  int v = prefs.getInt(PREFS_KEY_SPI, -1);
  prefs.end();
  if (v < 0 || v > 1) return -1;
  return v;
}

static void save_spi_to_nvs(int v) {
  Preferences prefs;
  if (!prefs.begin(PREFS_NAMESPACE, false)) {
    return;
  }
  prefs.putInt(PREFS_KEY_SPI, v);
  prefs.end();
}

static void spi_freq_apply(int mode) {
  menu_spi = (mode == 1) ? 1 : 0;
  // 下一次 SPI 事务就按新时钟走，不需要重新初始化面板
  tft.set_spi_freq(menu_spi ? 80000000u : 40000000u);
  Serial.printf("[CFG] LCD SPI -> %u MHz\n", menu_spi ? 80u : 40u);
}

// 圆角矩形填充。半径很小时用圆弧逐行计算即可，菜单绘制频率低，不影响帧率。
static void fill_round_rect(int x, int y, int w, int h, int r, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;
  if (r <= 0) {
    tft.drawFilledRect(x, y, w, h, color);
    return;
  }

  tft.drawFilledRect(x + r, y, w - 2 * r, h, color);              // 中间竖条
  tft.drawFilledRect(x, y + r, r, h - 2 * r, color);              // 左条
  tft.drawFilledRect(x + w - r, y + r, r, h - 2 * r, color);      // 右条

  // 四角圆弧：左右两侧都要画，之前漏了右侧导致右边是直角的
  for (int i = 0; i < r; i++) {
    int dy = r - i;
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= r * r) dx++;
    int cw = dx * 2;
    tft.drawFilledRect(x + r - dx, y + i, cw, 1, color);              // 左上
    tft.drawFilledRect(x + w - r - dx, y + i, cw, 1, color);          // 右上
    tft.drawFilledRect(x + r - dx, y + h - 1 - i, cw, 1, color);      // 左下
    tft.drawFilledRect(x + w - r - dx, y + h - 1 - i, cw, 1, color);  // 右下
  }
}

// 右侧的 ">" chevron 指示符。
//
// 原来每个点一次 1×1 drawFilledRect —— 一个 chevron 10 个像素就是 10 次完整
// SPI 窗口事务，游戏列表 5 行 = 50 次事务只为 50 个像素。现在拼成 5×9 小图
// 一次推完；bg 必须一起传，因为推图会覆盖整个矩形（不能留透明像素）。
static void draw_chevron(int x, int y, uint16_t color, uint16_t bg) {
  static uint16_t buf[9 * 5];
  for (int r = 0; r < 9; r++) {
    for (int c = 0; c < 5; c++) {
      bool on = (r < 5) ? (c == r) : (c == 8 - r);
      buf[r * 5 + c] = on ? color : bg;
    }
  }
  tft.pushImage(x, y, 5, 9, buf);
}

// ============================================================================
// UI 组件层
//
// 改造前，游戏列表 / 设置页 / 语言选择页各自手写绘制，同一个「选中行」有四个
// 参数互不相同（行高 32/44/44、蓝条 18/16/20、文字偏移 y+8/y+9/y+14、
// 行内左侧 16/18/20），所以三个页面看起来像三个人做的。
// 现在所有页面只通过下面这几个函数绘制，几何全部来自 ui_theme.h。
// ============================================================================

// 页面标题。
//   centered=false：iOS Settings 的 large title 风格，左对齐；trailing 非空时
//                   在右上角画一个次要色的小字（游戏列表用来显示总数）。
//   centered=true ：首次开机这种模态页用，标题居中、留白更大。
static void ui_page(const char *title, const char *trailing, bool centered) {
  tft.fillScreen(UI_BG);

  if (centered) {
    draw_cn_center(UI_TITLE_CTR, title, UI_TXT, UI_BG);
  } else {
    draw_cn_text(UI_TITLE_X, UI_TITLE_Y, title, UI_TXT, UI_BG);
  }

  if (trailing) {
    int tw = cn_text_width(trailing);
    draw_cn_text(DISPLAY_WIDTH - UI_TITLE_X - tw, UI_TITLE_Y + 2, trailing, UI_TXT2, UI_BG);
  }
}

// 切页时清掉"卡片区域以外"的背景。
//
// 【为什么不整屏 fillScreen】整屏 115200 字节 / 40MHz = 23ms，其中卡片矩形
// （y=40..card_bottom、宽 220）随后一定会被 ui_group() 完整覆盖 —— 那 72KB
// 属于白刷。改成只刷「页签那一行 + 卡片下面那一条」，数据量降到 40KB。
//
// 三个满页的卡片都从 x=UI_PAGE_PAD 开始、宽度相同，卡片左右两侧（x<10、x>229）
// 在旧页面上也没有任何内容，所以左右两列不用清。
static void ui_clear_around_cards(int card_bottom) {
  tft.drawFilledRect(0, 0, DISPLAY_WIDTH, UI_GROUP_Y, UI_BG);
  if (card_bottom < DISPLAY_HEIGHT) {
    tft.drawFilledRect(0, card_bottom, DISPLAY_WIDTH, DISPLAY_HEIGHT - card_bottom, UI_BG);
  }
}

// 顶层页签：游戏 / 音乐 / 设置。
//
// 【为什么放顶部而不是 iOS 标准的底部 Tab Bar】
//   240 高的屏，底部 Tab Bar 至少要吃掉 40px —— 游戏列表会从 5 行掉到 4 行，
//   391 个游戏的翻页成本直接上去。这一行本来只放一个标题，拿来当页签等于白赚。
//
// 选中页签：主文字色 + 下方 2px 系统蓝指示条；未选中：次要文字色。
//
// card_bottom：本页分组卡片的底边 y。<0 表示"整屏清"——空状态页的卡片位置
// （ey=84）和三个满页不一样，靠局部清屏会留下旧内容的残影，那种页必须全清。
static void ui_page_tabs(int active, const char *trailing, int card_bottom = -1) {
  if (card_bottom < 0) {
    tft.fillScreen(UI_BG);
  } else {
    ui_clear_around_cards(card_bottom);
  }

  int x = UI_TITLE_X;
  for (int i = 0; i < PAGE_COUNT; i++) {
    const char *label = TXT_TABS[i][ui_lang];
    int w = cn_text_width(label);
    bool on = (i == active);

    draw_cn_text(x, UI_TITLE_Y, label, on ? UI_TXT : UI_TXT2, UI_BG);
    if (on) {
      tft.drawFilledRect(x, UI_TITLE_Y + CN_GLYPH_H + UI_TAB_BAR_DY,
                         w, UI_TAB_BAR_H, UI_ACCENT);
    }
    x += w + UI_TAB_GAP;
  }

  if (trailing) {
    int tw = cn_text_width(trailing);
    draw_cn_text(DISPLAY_WIDTH - UI_TITLE_X - tw, UI_TITLE_Y + 2, trailing, UI_TXT2, UI_BG);
  }
}

// SELECT 向后切页、B 向前切页。
// 返回 true 表示这一帧已经把页面切走了，调用方要立刻 return 回主调度器 ——
// 不要在同一帧里继续处理旧页面的按键，否则会连吃两个事件。
static bool ui_page_switch(uint8_t hw) {
  if (hw & HW_MASK_SELECT) {
    g_page = (ui_page_t)((g_page + 1) % PAGE_COUNT);
    return true;
  }
  if (hw & HW_MASK_B) {
    g_page = (ui_page_t)((g_page + PAGE_COUNT - 1) % PAGE_COUNT);
    return true;
  }
  return false;
}

// 页面底部：提示文字 + Home 指示条。
// hint2 为 NULL 时只画一行提示。先按行数清掉底部区域，这样语言切换后
// 旧提示不会和新的叠在一起（改造前只有设置页做了这件事）。
// 页面底部：左侧"本页操作"、右侧"全局导航"，最下方 Home 指示条。
//
//   A开始                              SELECT 切页
//   ─────────────────────────────────────────────
//                     ▬▬▬▬
//
// 为什么分成两段而不是合成一句居中：三页导航上线后，左边是本页专属操作
// （A 开始 / 播放暂停 / 左右调节），右边是每一页都一样的切页键。合成一句会
// 长得放不下（英文尤其明显），分开摆也更符合"局部操作 / 全局导航"的层级。
//
// nav 传 NULL 表示这个页面没有切页语义（首次开机的语言选择是模态页）。
static void ui_page_footer(const char *hint, const char *nav) {
  int clear_y = DISPLAY_HEIGHT - UI_FOOTER_CLEAR;
  tft.drawFilledRect(0, clear_y, DISPLAY_WIDTH, DISPLAY_HEIGHT - clear_y, UI_BG);

  const int y = DISPLAY_HEIGHT - 30;

  if (nav) {
    // 右侧：全局导航键，用更暗的第三级文字色，避免和左侧操作抢注意力
    int nw = cn_text_width(nav);
    draw_cn_text(DISPLAY_WIDTH - UI_PAGE_PAD - 4 - nw, y, nav, UI_TXT3, UI_BG);
  }
  if (hint && hint[0]) {
    // 左侧：本页操作
    draw_cn_text(UI_PAGE_PAD + 4, y, hint, UI_TXT2, UI_BG);
  }

  fill_round_rect((DISPLAY_WIDTH - UI_HOME_W) / 2, DISPLAY_HEIGHT - 8,
                  UI_HOME_W, UI_HOME_H, 2, UI_HOMEIND);
}

// 分组卡片底。整卡画一次圆角矩形，行内容之后画在上面。
// 改造前设置页没有整卡底（每行自己画一块），这是它和游戏列表最大的结构差异。
static void ui_group(int y, int rows, int row_h) {
  fill_round_rect(UI_PAGE_PAD, y, DISPLAY_WIDTH - 2 * UI_PAGE_PAD,
                  row_h * rows + UI_ROW_GAP, UI_CARD_R, UI_CARD);
}

// 一行。覆盖范围固定为「整行」，所以重复调用不会留下上次的残影。
//   sel     ：圆角高亮 + 左侧蓝色侧条；否则用卡片底色直角色覆盖
//   value   ：右侧对齐的次要文字（可为 NULL）
//   chevron ：右侧 ">" 指示（可为 NULL 行为）
//   sep     ：本行下方是否画分隔线 —— 由调用方判断，避免画到下一行的圆角上
//   bg_covered：调用方保证「这行背后已经是卡片底色」—— 首次绘制刚填过整卡，
//               或者翻页时卡片底根本没动。此时非选中行只清文字会落到的
//               216×16 区域，省掉整行 216×32 = 13.8KB 的重复写入。
//               选中行仍然整行画高亮（高亮本来就要盖住整行）。
//               注意：调用方必须保证「这行上一次也不是选中」—— 旧的高亮
//               圆角有 8px 高，不在 16px 文字带里，只能靠整行覆盖恢复。
static void ui_row(int y, int h, bool sel, const char *label,
                   const char *value, bool chevron, bool sep,
                   bool bg_covered = false) {
  const int x = UI_PAGE_PAD;
  const int w = DISPLAY_WIDTH - 2 * UI_PAGE_PAD;
  const int ty = y + (h - CN_GLYPH_H) / 2;   // 统一基线，不再 y+8 / y+9 / y+14

  if (sel) {
    fill_round_rect(x + UI_ROW_INSET, y, w - 2 * UI_ROW_INSET, h, UI_ROW_R, UI_CARD_SEL);
    int bar_h = UI_BAR_H(h);
    fill_round_rect(x + UI_BAR_INSET, y + (h - bar_h) / 2, UI_BAR_W, bar_h, 2, UI_ACCENT);
  } else if (bg_covered) {
    // 卡片底色没变，只清文字/value/chevron 的矩形带。
    // 上一屏更长的文字必须被覆盖：按整段可用宽度清，而不是只清新文字宽度。
    tft.drawFilledRect(x + UI_ROW_PAD, ty,
                       w - UI_ROW_PAD - UI_ROW_END + 1, CN_GLYPH_H, UI_CARD);
    if (!sep) {
      // 这行没有分隔线：把上一屏可能留下的那条 1px 线也擦掉
      tft.drawFilledRect(x + UI_ROW_PAD, y + h - 1,
                         w - UI_ROW_PAD - UI_ROW_END, 1, UI_CARD);
    }
  } else {
    // 必须用卡片底色覆盖，绝不能用背景黑 —— 那会先看到"擦掉再重画"闪一下
    tft.drawFilledRect(x + UI_ROW_INSET, y, w - 2 * UI_ROW_INSET, h, UI_CARD);
  }

  uint16_t bg = sel ? UI_CARD_SEL : UI_CARD;
  // 内容类文字（曲名 / ROM 名）必须按卡片可用宽度截断：
  // 长名字如果画到卡片外，重绘时清不掉，换短名字后会留下右边一截残影。
  int label_max = w - UI_ROW_PAD - UI_ROW_END - (chevron ? 12 : 0);
  draw_text_fit(x + UI_ROW_PAD, ty, label, label_max, sel ? UI_TXT : UI_TXT2, bg);

  if (value) {
    int vw = cn_text_width(value);
    draw_cn_text(x + w - UI_ROW_END - vw, ty, value, sel ? UI_ACCENT : UI_TXT2, bg);
  }
  if (chevron) {
    draw_chevron(x + w - UI_ROW_END - 4, y + (h - 9) / 2, UI_TXT2, bg);
  }
  if (sep && !sel) {
    tft.drawFilledRect(x + UI_ROW_PAD, y + h - 1, w - UI_ROW_PAD - UI_ROW_END, 1, UI_SEP);
  }
}

// 右侧滚动条（iOS 的 scroll indicator）。391 个游戏没有它完全没有位置感。
static void ui_scrollbar(int top, int h, int total, int visible, int first) {
  if (total <= visible || visible <= 0) {
    return;
  }
  const int x = DISPLAY_WIDTH - UI_SCROLL_W - 6;
  int bar_h = h * visible / total;
  if (bar_h < 12) {
    bar_h = 12;
  }
  int span = total - visible;
  int bar_y = top + (h - bar_h) * (span > 0 ? first : 0) / (span > 0 ? span : 1);

  fill_round_rect(x, top, UI_SCROLL_W, h, 1, UI_CARD_SEL);
  fill_round_rect(x, bar_y, UI_SCROLL_W, bar_h, 1, UI_TXT3);
}

// ---------------- 统一按键读取 ----------------
// 替换改造前三处各自手写的 (millis()-last_input) > 150 / 180 节流。
//
//   方向键：按下沿立即响应；按住 400ms 后进入连发，每 70ms 一格。
//           改造前是恒定 150ms/格，391 个游戏从头翻到尾要按一分多钟。
//   其它键：只认按下沿，按住不重复 —— 避免按住 A 键连选好几个游戏。
static uint8_t  s_prev_mask  = 0;
static uint32_t s_hold_since = 0;
static uint32_t s_last_fire  = 0;

static uint8_t ui_input_poll(void) {
  const uint8_t DIRS = HW_MASK_UP | HW_MASK_DOWN | HW_MASK_LEFT | HW_MASK_RIGHT;

  uint8_t now = (uint8_t)nes_get_gamepad_state();
  uint32_t t  = millis();
  uint8_t out = 0;

  uint8_t edges    = (uint8_t)(now & ~s_prev_mask);   // 本帧新按下的键
  uint8_t dir_edge = (uint8_t)(edges & DIRS);
  uint8_t dir_held = (uint8_t)(now & DIRS);

  out |= (uint8_t)(edges & ~DIRS);                    // 非方向键：只认按下沿

  if (dir_edge) {
    out |= dir_edge;
    s_hold_since = t;
    s_last_fire  = t;
  } else if (dir_held) {
    uint32_t threshold = (t - s_hold_since > 400) ? 70 : 400;
    if (t - s_last_fire >= threshold) {
      out |= dir_held;
      s_last_fire = t;
    }
  }

  s_prev_mask = now;
  return out;
}

// 切页面时调用：以当前按键状态为基准，这样"按住 SELECT 进设置页"不会被
// 新页面误判成一次新的按下（改造前靠一段 1500ms 的等松手循环解决）。
static void ui_input_reset(void) {
  s_prev_mask  = (uint8_t)nes_get_gamepad_state();
  s_hold_since = millis();
  s_last_fire  = millis();
}

// 首次开机的语言选择界面。选项固定为「中文 / English」——
// 两种写法本身就能让对应语言的用户认出来，不需要额外的双语标签。
// 这是一个模态页（没有上一层可返回），所以标题居中、卡片下移，和列表类页面区分开。
static void show_language_picker() {
  int selected = (ui_lang == LANG_EN) ? 1 : 0;
  const int rows = 2;
  const int y0 = 74;

  auto draw_picker = [&]() {
    ui_page(TXT_LANG_TITLE[selected], NULL, true);
    ui_group(y0, rows, UI_ROW_TALL);

    for (int i = 0; i < rows; i++) {
      int y = y0 + UI_ROW_INSET + i * UI_ROW_TALL;
      bool sel = (i == selected);
      // 只有非最后一行、且下一行没被选中时才画分隔线（别画到选中圆角上）
      bool sep = (i < rows - 1) && !(i + 1 == selected);
      const char *name = (i == 0) ? "中文" : "English";
      ui_row(y, UI_ROW_TALL, sel, name, NULL, false, sep);
    }

    ui_page_footer(TXT_LANG_HINT[selected], NULL);
  };

  draw_picker();
  ui_input_reset();
  unsigned long picker_start_ms = millis();   // 无按键时的超时兜底基准

  while (true) {
    uint8_t hw = ui_input_poll();

    int next = selected;
    if (hw & HW_MASK_UP)   next = 0;
    if (hw & HW_MASK_DOWN) next = 1;
    if (next != selected) {
      selected = next;
      draw_picker();
    }

    // A 确认；B 也确认（这个页面没有上一层，B 当成"用当前选中项继续"，
    // 免得用户按了习惯性的返回键反而卡住）。
    if (hw & (HW_MASK_A | HW_MASK_B)) {
      ui_lang = (ui_lang_t)selected;
      save_lang_to_nvs(ui_lang);
      tft.fillScreen(UI_BG);
      delay(120);
      return;
    }

    // 10 秒内没有任何按键操作就默认中文继续。
    // 这样在还没把按键焊完、只想先验证屏幕的阶段也不会卡在这个界面。
    if (millis() - picker_start_ms > 10000) {
      ui_lang = LANG_ZH;
      save_lang_to_nvs(ui_lang);
      Serial.println("[I18N] 10 秒无操作，默认中文");
      tft.fillScreen(UI_BG);
      return;
    }

    delay(20);
  }
}

// 由 .ino 在 tft.init() 之后、show_menu() 之前调用。
// 已经保存过语言就直接用，否则弹一次选择界面。
extern "C" void osd_ui_language_setup(void) {
  // 先应用屏幕 SPI 时钟档位：tft.init() 已经跑完，按键也已初始化，
  // 而且这个值会影响后面所有页面的绘制速度，越早设越好。
  //
  // 兜底：开机时按住 B = 强制 40MHz 并写回 NVS。80MHz 万一在用户自己的
  // 走线上花屏到看不清菜单，这是唯一不依赖屏幕的恢复路径。
  int saved_spi = load_spi_from_nvs();
  if (nes_get_gamepad_state() & HW_MASK_B) {
    saved_spi = 0;
    save_spi_to_nvs(0);
    Serial.println("[CFG] 开机检测到 B 键按住 -> 屏幕时钟强制回 40MHz");
  }
  if (saved_spi < 0) saved_spi = 1;    // 没存过：默认 80MHz（SPI 硬件上限）
  spi_freq_apply(saved_spi);

  int saved = load_lang_from_nvs();
  if (saved >= 0) {
    ui_lang = (ui_lang_t)saved;
    Serial.printf("[I18N] 使用已保存语言: %s\n", saved == LANG_EN ? "en" : "zh");
    return;
  }
  Serial.println("[I18N] 首次开机，显示语言选择");
  show_language_picker();
  Serial.printf("[I18N] 已选择: %s\n", ui_lang == LANG_EN ? "en" : "zh");
}

// 让用户能在菜单里改语言（保留给以后加设置页用）
extern "C" void osd_ui_language_reset(void) {
  show_language_picker();
}

static void draw_magic(uint8_t alpha) {
  static uint16_t line[DISPLAY_WIDTH];
  // 开场图是 280 宽（turning.h 的 hakki_W），面板只有 240 宽，居中裁掉左右各 20 列，
  // 否则 pushImage 会把列地址写到 240 以外，画面花掉。
  const int crop = (hakki_W - DISPLAY_WIDTH) / 2;

  for (int y = 0; y < hakki_H && y < DISPLAY_HEIGHT; y++) {
    int row = y * hakki_W + crop;
    for (int x = 0; x < DISPLAY_WIDTH; x++) {
      uint16_t c = pgm_read_word(&hakki[row + x]);
      c = swap_rb_565(c);
      line[x] = scale_color_565(c, alpha);
    }
    tft.pushImage(0, y, DISPLAY_WIDTH, 1, line);
  }
}

static void show_magic() {
  static bool shown = false;
  if (shown) {
    return;
  }
  shown = true;

  const int fade_in_ms = 120;
  const int hold_ms = 150;
  const int fade_out_ms = 120;
  // 每一步都是整屏重绘（115KB/帧，约 23ms），步数太多会把开机拖成好几秒
  const int steps = 8;
  const int step_delay_in = fade_in_ms / steps;
  const int step_delay_out = fade_out_ms / steps;

  tft.fillScreen(0x0000);
  for (int i = 0; i <= steps; i++) {
    uint8_t alpha = (uint8_t)((i * 255) / steps);
    draw_magic(alpha);
    delay(step_delay_in);
  }

  delay(hold_ms);

  for (int i = steps; i >= 0; i--) {
    uint8_t alpha = (uint8_t)((i * 255) / steps);
    draw_magic(alpha);
    delay(step_delay_out);
  }

  tft.fillScreen(0x0000);
}
 
// 单声道音频缓冲：apu_process() 直接往里写 int16 单声道样本。
// 【重要】I2S 必须按「立体声帧」输出：MAX98357A 是标准 I2S 器件，期望每帧左右两个
// 16-bit slot（BCLK = 采样率 × 32）。如果用 I2S_SLOT_MODE_MONO，ESP32-S3 会把帧配成
// 单 slot（BCLK 减半），功放解析错乱 → 输出直流 + 底噪 → 喇叭音圈发热。
// 所以每帧把单声道样本复制到左右两个 slot，功放内部再取 (L+R)/2。
// 1024 个样本足够 48kHz/60fps（800）用；太小会在 -sample 设大时溢出。
#define AUDIO_MAX_SAMPLES 1024
static int16_t mono_buffer[AUDIO_MAX_SAMPLES];
static int16_t stereo_buffer[AUDIO_MAX_SAMPLES * 2];
static void (*emulator_audio_callback)(void *buffer, int length) = NULL;

#if ENABLE_SOUND
// ESP_I2S 库只提供 I2SClass，不提供全局 I2S 对象（examples 里也是自己 new 一个）
static I2SClass i2s_audio;
static int      s_i2s_rate = 0;      // 0 = 通道没打开

// 打开 / 重配 I2S 通道。
//
// 【为什么要有采样率跟踪】NES 音频是 22050Hz，音乐播放器用的是歌曲自己的
// 采样率（实测 44.1kHz）。两者不能共用同一份 I2S 配置，切过去必须真的
// end() + begin() 重配，否则音调会整整差一倍。
//
// rate 和当前一致时直接返回、不重配 —— 否则每次进游戏都重建一次 DMA 通道，
// 切换瞬间会爆音。
extern "C" int osd_i2s_begin(int rate) {
  if (s_i2s_rate == rate) {
    return 0;
  }
  if (s_i2s_rate != 0) {
    i2s_audio.end();
    s_i2s_rate = 0;
  }
  // MAX98357A 是标准 I2S 器件：必须用立体声帧（每帧左+右两个 16-bit slot）。
  // 用 MONO 会让 BCLK 减半、功放解析错乱（底噪 + 喇叭吃直流发烫，见 HANDOFF 坑 #19）。
  i2s_audio.setPins(I2S_BCK, I2S_WS, I2S_DO);
  // 正常情况下写一帧立刻返回；给 50ms 上限是防止 I2S 卡住把整个模拟器拖死。
  i2s_audio.setTimeout(50);

  if (!i2s_audio.begin(I2S_MODE_STD, rate,
                       I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.printf("[I2S] begin(%d) failed: %d\n", rate, i2s_audio.lastError());
    return -1;
  }
  s_i2s_rate = rate;
  Serial.printf("[I2S] opened at %d Hz stereo\n", rate);
  return 0;
}

// 关闭通道：在"播放器 ↔ NES"之间转移 I2S 所有权时用
extern "C" void osd_i2s_end(void) {
  if (s_i2s_rate == 0) return;
  i2s_audio.end();
  s_i2s_rate = 0;
  Serial.println("[I2S] closed");
}

extern "C" int osd_i2s_rate(void) { return s_i2s_rate; }

// 写 16-bit 立体声交错 PCM。返回写入字节数，-1 表示通道没开。
extern "C" int osd_i2s_write(const void *data, int bytes) {
  if (s_i2s_rate == 0) return -1;
  return (int)i2s_audio.write((const uint8_t *)data, (size_t)bytes);
}

// 把单声道样本复制成左右声道写入 I2S（samples ≤ AUDIO_MAX_SAMPLES）
static void i2s_write_mono_as_stereo(const int16_t *mono, int samples) {
  int n = 0;
  for (int i = 0; i < samples; i++) {
    stereo_buffer[n++] = mono[i];
    stereo_buffer[n++] = mono[i];
  }
  osd_i2s_write(stereo_buffer, n * 2);
}
#endif

static uint16_t myPalette565[256];
static uint16_t myPalette565_swapped[256];
static bitmap_t *game_bitmap = NULL;
static bool video_ready = false;
static uint16_t *frame_buffer = NULL;  
static bool low_mem_video_mode = false;
 
unsigned long frame_start_time = 0;
unsigned long frame_count = 0;
unsigned long last_fps_update = 0;
int current_fps = 0;
 
#define INP_JOYPAD0 0x0001
static nesinput_t joypad_p1;


#define INP_PAD_A 0x01
#define INP_PAD_B 0x02
#define INP_PAD_SELECT 0x04
#define INP_PAD_START 0x08
#define INP_PAD_UP 0x10
#define INP_PAD_DOWN 0x20
#define INP_PAD_LEFT 0x40
#define INP_PAD_RIGHT 0x80
 
esp_timer_handle_t nes_timer_handle = NULL;
void (*emu_timer_callback)(void) = NULL;

void IRAM_ATTR timer_callback_handler(void *arg) {
  if (emu_timer_callback) emu_timer_callback();
}
 
extern char *global_rom_data;
extern int global_rom_size;

extern "C" bool vid_preload_rom(const char *path);
extern "C" int osd_rom_open(const char *path);
extern "C" int osd_rom_read(void *dst, int len);
extern "C" void osd_rom_close(void);
 
// ROM 全部放在 SD 卡根目录（4MB flash 已经不再分区放游戏）。
// names[] 是显示名（去掉 .nes 后缀的 UTF-8 文件名），labels[] 是 SD 卡完整路径。
// 名字截到 30 字节左右（约 24 个 ASCII 或 10 个汉字），再长会顶到右侧的 ">" 箭头。
#define GAME_NAME_MAX 32
#define GAME_PATH_MAX 80

// 【没有固定上限】游戏列表按 SD 卡根目录里实际 .nes 的数量动态分配：
//   每个游戏 32B 显示名 + 80B 路径 = 112B，优先放 PSRAM（2MB 可放上万个），
//   PSRAM 分配失败才退回内部 RAM。scan_games() 先数一遍再分配。
static int game_capacity = 0;

struct {
  char (*names)[GAME_NAME_MAX];
  char (*labels)[GAME_PATH_MAX];
  int count;
  int selected;
} game_list = {nullptr, nullptr, 0, 0};

// 按需扩容（留 1.5 倍余量，重新扫描时不用每次重新分配）
static bool game_list_reserve(int need) {
  if (need <= game_capacity && game_list.names && game_list.labels) return true;
  int cap = need + need / 2;
  if (cap < 64) cap = 64;
  size_t nameBytes = (size_t)cap * GAME_NAME_MAX;
  size_t labelBytes = (size_t)cap * GAME_PATH_MAX;

  void *nm = heap_caps_malloc(nameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  void *lb = heap_caps_malloc(labelBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!nm || !lb) {
    if (nm) heap_caps_free(nm);
    if (lb) heap_caps_free(lb);
    nm = heap_caps_malloc(nameBytes, MALLOC_CAP_8BIT);   // PSRAM 没了就退回内部 RAM
    lb = heap_caps_malloc(labelBytes, MALLOC_CAP_8BIT);
    if (!nm || !lb) {
      if (nm) heap_caps_free(nm);
      if (lb) heap_caps_free(lb);
      return false;
    }
  }
  if (game_list.names) heap_caps_free(game_list.names);
  if (game_list.labels) heap_caps_free(game_list.labels);
  game_list.names = (char (*)[GAME_NAME_MAX])nm;
  game_list.labels = (char (*)[GAME_PATH_MAX])lb;
  game_capacity = cap;
  return true;
}

int menu_volume = 100;
int menu_tone   = MP3_TONE_DEFAULT;

static bool has_nes_suffix(const char *name) {
  size_t n = strlen(name);
  if (n < 4) return false;
  const char *ext = name + n - 4;
  return ext[0] == '.' && (ext[1] == 'n' || ext[1] == 'N') &&
         (ext[2] == 'e' || ext[2] == 'E') && (ext[3] == 's' || ext[3] == 'S');
}

// 按字节截断中文文件名时，可能正好切在某个 3 字节汉字的中间；
// 剩下的半个 UTF-8 序列会让 draw_cn_text() 画出乱码方块，所以把不完整的那截切掉。
static void utf8_trim_incomplete(char *s) {
  size_t n = strlen(s);
  if (n == 0) return;
  int i = (int)n - 1;
  while (i >= 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
  if (i < 0) return;
  unsigned char c = (unsigned char)s[i];
  int need;
  if (c < 0x80) need = 1;
  else if ((c & 0xE0) == 0xC0) need = 2;
  else if ((c & 0xF0) == 0xE0) need = 3;
  else if ((c & 0xF8) == 0xF0) need = 4;
  else { s[i] = '\0'; return; }
  if ((int)n - i < need) {
    s[i] = '\0';
  }
}

// 遍历 SD 卡根目录，只收 .nes / .NES 文件。
// 文件名是 FATFS 给出的 UTF-8 长文件名，中文能直接由 draw_cn_text() 显示。
// 【性能】必须用 getNextFileName() 而不是 openNextFile()：
//   openNextFile() 会为每个目录项真正 fopen 一次，卡上几百个文件时能慢到 7 秒；
//   getNextFileName() 只 readdir 读目录项，391 个文件只要几十毫秒。
void scan_games() {
  game_list.count = 0;

  File root = SD.open("/");
  if (!root) {
    Serial.println("[MENU] SD 卡根目录打开失败（卡没插好 / 没格式化？）");
    return;
  }
  if (!root.isDirectory()) {
    Serial.println("[MENU] SD 卡根路径不是目录");
    root.close();
    return;
  }

  // 一次性预分配足够容量，然后单遍扫描填充（没有固定上限，优先 PSRAM）
  if (!game_list_reserve(1024)) {
    Serial.println("[MENU] 内存不足，无法分配游戏列表");
    root.close();
    return;
  }

  for (;;) {
    String name = root.getNextFileName();   // 只读目录项，不打开文件
    if (name.isEmpty()) break;

    // getNextFileName() 返回的名字可能自带前导 "/"，先统一去掉，
    // 否则显示名和路径都会多一个斜杠（菜单里会看到 "/赤影战士 ..."）。
    const char *fname = name.c_str();
    while (*fname == '/') fname++;
    if (!has_nes_suffix(fname)) continue;   // 目录名不会以 .nes 结尾，自然被过滤掉
    if (game_list.count >= game_capacity) {
      Serial.printf("[MENU] 游戏太多（超过 %d 个），后面的忽略\n", game_capacity);
      break;
    }

    int n = game_list.count;

    // 显示名：文件名去掉 .nes 后缀
    size_t len = strlen(fname);
    if (len > GAME_NAME_MAX - 1) len = GAME_NAME_MAX - 1;
    memcpy(game_list.names[n], fname, len);
    game_list.names[n][len] = '\0';
    size_t nlen = strlen(game_list.names[n]);
    if (nlen >= 4 && has_nes_suffix(game_list.names[n])) {
      game_list.names[n][nlen - 4] = '\0';
    }
    utf8_trim_incomplete(game_list.names[n]);
    if (game_list.names[n][0] == '\0') {
      snprintf(game_list.names[n], GAME_NAME_MAX, "%s %d", TXT_GAME[ui_lang], n + 1);
    }

    // 完整路径：SD.open() 只接受 "/" 开头的路径。
    // 注意 getNextFileName() 返回的名字有时自带前导 "/"，先去掉再拼，别拼出 "//"。
    const char *p = fname;
    while (*p == '/') p++;
    if (strlen(p) >= GAME_PATH_MAX - 1) {
      Serial.printf("[MENU] 文件名太长，跳过: %s\n", fname);
      continue;
    }
    snprintf(game_list.labels[n], GAME_PATH_MAX, "/%s", p);

    game_list.count++;
  }
  root.close();
}

// 给 .ino 用的：重新扫一遍 SD 卡 / 读取扫描到的游戏数量（启动日志用）
extern "C" void osd_scan_games(void) {
  scan_games();
}

extern "C" int osd_game_count(void) {
  return game_list.count;
}

// ---------------- 设置页 ----------------
// 从游戏列表按 SELECT 进入；上下选项、左右调值、B 或 SELECT 返回。
// 调整立即写 NVS，下次开机保持。
// ---------------- 设置页（顶层页签之一）----------------
static void page_settings_loop(void) {
  // 五行：语言 / 音量 / 音色 / LED 脚 / 屏幕（SPI 时钟档）。
  // 行高收到 UI_ROW_H(32)：5×32+4 = 164，卡片落在 40..203，和游戏列表完全对齐，
  // footer 从 y=204 开始清屏，刚好不重叠。
  const int ROWS  = 5;
  const int ROW_H = UI_ROW_H;
  const int y0    = UI_GROUP_Y;
  // 本页卡片底边（局部清屏用，见 ui_page_tabs）
  const int card_bottom = y0 + ROW_H * ROWS + UI_ROW_GAP;

  // 音量离散条：10 格，每格 10%。比连续滑块更适合只有左右两个按键的设备，
  // 也比纯数字直观。起点要避开最长的标签（英文 "VOLUME" = 6*7 = 42px）。
  const int label_w_max = 44;
  const int bar_x   = UI_PAGE_PAD + UI_ROW_PAD + label_w_max + 8;
  const int seg     = 8;    // 单格宽
  const int seg_gap = 2;
  const int bar_h   = 8;

  int  sel       = 0;
  int  last_sel  = -1;
  bool dirty_all = true;        // 首次进入 / 语言切换 / 频率切换：整页重画
  int  dirty_row = -1;          // 值变化：只刷这一行
  bool first_paint = true;

  auto draw_row = [&](int i, bool on) {
    int y = y0 + UI_ROW_INSET + i * ROW_H;
    bool sep = (i < ROWS - 1) && !(i + 1 == sel);

    const char *label = (i == 0) ? TXT_LANG_ITEM[ui_lang]
                      : (i == 1) ? TXT_VOL_ITEM[ui_lang]
                      : (i == 2) ? TXT_TONE_ITEM[ui_lang]
                      : (i == 3) ? TXT_LED_ITEM[ui_lang]
                                 : TXT_SCR_ITEM[ui_lang];
    char val[16];
    if (i == 0) {
      snprintf(val, sizeof(val), "%s", ui_lang == LANG_ZH ? "中文" : "English");
    } else if (i == 1) {
      snprintf(val, sizeof(val), "%d%%", menu_volume);
    } else if (i == 2) {
      snprintf(val, sizeof(val), "%s", TXT_TONE_VAL[ui_lang][menu_tone]);
    } else if (i == 3) {
      snprintf(val, sizeof(val), "%s", TXT_LED_VAL[ui_lang][menu_led]);
    } else {
      snprintf(val, sizeof(val), "%s", TXT_SCR_VAL[menu_spi]);
    }

    ui_row(y, ROW_H, on, label, val, false, sep);

    if (i == 1) {
      int by = y + (ROW_H - bar_h) / 2;
      int filled = (menu_volume + 5) / 10;   // 0..10 格
      for (int s = 0; s < 10; s++) {
        uint16_t c = (s < filled) ? UI_ACCENT : UI_TXT3;
        fill_round_rect(bar_x + s * (seg + seg_gap), by, seg, bar_h, 2, c);
      }
    }
  };

  // 上下移动改变选中行时，选中行**上一行**的分隔线状态也会变（选中行上方
  // 不画线，避免压到高亮圆角），但那一行的内容完全没变。单独补一条 1px 线，
  // 不要为了它把整行 13.8KB 重刷一遍。
  auto draw_sep_line = [&](int i) {
    if (i < 0 || i >= ROWS || i == sel) return;
    int y = y0 + UI_ROW_INSET + i * ROW_H;
    bool sep = (i < ROWS - 1) && !(i + 1 == sel);
    tft.drawFilledRect(UI_PAGE_PAD + UI_ROW_PAD, y + ROW_H - 1,
                       DISPLAY_WIDTH - 2 * UI_PAGE_PAD - UI_ROW_PAD - UI_ROW_END, 1,
                       sep ? UI_SEP : UI_CARD);
  };

  ui_input_reset();   // 以当前按键状态为基准，仍按着的 SELECT 不算新的按下

  for (;;) {
    if (dirty_all) {
      // 整页重画：只在首次进入、语言变化、SPI 频率变化时走这里
      if (first_paint) {
        ui_page_tabs(PAGE_SETTINGS, NULL, card_bottom);
        ui_group(y0, ROWS, ROW_H);
        // footer 文案只在语言变化时才需要重画，不要跟着每次调值一起刷
        ui_page_footer(TXT_HINT_CFG[ui_lang], TXT_NAV_SELECT[ui_lang]);
        first_paint = false;
      }

      for (int i = 0; i < ROWS; i++) {
        draw_row(i, i == sel);
      }
      last_sel  = sel;
      dirty_row = -1;
      dirty_all = false;
    } else if (last_sel != sel) {
      // 上下切换：只有两行的选中态变了，其余 3 行内容完全一样 ——
      // 重画它们等于每按一下白刷 41KB。和游戏列表同一套思路。
      draw_row(last_sel, false);
      draw_row(sel, true);
      draw_sep_line(last_sel - 1);
      draw_sep_line(sel - 1);
      last_sel = sel;
    } else if (dirty_row >= 0) {
      // 左右调值：只刷被调的那一行（音量行会连进度条一起重画）
      draw_row(dirty_row, dirty_row == sel);
      dirty_row = -1;
    }

    uint8_t hw = ui_input_poll();

    // 改造前三处按钮里 UP 和 DOWN 都写成 sel = (sel+1)%2（语义相同，只是两项时
    // 凑巧能用）。这里改成真正的上一项/下一项。
    if (hw & HW_MASK_UP) {
      sel = (sel + ROWS - 1) % ROWS;
    }
    if (hw & HW_MASK_DOWN) {
      sel = (sel + 1) % ROWS;
    }
    if (hw & (HW_MASK_LEFT | HW_MASK_RIGHT)) {
      int dir = (hw & HW_MASK_LEFT) ? -1 : 1;
      if (sel == 0) {
        ui_lang = (ui_lang_t)((ui_lang + LANG_COUNT + dir) % LANG_COUNT);
        save_lang_to_nvs(ui_lang);
        first_paint = true;   // 界面语言变了，标题/标签/footer 全要重画
        dirty_all = true;
      } else if (sel == 1) {
        menu_volume += dir * 10;
        if (menu_volume < 0) menu_volume = 0;
        if (menu_volume > 100) menu_volume = 100;
        master_volume = menu_volume;
        save_volume_to_nvs(menu_volume);
        dirty_row = sel;
      } else if (sel == 2) {
        // 音色：立即生效。音乐可以一边放一边切到设置页，左右直接 A/B
        menu_tone = (menu_tone + MP3_TONE_COUNT + dir) % MP3_TONE_COUNT;
        mp3_set_tone(menu_tone);
        save_tone_to_nvs(menu_tone);
        Serial.printf("[CFG] 音色 -> %d\n", menu_tone);
        dirty_row = sel;
      } else if (sel == 3) {
        // LED 脚：浮空 / 拉低 / 拉高。改完立刻看板子上哪颗灯跟着变
        menu_led = (menu_led + 3 + dir) % 3;
        led_pin_apply(menu_led);
        save_led_to_nvs(menu_led);
        dirty_row = sel;
      } else {
        // 屏幕 SPI 时钟：40M ↔ 80M，立即生效。
        // 切完必须整页重画一遍：万一 80M 花屏，按一下切回 40M 时画面要能自己恢复，
        // 而不是停在花屏帧上等用户盲操作。
        spi_freq_apply(1 - menu_spi);
        save_spi_to_nvs(menu_spi);
        first_paint = true;
        dirty_all = true;
      }
    }

    // SELECT 切到下一个页面、B 切到上一个页面（设置页没有"返回"语义了，
    // 它就是三个顶层页面之一）。
    if (ui_page_switch(hw)) {
      Serial.printf("[MENU] settings -> page %d (lang=%d vol=%d tone=%d led=%d spi=%d)\n",
                    (int)g_page, (int)ui_lang, menu_volume, menu_tone, menu_led, menu_spi);
      return;
    }

    delay(20);
  }
}


// ============================================================================
// 三个顶层页面
//
// 页面之间是**并列**关系（SELECT 循环向后切、B 循环向前切），不是"从列表进
// 设置"的父子关系。每个 xxx_loop() 自己处理本页按键，切页时直接 return，
// 由 show_menu() 里的调度器负责画下一页。
// ============================================================================

// ---------------- 游戏页 ----------------
// 返回 >=0：用户按 A 选中了某个游戏（交给 .ino 启动 nofrendo）
// 返回 -1 ：切页了（或者 SD 卡里没有游戏）
static int page_games_loop(void) {
  // 空状态也用同一套骨架 + 卡片，并且**不再死等几秒然后提示重启** ——
  // 没有游戏时用户照样能切到音乐/设置页。
  if (game_list.count == 0) {
    Serial.println("[MENU] SD 卡根目录下没有 .nes 文件（把 ROM 拷到根目录）");
    ui_page_tabs(PAGE_GAMES, NULL);
    const int ey = 84;
    ui_group(ey, 1, 56);
    draw_cn_center(ey + 12, TXT_NO_GAMES[ui_lang], UI_TXT, UI_CARD);
    draw_cn_center(ey + 34, TXT_NO_GAMES_TIP[ui_lang], UI_TXT2, UI_CARD);
    // 没有游戏时按 A 什么也不会发生，别在 footer 写 "A开始" 误导 ——
    // 导航靠顶部那三个页签已经够清楚了。
    ui_page_footer("", TXT_NAV_SELECT[ui_lang]);
    ui_input_reset();
    for (;;) {
      uint8_t hw = ui_input_poll();
      if (ui_page_switch(hw)) return -1;
      delay(20);
    }
  }

  // 一屏 5 行，其余靠上下滚动（行高/内边距全部来自 ui_theme.h）
  const int VISIBLE_ROWS = 5;

  // 这些状态要跨"切页再切回来"保留，所以是 static ——
  // 从设置页回来时光标还停在刚才那款游戏上，不用重新翻。
  static int  top_index = 0;
  static int  last_selected = -1;
  static int  last_top = 0;      // 上次绘制时的滚动窗口起点（算旧选中行位置用）
  static bool needs_full_redraw = true;
  static bool needs_list_redraw = false;
  static bool games_first = true;

  if (games_first) {
    game_list.selected = 0;
    top_index = 0;
    games_first = false;
  } else {
    // 切页回来时把滚动窗口夹回合法范围
    if (top_index > game_list.count - VISIBLE_ROWS) top_index = game_list.count - VISIBLE_ROWS;
    if (top_index < 0) top_index = 0;
    if (game_list.selected < top_index) top_index = game_list.selected;
    if (game_list.selected >= top_index + VISIBLE_ROWS) {
      top_index = game_list.selected - (VISIBLE_ROWS - 1);
    }
    needs_full_redraw = true;      // 整页重画一次（页签也可能已经变了）
    last_selected = -1;
  }

  auto visible_rows = [&]() {
    int n = game_list.count - top_index;
    if (n > VISIBLE_ROWS) n = VISIBLE_ROWS;
    return n < 0 ? 0 : n;
  };

  auto row_y = [](int si) { return UI_GROUP_Y + UI_ROW_INSET + si * UI_ROW_H; };

  // 单行重绘：只覆盖这一行，用于选中项移动，避免整块刷新造成的闪屏。
  //   bg_covered：这行背后已经是卡片底色（首次绘制刚填过整卡，或者翻页时
  //               卡片底根本没动）。此时非选中行只清 16px 高的文字带；
  //               「上一次是选中、这一次不是」的行必须整行覆盖 —— 高亮圆角
  //               有 8px 高，不在文字带里，只清文字带会留下高亮残角。
  auto draw_one_row = [&](int si, bool sel, bool bg_covered) {
    int n = visible_rows();
    if (si < 0 || si >= n) return;
    int gi = top_index + si;
    // 分隔线不能画到下一行的选中圆角上
    bool sep = (si < n - 1) && !((top_index + si + 1) == game_list.selected);
    ui_row(row_y(si), UI_ROW_H, sel, game_list.names[gi], NULL, true, sep, bg_covered);
  };

  // 整块重绘。
  //   card_bg   ：是否重画卡片底。首次绘制（fillScreen 之后）必须画；翻页时
  //               卡片底完全没变 —— 391 个游戏里它始终是同样位置同样大小的
  //               深灰圆角块，重画等于把 72KB 原样再写一遍。
  //   restore_si：上一次是选中、这一次不再是的那一行（屏幕内行号）。
  auto draw_list = [&](bool card_bg, int restore_si) {
    int n = visible_rows();
    if (n <= 0) return;
    if (card_bg) {
      ui_group(UI_GROUP_Y, n, UI_ROW_H);
    }
    for (int i = 0; i < n; i++) {
      bool covered = (i != restore_si);
      draw_one_row(i, (top_index + i) == game_list.selected, covered);
    }
  };

  // 391 个游戏没有滚动条就完全没有位置感。轨道高度按「满屏 5 行」算，不随行数变化。
  auto draw_scroll = [&]() {
    ui_scrollbar(UI_GROUP_Y, UI_ROW_H * VISIBLE_ROWS + UI_ROW_GAP,
                 game_list.count, VISIBLE_ROWS, top_index);
  };

  ui_input_reset();

  for (;;) {
    if (needs_full_redraw) {
      char cnt[8];
      snprintf(cnt, sizeof(cnt), "%d", game_list.count);
      ui_page_tabs(PAGE_GAMES, cnt, UI_GROUP_Y + UI_ROW_H * VISIBLE_ROWS + UI_ROW_GAP);
      ui_page_footer(TXT_HINT[ui_lang], TXT_NAV_SELECT[ui_lang]);
      draw_list(true, -1);      // ui_page_tabs 里刚 fillScreen 过，卡片底要画
      draw_scroll();

      last_selected = game_list.selected;
      last_top = top_index;
      needs_full_redraw = false;
      needs_list_redraw = false;
    } else if (needs_list_redraw) {
      // 翻页（滚动一屏）。卡片底没有变化，跳过它；只剩
      // 「上一次是选中、这一次不是」的那一行需要整行覆盖。
      // 只有滚到末尾、可见行数不足满屏时，卡片高度变了，卡片底才必须重画。
      int n = visible_rows();
      int old_si = last_selected - last_top;
      int new_si = game_list.selected - top_index;
      int restore_si = (old_si >= 0 && old_si < n && old_si != new_si) ? old_si : -1;
      draw_list(n != VISIBLE_ROWS, restore_si);
      draw_scroll();
      last_selected = game_list.selected;
      last_top = top_index;
      needs_list_redraw = false;
    } else if (last_selected != game_list.selected) {
      // 只重绘变化的这两行，其余行不动 —— 这是消除切换闪烁的关键
      int n = visible_rows();
      if (last_selected >= top_index && last_selected < top_index + n) {
        draw_one_row(last_selected - top_index, false, false);
      }
      if (game_list.selected >= top_index && game_list.selected < top_index + n) {
        draw_one_row(game_list.selected - top_index, true, false);
      }
      draw_scroll();
      last_selected = game_list.selected;
      last_top = top_index;
    }

    uint8_t hw = ui_input_poll();

    // 调试：按下任意键时每 400ms 打一次原始状态，方便确认键位接线对不对
    // （游戏内不需要，只在菜单里打）
    static unsigned long last_btn_log = 0;
    if (hw != 0 && (millis() - last_btn_log) > 400) {
      Serial.printf("[BTN] pressed mask=0x%02X%s%s%s%s%s%s%s%s\n", hw,
                    (hw & HW_MASK_A) ? " A" : "",
                    (hw & HW_MASK_B) ? " B" : "",
                    (hw & HW_MASK_SELECT) ? " SELECT" : "",
                    (hw & HW_MASK_START) ? " START" : "",
                    (hw & HW_MASK_UP) ? " UP" : "",
                    (hw & HW_MASK_DOWN) ? " DOWN" : "",
                    (hw & HW_MASK_LEFT) ? " LEFT" : "",
                    (hw & HW_MASK_RIGHT) ? " RIGHT" : "");
      last_btn_log = millis();
    }

    if (hw & HW_MASK_UP) {
      if (game_list.selected > 0) {
        game_list.selected--;
        if (game_list.selected < top_index) {
          top_index = game_list.selected;
          needs_list_redraw = true;
        }
      }
    }
    if (hw & HW_MASK_DOWN) {
      if (game_list.selected < game_list.count - 1) {
        game_list.selected++;
        if (game_list.selected >= top_index + VISIBLE_ROWS) {
          top_index = game_list.selected - (VISIBLE_ROWS - 1);
          needs_list_redraw = true;
        }
      }
    }
    if (hw & HW_MASK_A) {
      tft.fillScreen(UI_BG);
      delay(120);
      return game_list.selected;
    }

    if (ui_page_switch(hw)) {
      return -1;
    }

    // 长按 START 0.6 秒切换界面语言。这是早期版本就有的快捷键，保留给老用户；
    // 设置页里的「语言」项是正式入口。这里直接用原始电平判断，
    // 不走 ui_input_poll（那个只给按下沿，按住不会连续触发）。
    static uint32_t start_hold_since = 0;
    if (nes_get_gamepad_state() & HW_MASK_START) {
      if (start_hold_since == 0) {
        start_hold_since = millis();
      } else if (millis() - start_hold_since > 600) {
        start_hold_since = 0;
        ui_lang = (ui_lang == LANG_ZH) ? LANG_EN : LANG_ZH;
        save_lang_to_nvs(ui_lang);
        Serial.printf("[I18N] 切换语言 -> %s\n", ui_lang == LANG_EN ? "en" : "zh");
        needs_full_redraw = true;
        last_selected = -1;
        delay(400);   // 等松手，避免一次长按连续切换
      }
    } else {
      start_hold_since = 0;
    }

    delay(20);
  }
  return -1;
}

// ---------------- 音乐页 ----------------
// 曲目表来自 mp3_player.cpp 的扫描，解码跑在那边一个 32KB 栈的独立任务里
// （原因见 HANDOFF 第十三节：minimp3 的 scratch 有 14KB 在栈上，
//  loopTask 的 8KB 栈直接调用会 Stack canary 崩溃）。
//
// 按键分工（和全局导航保持一致，B/SELECT 永远是切页）：
//   上下 = 选曲（只移动高亮）      A = 播放选中曲 / 播放暂停当前曲
//   左右 = 上一首 / 下一首         B / SELECT = 切页
static void page_music_loop(void) {
  mp3_scan();

  const int VISIBLE_ROWS = 5;
  const int total = mp3_count();

  // 空状态：和游戏页同一套骨架，不再是一页裸文字
  if (total == 0) {
    Serial.println("[MUSIC] SD 根目录没有 .mp3");
    ui_page_tabs(PAGE_MUSIC, NULL);
    const int ey = 84;
    ui_group(ey, 1, 56);
    draw_cn_center(ey + 12, TXT_MUSIC_TITLE[ui_lang], UI_TXT, UI_CARD);
    draw_cn_center(ey + 34, TXT_MUSIC_NONE[ui_lang], UI_TXT2, UI_CARD);
    ui_page_footer(TXT_MUSIC_EMPTY_HINT[ui_lang], TXT_NAV_SELECT[ui_lang]);
    ui_input_reset();
    for (;;) {
      uint8_t hw = ui_input_poll();
      if (ui_page_switch(hw)) return;
      delay(20);
    }
  }

  static int  sel       = 0;
  static int  last_cur  = -2;
  static int  last_pct  = -1;
  static bool last_play = false;
  static int  last_sel  = -1;      // 上次画的是哪一行（用于只刷变化的两行）
  static int  last_top  = -1;      // 上次的滚动窗口起点（变了就得整块重画）

  if (sel >= total) sel = total - 1;
  if (sel < 0) sel = 0;

  auto top_of = [&]() {
    int t = (sel >= VISIBLE_ROWS) ? (sel - (VISIBLE_ROWS - 1)) : 0;
    if (t > total - VISIBLE_ROWS) t = total - VISIBLE_ROWS;
    if (t < 0) t = 0;
    return t;
  };

  // 正在播放的那首用强调色标出来 —— 用户翻到别的页也能一眼看到在放哪首
  auto row_color = [&](int gi, bool is_sel) -> uint16_t {
    if (is_sel) return UI_TXT;
    if (gi == mp3_current()) return UI_ACCENT;
    return UI_TXT2;
  };

  auto draw_row = [&](int si, bool is_sel, bool bg_covered) {
    int top = top_of();
    int gi = top + si;
    int y = UI_GROUP_Y + UI_ROW_INSET + si * UI_ROW_H;
    int ty = y + (UI_ROW_H - CN_GLYPH_H) / 2;
    bool sep = (si < VISIBLE_ROWS - 1) && (gi < total - 1);

    // 只让 ui_row 画背景和分隔线，文字自己画：ui_row 的 label 只有
    // "选中/未选中"两种颜色，而"正在播放但没选中"需要第三种（强调色）。
    ui_row(y, UI_ROW_H, is_sel, "", NULL, false, sep, bg_covered);
    // 曲名是变长内容，必须按可用宽度截断 —— 否则长名换短名会在右侧留残影
    int max_px = DISPLAY_WIDTH - 2 * UI_PAGE_PAD - UI_ROW_PAD - UI_ROW_END;
    draw_text_fit(UI_PAGE_PAD + UI_ROW_PAD, ty, mp3_name(gi), max_px,
                  row_color(gi, is_sel), is_sel ? UI_CARD_SEL : UI_CARD);
  };

  // 42 首歌没有滚动条完全没有位置感（和游戏列表保持一致）
  auto draw_scroll = [&]() {
    ui_scrollbar(UI_GROUP_Y, UI_ROW_H * VISIBLE_ROWS + UI_ROW_GAP,
                 total, VISIBLE_ROWS, top_of());
  };

  // 整块重绘。card_bg=false 表示卡片底没动（翻页/换播放态），跳过 72KB 的
  // 重复填充；restore_si 是「上次选中、这次不再选中」的屏幕行号，必须整行覆盖
  // （高亮圆角在 16px 文字带之外）。参数语义和游戏列表一致。
  auto draw_list = [&](bool card_bg, int restore_si) {
    if (card_bg) {
      ui_group(UI_GROUP_Y, VISIBLE_ROWS, UI_ROW_H);
    }
    int top = top_of();
    for (int i = 0; i < VISIBLE_ROWS; i++) {
      bool covered = (i != restore_si);
      draw_row(i, (top + i) == sel, covered);
    }
    draw_scroll();
  };

  auto draw_status = [&]() {
    char buf[64];
    int cur = mp3_current();
    int pct = mp3_progress_pct();
    if (cur < 0) {
      snprintf(buf, sizeof(buf), "%s", TXT_MUSIC_HINT[ui_lang]);
    } else if (mp3_is_paused()) {
      snprintf(buf, sizeof(buf), TXT_MUSIC_PAUSED[ui_lang], pct);
    } else if (mp3_is_playing()) {
      snprintf(buf, sizeof(buf), TXT_MUSIC_PLAYING[ui_lang], pct);
    } else {
      snprintf(buf, sizeof(buf), "%s", TXT_MUSIC_HINT[ui_lang]);
    }
    ui_page_footer(buf, TXT_NAV_SELECT[ui_lang]);
  };

  char trailing[8];
  snprintf(trailing, sizeof(trailing), "%d", total);

  ui_page_tabs(PAGE_MUSIC, trailing, UI_GROUP_Y + UI_ROW_H * VISIBLE_ROWS + UI_ROW_GAP);
  draw_list(true, -1);          // 刚 fillScreen 过，卡片底要画
  draw_status();

  last_cur  = mp3_current();
  last_pct  = mp3_progress_pct();
  last_play = mp3_is_playing();
  last_sel  = sel;
  last_top  = top_of();
  ui_input_reset();

  bool need_list = false;

  for (;;) {
    uint8_t hw = ui_input_poll();

    if (ui_page_switch(hw)) return;

    if (hw & HW_MASK_UP) {
      if (sel > 0) { sel--; need_list = true; }
    }
    if (hw & HW_MASK_DOWN) {
      if (sel < total - 1) { sel++; need_list = true; }
    }
    if (hw & HW_MASK_A) {
      if (mp3_current() == sel) {
        mp3_toggle();          // 就是正在放的那首 → 播放/暂停
      } else {
        mp3_play_index(sel);   // 换一首 → 直接开始放
      }
      need_list = true;
    }
    if (hw & HW_MASK_LEFT)  { mp3_prev(); need_list = true; }
    if (hw & HW_MASK_RIGHT) { mp3_next(); need_list = true; }

    // 曲目变了要重画列表（强调色换位置）；只是进度变就只刷状态行
    int  cur  = mp3_current();
    int  pct  = mp3_progress_pct();
    bool play = mp3_is_playing();

    if (cur != last_cur) {
      last_cur  = cur;
      need_list = true;
    }
    if (need_list || pct != last_pct || play != last_play) {
      if (need_list) {
        // 只重画真正变了的行。整块重画 5 行会让翻选项时明显闪一下 ——
        // 游戏列表早就是这么做的（只画变化的 2 行），音乐页之前偷懒了。
        int top = top_of();
        if (top != last_top || last_sel < 0) {
          // 翻页了：卡片底没变，只重画行；旧选中行如果换到了别的屏幕行，
          // 那一行必须整行覆盖（高亮圆角不在 16px 文字带里）。
          int n = total - top;
          if (n > VISIBLE_ROWS) n = VISIBLE_ROWS;
          int old_si = last_sel - last_top;
          int new_si = sel - top;
          int restore_si = (old_si >= 0 && old_si < n && old_si != new_si) ? old_si : -1;
          draw_list(n != VISIBLE_ROWS, restore_si);   // 不满屏时卡片高度会变，得重画卡片底
        } else if (last_sel != sel) {
          draw_row(last_sel - top, false, false);   // 取消选中（整行恢复）
          draw_row(sel - top, true, false);         // 新选中（整行高亮）
          draw_scroll();                            // 只画了两行，滚动条得单独更新
        } else {
          // 播放态变化：颜色要跟着换，但卡片底没变
          draw_list(false, -1);
        }
        last_sel = sel;
        last_top = top;
        need_list = false;
      }
      last_pct  = pct;
      last_play = play;
      draw_status();
    }

    delay(20);
  }
}

extern "C" int show_menu() {
  unsigned long t_menu = millis();
  Serial.println("\n[MENU] Starting menu system...");

  // 音量：设置页改过就从 NVS 恢复。
  //
  // 【这里曾经有个 bug】save_volume_to_nvs() 在设置页里确实写了 NVS，但
  // load_volume_from_nvs() 只在 osd_main()（= 启动游戏时）调用。于是重启后
  // 菜单里一直显示默认的 100%，非得进一次游戏才会"变回来"——用户观感就是
  // "音量没存档"。所以必须在进菜单之前主动读一次。
  static bool vol_restored = false;
  if (!vol_restored) {
    vol_restored = true;
    int v = load_volume_from_nvs();
    if (v >= 0) {
      menu_volume = v;
      master_volume = v;
      Serial.printf("[CFG] 音量恢复: %d%%\n", v);
    }
    // 音色档位同理：必须在第一次进设置页/开始播放之前就恢复，
    // 否则用户会觉得"音色没存档"。
    int t = load_tone_from_nvs();
    if (t >= 0) {
      menu_tone = t;
      mp3_set_tone(t);
      Serial.printf("[CFG] 音色恢复: %d\n", t);
    }
    // LED 手动测试档：0（浮空）是默认态，不需要动引脚；>0 才真正驱动
    int l = load_led_from_nvs();
    if (l > 0) {
      led_pin_apply(l);
      Serial.printf("[CFG] LED 档位恢复: %d\n", l);
    }
  }

  show_magic();
  Serial.printf("[T] menu magic done [%lu ms]\n", millis() - t_menu);

  // 游戏列表在 setup 里已经扫描过（osd_scan_games）；从游戏退出回菜单时
  // SD 卡内容不会变，没必要把 391 个文件再扫一遍 —— 只有列表为空时才补扫。
  if (game_list.count == 0) {
    scan_games();
  }
  Serial.printf("[MENU] Found %d games [%lu ms]\n", game_list.count, millis() - t_menu);

  // 按键自检：面包板上什么都不按时，八个键都应该是 1（INPUT_PULLUP 拉高）。
  // 如果某个键一直是 0，说明那根 GPIO 被持续拉低 —— 接线短地/接错脚，或该引脚不适合做普通输入。
  Serial.printf("[BTN] UP(GPIO%d)=%d DOWN(%d)=%d LEFT(%d)=%d RIGHT(%d)=%d "
                "A(%d)=%d B(%d)=%d START(%d)=%d SELECT(%d)=%d\n",
                BTN_UP,     digitalRead(BTN_UP),
                BTN_DOWN,   digitalRead(BTN_DOWN),
                BTN_LEFT,   digitalRead(BTN_LEFT),
                BTN_RIGHT,  digitalRead(BTN_RIGHT),
                BTN_A,      digitalRead(BTN_A),
                BTN_B,      digitalRead(BTN_B),
                BTN_START,  digitalRead(BTN_START),
                BTN_SELECT, digitalRead(BTN_SELECT));

  // 顶层页面调度器。三个页面各跑各的死循环，切页时把 g_page 改掉直接 return，
  // 回到这里重画下一页。
  // 正常情况下这个函数只在"用户按 A 选中游戏"时返回 >=0；
  // .ino 里 selected < 0 那个分支现在实际上不会走到（保留作防御）。
  for (;;) {
    switch (g_page) {
      case PAGE_GAMES: {
        int sel = page_games_loop();
        if (sel >= 0) {
          return sel;
        }
        break;
      }
      case PAGE_MUSIC:
        page_music_loop();
        break;
      case PAGE_SETTINGS:
        page_settings_loop();
        break;
      default:
        g_page = PAGE_GAMES;
        break;
    }
  }
}


extern "C" const char *get_selected_game() {
  // 返回 SD 卡上的完整路径，.ino 会把它原样传给 nofrendo_main() -> osd_rom_open()
  static char game_path[GAME_PATH_MAX];
  if (game_list.selected >= 0 && game_list.selected < game_list.count) {
    snprintf(game_path, sizeof(game_path), "%s", game_list.labels[game_list.selected]);
    return game_path;
  }
  return "";   // 还没在菜单里选过
}

// 当前 nofrendo 的 rom_load() 走的是后面的 osd_rom_open/read 流式接口，
// osd_getromdata() 这条旧的整块加载路径在本移植里不会被调用，保留只为满足 osd.h 接口约定。
extern "C" char *osd_getromdata(void) {
  return global_rom_data;
}

extern "C" void osd_release_romdata(void) {
  if (global_rom_data) {
    free(global_rom_data);
    global_rom_data = NULL;
    global_rom_size = 0;
  }
}

extern "C" int osd_init_sound(void) {
  if (!runtime_sound_enabled) {
    Serial.println("Audio disabled by runtime config (-sound not set)");
    return 0;
  }
#if ENABLE_SOUND
  // 进游戏前先把音乐播放器停掉、把 I2S 让出来。
  // 播放器用的是歌曲的采样率（44.1kHz），NES 要 22050Hz，同一个通道不能共存。
  mp3_suspend();

  Serial.printf("Initializing I2S audio (BCK=%d WS=%d DO=%d, %d Hz stereo)...\n",
                I2S_BCK, I2S_WS, I2S_DO, audio_sample_rate);

  // osd_i2s_begin() 内部会比对采样率：已经是这个频率就直接返回，
  // 不会重复拆建 DMA 通道（那会在每次进游戏时爆一下音）。
  if (osd_i2s_begin(audio_sample_rate) != 0) {
    return -1;
  }
  Serial.println("I2S audio initialized successfully");
  return 0;
#else
  Serial.println("Sound disabled in configuration");
  return 0;
#endif
}

extern "C" void osd_boot_sound(void) {
#if ENABLE_SOUND
  // 开机音效跑在 nofrendo_main() 之前，那时还没有 -sound 参数，
  // runtime_sound_enabled 还是 false，osd_init_sound() 会直接跳过 I2S 初始化。
  // 这里临时把它置 true 让音频通道真正建起来（游戏启动时会重新置 true）。
  runtime_sound_enabled = true;
  if (osd_init_sound() != 0) {
    return;
  }

  // 三个上升音 C5 → E5 → G5，验证功放/喇叭（不需要按键）。
  static const float notes[3] = {523.25f, 659.25f, 783.99f};
  const int note_ms = 110;
  const int samples = (audio_sample_rate * note_ms) / 1000;
  const int fade = (samples / 8) > 0 ? (samples / 8) : 1;
  int16_t buf[256];

  for (int n = 0; n < 3; n++) {
    float phase = 0.0f;
    float step = 2.0f * (float)M_PI * notes[n] / (float)audio_sample_rate;
    int idx = 0;
    for (int i = 0; i < samples; i++) {
      float env = 1.0f;
      if (i < fade) env = (float)i / (float)fade;
      else if (i >= samples - fade) env = (float)(samples - i) / (float)fade;
      buf[idx++] = (int16_t)(sinf(phase) * 9000.0f * env);
      phase += step;
      if (phase >= 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
      if (idx >= 256) {
        i2s_write_mono_as_stereo(buf, idx);
        idx = 0;
      }
    }
    if (idx > 0) i2s_write_mono_as_stereo(buf, idx);
  }

  // 尾部静音，把 DMA 里剩下的样本冲干净，免得菜单阶段还残留
  memset(buf, 0, sizeof(buf));
  for (int i = 0; i < 4; i++) {
    i2s_write_mono_as_stereo(buf, 256);
  }
  delay(150);
#endif
}

extern "C" void osd_getsoundinfo(sndinfo_t *info) {
  info->sample_rate = audio_sample_rate;
  info->bps = 16;
}

extern "C" void osd_setsound(void (*playfunc)(void *buffer, int length)) {
  emulator_audio_callback = runtime_sound_enabled ? playfunc : NULL;
}

extern "C" void osd_stopsound(void) {
  if (!runtime_sound_enabled) return;
#if ENABLE_SOUND
  // 新 API 没有 i2s_zero_dma_buffer()，直接往 DMA 里灌几段静音把它冲干净，
  // 避免退出游戏回菜单后还在放上一帧的残留声音。
  static const int16_t silence[128] = {0};
  for (int i = 0; i < 8; i++) {
    i2s_write_mono_as_stereo(silence, 128);
  }
#endif
}

extern "C" void osd_writesound(void *stream, int len) {}

extern "C" void osd_initvideo(int *lines) {
  *lines = NES_SCREEN_HEIGHT;
  Serial.printf("osd_initvideo called, lines=%d\n", *lines);
}

extern "C" void osd_shutdownvideo() {}

extern "C" void osd_setscreen(int x, int y, int width, int height) {}

extern "C" void osd_setpalette(rgb_t *pal) {
  if (!pal) return;
  for (int i = 0; i < 256; i++) {
    uint16_t c = tft.color565(pal[i].r, pal[i].g, pal[i].b);
    myPalette565[i] = c;
    myPalette565_swapped[i] = __builtin_bswap16(c);
  }
}

extern "C" int nes_get_gamepad_state();
extern "C" void main_quit(void);   // nofrendo 的退出入口：置位 console.quit 让主循环自然结束

extern "C" void osd_getinput(void) {
  int hw = nes_get_gamepad_state();
  int nes_data = 0;

  // SELECT + START 同时按住 0.6 秒 → 退出当前游戏回到菜单。
  // 用 main_quit() 让 nofrendo 的主循环自然结束，nofrendo_main() 返回后
  // .ino 里的循环会重新显示菜单 —— 不重启芯片，切换是即时的。
  static uint32_t exit_combo_since = 0;
  if ((hw & HW_MASK_SELECT) && (hw & HW_MASK_START)) {
    if (exit_combo_since == 0) {
      exit_combo_since = millis();
    } else if (millis() - exit_combo_since > 600) {
      exit_combo_since = 0;
      tft.fillScreen(0x0000);
      draw_cn_center(112, TXT_BACK[ui_lang], UI_TXT, UI_BG);
      delay(200);
      main_quit();
    }
  } else {
    exit_combo_since = 0;
  }

  if (hw & HW_MASK_A) nes_data |= INP_PAD_A;
  if (hw & HW_MASK_B) nes_data |= INP_PAD_B;
  if (hw & HW_MASK_SELECT) nes_data |= INP_PAD_SELECT;
  if (hw & HW_MASK_START) nes_data |= INP_PAD_START;
  if (hw & HW_MASK_UP) nes_data |= INP_PAD_UP;
  if (hw & HW_MASK_DOWN) nes_data |= INP_PAD_DOWN;
  if (hw & HW_MASK_LEFT) nes_data |= INP_PAD_LEFT;
  if (hw & HW_MASK_RIGHT) nes_data |= INP_PAD_RIGHT;
  joypad_p1.data = nes_data;
}

extern "C" int osd_init() {
  Serial.println("[OSD] Initializing...");
  Serial.printf("[OSD] Heap=%u, SPIRAM=%u\n", esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  
  Serial.println("[OSD] Initializing joypad...");
  joypad_p1.type = INP_JOYPAD0;
  joypad_p1.data = 0;

  // 输入设备只需要注册一次：nesinput.c 里那张静态表在换游戏时不会清空，
  // 每局都 input_register() 的话，来回切 32 局之后就会把 nes_input[] 写爆。
  static bool joypad_registered = false;
  if (!joypad_registered) {
    input_register(&joypad_p1);
    joypad_registered = true;
    Serial.println("[OSD] Input registered successfully");
  }
  
  if (osd_init_sound() != 0) { 
    Serial.println("[OSD] Sound Init FAILED"); 
  }
  
  Serial.printf("[OSD] Post-init Heap=%u, SPIRAM=%u\n", esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  Serial.println("[OSD] Initialization complete");
  return 0;
}

extern "C" void osd_shutdown() {
  if (nes_timer_handle) {
    esp_timer_stop(nes_timer_handle);
    esp_timer_delete(nes_timer_handle);
    nes_timer_handle = NULL;
  }
}

extern "C" int osd_installtimer(int freq, void *func, int func_param, void *func2, int func2_param) {
  // 每进一局游戏都会装一个新定时器；上一局留下的那个必须停掉删掉。
  // 否则多个 60Hz 定时器会同时往 nofrendo_ticks 里加，切过一次游戏之后
  // frames_to_render 就会翻倍 —— 表现是画面明显丢帧、音频也少一半。
  if (nes_timer_handle) {
    esp_timer_stop(nes_timer_handle);
    esp_timer_delete(nes_timer_handle);
    nes_timer_handle = NULL;
  }

  emu_timer_callback = (void (*)(void))func;
  const esp_timer_create_args_t timer_args = { .callback = &timer_callback_handler, .name = "nes_timer" };
  if (esp_timer_create(&timer_args, &nes_timer_handle) != ESP_OK) {
    nes_timer_handle = NULL;
    return -1;
  }
  esp_timer_start_periodic(nes_timer_handle, 1000000 / freq);
  return 0;
}

extern "C" int osd_gettime(void) {
  return millis();
}

extern "C" int osd_makesnapname(char *buf, int len) {
  return 0;
}

extern "C" void osd_getmouse(int *x, int *y, int *button) {
  *x = 0;
  *y = 0;
  *button = 0;
}

char *global_rom_data = NULL;
int global_rom_size = 0;

// ---- ROM 改成从 SD 卡流式读取（ROM 不再烧进 flash 分区）----
// nofrendo 的 rom_load() 走 osd_rom_open/read/close 这条流式接口，
// 每次 read 都会落到 FATFS → SD 卡 SPI 事务上，所以这里不要持有 SPI 总线。
static File                   rom_file;
static unsigned long          rom_read_total_us = 0;
static unsigned long          rom_open_ms = 0;

// 由 .ino 在调用 nofrendo_main() 之前设置，用于统计"启动游戏到第一帧"的耗时
unsigned long osd_game_start_ms = 0;

// 旧的整块预加载接口：本项目用 SD 流式读取，保留只为匹配 osd.h 的接口约定
extern "C" bool vid_preload_rom(const char *path) {
  Serial.printf("[ROM] vid_preload_rom(%s) 被调用，但本项目直接从 SD 卡流式读取，已忽略\n",
                path ? path : "(null)");
  return false;
}

extern "C" void IRAM_ATTR osd_blit(bitmap_t *bmp) {
  if (!video_ready || !bmp || !bmp->line || !bmp->line[0] || !frame_buffer) {
    return;
  }
  
  // 240 宽面板：跳过 NES 画面左右各 NES_X_CROP 列，只渲染中间的 NES_VISIBLE_WIDTH 列
  for (int y = 0; y < NES_SCREEN_HEIGHT; y++) {
    uint8_t *src = bmp->line[y] + NES_X_CROP;
    uint16_t *dst = &frame_buffer[y * NES_VISIBLE_WIDTH];

    for (int x = 0; x < NES_VISIBLE_WIDTH; x++) {
      dst[x] = myPalette565_swapped[src[x]];
    }
  }
}

// 流式渲染：每次只把 STREAM_ROWS 行转换到内部 RAM 的块缓冲，立刻通过 SPI 发出去。
// 不再经过 PSRAM 的整帧帧缓冲 —— PSRAM 读写慢，而且 SPI DMA 也访问不到它。
#define STREAM_ROWS 60
static uint16_t stream_buf[STREAM_ROWS * NES_VISIBLE_WIDTH];  // 60*240*2 = 28800 字节

extern "C" void vid_flush() {
  frame_start_time = micros();

  // 首帧时间点：从按 A 键开始加载到最后真正画出第一帧的耗时
  static bool first_frame_logged = false;
  if (!first_frame_logged && osd_game_start_ms != 0) {
    first_frame_logged = true;
    Serial.printf("[PERF] 启动游戏 -> 第一帧: %lu ms (ROM 读取累计 %luus)\n",
                  (unsigned long)(millis() - osd_game_start_ms), rom_read_total_us);
  }

  unsigned long t1 = micros();

  // 不要在这里按墙上时间节流：外层 nes_emulate() 已经用 60Hz 定时器决定了
  // "这一帧该不该显示"，这里再判断一次只会把定时器要显示的帧又丢掉。
  if (game_bitmap && game_bitmap->line && game_bitmap->line[0]) {
    for (int y = 0; y < NES_SCREEN_HEIGHT; y += STREAM_ROWS) {
      int rows = NES_SCREEN_HEIGHT - y;
      if (rows > STREAM_ROWS) {
        rows = STREAM_ROWS;
      }

      // 转 STREAM_ROWS 行 RGB565 到内部 RAM
      for (int r = 0; r < rows; r++) {
        const uint8_t *src = game_bitmap->line[y + r] + NES_X_CROP;
        uint16_t *dst = &stream_buf[r * NES_VISIBLE_WIDTH];
        for (int x = 0; x < NES_VISIBLE_WIDTH; x++) {
          dst[x] = myPalette565_swapped[src[x]];
        }
      }

      // 每块自带窗口设置。实测比"整帧只设一次窗口"更快：
      // 后者把 CS 一直拉低，ST7789 反而处理不好，帧率明显下降。
      tft.pushImage(0, y, NES_VISIBLE_WIDTH, rows, stream_buf, true);
    }
  }

  unsigned long push_t = micros() - t1;
  (void)push_t;

  // 游戏过程中默认完全不输出串口。
  // ESP32-S3 的 USB-Serial-JTAG：主机端没及时取走数据时，USB FIFO 一满，
  // Serial.printf 会阻塞等待，表现出来就是"玩一会儿卡一下"的周期性停顿。
  // 需要测帧率时把 DEBUG_FPS_LOG 改成 1。
#ifndef DEBUG_FPS_LOG
#define DEBUG_FPS_LOG 0
#endif
#if DEBUG_FPS_LOG
  static unsigned long last_print = 0;
  if (millis() - last_print > 10000) {
    Serial.printf("push=%luus emul_fps=%d\n", push_t, current_fps);
    last_print = millis();
  }
#endif

  if (runtime_sound_enabled && emulator_audio_callback) {
    int samples_needed = audio_sample_rate / 60;
    if (samples_needed < 1) samples_needed = 1;
    if (samples_needed > AUDIO_MAX_SAMPLES) samples_needed = AUDIO_MAX_SAMPLES;

    memset(mono_buffer, 0, samples_needed * 2);
    // 回调是 apu_process()：按 int16 单声道填 samples_needed 个样本
    emulator_audio_callback(mono_buffer, samples_needed);

#if ENABLE_SOUND
    for (int i = 0; i < samples_needed; i++) {
      int32_t sample = mono_buffer[i];

      sample = (sample * master_volume) / 100;
      sample = (sample * AUDIO_GAIN) / 100;

      if (sample > 32767) sample = 32767;
      if (sample < -32768) sample = -32768;

      mono_buffer[i] = (int16_t)sample;
    }

    // MAX98357A 期望标准立体声 I2S 帧：把单声道样本复制到左右两个 slot 再写。
    // I2S 的 DMA 缓冲有上百毫秒，正常情况下 write() 立刻返回；
    // 只有音频缓冲满了（模拟器跑得比实时快）才会短暂阻塞，正好用来压帧率。
    i2s_write_mono_as_stereo(mono_buffer, samples_needed);
#endif
  }

  frame_count++;
  if (millis() - last_fps_update >= 1000) {
    current_fps = frame_count;
    frame_count = 0;
    last_fps_update = millis();
  }
}

extern "C" int vid_init(int width, int height, viddriver_t *osd_driver) {
  if (!game_bitmap) game_bitmap = bmp_create(NES_SCREEN_WIDTH, NES_SCREEN_HEIGHT, 0); 
  frame_buffer = NULL;
  low_mem_video_mode = false;
  video_ready = (game_bitmap && game_bitmap->data);
  Serial.printf("vid_init heap snapshot: heap=%u spiram=%u\n", esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  Serial.printf("vid_init: bitmap %p, frame_buffer %p, ready %d, deferred_fb_alloc=1\n", game_bitmap, frame_buffer, video_ready);
  return video_ready ? 0 : -1;
}

// ---- 从 SD 卡打开 ROM ----
// path 是 SD 卡上的完整路径，例如 "/Contra (U).nes"（由 scan_games() 写进 labels[]）。
extern "C" int osd_rom_open(const char *path) {
  osd_rom_close();

  if (!path || path[0] == '\0') {
    Serial.println("[ROM] osd_rom_open: path 为空");
    return -1;
  }

  unsigned long t0 = millis();
  rom_file = SD.open(path, FILE_READ);
  if (!rom_file) {
    Serial.printf("[ROM] SD 卡上打不开: %s\n", path);
    return -1;
  }

  size_t size = rom_file.size();
  if (size < 16) {
    Serial.printf("[ROM] 文件太小 (%u 字节): %s\n", (unsigned)size, path);
    rom_file.close();
    return -1;
  }

  // 只做个基本检查：合法的 iNES 文件头是 "NES\x1A"。
  // 不是的话也照样往下走（让 nofrendo 自己报错），但先提示一句，
  // 方便排查"SD 卡里放了个压缩包/坏文件"这种问题。
  uint8_t hdr[4];
  int n = rom_file.read(hdr, sizeof(hdr));
  rom_file.seek(0);
  if (n != (int)sizeof(hdr) || memcmp(hdr, "NES\x1A", 4) != 0) {
    Serial.printf("[ROM] 警告: %s 不是标准 iNES 文件（前 4 字节不是 NES\\x1A）\n", path);
  }

  rom_open_ms = millis() - t0;
  Serial.printf("[ROM] 打开 %s: %u 字节, 耗时 %lums (上次读取累计 %luus)\n",
                path, (unsigned)size, rom_open_ms, rom_read_total_us);
  rom_read_total_us = 0;
  return 0;
}

// nofrendo 的 rom_read_exact() 要求"要么读满 len 字节，要么算失败"，
// 所以这里必须循环读到 len（File::read 在跨越簇/遇到短读时可能一次读不满）。
extern "C" int osd_rom_read(void *dst, int len) {
  if (!rom_file || !dst || len <= 0) {
    return -1;
  }

  unsigned long t0 = micros();
  int total = 0;
  uint8_t *p = (uint8_t *)dst;
  while (total < len) {
    int n = rom_file.read(p + total, len - total);
    if (n <= 0) {
      break;
    }
    total += n;
  }
  rom_read_total_us += micros() - t0;
  return total;
}

extern "C" void osd_rom_close(void) {
  if (rom_file) {
    rom_file.close();
  }
}

extern "C" void vid_shutdown() {
  video_ready = false;
}

extern "C" int vid_setmode(int width, int height) {
  return 0;
}

extern "C" void vid_setpalette(rgb_t *pal) {
  osd_setpalette(pal);
}

extern "C" bitmap_t *vid_getbuffer() {
  if (!game_bitmap) game_bitmap = bmp_create(NES_SCREEN_WIDTH, NES_SCREEN_HEIGHT, 0);
  video_ready = (game_bitmap && game_bitmap->data);
  return game_bitmap;
}

extern "C" void osd_getvideoinfo(vidinfo_t *info) {
  info->default_width = NES_SCREEN_WIDTH;
  info->default_height = NES_SCREEN_HEIGHT;
  info->driver = 0;
}

extern "C" void osd_togglefullscreen(int code) {}

extern "C" char *osd_newextension(char *string, char *ext) {
  return string;
}

extern "C" void osd_fullname(char *fullname, const char *shortname) {
  strcpy(fullname, shortname);
}

extern "C" int osd_main(int argc, char *argv[]) {
  runtime_sound_enabled = false;
  for (int i = 1; i < argc; i++) {
    if (!argv[i]) continue;
    if (strcmp(argv[i], "-sound") == 0) {
      runtime_sound_enabled = true;
    }
    if (strcmp(argv[i], "-nosound") == 0) {
      runtime_sound_enabled = false;
    }
    // -volume N / -sample N：在 osd_init_sound() 和 osd_getsoundinfo() 之前生效，
    // 所以必须在 main_loop() 之前解析完。
    if (strcmp(argv[i], "-volume") == 0 && i + 1 < argc && argv[i + 1]) {
      int v = atoi(argv[i + 1]);
      if (v < 0) v = 0;
      if (v > 100) v = 100;
      master_volume = v;
      menu_volume = v;
      i++;
    }
    if (strcmp(argv[i], "-sample") == 0 && i + 1 < argc && argv[i + 1]) {
      int s = atoi(argv[i + 1]);
      if (s >= 8000 && s <= 48000) {
        audio_sample_rate = s;
      }
      i++;
    }
  }

  // 音量：设置页保存过就优先用保存值（.ino 传的 -volume 只当默认值）
  {
    int saved_vol = load_volume_from_nvs();
    if (saved_vol >= 0) {
      master_volume = saved_vol;
      menu_volume = saved_vol;
    }
  }

  Serial.printf("[OSD] sound=%d volume=%d sample_rate=%d\n",
                runtime_sound_enabled ? 1 : 0, master_volume, audio_sample_rate);

  char *rom_name = "";
  if (argc > 0) rom_name = argv[argc - 1];
  return main_loop(rom_name, system_nes);
}

extern "C" int nofrendo_log_init(void) {
  return 0;
}

extern "C" void nofrendo_log_shutdown(void) {}

extern "C" int nofrendo_log_print(const char *s) {
  if (s) {
    Serial.print(s);
  }
  return 0;
}

extern "C" int nofrendo_log_printf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  int written = Serial.vprintf(format, args);
  va_end(args);
  return written;
}

extern "C" void nofrendo_log_assert(int expr, int line, const char *file, char *msg) {
  if (!expr) {
    Serial.printf("ASSERT FAILED: %s:%d %s\n", file ? file : "?", line, msg ? msg : "");
  }
}

