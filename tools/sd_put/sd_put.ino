// 临时工具固件：通过 USB 串口把电脑上的文件写进板子的 SD 卡。
//
// 为什么要它：板子的 USB 只跑 USB-Serial-JTAG（当串口用），电脑的资源管理器
// 看不到 SD 卡，所以没法直接拖放。这个固件在串口上开一个极简协议收文件。
// 传完请立刻烧回主固件（Dsn_nes_Emulator.ino.merged.bin）。
//
// 协议（行式握手 + 裸数据）：
//     主机: PUT <size> <name>\n      设备: READY\n
//     主机: <size 字节原始数据>
//     设备: OK <written> <expected> <ms>\n
//     主机: PING\n                   设备: PONG\n       （握手/探活）
//
// 引脚直接写死（和 hw_config.h 一致）：SD_CS=7 MISO=8 SCK=12 MOSI=11

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

static const int PIN_SD_CS   = 7;
static const int PIN_SD_MISO = 8;
static const int PIN_SD_SCK  = 12;
static const int PIN_SD_MOSI = 11;

static uint8_t rxbuf[8192];
static bool    sd_ready = false;

static bool mount_sd() {
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);
  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);

  const uint32_t freqs[] = {20000000, 10000000, 4000000, 1000000};
  for (int i = 0; i < 4; i++) {
    SD.end();
    if (SD.begin(PIN_SD_CS, SPI, freqs[i])) {
      Serial.printf("[SD] mounted at %u kHz\n", (unsigned)(freqs[i] / 1000));
      return true;
    }
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(400);

  Serial.println("\n=== SD writer (temporary tool) ===");
  sd_ready = mount_sd();

  if (!sd_ready) {
    Serial.println("ERR SD mount failed");
    return;
  }
  Serial.printf("INFO card %llu MB\n",
                (unsigned long long)(SD.cardSize() / (1024ULL * 1024ULL)));
  Serial.println("PONG");   // 主机用 PONG 判断链路通
}

void loop() {
  if (!sd_ready) {
    delay(1000);
    return;
  }
  if (!Serial.available()) {
    delay(5);
    return;
  }

  char line[320];
  size_t n = Serial.readBytesUntil('\n', line, sizeof(line) - 1);
  if (n == 0) return;
  line[n] = '\0';
  // 去掉行尾 \r
  if (n > 0 && line[n - 1] == '\r') line[n - 1] = '\0';

  if (strcmp(line, "PING") == 0) {
    Serial.println("PONG");
    return;
  }
  if (strncmp(line, "PUT ", 4) != 0) {
    Serial.printf("ERR unknown cmd\n");
    return;
  }

  // PUT <size> <name>
  char *p = line + 4;
  char *sp = strchr(p, ' ');
  if (!sp) {
    Serial.println("ERR format");
    return;
  }
  *sp = '\0';
  unsigned long size = strtoul(p, NULL, 10);
  const char *name = sp + 1;
  while (*name == '/') name++;              // 只写根目录
  if (*name == '\0' || size == 0) {
    Serial.println("ERR name/size");
    return;
  }

  char path[160];
  snprintf(path, sizeof(path), "/%s", name);

  File f = SD.open(path, FILE_WRITE);
  if (!f) {
    Serial.printf("ERR open %s\n", path);
    return;
  }

  Serial.println("READY");                  // 主机收到这行才开始发数据

  unsigned long got = 0;
  uint32_t t0 = millis();
  uint32_t last_progress = 0;

  while (got < size) {
    size_t want = sizeof(rxbuf);
    if ((unsigned long)want > size - got) want = size - got;
    size_t r = Serial.readBytes(rxbuf, want);
    if (r == 0) {
      break;                                // 超时，放弃这个文件
    }
    size_t w = f.write(rxbuf, r);
    got += w;
    if (w != r) {
      break;                                // 卡写失败
    }
    // 大文件时每秒给主机一个进度行，方便脚本显示
    if (millis() - last_progress > 1000) {
      last_progress = millis();
      Serial.printf("PROG %lu %lu\n", got, size);
    }
  }

  f.close();
  Serial.printf("OK %lu %lu %lu\n", got, size, millis() - t0);
}
