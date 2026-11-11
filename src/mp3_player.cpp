// MP3 播放：SD 卡 → minimp3 解码 → I2S。
//
// 【为什么必须开独立任务】
//   mp3dec_decode_frame() 内部有一个 mp3dec_scratch_t **局部变量**，约 14KB：
//       float grbuf[2][576]   (4608 B)
//       float syn[33][64]     (8448 B)
//       uint8_t maindata[1022]
//   全在栈上。Arduino 的 loopTask 只有 8KB 栈，直接在 setup()/loop() 里调用会
//   触发 "Stack canary watchpoint triggered (loopTask)" 然后无限重启。
//   所以解码跑在下面这个 32KB 栈的专用任务里。
//
// 【解码性能】实测（320kbps CBR / 44.1kHz / 立体声，真板子）：
//   9.66x 实时，CPU 约 10%。余量足够，不需要降采样。

#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <math.h>

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include "mp3_player.h"

// ---- osd.cpp 提供的 I2S 抽象（带采样率跟踪） ----
extern "C" int  osd_i2s_begin(int rate);
extern "C" void osd_i2s_end(void);
extern "C" int  osd_i2s_write(const void *data, int bytes);

extern int master_volume;          // 0..100，设置页里改

#define MP3_NAME_MAX    48
#define MP3_PATH_MAX    80
#define MP3_MAX_TRACKS  512
#define MP3_TASK_STACK  32768
// 读缓冲 32KB：一次只消费一帧（~1.4KB），但**绝对不能每帧都去读卡**。
// ESP_I2S 的 DMA 是 6×240 帧 ≈ 33ms，SD 卡一次随机读的延迟经常到十几 ms，
// 每帧读一次迟早撞上 underflow（auto_clear 填 0，听感就是断续/咔哒）。
// 现在缓冲降到一半以下才补读，大约每 0.5 秒一次，单次 16KB ≈ 6~10ms，余量充足。
#define MP3_BUF_SIZE    32768
#define MP3_MIN_FRAME   1441       // Layer III 单帧最大字节数（320kbps）

// ---------------- 曲目表 ----------------
static int   s_count = 0;
static char (*s_names)[MP3_NAME_MAX] = NULL;
static char (*s_paths)[MP3_PATH_MAX] = NULL;
static bool  s_scanned = false;

// ---------------- 播放状态 ----------------
// UI 线程读、播放任务写。每个字段都是单字写，volatile 足够 ——
// 最坏情况是 UI 晚一帧看到新值，不会读到半截数据。
static volatile int  s_cur       = -1;     // 当前曲目索引
static volatile bool s_active    = false;  // 有曲子打开着（含暂停）
static volatile bool s_paused    = false;
static volatile int  s_progress  = 0;      // 0..100
static volatile bool s_task_busy = false;  // 任务正在处理（suspend 要等它）
static volatile int  s_req       = -1;     // 待打开的曲目
static volatile int  s_skip      = 0;      // +1 下一首 / -1 上一首

static TaskHandle_t s_task = NULL;

// ============================================================================
// 曲目扫描
// ============================================================================
static bool has_mp3_ext(const char *name) {
  size_t n = strlen(name);
  if (n < 4) return false;
  const char *e = name + n - 4;
  return e[0] == '.' && (e[1] == 'm' || e[1] == 'M') &&
         (e[2] == 'p' || e[2] == 'P') && (e[3] == '3');
}

void mp3_scan(void) {
  if (s_scanned) return;
  s_scanned = true;
  s_count = 0;

  File root = SD.open("/");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    Serial.println("[MP3] SD 根目录打不开");
    return;
  }

  s_names = (char (*)[MP3_NAME_MAX])heap_caps_malloc(
      (size_t)MP3_NAME_MAX * MP3_MAX_TRACKS, MALLOC_CAP_SPIRAM);
  s_paths = (char (*)[MP3_PATH_MAX])heap_caps_malloc(
      (size_t)MP3_PATH_MAX * MP3_MAX_TRACKS, MALLOC_CAP_SPIRAM);
  if (!s_names || !s_paths) {
    if (s_names) { heap_caps_free(s_names); s_names = NULL; }
    if (s_paths) { heap_caps_free(s_paths); s_paths = NULL; }
    root.close();
    Serial.println("[MP3] PSRAM 不足，建不了曲目表");
    return;
  }

  for (;;) {
    String name = root.getNextFileName();
    if (name.isEmpty()) break;
    const char *p = name.c_str();
    while (*p == '/') p++;
    if (!has_mp3_ext(p)) continue;
    if (s_count >= MP3_MAX_TRACKS) break;

    size_t n = strlen(p);
    if (n > MP3_NAME_MAX - 1) n = MP3_NAME_MAX - 1;
    memcpy(s_names[s_count], p, n);
    s_names[s_count][n] = '\0';

    // 显示名去掉 .mp3 后缀
    size_t dn = strlen(s_names[s_count]);
    if (dn >= 4 && has_mp3_ext(s_names[s_count])) {
      s_names[s_count][dn - 4] = '\0';
    }

    snprintf(s_paths[s_count], MP3_PATH_MAX, "/%s", p);
    s_count++;
  }
  root.close();
  Serial.printf("[MP3] indexed %d tracks\n", s_count);
}

int mp3_count(void) { return s_count; }

const char *mp3_name(int i) {
  if (!s_names || i < 0 || i >= s_count) return "";
  return s_names[i];
}

// ============================================================================
// 输出端处理：音色滤波（高通 + 可选低通）+ 削顶统计
// ============================================================================
//
// 三档音色（设置页可选；只作用于 MP3，游戏音效不走这条链）：
//   0 直通 —— 不滤波。留这一档才能听出"滤波到底改变了什么"，不是凑数
//   1 默认 —— 180Hz 二阶高通：小喇叭 200Hz 以下基本没有声压输出，但 D 类功放
//             （MAX98357A，9dB 固定增益）仍要为那段低频输出电流，能量全变成热，
//             滤掉能给中高频腾出一点动态余量
//   2 柔和 —— 默认 + 8kHz 二阶低通：高音发毛/刺时用它换柔顺，代价是镲片变糊
//
// 两个都是 RBJ cookbook 二阶（Q=0.7071，Butterworth），通带增益 1.0，
// 切档不会让整体音量跳变（切档瞬间可能有轻微咔哒，属于可接受范围）。

#define HP_FC_HZ 180.0f
#define LP_FC_HZ 8000.0f

typedef struct { float z1, z2; } bq_state_t;

static float s_hp_b0, s_hp_b1, s_hp_b2, s_hp_a1, s_hp_a2;
static bq_state_t s_hp_l, s_hp_r;
static float s_lp_b0, s_lp_b1, s_lp_b2, s_lp_a1, s_lp_a2;
static bq_state_t s_lp_l, s_lp_r;
static bool s_tone_ready = false;
static int  s_tone_fs    = 0;
static volatile int s_tone_mode = MP3_TONE_DEFAULT;
static volatile int32_t s_peak = 0;      // 本次播放的最大 |样本|，用于判断是否削顶

// RBJ cookbook 二阶：type 0 = 高通，1 = 低通
static void bq_design(int fs, float fc, int type,
                      float *b0, float *b1, float *b2, float *a1, float *a2) {
  const float q  = 0.7071f;
  const float w0 = 2.0f * (float)M_PI * fc / (float)fs;
  const float cw = cosf(w0);
  const float sw = sinf(w0);
  const float alpha = sw / (2.0f * q);
  const float a0 = 1.0f + alpha;

  if (type == 0) {                       // high-pass
    *b0 = ((1.0f + cw) * 0.5f) / a0;
    *b1 = -(1.0f + cw) / a0;
  } else {                               // low-pass
    *b0 = ((1.0f - cw) * 0.5f) / a0;
    *b1 = (1.0f - cw) / a0;
  }
  *b2 = *b0;
  *a1 = (-2.0f * cw) / a0;
  *a2 = (1.0f - alpha) / a0;
}

static void tone_init(int fs) {
  bq_design(fs, HP_FC_HZ, 0, &s_hp_b0, &s_hp_b1, &s_hp_b2, &s_hp_a1, &s_hp_a2);
  bq_design(fs, LP_FC_HZ, 1, &s_lp_b0, &s_lp_b1, &s_lp_b2, &s_lp_a1, &s_lp_a2);
  s_hp_l.z1 = s_hp_l.z2 = s_hp_r.z1 = s_hp_r.z2 = 0.0f;
  s_lp_l.z1 = s_lp_l.z2 = s_lp_r.z1 = s_lp_r.z2 = 0.0f;
  s_tone_ready = true;

  Serial.printf("[MP3] tone HP %.0f Hz / LP %.0f Hz @ %d Hz (mode %d)\n",
                (double)HP_FC_HZ, (double)LP_FC_HZ, fs, (int)s_tone_mode);
}

// Transposed Direct Form II：每样本 1 次乘加链，FPU 跑得动
static inline float bq_tick(bq_state_t *s, float x,
                            float b0, float b1, float b2, float a1, float a2) {
  float y = b0 * x + s->z1;
  s->z1 = b1 * x - a1 * y + s->z2;
  s->z2 = b2 * x - a2 * y;
  return y;
}

// ============================================================================
// 播放任务
// ============================================================================
static void mp3_task(void *arg) {
  // 这几个缓冲故意是 static：任务栈只有 32KB，而解码 scratch 就吃掉 14KB，
  // 把 PCM 和读缓冲放栈上会把余量吃光。
  static mp3dec_t dec;
  static int16_t  pcm[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];   // 交错立体声
  static uint8_t  rdbuf[MP3_BUF_SIZE];

  File   f;
  int    opened   = -1;
  size_t avail    = 0;        // rdbuf 里的有效字节
  size_t pos      = 0;        // 已消费偏移（软压缩，避免每帧 memmove）
  size_t file_len = 0;
  int    hz       = 0;

  auto close_file = [&]() {
    if (f) f.close();
    opened = -1;
    avail = 0;
    pos = 0;
    hz = 0;
  };

  for (;;) {
    // ---- 1. 切歌请求 ----
    int skip = s_skip;
    if (skip != 0) {
      s_skip = 0;
      if (s_count > 0) {
        int base = (s_cur >= 0) ? s_cur : 0;
        s_req = (base + skip + s_count) % s_count;
      }
    }

    int req = s_req;
    if (req >= 0) {
      s_req = -1;
      close_file();
      s_cur      = req;
      s_active   = true;
      s_paused   = false;
      s_progress = 0;
    }

    // ---- 2. 空闲 ----
    if (!s_active || s_cur < 0) {
      close_file();
      s_task_busy = false;
      vTaskDelay(pdMS_TO_TICKS(30));
      continue;
    }

    s_task_busy = true;

    // ---- 3. 打开文件 ----
    if (opened != s_cur) {
      close_file();
      // 上一首播完，报告这次观察到的最大样本值：接近 32767 说明在削顶，
      // 那时候唯一有效的办法就是继续降音量（或者改 MAX98357A 的 GAIN 脚）。
      if (s_peak > 0) {
        Serial.printf("[MP3] 上一首峰值 %ld/32767 (%d%%)\n",
                      (long)s_peak, (int)((int64_t)s_peak * 100 / 32767));
        s_peak = 0;
      }
      if (s_cur >= s_count) { s_active = false; continue; }
      f = SD.open(s_paths[s_cur], FILE_READ);
      if (!f) {
        Serial.printf("[MP3] 打开失败: %s\n", s_paths[s_cur]);
        s_active = false;
        continue;
      }
      file_len = f.size();
      mp3dec_init(&dec);
      opened = s_cur;
      Serial.printf("[MP3] play %s (%u B)\n", s_paths[s_cur], (unsigned)file_len);
    }

    // ---- 4. 暂停：继续灌静音，别让 DMA underflow 出咔哒声 ----
    if (s_paused) {
      static const int16_t sil[256] = {0};
      osd_i2s_write(sil, sizeof(sil));
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    // ---- 5. 补数据：缓冲掉到一半以下才碰 SD 卡 ----
    // 每帧都 f.read 会把 SD 的随机延迟直接暴露给 I2S（DMA 只有 ~33ms），
    // 撞上一次就是一小段静音填充，听感就是"断续/咔哒"。
    if (avail - pos < MP3_BUF_SIZE / 2) {
      if (pos > 0) {
        memmove(rdbuf, rdbuf + pos, avail - pos);
        avail -= pos;
        pos = 0;
      }
      while (avail < MP3_BUF_SIZE) {
        int n = f.read(rdbuf + avail, MP3_BUF_SIZE - avail);
        if (n <= 0) break;
        avail += n;
      }
    }
    if (avail - pos == 0) {
      // 文件读完：自动下一首
      if (s_count > 1) {
        s_skip = +1;
      } else {
        s_active = false;
        s_progress = 100;
      }
      continue;
    }

    // ---- 6. 解码一帧 ----
    mp3dec_frame_info_t info;
    int samples = mp3dec_decode_frame(&dec, rdbuf + pos, avail - pos, pcm, &info);

    if (info.frame_bytes == 0) {
      // 剩余数据凑不出一帧（文件尾的 padding），当作播完
      if (s_count > 1) {
        s_skip = +1;
      } else {
        s_active = false;
        s_progress = 100;
      }
      continue;
    }

    // 只推进消费指针，不做 memmove —— 压缩推迟到下次补数据时一起做
    pos += info.frame_bytes;

    if (samples <= 0) {
      continue;                      // 该帧不含音频（比如纯头信息）
    }

    // ---- 7. 第一次拿到采样率就配 I2S（歌曲可能不是 44.1k） ----
    if (hz == 0) {
      hz = info.hz;
      if (osd_i2s_begin(hz) != 0) {
        Serial.printf("[MP3] I2S 打开失败 (%d Hz)\n", hz);
        s_active = false;
        continue;
      }
    }

    // ---- 8. 先统一成交错立体声（后面高通和 I2S 都按立体声处理）----
    int nch = info.channels;
    if (nch == 1) {
      for (int i = samples - 1; i >= 0; i--) {   // 倒着原地扩展，避免覆盖
        pcm[i * 2]     = pcm[i];
        pcm[i * 2 + 1] = pcm[i];
      }
    }

    // ---- 9. 音色滤波 + 音量 + 削顶统计 ----
    //
    // 滤波只在采样率变化时重算系数（切歌换了 hz 才需要）；
    // 音色档位是全局变量，设置页随时可以改，下一帧就生效（可以边播边听差别）。
    if (!s_tone_ready || s_tone_fs != hz) {
      tone_init(hz);
      s_tone_fs = hz;
    }

    int vol = master_volume;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    const float gain = (float)vol / 100.0f;
    const int   tone = s_tone_mode;
    const bool  hp_on = (tone != MP3_TONE_FLAT);
    const bool  lp_on = (tone == MP3_TONE_SOFT);

    int32_t peak = 0;
    for (int i = 0; i < samples; i++) {
      float l = (float)pcm[i * 2];
      float r = (float)pcm[i * 2 + 1];

      if (hp_on) {
        l = bq_tick(&s_hp_l, l, s_hp_b0, s_hp_b1, s_hp_b2, s_hp_a1, s_hp_a2);
        r = bq_tick(&s_hp_r, r, s_hp_b0, s_hp_b1, s_hp_b2, s_hp_a1, s_hp_a2);
      }
      if (lp_on) {
        l = bq_tick(&s_lp_l, l, s_lp_b0, s_lp_b1, s_lp_b2, s_lp_a1, s_lp_a2);
        r = bq_tick(&s_lp_r, r, s_lp_b0, s_lp_b1, s_lp_b2, s_lp_a1, s_lp_a2);
      }
      l *= gain;
      r *= gain;

      int32_t li = (int32_t)l;
      int32_t ri = (int32_t)r;
      if (li > 32767) li = 32767; else if (li < -32768) li = -32768;
      if (ri > 32767) ri = 32767; else if (ri < -32768) ri = -32768;

      const int32_t al = (li < 0) ? -li : li;
      const int32_t ar = (ri < 0) ? -ri : ri;
      if (al > peak) peak = al;
      if (ar > peak) peak = ar;

      pcm[i * 2]     = (int16_t)li;
      pcm[i * 2 + 1] = (int16_t)ri;
    }
    if (peak > s_peak) s_peak = peak;

    // ---- 10. 写 I2S ----
    osd_i2s_write(pcm, samples * 4);

    // ---- 10. 进度（按已读文件字节估算，CBR 下准确，VBR 下近似） ----
    if (file_len > 0) {
      size_t rp = f.position();
      s_progress = (int)((uint64_t)rp * 100 / file_len);
      if (s_progress > 99) s_progress = 99;
    }
  }
}

static bool ensure_task(void) {
  if (s_task) return true;
  BaseType_t ok = xTaskCreatePinnedToCore(mp3_task, "mp3", MP3_TASK_STACK,
                                          NULL, 5, &s_task, 0);
  if (ok != pdPASS) {
    s_task = NULL;
    Serial.println("[MP3] 任务创建失败（内部 RAM 不够？）");
    return false;
  }
  Serial.printf("[MP3] decode task started (stack %d B)\n", MP3_TASK_STACK);
  return true;
}

// ============================================================================
// 对外控制
// ============================================================================
bool mp3_play_index(int i) {
  if (i < 0 || i >= s_count) return false;
  if (!ensure_task()) return false;
  s_req = i;
  return true;
}

void mp3_toggle(void) {
  if (!s_active || s_cur < 0) {
    // 没在放：从当前选中/第一首开始
    int i = (s_cur >= 0) ? s_cur : 0;
    mp3_play_index(i);
    return;
  }
  s_paused = !s_paused;
}

void mp3_next(void) { if (s_count > 0) s_skip = +1; }
void mp3_prev(void) { if (s_count > 0) s_skip = -1; }

void mp3_stop(void) {
  s_active = false;
  s_paused = false;
}

int  mp3_current(void)      { return s_cur; }
bool mp3_is_playing(void)   { return s_active && !s_paused; }
bool mp3_is_paused(void)    { return s_active && s_paused; }
int  mp3_progress_pct(void) { return s_progress; }

// 音色档位：只改一个 int，解码任务下一帧读到的就是新值，所以可以边播边切。
// 滤波器的系数不随档位变（两种模式共用同一组系数），所以切换不会重算、
// 也不会重置滤波器状态 —— 代价是切换瞬间波形有小跳变，可能听到一声轻咔哒。
void mp3_set_tone(int mode) {
  if (mode < 0 || mode >= MP3_TONE_COUNT) return;
  s_tone_mode = mode;
}

int mp3_get_tone(void) { return s_tone_mode; }

void mp3_suspend(void) {
  s_active = false;
  s_paused = false;
  // 等任务真的退出解码循环（它可能正阻塞在 osd_i2s_write 里）
  uint32_t t0 = millis();
  while (s_task_busy && millis() - t0 < 400) {
    delay(5);
  }
  s_task_busy = false;
  osd_i2s_end();
}

void mp3_resume(void) {
  // 出游戏后不自动接着放：用户在游戏里可能已经忘了刚才在听什么，
  // 强行续播会突兀。I2S 保持 osd_init_sound() 配置的 22050Hz，
  // 用户回到音乐页按 A 时播放器会自己重配回歌曲的采样率。
}
