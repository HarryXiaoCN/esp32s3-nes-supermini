# ESP32-S3 SuperMini 掌机 —— NES 模拟器 + MP3 播放器

一块 22.5×18mm 的 ESP32-S3 SuperMini，加上 240×240 屏幕、SD 卡、8 个按键和一个
I2S 功放，做成一台能装在口袋里的掌机：**从 SD 卡读 ROM 玩 NES，也能放 MP3**。

界面是 iOS 深色风格的中文 UI（可切英文），三个顶层页面用 SELECT 键循环切换：
**游戏 → 音乐 → 设置**。

> 这是系列的第二块板子。第一块是 [esp32s3-nes-breadboard](https://github.com/HarryXiaoCN/esp32s3-nes-breadboard)
> （N16R8 + ROM 烧在 flash 分区里）。本项目的差异：SuperMini 只有 4MB flash / 2MB PSRAM，
> 所以 ROM 和音乐都改从 SD 卡读，并且多了 I2S 音频输出。

## 特性

- **NES 模拟器**：nofrendo 核心（6502 / PPU / APU / MMC 全保留），支持 60 多种 mapper
- **MP3 播放器**：minimp3 解码，实测 **9.66x 实时 @320kbps、CPU 占用 10%**
- **三页页签导航**：游戏 / 音乐 / 设置并列，SELECT 向后切、B 向前切
- **iOS 深色设计**：分组卡片、系统蓝选中条、滚动条、离散填充式音量条
- **中英双语**：编译进 16×16 点阵字库（7550 字，含 GB2312 一级/二级汉字 + 日文假名），
  首次开机选择语言，之后存 NVS
- **流式渲染**：分块转 RGB565 到内部 RAM 再推 SPI，不占整帧缓冲

## 硬件

| 部件 | 型号 |
|---|---|
| 主控 | ESP32-S3 SuperMini（ESP32-S3FH4R2：4MB flash + 2MB QSPI PSRAM） |
| 屏幕 | ST7789 240×240，SPI，只写不读 |
| 存储 | microSD 卡模块（带 AMS1117 稳压，**VCC 必须接 5V**） |
| 音频 | MAX98357A I2S 功放 + 4Ω/8Ω 小喇叭 |
| 按键 | 8 个（UP/DOWN/LEFT/RIGHT/A/B/START/SELECT），内部上拉 |

## 接线

以 `firmware/hw_config.h` 的实际定义为准。改引脚时先改代码再改本文。

### 屏幕 ST7789

| 屏幕引脚 | 接到 | 板子位置 |
|---|---|---|
| VCC / BLK | 3V3 | 侧 B |
| GND | GND | 侧 B |
| SCL (SCK) | **12** | 侧 B |
| SDA (MOSI) | **11** | 侧 B |
| RES | **9** | 侧 B |
| DC | **13** | 侧 B |
| CS | **10** | 侧 B |

### SD 卡模块

| 模块引脚 | 接到 | 说明 |
|---|---|---|
| VCC | **5V** | ⚠️ 带 AMS1117 稳压器的模块**必须接 5V**：接 3V3 时压差不够，卡实际只有 ~2.2V，会出现「命令能应答、读数据全 0」的怪现象 |
| GND | GND | |
| MISO | **8** | 只有 SD 用 |
| MOSI | **11** | **与屏幕共用** |
| SCK | **12** | **与屏幕共用** |
| CS | **7** | 独立片选 |

> SPI 总线共享：屏幕和 SD 并联在 11/12 上，靠各自的 CS 分时访问。

### MAX98357A

| 模块引脚 | 接到 | 说明 |
|---|---|---|
| VIN | **5V** | 5V 比 3.3V 声音大 |
| GND | GND | |
| LRC | **5** | 声道时钟 |
| BCLK | **6** | 位时钟 |
| DIN | **4** | 数据 |
| SD / GAIN | 悬空 | 悬空 = 常开、9dB 增益 |
| OUT+ / OUT- | 小喇叭 | |

> ⚠️ 驱动代码必须用 `I2S_SLOT_MODE_STEREO`（每帧左右两个 16-bit slot）。用 MONO 会让
> BCLK 减半、功放解析错乱，输出直流把音圈烧热。

### 按键（8 个，全部一端接 GPIO、另一端接 GND）

| 按键 | GPIO | 丝印/位置 |
|---|---|---|
| UP / DOWN / LEFT | 1 / 2 / 3 | 侧 A |
| RIGHT / A | 43（`TX`）/ 44（`RX`） | 侧 A |
| B / START / SELECT | 14 / 16 / 42 | **底部焊盘** |

> 全部 `INPUT_PULLUP`，按下为低电平。

### 引脚占用

```
边缘 15 个（全用满）：
  侧 A: TX  RX  1  2  3  4  5  6  7      ← 9 个全是 GPIO
  侧 B: 13  12  11  10  9  8             ← 6 个 GPIO（另 3 个是电源）
底部 3 个：14（B）、16（START）、42（SELECT）
```

**禁用**：`17`/`39`（本板已焊坏）、`19`/`20`（USB）、`26~32`（片内 flash）、
`33~38`（PSRAM）、`45`/`46`（strapping）、`48`（板载 RGB LED）。

## 用法

| 场景 | 按键 |
|---|---|
| 切换页面 | **SELECT** 下一页 / **B** 上一页 |
| 游戏页 | 上下选游戏、**A** 开始 |
| 音乐页 | 上下选曲、**A** 播放/暂停、左右切歌 |
| 设置页 | 上下选项目、左右调值 |
| 游戏内退出 | 长按 **SELECT+START** 0.6 秒 |
| 切换界面语言 | 长按 **START** 0.6 秒（或进设置页） |

游戏 ROM 放 SD 卡**根目录**，`.nes` 后缀；音乐放根目录，`.mp3` 后缀。卡必须是 **FAT32**。

## 编译

Arduino IDE 1.8.19 / ESP32 core 3.3.12：

```
Board           : ESP32S3 Dev Module
Flash Size      : 4MB (32Mb)
PSRAM           : QSPI PSRAM        ← 不是 OPI
Flash Mode      : QIO 80MHz
CPU Frequency   : 240MHz
USB CDC On Boot : Enabled

fqbn: esp32:esp32:esp32s3:FlashSize=4M,PSRAM=enabled,FlashMode=qio,CPUFreq=240,USBMode=hwcdc,CDCOnBoot=cdc
```

`src/` 作为库安装到 `Documents/Arduino/libraries/`，`firmware/` 是 sketch。

## 烧录

```powershell
# 整片镜像（首次 / 改了分区表或 bootloader）
esptool --chip esp32s3 -p COMx -b 921600 write-flash 0x0 .\firmware\build\Dsn_nes_Emulator.ino.merged.bin

# 日常改代码：只烧 app 分区，2.4 秒，且不擦 NVS（语言/音量设置保留）
esptool --chip esp32s3 -p COMx -b 921600 write-flash 0x10000 .\firmware\build\Dsn_nes_Emulator.ino.bin
```

> 必须烧 `merged.bin` 整片镜像的场景：分开烧 bootloader/partitions/app 会导致
> `TG0WDT` 反复复位。

## 性能

- SPI 40MHz，整屏 115KB 约 23ms；渲染走 60 行分块，只有推到屏幕的部分占带宽
- MP3 解码：320kbps / 44.1kHz 立体声 → **9.66x 实时**，CPU 约 10%
- 解码跑在独立 32KB 栈任务里（`mp3dec_scratch_t` 有约 14KB 在栈上，Arduino 默认
  8KB 栈的任务会 Stack canary 崩溃）

## 目录结构

```
src/                作为 Arduino 库编译
  osd.cpp           UI 页面、SD ROM 读取、I2S 音频
  ui_theme.h        UI 设计 token（颜色/行高/内边距的唯一来源）
  mp3_player.cpp    MP3 播放（扫描/解码任务/音量/高通）
  minimp3.h         MP3 解码器（单头文件，CC0）
  tft_driver.h      ST7789 驱动
  cn_font.h         16×16 点阵字库（生成物，不入库）
  nes*.c / map*.c   nofrendo 核心（不要改）
firmware/
  Dsn_nes_Emulator.ino  主流程
  hw_config.h           引脚定义 + 编译期护栏
  controller.cpp        按键读取
tools/
  mock_menu.py          像素级 UI 模拟器（解析固件源码重放画面）
  gen_cn_font.py        生成字库
  extend_cn_font.py     补字库缺字
  mp3_bench/            解码性能基准
  sd_put/               通过串口往 SD 卡写文件
docs/                 UI 模拟图
```

## 已知限制

- **音质受硬件限制**：MAX98357A 固定 9dB 增益，音量接近 100% 时会削顶（高音最明显）。
  驱动里加了 180Hz 二阶高通（小喇叭放不出低频，滤掉能给中高频腾出动态余量），
  并把音量放在 60~70% 最干净
- **字库**：一级汉字用像素字体（观感好），新增的二级汉字/假名用系统黑体补的，笔画多的字略糊
- **帧率**：设计目标 ~40fps，SuperMini 上的实测数据待补

## 许可

**GPL-3.0**。nofrendo 核心是 GPL，整个项目随之。

仓库**不含**游戏 ROM、字体文件和生成的 `cn_font.h`（均有版权或不可再分发），
见 `.gitignore`。字库请用 `tools/gen_cn_font.py` 从你本机的字体自行生成。
