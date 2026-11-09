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

static void draw_cn_glyph(int x, int y, const cn_glyph_t *g, uint16_t fg, uint16_t bg) {
  static uint16_t line[CN_GLYPH_W];
  for (int row = 0; row < CN_GLYPH_H; row++) {
    uint32_t v = 0;
    for (int k = 0; k < CN_GLYPH_BYTES_PER_ROW; k++) {
      v = (v << 8) | g->bits[row * CN_GLYPH_BYTES_PER_ROW + k];
    }
    for (int col = 0; col < CN_GLYPH_W; col++) {
      line[col] = (v & (1u << (CN_GLYPH_W - 1 - col))) ? fg : bg;
    }
    tft.pushImage(x, y + row, CN_GLYPH_W, 1, line);
  }
}

// 中英混排绘制，返回实际宽度
static int draw_cn_text(int x, int y, const char *utf8, uint16_t fg, uint16_t bg) {
  int cx = x;
  const char *s = utf8;
  while (*s) {
    uint16_t code = utf8_next(&s);
    if (code == 0) break;
    if (code < 0x80) {
      if (code == ' ') {
        cx += 10;
        continue;
      }
      tft.drawChar(cx, y + (CN_GLYPH_H - 7) / 2, (char)code, fg, bg, 1);
      cx += 7;
    } else {
      const cn_glyph_t *g = find_cn_glyph(code);
      if (g) {
        draw_cn_glyph(cx, y, g, fg, bg);
      } else {
        // 字库里没有这个字，画个空心方块占位，方便发现漏字
        tft.drawFilledRect(cx + 2, y + 2, CN_GLYPH_W - 4, CN_GLYPH_H - 4, fg);
      }
      cx += CN_GLYPH_W;
    }
  }
  return cx - x;
}

// 水平居中绘制
static void draw_cn_center(int y, const char *utf8, uint16_t fg, uint16_t bg) {
  int w = cn_text_width(utf8);
  draw_cn_text((DISPLAY_WIDTH - w) / 2, y, utf8, fg, bg);
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

// ---------------- 界面配色（iOS 深色模式取色，转 RGB565）----------------
#define UI_BG       0x0000   // #000000 背景
#define UI_CARD     0x18E3   // #1C1C1E 分组卡片
#define UI_CARD_SEL 0x2965   // #2C2C2E 选中行卡片
#define UI_SEP      0x39C7   // #38383A 分隔线
#define UI_TXT      0xFFFF   // #FFFFFF 主文字
#define UI_TXT2     0x8C72   // #8E8E93 次要文字
#define UI_ACCENT   0x0C3F   // #0A84FF iOS 系统蓝
#define UI_HOMEIND  0x39E7   // #3A3A3C Home 指示条

// ---------------- 多语言 ----------------
// 中英文各自用最合适的字库：汉字走 16x16 点阵，英文走 5x7 小字库（7px 宽），
// 这样英文能在一行里塞下更长的提示，不用为了对齐牺牲内容。
typedef enum { LANG_ZH = 0, LANG_EN = 1, LANG_COUNT = 2 } ui_lang_t;

static ui_lang_t ui_lang = LANG_ZH;

static const char *TXT_TITLE[LANG_COUNT]     = { "游戏",           "GAMES" };
static const char *TXT_HINT[LANG_COUNT]      = { "上下选择  A开始", "UP/DN: SELECT   A: PLAY" };
static const char *TXT_NO_GAMES[LANG_COUNT]  = { "未找到游戏",     "NO GAMES FOUND" };
static const char *TXT_BACK[LANG_COUNT]      = { "返回菜单",       "BACK TO MENU" };
static const char *TXT_GAME[LANG_COUNT]      = { "游戏",           "GAME" };
static const char *TXT_LANG_TITLE[LANG_COUNT] = { "选择语言",      "SELECT LANGUAGE" };
static const char *TXT_LANG_HINT[LANG_COUNT] = { "上下选择  A确认", "UP/DN: MOVE   A: OK" };
// 设置页 + 页面切换
static const char *TXT_SETTINGS[LANG_COUNT]  = { "设置",           "SETTINGS" };
static const char *TXT_LANG_ITEM[LANG_COUNT] = { "语言",           "LANGUAGE" };
static const char *TXT_VOL_ITEM[LANG_COUNT]  = { "音量",           "VOLUME" };
static const char *TXT_HINT_CFG[LANG_COUNT]  = { "上下选择 左右调节", "UP/DN: MOVE   L/R: ADJUST" };
static const char *TXT_HINT_SEL[LANG_COUNT]  = { "SELECT 返回",    "SELECT: BACK" };
static const char *TXT_HINT_PAGE[LANG_COUNT] = { "SELECT 切换页面", "SELECT: NEXT PAGE" };

// NVS 里存的键名与命名空间
#define PREFS_NAMESPACE "nes"
#define PREFS_KEY_LANG  "lang"
#define PREFS_KEY_VOL   "vol"

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

// 右侧的 ">" chevron 指示符
static void draw_chevron(int x, int y, uint16_t color) {
  for (int i = 0; i < 5; i++) {
    tft.drawFilledRect(x + i, y + i, 1, 1, color);
    tft.drawFilledRect(x + i, y + 8 - i, 1, 1, color);
  }
}

// 首次开机的语言选择界面。选项固定为「中文 / English」——
// 两种写法本身就能让对应语言的用户认出来，不需要额外的双语标签。
static void show_language_picker() {
  int selected = (ui_lang == LANG_EN) ? 1 : 0;

  auto draw_picker = [&]() {
    tft.fillScreen(UI_BG);
    draw_cn_center(18, TXT_LANG_TITLE[selected], UI_TXT, UI_BG);

    const int card_x = 10;
    const int card_w = DISPLAY_WIDTH - 20;
    const int y0 = 74;
    const int row_h = 44;

    fill_round_rect(card_x, y0, card_w, row_h * 2 + 4, 10, UI_CARD);

    for (int i = 0; i < 2; i++) {
      int y = y0 + 2 + i * row_h;
      bool sel = (i == selected);

      if (sel) {
        fill_round_rect(card_x + 2, y, card_w - 4, row_h, 8, UI_CARD_SEL);
        fill_round_rect(card_x + 4, y + 12, 4, row_h - 24, 2, UI_ACCENT);
      } else if (i == 0) {
        tft.drawFilledRect(card_x + 16, y + row_h - 1, card_w - 28, 1, UI_SEP);
      }

      const char *name = (i == 0) ? "中文" : "English";
      draw_cn_text(card_x + 20, y + (row_h - CN_GLYPH_H) / 2, name,
                   sel ? UI_TXT : UI_TXT2, sel ? UI_CARD_SEL : UI_CARD);
    }

    draw_cn_center(DISPLAY_HEIGHT - 30, TXT_LANG_HINT[selected], UI_TXT2, UI_BG);
    fill_round_rect(DISPLAY_WIDTH / 2 - 30, DISPLAY_HEIGHT - 8, 60, 3, 2, UI_HOMEIND);
  };

  draw_picker();

  unsigned long last_input = 0;
  unsigned long picker_start_ms = millis();   // 无按键时的超时兜底基准
  while (true) {
    int hw = nes_get_gamepad_state();
    if ((millis() - last_input) > 180) {
      if (hw & HW_MASK_UP) {
        selected = 0;
        draw_picker();
        last_input = millis();
      }
      if (hw & HW_MASK_DOWN) {
        selected = 1;
        draw_picker();
        last_input = millis();
      }
      if (hw & HW_MASK_A) {
        ui_lang = (ui_lang_t)selected;
        save_lang_to_nvs(ui_lang);
        tft.fillScreen(UI_BG);
        delay(120);
        return;
      }
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

// 把单声道样本复制成左右声道写入 I2S（samples ≤ AUDIO_MAX_SAMPLES）
static void i2s_write_mono_as_stereo(const int16_t *mono, int samples) {
  int n = 0;
  for (int i = 0; i < samples; i++) {
    stereo_buffer[n++] = mono[i];
    stereo_buffer[n++] = mono[i];
  }
  i2s_audio.write((const uint8_t *)stereo_buffer, (size_t)n * 2);
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
bool menu_brightness = true;

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

// 注：draw_menu() / draw_settings() 是原项目遗留的整屏绘制函数，
// 实际使用的菜单逻辑在 show_menu() 里内联实现。这里一并改成中文，保持一致。
void draw_menu() {
  tft.fillScreen(0x0000);
  delay(100);

  tft.drawFilledRect(0, 0, DISPLAY_WIDTH, 34, 0xF800);
  delay(50);

  draw_cn_center(9, "游戏模拟器", 0xFFFF, 0xF800);

  int start_y = 50;
  for (int i = 0; i < game_list.count && i < 3; i++) {
    uint16_t color = (i == game_list.selected) ? 0xF800 : 0xE71C;
    int w = cn_text_width(game_list.names[i]);
    draw_cn_text((DISPLAY_WIDTH - w) / 2, start_y + (i * 36), game_list.names[i], color, 0x0000);
  }

  draw_cn_center(170, "上下选择", 0x07E0, 0x0000);
  draw_cn_center(196, "开始游戏", 0x07E0, 0x0000);
  delay(100);
}

void draw_settings() {
  tft.fillScreen(0x0000);
  delay(50);

  tft.drawFilledRect(0, 0, DISPLAY_WIDTH, 34, 0x001F);

  draw_cn_center(9, "设置", 0xFFFF, 0x001F);

  draw_cn_center(60, "音量", 0xFFFF, 0x0000);
  char vol_str[10];
  snprintf(vol_str, 10, "%d%%", menu_volume);
  draw_cn_center(86, vol_str, 0x07E0, 0x0000);

  draw_cn_center(170, "上下调节", 0x07E0, 0x0000);
  draw_cn_center(196, "按键保存", 0x07E0, 0x0000);
}

// ---------------- 设置页 ----------------
// 从游戏列表按 SELECT 进入；上下选项目、左右调值、SELECT 返回。
// 调整立即写 NVS，下次开机保持。
static void show_settings_page() {
  // 等 SELECT 松开，避免刚进来就被仍按着的 SELECT 立刻弹回
  {
    unsigned long t0 = millis();
    while ((millis() - t0) < 1500) {
      if (!(nes_get_gamepad_state() & HW_MASK_SELECT)) break;
      delay(20);
    }
  }

  int sel = 0;
  bool dirty = true;
  bool first_paint = true;
  unsigned long last_input = 0;

  const int CARD_X = 10;
  const int CARD_W = DISPLAY_WIDTH - 20;
  const int ROW_Y0 = 58;
  const int ROW_H = 44;

  for (;;) {
    if (dirty) {
      // 只有第一次进来才全屏清一次；之后调值/换选项只重绘两行卡片，
      // 否则每按一下都全屏重绘（115KB / 约 23ms）会明显闪。
      if (first_paint) {
        tft.fillScreen(UI_BG);
        draw_cn_center(18, TXT_SETTINGS[ui_lang], UI_TXT, UI_BG);
        // 底部提示在语言切换后也要重画，所以不放 first_paint 里
        first_paint = false;
      }

      for (int i = 0; i < 2; i++) {
        int y = ROW_Y0 + i * ROW_H;
        bool on = (i == sel);
        uint16_t bg = on ? UI_CARD_SEL : UI_CARD;
        uint16_t fg = on ? UI_TXT : UI_TXT2;

        if (on) {
          fill_round_rect(CARD_X + 2, y, CARD_W - 4, ROW_H - 6, 8, bg);
          fill_round_rect(CARD_X + 6, y + 11, 4, ROW_H - 28, 2, UI_ACCENT);
        } else {
          tft.drawFilledRect(CARD_X + 2, y, CARD_W - 4, ROW_H - 6, bg);
        }

        const char *label = (i == 0) ? TXT_LANG_ITEM[ui_lang] : TXT_VOL_ITEM[ui_lang];
        draw_cn_text(CARD_X + 18, y + 9, label, fg, bg);

        char val[16];
        if (i == 0) {
          snprintf(val, sizeof(val), "%s", ui_lang == LANG_ZH ? "中文" : "English");
        } else {
          snprintf(val, sizeof(val), "%d%%", menu_volume);
        }
        int vw = cn_text_width(val);
        draw_cn_text(CARD_X + CARD_W - 16 - vw, y + 9, val, on ? UI_ACCENT : UI_TXT2, bg);
      }

      // 底部提示用背景色覆盖再写（语言可能刚被切换）
      tft.drawFilledRect(0, DISPLAY_HEIGHT - 52, DISPLAY_WIDTH, 52, UI_BG);
      draw_cn_center(DISPLAY_HEIGHT - 46, TXT_HINT_CFG[ui_lang], UI_TXT2, UI_BG);
      draw_cn_center(DISPLAY_HEIGHT - 26, TXT_HINT_SEL[ui_lang], UI_TXT2, UI_BG);
      dirty = false;
    }

    int hw = nes_get_gamepad_state();
    if ((millis() - last_input) > 180) {
      if (hw & HW_MASK_UP) {
        sel = (sel + 1) % 2;
        dirty = true;
        last_input = millis();
      }
      if (hw & HW_MASK_DOWN) {
        sel = (sel + 1) % 2;
        dirty = true;
        last_input = millis();
      }
      if (hw & HW_MASK_LEFT) {
        if (sel == 0) {
          ui_lang = (ui_lang_t)((ui_lang + LANG_COUNT - 1) % LANG_COUNT);
          save_lang_to_nvs(ui_lang);
        } else {
          menu_volume = (menu_volume >= 10) ? (menu_volume - 10) : 0;
          master_volume = menu_volume;
          save_volume_to_nvs(menu_volume);
        }
        dirty = true;
        last_input = millis();
      }
      if (hw & HW_MASK_RIGHT) {
        if (sel == 0) {
          ui_lang = (ui_lang_t)((ui_lang + 1) % LANG_COUNT);
          save_lang_to_nvs(ui_lang);
        } else {
          menu_volume = (menu_volume <= 90) ? (menu_volume + 10) : 100;
          master_volume = menu_volume;
          save_volume_to_nvs(menu_volume);
        }
        dirty = true;
        last_input = millis();
      }
      if (hw & HW_MASK_SELECT) {
        unsigned long t0 = millis();
        while ((millis() - t0) < 1500) {
          if (!(nes_get_gamepad_state() & HW_MASK_SELECT)) break;
          delay(20);
        }
        // 不在这里清屏：show_menu() 的 needs_full_redraw 会统一重绘一次，
        // 两次 fillScreen 会让页面切换看起来"闪一下"。
        Serial.printf("[MENU] settings closed (lang=%d vol=%d)\n",
                      (int)ui_lang, menu_volume);
        return;
      }
    }
    delay(20);
  }
}

extern "C" int show_menu() {
  unsigned long t_menu = millis();
  Serial.println("\n[MENU] Starting menu system...");

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
  
  if (game_list.count == 0) {
    Serial.println("[MENU] SD 卡根目录下没有 .nes 文件（把 ROM 拷到卡根目录再开机）");
    tft.fillScreen(0x0000);
    draw_cn_center(DISPLAY_HEIGHT / 2 - 12, TXT_NO_GAMES[ui_lang], UI_TXT2, UI_BG);
    delay(3000);
    return -1;
  }
  
  game_list.selected = 0;
  int top_index = 0; 
  bool in_menu = true;
  unsigned long last_input = 0;
  int last_selected = -1;
  
  bool needs_full_redraw = true;
  bool needs_list_redraw = false;

  // 一屏显示 5 项，其余靠上下滚动
  const int VISIBLE_ROWS = 5;
  const int ROW_Y0 = 40;      // 卡片顶部 y（标题在 y=10..26）
  const int ROW_H = 32;       // 行高
  const int CARD_X = 10;
  const int CARD_W = DISPLAY_WIDTH - 20;

  auto visible_rows = [&]() {
    int n = game_list.count - top_index;
    if (n > VISIBLE_ROWS) n = VISIBLE_ROWS;
    return n < 0 ? 0 : n;
  };

  // 单行重绘：只覆盖这一行，用于选中项移动，避免整块刷新造成的闪屏
  auto draw_one_row = [&](int si, bool sel) {
    int n = visible_rows();
    if (si < 0 || si >= n) return;
    int gi = top_index + si;
    int y = ROW_Y0 + 2 + si * ROW_H;

    if (sel) {
      fill_round_rect(CARD_X + 2, y, CARD_W - 4, ROW_H, 8, UI_CARD_SEL);
      fill_round_rect(CARD_X + 4, y + 7, 4, ROW_H - 14, 2, UI_ACCENT);
    } else {
      // 用卡片底色直角覆盖，把上一次的圆角高亮抹平
      tft.drawFilledRect(CARD_X + 2, y, CARD_W - 4, ROW_H, UI_CARD);
      bool next_sel = ((top_index + si + 1) == game_list.selected);
      if (si < n - 1 && !next_sel) {
        tft.drawFilledRect(CARD_X + 16, y + ROW_H - 1, CARD_W - 28, 1, UI_SEP);
      }
    }

    draw_cn_text(CARD_X + 16, y + 8, game_list.names[gi],
                 sel ? UI_TXT : UI_TXT2, sel ? UI_CARD_SEL : UI_CARD);
    draw_chevron(CARD_X + CARD_W - 18, y + 12, UI_TXT2);
  };

  // 整块重绘。注意用卡片底色覆盖，绝不能用背景黑清空 ——
  // 那样会先看到一次"擦掉再重画"，表现就是切换选项闪一下黑。
  auto draw_list = [&]() {
    int n = visible_rows();
    if (n <= 0) return;
    fill_round_rect(CARD_X, ROW_Y0, CARD_W, ROW_H * n + 4, 10, UI_CARD);
    for (int i = 0; i < n; i++) {
      draw_one_row(i, (top_index + i) == game_list.selected);
    }
  };

  while (in_menu) {
    if (needs_full_redraw) {
      tft.fillScreen(UI_BG);

      // 左上大标题 + 右上角数量
      draw_cn_text(14, 10, TXT_TITLE[ui_lang], UI_TXT, UI_BG);
      char cnt[8];
      snprintf(cnt, sizeof(cnt), "%d", game_list.count);
      draw_cn_text(DISPLAY_WIDTH - 14 - (int)strlen(cnt) * 7, 12, cnt, UI_TXT2, UI_BG);

      draw_list();

      // 底部提示 + Home 指示条
      draw_cn_center(DISPLAY_HEIGHT - 30, TXT_HINT[ui_lang], UI_TXT2, UI_BG);
      fill_round_rect(DISPLAY_WIDTH / 2 - 30, DISPLAY_HEIGHT - 8, 60, 3, 2, UI_HOMEIND);

      last_selected = game_list.selected;
      needs_full_redraw = false;
      needs_list_redraw = false;
    } 
    else if (needs_list_redraw) {
      draw_list();
      last_selected = game_list.selected;
      needs_list_redraw = false;
    }
    else if (last_selected != game_list.selected) {
      // 只重绘变化的这两行，其余行不动 —— 这是消除切换闪烁的关键
      int n = visible_rows();
      if (last_selected >= top_index && last_selected < top_index + n) {
        draw_one_row(last_selected - top_index, false);
      }
      if (game_list.selected >= top_index && game_list.selected < top_index + n) {
        draw_one_row(game_list.selected - top_index, true);
      }
      last_selected = game_list.selected;
    }
    
    int hw = nes_get_gamepad_state();

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
    
    if ((millis() - last_input) > 150) {
      if (hw & HW_MASK_UP) {
        if (game_list.selected > 0) {
          game_list.selected--;
          if (game_list.selected < top_index) {
            top_index = game_list.selected;
            needs_list_redraw = true; 
          }
          last_input = millis();
        }
      }
      if (hw & HW_MASK_DOWN) {
        if (game_list.selected < game_list.count - 1) {
          game_list.selected++;
          if (game_list.selected >= top_index + VISIBLE_ROWS) {
            top_index = game_list.selected - (VISIBLE_ROWS - 1);
            needs_list_redraw = true;
          }
          last_input = millis();
        }
      }
      if (hw & HW_MASK_A) {
        tft.fillScreen(0x0000);  
        delay(120);
        return game_list.selected;
      }

      // SELECT：进入设置页（语言 / 音量）。菜单里 SELECT 没有其它用途，
      // 游戏内它仍然是 NES 手柄的 SELECT 键（见 osd_getinput）。
      if (hw & HW_MASK_SELECT) {
        show_settings_page();
        needs_full_redraw = true;
        last_input = millis();
      }

      // 长按 START 0.6 秒切换界面语言（START 在菜单里没有其它用途）。
      // 切换后立刻存进 NVS，下次开机会沿用。
      static uint32_t lang_hold_since = 0;
      if (hw & HW_MASK_START) {
        if (lang_hold_since == 0) {
          lang_hold_since = millis();
        } else if (millis() - lang_hold_since > 600) {
          lang_hold_since = 0;
          ui_lang = (ui_lang == LANG_ZH) ? LANG_EN : LANG_ZH;
          save_lang_to_nvs(ui_lang);
          Serial.printf("[I18N] 切换语言 -> %s\n", ui_lang == LANG_EN ? "en" : "zh");
          tft.fillScreen(UI_BG);
          needs_full_redraw = true;
          last_input = millis();
          delay(250);
        }
      } else {
        lang_hold_since = 0;
      }

      // 原项目这里是 SELECT 键进入「音量设置」页，本移植仍然没有设置页：
      // SELECT 在菜单里不占用途，游戏内它还是 NES 手柄的 SELECT 键（见 osd_getinput）。
      // 音量目前由启动参数 -volume 决定（.ino 里传 100）。
    }
    delay(20);
  }
  return -1;
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
  // 每局游戏 nofrendo_main() 都会走到这里；I2S 只初始化一次，
  // 重复 begin() 会把上一个 DMA 通道拆掉重建，切歌时容易爆音。
  static bool i2s_ready = false;
  if (i2s_ready) {
    return 0;
  }

  Serial.printf("Initializing I2S audio (BCK=%d WS=%d DO=%d, %d Hz stereo)...\n",
                I2S_BCK, I2S_WS, I2S_DO, audio_sample_rate);

  // MAX98357A 是标准 I2S 器件：必须用立体声帧（每帧左+右两个 16-bit slot）。
  // 用 MONO 会让 BCLK 减半、功放解析错乱（底噪 + 喇叭吃直流发烫）。
  i2s_audio.setPins(I2S_BCK, I2S_WS, I2S_DO);
  // 每帧只写 ~1/60 秒的样本，正常情况下立刻返回。
  // 给 50ms 上限是防止 I2S 卡住时把整个模拟器拖死。
  i2s_audio.setTimeout(50);

  if (!i2s_audio.begin(I2S_MODE_STD, audio_sample_rate,
                       I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.printf("I2S init failed: %d\n", i2s_audio.lastError());
    return -1;
  }
  i2s_ready = true;
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

