// minimp3 在 ESP32-S3 上的解码速度基准。
//
// 目的：回答"自己做 MP3 播放行不行" —— 解码必须比实时快，否则要么丢帧要么没得玩。
// 这里把一段 5 秒 44.1kHz 立体声 128kbps 的 MP3 整段解码若干次，比较
// 「解码耗时」和「音频时长」。
//
// 判读：speed > 1.0 才能实时播放；建议 > 1.5 才留得出余量给 I2S 写入和 UI。
//
// 【必须跑在独立任务里】
//   mp3dec_decode_frame() 内部有 mp3dec_scratch_t 局部变量：
//     float grbuf[2][576] + float syn[33][64] + maindata[1022] + ...
//   ≈ 14KB 全在栈上。Arduino 的 loopTask 只有 8KB 栈，直接在 setup() 里调用
//   会触发 "Stack canary watchpoint triggered (loopTask)" 然后无限重启。
//   所以这里用 xTaskCreatePinnedToCore 开一个 32KB 栈的任务来跑。

#define MINIMP3_IMPLEMENTATION
#include <Arduino.h>
#include <esp_heap_caps.h>
#include "minimp3.h"
#include "test_mp3.h"

#define BENCH_STACK_BYTES 32768

static mp3dec_t g_dec;
static short     g_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

// 跑一遍完整解码，返回耗时（ms）。quiet=false 时打印这一轮的统计。
static float run_once(bool quiet, int *out_rate, unsigned long *out_samples) {
  mp3dec_init(&g_dec);

  size_t        pos     = 0;
  unsigned long samples = 0;
  int           rate    = 0;
  int           ch      = 0;
  int           frames  = 0;

  unsigned long t0 = micros();

  while (pos < test_mp3_len) {
    mp3dec_frame_info_t info;
    int n = mp3dec_decode_frame(&g_dec, test_mp3 + pos, test_mp3_len - pos, g_pcm, &info);
    if (info.frame_bytes == 0) {
      break;                       // 数据耗尽 / 坏帧，帧级 API 直接停
    }
    pos += info.frame_bytes;
    if (n > 0) {
      frames++;
      samples += (unsigned long)n;
      rate = info.hz;
      ch   = info.channels;
    }
  }

  unsigned long t1 = micros();
  float ms = (t1 - t0) / 1000.0f;

  if (!quiet && rate > 0) {
    float audio_ms = samples * 1000.0f / (float)rate;
    Serial.printf("    frames=%d samples=%lu rate=%d ch=%d\n", frames, samples, rate, ch);
    Serial.printf("    audio=%.0f ms  decode=%.1f ms  speed=%.2fx  cpu=%.0f%%\n",
                  audio_ms, ms, audio_ms / ms, ms / audio_ms * 100.0f);
  }
  if (out_rate)    *out_rate = rate;
  if (out_samples) *out_samples = samples;
  return ms;
}

static void bench_task(void *arg) {
  Serial.println("====================================");
  Serial.println("minimp3 decode benchmark (ESP32-S3)");
  Serial.println("====================================");
  Serial.printf("test mp3 : %u bytes\n", test_mp3_len);
  Serial.printf("sizeof(mp3dec_t)      = %u B\n", (unsigned)sizeof(mp3dec_t));
  Serial.printf("task stack            = %u B\n", (unsigned)BENCH_STACK_BYTES);
  Serial.printf("internal RAM free     = %u B\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  Serial.printf("PSRAM free            = %u B\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  Serial.printf("CPU                   = %u MHz\n", (unsigned)getCpuFrequencyMhz());

  Serial.println("[warmup]");
  run_once(true, NULL, NULL);

  Serial.println("[measure x3]");
  float total = 0;
  int   rate  = 0;
  unsigned long samples = 0;
  for (int i = 0; i < 3; i++) {
    total += run_once(false, &rate, &samples);
  }
  float avg = total / 3.0f;

  Serial.printf("[avg decode] %.1f ms\n", avg);
  if (rate > 0) {
    float audio_ms = samples * 1000.0f / (float)rate;
    Serial.printf("=> audio %.0f ms / decode %.1f ms = %.2fx realtime (cpu %.0f%%)\n",
                  audio_ms, avg, audio_ms / avg, avg / audio_ms * 100.0f);
    if (audio_ms / avg >= 1.5f) {
      Serial.println("VERDICT: 可以做，余量充足");
    } else if (audio_ms / avg >= 1.0f) {
      Serial.println("VERDICT: 能实时，但余量偏紧（要小心 I2S 阻塞和 UI 刷新）");
    } else {
      Serial.println("VERDICT: 跑不动，需要换更快的解码器或降采样");
    }
  }
  Serial.println("=== benchmark done ===");

  vTaskDelete(NULL);
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(800);
  Serial.println("\n[boot] creating bench task ...");
  BaseType_t ok = xTaskCreatePinnedToCore(bench_task, "mp3bench", BENCH_STACK_BYTES,
                                          NULL, 5, NULL, 0);
  Serial.printf("[boot] xTaskCreate -> %d\n", (int)ok);
  if (ok != pdPASS) {
    Serial.println("[boot] TASK CREATE FAILED");
  }
}

void loop() {
  delay(1000);
}
