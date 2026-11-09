// ESP32-S3 SuperMini + ST7789 240x240 + SD 卡 + MAX98357A(I2S) NES 模拟器
//
// 从「ESP32-S3 N16R8 + ROM 烧在 13 个 flash 分区里」的版本移植而来：
//   - 硬件换成 ESP32-S3 SuperMini（ESP32-S3FH4R2，4MB flash + 2MB QSPI PSRAM）
//   - ROM 改成从 SD 卡（SPI，和屏幕共享 SCK/MOSI）读取，flash 只放固件
//   - 增加 MAX98357A I2S 音频输出（-sound）
//   - 保留：中文界面 / iOS 风格菜单 / 流式渲染 / 多语言 NVS 保存 / 游戏内 SELECT+START 退出
//
// 接线见同目录 hw_config.h。开机用 UP/DOWN 选游戏、A 开始，游戏内长按 SELECT+START 回菜单。
//
// 【SD 卡踩坑记录 2026-11】
//   现象：屏幕显示"未找到游戏"，core 日志 sdcard_mount(): f_mount failed
//   根因：SD 模块的 VCC 接到了 RX（GPIO44）而不是电源脚，卡等于没供电
//   排障要点：CMD0 能读到响应(FF 01) 说明 SPI 链路通；读扇区全 0x00 / R1=0x20
//   说明卡供电不足。SD 模块带 AMS1117 稳压器时 VCC 必须接 5V（接 3V3 会掉到 ~2.2V）。

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include "hw_config.h"
#include "tft_driver.h"

// 开机自检：依次刷 红/绿/蓝/白/黑 再画一圈白边框。
// 用来判断"背光亮但没画面"到底是 SPI 不通还是显示窗口偏移。
// 屏幕确认正常后改成 0 即可跳过。
#define SCREEN_SELF_TEST 0

// 临时测试开关：开机跳过菜单直接启动 SD 卡上的第一个游戏，用于自动化测帧率。测完改回 0。
#define AUTO_START_TEST 0

// ============================================================================
// SD 卡挂载：逐档降频 + 失败时给出简明诊断
// ============================================================================
// 杜邦线越长 / 卡座到主板的线越乱，SPI 时钟就必须越低。这里从高到低试，
// 成功一档就锁死那一档，后面的读卡也按这个速度走。
static const uint32_t SD_FREQ_TRIES[] = {20000000, 10000000, 4000000, 1000000};
static uint32_t sd_freq_used = 0;

// 绕过 SD 库，直接用手动 SPI 发 CMD0（GO_IDLE_STATE）。
//   >=0 ：第一个不是 0xFF 的字节（卡正常应回 0x01）
//   -1  ：10 个字节全是 0xFF，卡完全没说话
// 用来区分「卡/接线/供电问题」和「文件系统不是 FAT32」。
static int sd_probe_cmd0() {
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  for (int i = 0; i < 12; i++) SPI.transfer(0xFF);   // 上电后至少 74 个时钟
  digitalWrite(SD_CS, LOW);
  delayMicroseconds(100);
  SPI.transfer(0x40); SPI.transfer(0x00); SPI.transfer(0x00);
  SPI.transfer(0x00); SPI.transfer(0x00); SPI.transfer(0x95);  // CMD0 + 正确 CRC
  int r = -1;
  for (int i = 0; i < 10; i++) {
    uint8_t v = SPI.transfer(0xFF);
    if (v != 0xFF) { r = (int)v; break; }
  }
  digitalWrite(SD_CS, HIGH);
  SPI.transfer(0xFF);
  SPI.endTransaction();
  return r;
}

static void sd_diagnostics() {
  int r = sd_probe_cmd0();
  if (r == 0x01) {
    Serial.println("[SD] card answered CMD0 -> 接线/供电正常，问题在文件系统");
    Serial.println("     只认 FAT16/FAT32，exFAT/NTFS/RAW 不行（64GB 以上要用 GUIFormat/Rufus）");
    Serial.println("     另外文件名必须是 .nes 后缀");
  } else if (r == 0x00) {
    Serial.println("[SD] MISO 恒低（读到 0x00）-> 卡没供电 / MISO 没接 / 短路到 GND");
    Serial.println("     重点查 VCC 和 GND：SD 模块带稳压器时 VCC 必须接 5V 而不是 3V3");
  } else if (r < 0) {
    Serial.println("[SD] CMD0 无响应 -> 卡没在 SPI 上说话");
    Serial.println("     查 CS=7 / MISO=8 / SCK=12 / MOSI=11、卡是否插到底、VCC 是否接对");
  } else {
    Serial.printf("[SD] CMD0 响应异常 0x%02X -> 接线/供电不稳\n", r);
  }
}

static bool sd_mount() {
  for (uint32_t f : SD_FREQ_TRIES) {
    unsigned long t0 = millis();
    Serial.printf("[SD] try %u kHz ... ", (unsigned)(f / 1000));
    SD.end();
    bool ok = SD.begin(SD_CS, SPI, f);
    Serial.printf("%s in %lu ms\r\n", ok ? "OK" : "FAIL", millis() - t0);
    if (ok) {
      sd_freq_used = f;
      return true;
    }
  }
  return false;
}

TFTDriver tft;

extern void setup_controller();
extern "C" int nofrendo_main(int argc, char* argv[]);
extern "C" void osd_ui_language_setup(void);
extern "C" void osd_boot_sound(void);
extern "C" int show_menu();
extern "C" const char* get_selected_game();
extern "C" void osd_scan_games(void);
extern "C" int osd_game_count(void);

// 启动参数：音量 100%、采样率 22050Hz（osd.cpp 里 osd_main 解析）。
// 注意：nofrendo 的 osd_main() 约定「最后一个参数是 ROM 文件名」，
// 所以 ROM 路径必须拼在最后。
#define NES_ARGC 7

#if SCREEN_SELF_TEST
static void screen_self_test() {
  const uint16_t colors[] = {0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000};  // 红 绿 蓝 白 黑
  for (int i = 0; i < 5; i++) {
    Serial.printf("  self-test color %d\n", i);
    tft.fillScreen(colors[i]);
    delay(700);
  }

  // 一圈白边框 + 中心十字：能同时看出尺寸和偏移
  tft.fillScreen(0x0000);
  for (int i = 0; i < 3; i++) {
    tft.drawFilledRect(i, i, DISPLAY_WIDTH - 2 * i, 1, 0xFFFF);
    tft.drawFilledRect(i, DISPLAY_HEIGHT - 1 - i, DISPLAY_WIDTH - 2 * i, 1, 0xFFFF);
    tft.drawFilledRect(i, i, 1, DISPLAY_HEIGHT - 2 * i, 0xFFFF);
    tft.drawFilledRect(DISPLAY_WIDTH - 1 - i, i, 1, DISPLAY_HEIGHT - 2 * i, 0xFFFF);
  }
  tft.drawFilledRect(DISPLAY_WIDTH / 2 - 1, 0, 2, DISPLAY_HEIGHT, 0x07E0);
  tft.drawFilledRect(0, DISPLAY_HEIGHT / 2 - 1, DISPLAY_WIDTH, 2, 0x07E0);
  Serial.println("  self-test frame done");
  delay(2500);
}
#endif

void setup() {
  Serial.begin(115200);
  unsigned long t_boot = millis();

  // 【关键】主机端没打开串口监视器时，USB-Serial-JTAG 的 FIFO 写满会让
  // Serial.printf 阻塞等待。菜单+加载阶段有几十条日志，累计能把进游戏拖到 1 分钟。
  // 设成 0 表示"不等待"，写不进去就直接丢，绝不阻塞主流程。
  Serial.setTxTimeoutMs(0);

  delay(50);

  Serial.println("\n\n====================================");
  Serial.println("NES Emulator for ESP32-S3 SuperMini");
  Serial.println("  ST7789 240x240 + SD card + MAX98357A I2S");
  Serial.println("====================================");

  setup_controller();
  Serial.println("[1/4] Controller ready");

  // SD 卡先标记成"未选中"（CS 拉高），再去初始化共享的 SPI 总线，
  // 免得屏幕初始化时 SD 卡也在跟着收 SCK。
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  // 【关键】屏幕和 SD 卡共用同一条 SPI 总线，只能有一次 SPI.begin()：
  //   SCK/MOSI 屏幕和 SD 共用，MISO 只有 SD 用（屏幕只写不读）。
  // tft_driver.h 里的 SPI.begin() 用的是同一组引脚（含 SD_MISO），
  // SPIClass 内部缓存了总线句柄，第二次 begin() 是空操作。
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  tft.init();
  tft.fillScreen(tft.color565(0, 0, 0));
  Serial.printf("[2/4] TFT ready: %dx%d (SPI %d MHz) [%lu ms]\n",
                DISPLAY_WIDTH, DISPLAY_HEIGHT, (int)(TFT_SPI_FREQ / 1000000),
                millis() - t_boot);

#if SCREEN_SELF_TEST
  Serial.println("  running screen self-test ...");
  screen_self_test();
#endif

  // 屏幕初始化完之后再挂 SD 卡：SD.begin() 内部按 400kHz 做初始化时序，
  // 之后每次访问都在自己的 beginTransaction(SPI 20MHz) 里，不会破坏屏幕的配置。
  //
  // 【开机速度】平时必须保持 setTxTimeoutMs(0)：没有串口监视器时，每条日志都会
  // 阻塞最多 1 秒，启动阶段十几条日志就能把开机拖到十几秒。
  // 只有 SD 挂载失败、要打完整诊断时，才临时允许阻塞（否则日志会被 USB CDC 丢字节）。
  if (!sd_mount()) {
    Serial.setTxTimeoutMs(1000);
    Serial.println("[3/4] SD mount FAILED");
    Serial.printf("      pins: CS=%d MISO=%d SCK=%d MOSI=%d (SCK/MOSI shared with TFT)\r\n",
                  SD_CS, SD_MISO, SD_SCK, SD_MOSI);
    sd_diagnostics();
    Serial.println("      display+menu still work, but there are no games to play.");
    Serial.setTxTimeoutMs(0);
  } else {
    uint64_t card_mb = SD.cardSize() / (1024ULL * 1024ULL);
    Serial.printf("[3/4] SD ready: %u MB, type=%d, SPI=%u kHz\r\n",
                  (unsigned)card_mb, (int)SD.cardType(), (unsigned)(sd_freq_used / 1000));
    osd_scan_games();
    Serial.printf("      found %d .nes game(s) in SD root [%lu ms]\r\n",
                  osd_game_count(), millis() - t_boot);
  }

  // 开机提示音：没有按键也能确认 MAX98357A 功放和喇叭是通的。
  // 三个上升音 C5-E5-G5，约 330ms，播放完才进语言选择/菜单。
  osd_boot_sound();
  Serial.printf("[T] boot sound done [%lu ms]\n", millis() - t_boot);

  // 语言：已经选过就直接用 NVS 里的，首次开机弹一次选择界面
  osd_ui_language_setup();
  Serial.printf("[T] language done [%lu ms]\n", millis() - t_boot);

  Serial.printf("[4/4] Heap=%u  PSRAM free=%u / total=%u\n",
                esp_get_free_heap_size(),
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) {
    Serial.println("WARNING: 没有检测到 PSRAM！SuperMini 用的是 ESP32-S3FH4R2");
    Serial.println("         （2MB QSPI PSRAM），Tools 里 PSRAM 要选 \"QSPI PSRAM\"/enabled，");
    Serial.println("         Flash Size 选 4MB。");
  }

#if AUTO_START_TEST
  Serial.println("AUTO_START_TEST: 直接启动 SD 卡上的第一个游戏");
  osd_scan_games();
  if (osd_game_count() > 0) {
    extern unsigned long osd_game_start_ms;
    const char* rom = get_selected_game();
    osd_game_start_ms = millis();
    char* auto_argv[] = { (char*)"nes", (char*)"-sound", (char*)"-volume", (char*)"100",
                          (char*)"-sample", (char*)"22050", (char*)rom };
    nofrendo_main(NES_ARGC, auto_argv);
  }
#else
  // 主循环：显示菜单 -> 启动游戏 -> 游戏内 SELECT+START 退出 -> 回到这里重新显示菜单。
  // 退出时 nofrendo_main() 会正常返回，不需要重启芯片。
  extern unsigned long osd_game_start_ms;

  for (;;) {
    int selected = show_menu();
    if (selected < 0) {
      Serial.println("\nSD 卡根目录没有 .nes 文件。");
      Serial.println("把游戏 ROM 拷到 SD 卡根目录（FAT32 格式，必须是 .nes 后缀），");
      Serial.println("然后按复位键重新开机。");
      delay(3000);
      continue;
    }

    const char* rom = get_selected_game();
    if (!rom || rom[0] == '\0') {
      Serial.println("菜单没有返回游戏路径，重新显示菜单");
      continue;
    }

    Serial.printf("Starting game from SD: %s\n", rom);

    // -sound 打开音频，-volume/-sample 由 osd.cpp 的 osd_main() 解析
    char* argv[] = { (char*)"nes", (char*)"-sound", (char*)"-volume", (char*)"100",
                     (char*)"-sample", (char*)"22050", (char*)rom };
    osd_game_start_ms = millis();   // 记录启动游戏的时间点，用于统计"到第一帧"的耗时
    nofrendo_main(NES_ARGC, argv);

    // 走到这里说明游戏被用户主动退出了，清屏后回到菜单
    tft.fillScreen(0x0000);
    delay(150);
  }
#endif
}

void loop() {
  delay(1000);
}
