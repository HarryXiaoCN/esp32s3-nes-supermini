#ifndef TFT_DRIVER_H
#define TFT_DRIVER_H

#include <Arduino.h>
#include <SPI.h>
#include "hw_config.h"
 
#define TFT_SWRESET   0x01
#define TFT_SLPOUT    0x11
#define TFT_COLMOD    0x3A
#define TFT_MADCTL    0x36
#define TFT_CASET     0x2A
#define TFT_RASET     0x2B
#define TFT_RAMWR     0x2C
#define TFT_DISPON    0x29
#define TFT_DISPOFF   0x28
#define TFT_INVON     0x21
#define TFT_INVOFF    0x20
 
#define DISPLAY_WIDTH  240
#define DISPLAY_HEIGHT 240
 
// 240x240 ST7789 面板（原项目为 280x240）。若画面整体错位，改这两个偏移。
// 常见 240x240 模组内部 GRAM 为 240x320，竖屏时行方向需 +80；
// 本项目 MADCTL=0x60 交换了 XY，所以偏移作用在列方向。先填 0，错位再调。
#define ST7789_COL_OFFSET 0
#define ST7789_ROW_OFFSET 0

// 屏幕 SPI 时钟。整帧 115200 字节：80MHz 约 11.5ms，40MHz 约 23ms。
// 本版本屏幕和 SD 卡共享 SCK/MOSI，总线上多挂了一个 SD 卡座（长杜邦线 + 卡座电容），
// 80MHz 很容易花屏，所以默认 40MHz。屏幕单独走线、确认稳定后可以
// 在编译选项里加 -DTFT_SPI_FREQ=80000000 提回 80MHz。
#ifndef TFT_SPI_FREQ
#define TFT_SPI_FREQ 40000000
#endif

// 单次 SPI 传输的像素块大小（4096 像素 = 8KB）。原来 512 像素一块，
// 整帧要发起 112 次传输，每次都有固定的 CS/DC/驱动开销；4096 只需 15 次。
#ifndef TFT_SPI_CHUNK_PIXELS
#define TFT_SPI_CHUNK_PIXELS 4096
#endif
 
#define TFT_BLACK   0x0000
#define TFT_WHITE   0xFFFF
#define TFT_RED     0xF800
#define TFT_GREEN   0x07E0
#define TFT_BLUE    0x001F
 
static const uint8_t font_5x7[] PROGMEM = {
    0x00, 0x00, 0x00, 0x00, 0x00, // 0x20 ' '
    0x00, 0x00, 0x5F, 0x00, 0x00, // 0x21 '!'
    0x00, 0x07, 0x00, 0x07, 0x00, // 0x22 '"'
    0x14, 0x7F, 0x14, 0x7F, 0x14, // 0x23 '#'
    0x24, 0x2A, 0x7F, 0x2A, 0x12, // 0x24 '$'
    0x23, 0x13, 0x08, 0x64, 0x62, // 0x25 '%'
    0x36, 0x49, 0x55, 0x22, 0x50, // 0x26 '&'
    0x00, 0x05, 0x03, 0x00, 0x00, // 0x27 '''
    0x00, 0x1C, 0x22, 0x41, 0x00, // 0x28 '('
    0x00, 0x41, 0x22, 0x1C, 0x00, // 0x29 ')'
    0x14, 0x08, 0x3E, 0x08, 0x14, // 0x2A '*'
    0x08, 0x08, 0x3E, 0x08, 0x08, // 0x2B '+'
    0x00, 0x50, 0x30, 0x00, 0x00, // 0x2C ','
    0x08, 0x08, 0x08, 0x08, 0x08, // 0x2D '-'
    0x00, 0x60, 0x60, 0x00, 0x00, // 0x2E '.'
    0x20, 0x10, 0x08, 0x04, 0x02, // 0x2F '/'
    0x3E, 0x51, 0x49, 0x45, 0x3E, // 0x30 '0'
    0x00, 0x42, 0x7F, 0x40, 0x00, // 0x31 '1'
    0x42, 0x61, 0x51, 0x49, 0x46, // 0x32 '2'
    0x21, 0x41, 0x45, 0x4B, 0x31, // 0x33 '3'
    0x18, 0x14, 0x12, 0x7F, 0x10, // 0x34 '4'
    0x27, 0x45, 0x45, 0x45, 0x39, // 0x35 '5'
    0x3C, 0x4A, 0x49, 0x49, 0x30, // 0x36 '6'
    0x01, 0x71, 0x09, 0x05, 0x03, // 0x37 '7'
    0x36, 0x49, 0x49, 0x49, 0x36, // 0x38 '8'
    0x06, 0x49, 0x49, 0x29, 0x1E, // 0x39 '9'
    0x00, 0x36, 0x36, 0x00, 0x00, // 0x3A ':'
    0x00, 0x56, 0x36, 0x00, 0x00, // 0x3B ';'
    0x08, 0x14, 0x22, 0x41, 0x00, // 0x3C '<'
    0x14, 0x14, 0x14, 0x14, 0x14, // 0x3D '='
    0x00, 0x41, 0x22, 0x14, 0x08, // 0x3E '>'
    0x02, 0x01, 0x51, 0x09, 0x06, // 0x3F '?'
    0x32, 0x49, 0x79, 0x41, 0x3E, // 0x40 '@'
    0x7E, 0x11, 0x11, 0x11, 0x7E, // 0x41 'A'
    0x7F, 0x49, 0x49, 0x49, 0x36, // 0x42 'B'
    0x3E, 0x41, 0x41, 0x41, 0x22, // 0x43 'C'
    0x7F, 0x41, 0x41, 0x22, 0x1C, // 0x44 'D'
    0x7F, 0x49, 0x49, 0x49, 0x41, // 0x45 'E'
    0x7F, 0x09, 0x09, 0x09, 0x01, // 0x46 'F'
    0x3E, 0x41, 0x49, 0x49, 0x7A, // 0x47 'G'
    0x7F, 0x08, 0x08, 0x08, 0x7F, // 0x48 'H'
    0x00, 0x41, 0x7F, 0x41, 0x00, // 0x49 'I'
    0x20, 0x40, 0x41, 0x3F, 0x01, // 0x4A 'J'
    0x7F, 0x08, 0x14, 0x22, 0x41, // 0x4B 'K'
    0x7F, 0x40, 0x40, 0x40, 0x40, // 0x4C 'L'
    0x7F, 0x02, 0x0C, 0x02, 0x7F, // 0x4D 'M'
    0x7F, 0x04, 0x08, 0x10, 0x7F, // 0x4E 'N'
    0x3E, 0x41, 0x41, 0x41, 0x3E, // 0x4F 'O'
    0x7F, 0x09, 0x09, 0x09, 0x06, // 0x50 'P'
    0x3E, 0x41, 0x51, 0x21, 0x5E, // 0x51 'Q'
    0x7F, 0x09, 0x19, 0x29, 0x46, // 0x52 'R'
    0x46, 0x49, 0x49, 0x49, 0x31, // 0x53 'S'
    0x01, 0x01, 0x7F, 0x01, 0x01, // 0x54 'T'
    0x3F, 0x40, 0x40, 0x40, 0x3F, // 0x55 'U'
    0x1F, 0x20, 0x40, 0x20, 0x1F, // 0x56 'V'
    0x7F, 0x20, 0x18, 0x20, 0x7F, // 0x57 'W'
    0x63, 0x14, 0x08, 0x14, 0x63, // 0x58 'X'
    0x03, 0x04, 0x78, 0x04, 0x03, // 0x59 'Y'
    0x61, 0x51, 0x49, 0x45, 0x43, // 0x5A 'Z'
    0x00, 0x7F, 0x41, 0x41, 0x00, // 0x5B '['
    0x02, 0x04, 0x08, 0x10, 0x20, // 0x5C '\'
    0x00, 0x41, 0x41, 0x7F, 0x00, // 0x5D ']'
    0x04, 0x02, 0x01, 0x02, 0x04, // 0x5E '^'
    0x40, 0x40, 0x40, 0x40, 0x40, // 0x5F '_'
    0x00, 0x01, 0x02, 0x04, 0x00, // 0x60 '`'
    0x20, 0x54, 0x54, 0x54, 0x78, // 0x61 'a'
    0x7F, 0x48, 0x44, 0x44, 0x38, // 0x62 'b'
    0x38, 0x44, 0x44, 0x44, 0x20, // 0x63 'c'
    0x38, 0x44, 0x44, 0x48, 0x7F, // 0x64 'd'
    0x38, 0x54, 0x54, 0x54, 0x18, // 0x65 'e'
    0x08, 0x7E, 0x09, 0x01, 0x02, // 0x66 'f'
    0x0C, 0x52, 0x52, 0x52, 0x3E, // 0x67 'g'
    0x7F, 0x08, 0x04, 0x04, 0x78, // 0x68 'h'
    0x00, 0x44, 0x7D, 0x40, 0x00, // 0x69 'i'
    0x20, 0x40, 0x44, 0x3D, 0x00, // 0x6A 'j'
    0x7F, 0x10, 0x28, 0x44, 0x00, // 0x6B 'k'
    0x00, 0x41, 0x7F, 0x40, 0x00, // 0x6C 'l'
    0x7C, 0x04, 0x18, 0x04, 0x78, // 0x6D 'm'
    0x7C, 0x08, 0x04, 0x04, 0x78, // 0x6E 'n'
    0x38, 0x44, 0x44, 0x44, 0x38, // 0x6F 'o'
    0x7C, 0x14, 0x14, 0x14, 0x08, // 0x70 'p'
    0x08, 0x14, 0x14, 0x18, 0x7C, // 0x71 'q'
    0x7C, 0x08, 0x04, 0x04, 0x08, // 0x72 'r'
    0x48, 0x54, 0x54, 0x54, 0x20, // 0x73 's'
    0x04, 0x3F, 0x44, 0x40, 0x20, // 0x74 't'
    0x3C, 0x40, 0x40, 0x20, 0x7C, // 0x75 'u'
    0x1C, 0x20, 0x40, 0x20, 0x1C, // 0x76 'v'
    0x3C, 0x40, 0x30, 0x40, 0x3C, // 0x77 'w'
    0x44, 0x28, 0x10, 0x28, 0x44, // 0x78 'x'
    0x0C, 0x50, 0x50, 0x50, 0x3C, // 0x79 'y'
    0x44, 0x64, 0x54, 0x4C, 0x44, // 0x7A 'z'
    0x00, 0x08, 0x36, 0x41, 0x00, // 0x7B '{'
    0x00, 0x00, 0x7F, 0x00, 0x00, // 0x7C '|'
    0x00, 0x41, 0x36, 0x08, 0x00, // 0x7D '}'
    0x10, 0x08, 0x08, 0x10, 0x08, // 0x7E '~'
    0x00, 0x00, 0x00, 0x00, 0x00  // 0x7F DEL
};

// 判断指针是否落在 ESP32-S3 的内部 RAM（可被 SPI DMA 直接访问）。
// 内部 DRAM/IRAM 在 0x3FC00000~0x60000000，外部 PSRAM 映射在 0x3C000000 附近，两者不重叠。
static inline bool is_internal_ram(const void *p) {
    uintptr_t a = (uintptr_t)p;
    return a >= 0x3FC00000u && a < 0x60000000u;
}

class TFTDriver {
private:
    static constexpr int TFT_DC   = HW_TFT_DC;
    static constexpr int TFT_RST  = HW_TFT_RST;
    static constexpr int TFT_CS   = HW_TFT_CS;
    static constexpr int TFT_SCK  = HW_TFT_SCK;
    static constexpr int TFT_MOSI = HW_TFT_MOSI;
    // 屏幕只写不读，但这条 SPI 总线上还挂着 SD 卡。SPIClass 的引脚配置是
    // 总线级的，第一次 SPI.begin() 决定挂哪些脚 —— 所以这里把 SD 的 MISO
    // 一起挂上，否则 SD.begin() 之后也读不到卡。
    static constexpr int TFT_MISO = SD_MISO;

    // ---------------- SPI 总线分时复用（屏幕 + SD 卡）----------------
    // SPIClass 的时钟/模式是总线级状态：SD 库内部用
    // beginTransaction(SPISettings(400kHz/20MHz, ...)) 包住每次读写，
    // 屏幕这边也必须同样用事务包住每次访问。否则 SD 卡一操作，
    // 屏幕残留的 40MHz 配置就被改掉；反过来屏幕也会破坏 SD 的时钟。
    static void bus_begin() {
        SPI.beginTransaction(SPISettings(TFT_SPI_FREQ, MSBFIRST, SPI_MODE0));
    }
    static void bus_end() {
        SPI.endTransaction();
    }
    static void cs_low()  { digitalWrite(TFT_CS, LOW); }
    static void cs_high() { digitalWrite(TFT_CS, HIGH); }

    // raw_* 只能在已经 bus_begin() 的事务里调用，且不负责 CS。
    // raw_cmd() 拉低 CS 并切到命令，raw_end() 拉高 CS；
    // 一条命令和它的参数必须在同一个 CS 低电平周期里连续发出。
    void raw_cmd(uint8_t cmd) {
        cs_low();
        digitalWrite(TFT_DC, LOW);
        SPI.write(cmd);
        digitalWrite(TFT_DC, HIGH);
    }
    void raw_byte(uint8_t data) { SPI.write(data); }
    void raw_word(uint16_t data) { SPI.write16(data); }
    void raw_end() { cs_high(); }

    // 一条不带参数的命令（自带事务）
    void cmd_only(uint8_t cmd) {
        bus_begin();
        raw_cmd(cmd);
        raw_end();
        bus_end();
    }
    // 一条命令 + 一个参数（自带事务）
    void cmd_byte(uint8_t cmd, uint8_t data) {
        bus_begin();
        raw_cmd(cmd);
        raw_byte(data);
        raw_end();
        bus_end();
    }

    // 设置写窗口。返回时 CS 已经是低、DC 已经是高，可以直接灌像素数据。
    void set_window(int x0, int y0, int x1, int y1) {
        raw_cmd(TFT_CASET); raw_word((uint16_t)x0); raw_word((uint16_t)x1);
        raw_cmd(TFT_RASET); raw_word((uint16_t)y0); raw_word((uint16_t)y1);
        raw_cmd(TFT_RAMWR);
    }
  public:
    TFTDriver() {}
    
    void init() {
        pinMode(TFT_DC, OUTPUT);
        pinMode(TFT_RST, OUTPUT);
        pinMode(TFT_CS, OUTPUT);
        
        digitalWrite(TFT_CS, HIGH);
        digitalWrite(TFT_DC, HIGH);
         
        // 屏幕和 SD 卡共用一条 SPI 总线。若总线已经由 .ino 初始化过，
        // 这里再 begin() 是空操作（SPIClass 内部缓存了 _spi），不会改引脚。
        // TFT_MISO 填的是 SD_MISO：一条总线只需要一份 MISO 配置。
        SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CS);
        // 不再用 SPI.setFrequency()/setDataMode()：总线由 SD 卡共享，
        // 每次传输都用 beginTransaction(SPISettings(...)) 临时设定（见 bus_begin()）。
        
        // 硬件复位（ST7789 要求复位后至少等 120ms）
        digitalWrite(TFT_RST, HIGH); delay(10);
        digitalWrite(TFT_RST, LOW);  delay(20);
        digitalWrite(TFT_RST, HIGH); delay(120);

        // 下面命令和它的参数要在同一个 CS 低电平周期内连续发出，
        // ST7789 才能把它们识别成一条带参数的指令。
        // cmd() 打开 SPI 事务并拉低 CS，endT() 拉高 CS 并关闭事务，必须成对。
        auto cmd   = [&](uint8_t c) { bus_begin(); cs_low(); digitalWrite(TFT_DC, LOW); SPI.write(c); digitalWrite(TFT_DC, HIGH); };
        auto dat   = [&](uint8_t d) { SPI.write(d); };
        auto endT  = [&]()          { cs_high(); bus_end(); };

        cmd(TFT_SWRESET); endT(); delay(150);
        cmd(TFT_SLPOUT);  endT(); delay(120);

        cmd(TFT_COLMOD); dat(0x55); endT(); delay(10);      // 16-bit/pixel

        cmd(TFT_MADCTL); dat(0x60); endT(); delay(10);      // 横屏扫描方向

        cmd(0xB2); dat(0x0C); dat(0x0C); dat(0x00); dat(0x33); dat(0x33); endT();  // PORCTRL 前后肩
        cmd(0xB7); dat(0x35); endT();                                              // GCTRL 栅极电压
        cmd(0xBB); dat(0x28); endT();                                              // VCOMS：缺这条最常见症状就是"背光亮全黑"
        cmd(0xC0); dat(0x2C); endT();                                              // LCMCTRL
        cmd(0xC2); dat(0x01); endT();                                              // VDV/VRH 使能
        cmd(0xC3); dat(0x12); endT();                                              // VRH 电压
        cmd(0xC4); dat(0x20); endT();                                              // VDV 电压
        cmd(0xC6); dat(0x0F); endT();                                              // 帧率 60Hz
        cmd(0xD0); dat(0xA4); dat(0xA1); endT();                                   // PWCTRL1 电源控制

        // 正/负伽马
        cmd(0xE0); dat(0xD0); dat(0x08); dat(0x11); dat(0x08); dat(0x0C); dat(0x15);
                   dat(0x39); dat(0x33); dat(0x50); dat(0x36); dat(0x13); dat(0x14);
                   dat(0x29); dat(0x2D); endT();
        cmd(0xE1); dat(0xD0); dat(0x08); dat(0x10); dat(0x08); dat(0x06); dat(0x06);
                   dat(0x39); dat(0x44); dat(0x51); dat(0x0B); dat(0x16); dat(0x14);
                   dat(0x2F); dat(0x31); endT();

        cmd(TFT_INVON); endT(); delay(10);   // IPS 面板需要反显
        cmd(0x13);      endT(); delay(10);   // 普通显示模式

        fillScreen(TFT_BLACK);
        delay(20);

        cmd(TFT_DISPON); endT(); delay(120);
    }
    
    void fillScreen(uint16_t color) {
        bus_begin();
        set_window(ST7789_COL_OFFSET,
                   ST7789_ROW_OFFSET,
                   ST7789_COL_OFFSET + DISPLAY_WIDTH - 1,
                   ST7789_ROW_OFFSET + DISPLAY_HEIGHT - 1);
        
        uint32_t pixel_count = (uint32_t)DISPLAY_WIDTH * DISPLAY_HEIGHT;
        static uint8_t buffer[512];  
         
        uint8_t high = color >> 8;
        uint8_t low = color & 0xFF;
        for (int i = 0; i < 512; i += 2) {
            buffer[i] = high;
            buffer[i + 1] = low;
        }
         
        for (uint32_t i = 0; i < pixel_count; i += 256) {
            uint32_t chunk_size = (pixel_count - i > 256) ? 256 : (pixel_count - i);
            SPI.writeBytes(buffer, chunk_size * 2);
        }
        
        raw_end();
        bus_end();
    }
    
    void pushImage(int x, int y, int w, int h, const uint16_t *data, bool preswapped = false) {
        bus_begin();
        set_window(x + ST7789_COL_OFFSET,
                   y + ST7789_ROW_OFFSET,
                   x + w - 1 + ST7789_COL_OFFSET,
                   y + h - 1 + ST7789_ROW_OFFSET);
         
        uint32_t pixel_count = (uint32_t)w * h;

        // buffer 必须在内部 RAM —— SPI DMA 访问不了 PSRAM。
        // 块大小 4096 像素(8KB)，整帧 57600 像素只需 15 次传输。
        static uint8_t buffer[TFT_SPI_CHUNK_PIXELS * 2];

        for (uint32_t i = 0; i < pixel_count; i += TFT_SPI_CHUNK_PIXELS) {
            uint32_t chunk_size = pixel_count - i;
            if (chunk_size > TFT_SPI_CHUNK_PIXELS) {
                chunk_size = TFT_SPI_CHUNK_PIXELS;
            }

            if (preswapped) {
                const uint8_t *src = (const uint8_t *)&data[i];
                // 数据本身就在内部 RAM 时（流式渲染的块缓冲），直接交给 SPI，省掉一次 memcpy。
                // PSRAM 的指针不能直接给 SPI DMA 用，必须先拷进内部 buffer。
                if (is_internal_ram(src)) {
                    SPI.writeBytes(src, chunk_size * 2);
                    continue;
                }
                memcpy(buffer, src, chunk_size * 2);
            } else {
                for (uint32_t j = 0; j < chunk_size; j++) {
                    uint16_t pixel = data[i + j];
                    buffer[j * 2] = pixel >> 8;
                    buffer[j * 2 + 1] = pixel & 0xFF;
                }
            }
            SPI.writeBytes(buffer, chunk_size * 2);
        }
         
        raw_end();
        bus_end();
    } 
    // ---- 整帧连续写入模式 ----
    // 设置一次显示窗口，之后连续 writePixels 即可：ST7789 的写入地址会自动递增，
    // 省掉每块都重发 CASET/RASET/RAMWR 的开销。
    // 注意：这三个函数共用同一个 SPI 事务（beginFullFrame 打开、endFullFrame 关闭），
    // 中间不能插入 SD 卡访问，否则总线会被别的设备抢走。
    void beginFullFrame(int w, int h) {
        bus_begin();
        set_window(ST7789_COL_OFFSET,
                   ST7789_ROW_OFFSET,
                   ST7789_COL_OFFSET + w - 1,
                   ST7789_ROW_OFFSET + h - 1);
    }

    // data 必须在内部 RAM（SPI DMA 访问不了 PSRAM）
    void writePixels(const uint16_t *data, uint32_t count) {
        SPI.writeBytes((const uint8_t *)data, count * 2);
    }

    void endFullFrame() {
        raw_end();
        bus_end();
    }

    void drawChar(int x, int y, char c, uint16_t fg, uint16_t bg, int scale = 1) {
        if (scale < 1) {
            scale = 1;
        }

        uint8_t idx = (c >= 0x20 && c <= 0x7F) ? static_cast<uint8_t>(c - 0x20) : static_cast<uint8_t>('?' - 0x20);
        const uint8_t *glyph = &font_5x7[idx * 5];

        for (int cy = 0; cy < 7; cy++) {
            for (int cx = 0; cx < 5; cx++) {
                uint8_t line = pgm_read_byte(&glyph[cx]);
                if (line & (1 << cy)) {
                    drawFilledRect(x + cx * scale, y + cy * scale, scale, scale, fg);
                } else {
                    drawFilledRect(x + cx * scale, y + cy * scale, scale, scale, bg);
                }
            }
        }
    }
      
    void drawFilledRect(int x, int y, int w, int h, uint16_t color) {
        if (w <= 0 || h <= 0) {
            return;
        }

        int x0 = max(0, x);
        int y0 = max(0, y);
        int x1 = min(DISPLAY_WIDTH - 1, x + w - 1);
        int y1 = min(DISPLAY_HEIGHT - 1, y + h - 1);

        if (x1 < x0 || y1 < y0) {
            return;
        }

        int clipped_w = x1 - x0 + 1;
        int clipped_h = y1 - y0 + 1;
         
        bus_begin();
        set_window(x0 + ST7789_COL_OFFSET,
                   y0 + ST7789_ROW_OFFSET,
                   x1 + ST7789_COL_OFFSET,
                   y1 + ST7789_ROW_OFFSET);
         
        uint32_t pixels = (uint32_t)clipped_w * clipped_h;
         
        for (uint32_t i = 0; i < pixels; i++) {
             SPI.write16(color);
        }
         
        raw_end();
        bus_end();
    }
      
    void drawNumber(int x, int y, int num, uint16_t fg, uint16_t bg, int scale = 1) {
        if (num < 0) num = 0;
        char buf[10];
        sprintf(buf, "%d", num);
         
        int px = x;
        for (int i = 0; buf[i] != '\0'; i++) {
            drawChar(px, y, buf[i], fg, bg, scale);
            px += (5 + 1) * scale; 
        }
    }
     
    void drawString(int x, int y, const char *str, uint16_t fg, uint16_t bg, int scale = 1) {
        int px = x;
        for (int i = 0; str[i] != '\0'; i++) {
            drawChar(px, y, str[i], fg, bg, scale);
            px += (5 + 1) * scale;  
            }
        }

        static uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
        return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        }
   };
#endif
