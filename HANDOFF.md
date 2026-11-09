# 交接手册 —— ESP32-S3 面包板 NES 掌机

> 写给接手的 AI / 开发者。读完这份就能继续干，不用翻聊天记录。
> 最后更新：2026-11

---

## 一、项目全景

同一个 NES 模拟器代码库，做了**两个硬件版本**：

| | **项目 A：N16R8 版（已完成、已开源）** | **项目 B：SuperMini SD 版（进行中）** |
|---|---|---|
| 代码位置 | 项目 A 仓库（已开源） | 本项目仓库 |
| 上游源项目 | Esp32-S3-nes-emulator-by-DSN（GitHub 开源） | 从 A 的代码复制改造 |
| 主控 | ESP32-S3-WROOM-1 **N16R8** | ESP32-S3**FH4R2**（SuperMini） |
| Flash / PSRAM | 16MB / **8MB OPI** | 4MB / **2MB QSPI** |
| ROM 来源 | **flash 分区**（13 槽 × 1MB） | **SD 卡**（SPI，FAT32） |
| 音频 | 无 | **MAX98357A I2S** |
| 屏幕 | ST7789 240×240 | 同左 |
| 按键 | 8 个（全部边缘） | 8 个（5 边缘 + **3 底部焊盘**） |
| 状态 | ✅ 已开源：https://github.com/HarryXiaoCN/esp32s3-nes-breadboard | ⏳ 编译通过、已烧录、**屏幕待确认** |

**项目 A 的 GitHub 仓库**（公开，GPLv3）：`HarryXiaoCN/esp32s3-nes-breadboard`
- 提交历史：`836a17b` 初始 → `0e7dd01` 双语 → `e948445` 语言选择图
- 不含 ROM、不含字体（`.gitignore` 拦住，另有 `tools/check_no_sensitive.ps1` 做提交前检查）

---

## 二、硬件接线

### 板子 A：N16R8（已完成，稳定运行）

| 用途 | 引脚 |
|---|---|
| 屏幕 SCK / MOSI / CS / DC / RST | 12 / 11 / 10 / 15 / 9 |
| 按键 UP/DOWN/LEFT/RIGHT | 13 / 14 / 8 / 17 |
| 按键 A/B/START/SELECT | 45 / 21 / 47 / 39 |

### 板子 B：SuperMini（进行中）

**边缘 15 个 GPIO（丝印）**
```
侧 A（9 个）：TX  RX  1  2  3  4  5  6  7      ← 全部是 GPIO，用满
侧 B（9 个）：5V GND 3V3 13 12 11 10 9 8      ← 6 个 GPIO + 3 个电源
```

**底部裸露焊盘（1.27mm 间距，很难焊）**
```
左列（上→下）：34  33  21  19  17  16  15  14
右列（上→下）：38  37  36  35  45  42  41  40  39
中间：        48(接RGB LED)  47  46(strapping)
```

**最终接线表**

| 功能 | GPIO | 位置 |
|---|---|---|
| 屏幕 SCK / MOSI / CS / DC / RST | **12 / 11 / 10 / 13 / 9** | 边缘 |
| SD MISO / CS | **8 / 7** | 边缘（SCK/MOSI 与屏幕共享） |
| I2S BCLK / LRC / DIN | **6 / 5 / 4** | 边缘 |
| UP / DOWN / LEFT | **1 / 2 / 3** | 边缘 |
| RIGHT / A | **43（丝印 TX）/ 44（丝印 RX）** | 边缘 |
| B / START / SELECT | **14 / 16 / 42** | **底部焊盘** |

**已焊坏、代码里已不再引用**：`GPIO17`、`GPIO39`

**绝对禁用**：`19`（USB D-）、`20`（USB D+）、`26~32`（片内 flash）、`33~38`（PSRAM）、`45/46`（strapping）、`48`（RGB LED）

---

## 三、项目 A 完成的功能（可直接复用）

1. **中文界面**：`cn_font.h` 里 3849 个 GB2312 一级汉字 16×16 点阵 + ASCII（120KB，放 flash 不占 RAM），二分查找
2. **中英双语**：`ui_lang_t` + `TXT_*` 字符串表 + NVS 持久化 + 菜单长按 START 切换 + 首次开机选择界面（10 秒无操作默认中文）
3. **iOS 风格 UI**：深色分组卡片 `#1C1C1E`、圆角、系统蓝 `#0A84FF` 选中条、chevron、Home 指示条
4. **流式渲染（性能关键）**：`STREAM_ROWS 60` 行一块转 RGB565 到**内部 RAM** `stream_buf`，再 `tft.pushImage()` 直推 SPI；**绝不能退回 PSRAM 整帧缓冲**（会掉一半帧率）
5. **游戏内退出**：长按 SELECT+START 0.6 秒 → `main_quit()` → `.ino` 的 `for(;;)` 循环回到菜单（不重启）
6. **完整 ST7789 初始化**：`tft_driver.h` 的 `init()` 里有 VCOMS(0xBB)、PWCTRL1(0xD0)、伽马等全部命令，**不要删**
7. **串口防阻塞**：`Serial.setTxTimeoutMs(0)`，且游戏过程中不输出串口

**性能数据（N16R8）**：push 18.3ms / 38~42fps。SPI 80MHz 理论 11.5ms，115200 字节/帧是硬成本。

---

## 四、踩过的坑（**接手前必读，别再踩一遍**）

### 已解决

| # | 现象 | 根因 | 解法 |
|---|---|---|---|
| 1 | ESP-IDF 下载卡死 10 分钟 | urllib 单连接无超时无续传 | 改用 curl `--retry --speed-limit -C -` 预下载，脚本留在本机（如 `tools/fetch-idf-tools.ps1`） |
| 2 | 烧完 app 反复复位 `TG0WDT`，USB 反复枚举 | **分开烧** bootloader/partitions/app | 必须烧 4MB/16MB 的 **`merged.bin` 整片镜像** |
| 3 | 屏幕背光亮但全黑 | ST7789 初始化缺 `VCOMS(0xBB)`、`PWCTRL1(0xD0)`、伽马 | 补全标准初始化序列 |
| 4 | 面包板 80MHz SPI 花屏 | 杜邦线信号质量 | 降到 40MHz（`TFT_SPI_FREQ`） |
| 5 | 进游戏要等 1 分钟 | **USB CDC 无主机读取时 `Serial.printf` 阻塞** | `Serial.setTxTimeoutMs(0)` + 游戏内不打印日志 |
| 6 | 帧率只有 23fps | 整帧经 PSRAM 中转 | 流式渲染 → 内部 RAM 分块 → 38fps |
| 7 | 切换选项闪黑 | 用背景黑清屏再重画 | 改用**卡片底色**覆盖 + 只重绘变化的两行 |
| 8 | 卡片只有左边是圆角 | `fill_round_rect` 漏画右侧两段圆弧 | 补齐四角 |
| 9 | 游戏名显示"游戏1/2/3" | 烧录脚本 `write-flash -q` 参数错，错误被 `2>&1 \| Out-Null` 吞掉 | 不吞错误 + 检查退出码 + 回读 flash 验证 |
| 10 | SuperMini 按键接 26~31 会死机 | **ESP32-S3 片内 flash 就在 GPIO26~32** | 全部改用 9~14 等安全脚 |
| 11 | 底部焊盘焊坏 2 个（17、39） | 1.27mm 间距，0.1mm² 以上线径 + 无胶固定 | 换 42；焊后必须用胶固定线 |
| 12 | 语言选择界面卡住 | 只接屏幕没接按键 | 加 10 秒超时默认中文 |
| 13 | 日志显示错误引脚 | `.ino` 里硬编码了旧字符串 | 已改为宏拼接 |
| 14 | 屏幕"未找到游戏"，`[3/4] SD 卡挂载失败` | **SD 模块 VCC 接到了 RX（GPIO44）**，卡等于没供电 | VCC 接到电源脚（见 #15） |
| 15 | 同上，接对脚后仍失败 | **SD 模块带 AMS1117 稳压器，VCC 必须接 5V**；接 3V3 时输出只有 ~2.2V | 模块 VCC → 板子 **5V**（卡座实测 3.3V） |
| 16 | 挂载成功但 "Found 1 games" | **源 ROM 全都没有 `.nes` 扩展名**，固件只认 `.nes` | 批量补 `.nes` 后缀再拷入根目录 |
| 17 | 诊断日志被截断成乱码/残缺 | `setTxTimeoutMs(0)` 时 USB CDC FIFO 满就丢字节 | 挂载/诊断阶段临时 `setTxTimeoutMs(1000)`，之后恢复 0 |
| 18 | 一直怀疑 MISO 读不到（`digitalRead` 恒 0） | SPI 模式下该脚不是 GPIO，`digitalRead()` 本身不可靠；真因是接线 | 用 `SPI.transfer()` 读、读 GPIO matrix `func_in_sel_cfg` 验证引脚映射，别信 `digitalRead` |
| 19 | **功放一开机喇叭就发烫 + 底噪** | **I2S 用了 `I2S_SLOT_MODE_MONO`**：ESP32-S3 把它配成单 slot 帧（BCLK = 采样率×16），而 MAX98357A 要标准立体声帧（×32）。帧错位后功放输出直流 → 音圈发热 | `osd_init_sound()` 改 `I2S_SLOT_MODE_STEREO`，写入用 `i2s_write_mono_as_stereo()` 复制左右声道（开机音效/游戏音频/退出静音三处都要改） |
| 20 | **开机要等 20 秒** | ①诊断时留的 `setTxTimeoutMs(1000)`：没开串口时每条日志阻塞 1 秒；②`openNextFile()` 为每个文件真 fopen 一次（391 个文件≈7 秒）；③`show_magic()` 30 步全屏重绘 3 秒；④`show_menu()` 又重复扫描一遍 | 全程 `setTxTimeoutMs(0)`（只在挂载失败诊断时临时 1000）；扫描改 `getNextFileName()`（不打开文件，**7.7s → 0.84s**）；开机动画 8 步；`show_menu()` 只在列表为空时补扫。**最终开机 ~2.6 秒** |
| 21 | 游戏路径变成 `//xxx.nes` | `getNextFileName()` 返回的名字可能自带前导 `/`，又拼了一个 | 先跳过前导斜杠再拼（不影响加载，只是日志难看） |
| 22 | START+SELECT 退不出游戏 | 实际是按键没接好/没按够时间；代码本身正常（0.6 秒长按） | 接好底部焊盘 START=16 / SELECT=42 后稳定可用；日志 `[BTN] pressed mask=0x0C SELECT START` 可确认 |

**SD 卡排障方法论（这次验证有效）**：
1. `SD.begin` 失败时先看 core 日志（临时加 `firmware/build_opt.h` 内容 `-DCORE_DEBUG_LEVEL=3`）：
   `f_mount failed (13)` = 卡通了但文件系统不对；`(11/1)` = 底层 IO 失败
2. 绕过 SD 库手动发 CMD0：`FF 01` = 卡和接线都正常；`00` = MISO 恒低（没供电/没接）；全 `FF` = 卡没说话
3. 读 `CMD58` 的 OCR 能验数据通道；OCR 正常但 `CMD17` 读扇区全 0 → 供电不足（典型 AMS1117 接 3V3）
4. 真需要时可以 bit-bang 读扇区做对照（这次证明 SPI 外设没问题，问题在线/供电）

### 待验证

| 项 | 状态 |
|---|---|
| **屏幕显示** | ✅ 正常（语言选择 → 游戏菜单） |
| **SD 卡挂载** | ✅ 成功（20MHz，7580MB，SDHC，391 个游戏） |
| **按键** | ✅ 8 个全部可读（UP/DOWN/LEFT/RIGHT/A/B/START/SELECT） |
| **游戏启动/退出** | ✅ 赤影战士(mapper 4) 246ms 进游戏；START+SELECT 0.6s 回菜单 |
| **I2S 音频** | ✅ STEREO 帧修复后不再发烫；开机音效/游戏声音正常 |
| 帧率 | ⏳ 待实测（日志有 `[PERF]` 与 push 耗时） |
| mapper 兼容性 | ⏳ 391 个 ROM 里有大量中文 RPG，需逐个试 |

---

## 五、项目 B 当前状态（精确）

```
位置:  本项目根目录
固件:  firmware/build/Dsn_nes_Emulator.ino.merged.bin  (4,194,304 字节)
       Sketch uses 756,316 bytes (57%)  /  全局变量 83,968 (25%)
编译:  通过，无 error
烧录:  已烧到 COM8（4MB @ 0x0，Hash verified）
屏幕:  ✅ 正常（语言选择 → 菜单）
SD 卡: ✅ 挂载成功，20MHz，7580MB，type=3(SDHC)
按键:  ✅ 8 个全部可读（含底部焊盘 B=14 / START=16 / SELECT=42）
音频:  ✅ MAX98357A，I2S 立体声帧，开机音效 + 游戏声
开机:  ~2.6 秒（TFT ready 705ms → 扫描 842ms → 音效 1374ms → 菜单 1229ms）
日志:  [SD] try 20000 kHz ... OK in 63 ms
       [3/4] SD ready: 7580 MB, type=3, SPI=20000 kHz
             found 391 .nes game(s) in SD root [842 ms]
       [T] boot sound done [1374 ms]
       [T] menu magic done [1229 ms]
       [MENU] Found 391 games
[PERF] 启动游戏 -> 第一帧: 246 ms（赤影战士，ROM 读取 183ms）
```

**板子信息**：`ESP32-S3 (QFN56) rev v0.2`，`Embedded Flash 4MB (XMC)`，`Embedded PSRAM 2MB`，MAC `d4:05:92:44:47:3c`，**端口 COM8**

**SD 卡内容（2026-11 已备好）**：7.4GB FAT32、512B 扇区、32KB 簇，根目录 **391 个 `.nes`**
（源 ROM 来自本机收藏，原本**全都没有 `.nes` 扩展名**，已批量补齐后拷入）
游戏列表已改为**动态分配（无固定上限）**，优先放 PSRAM，391 个全显示。

---

## 六、下一步待办（按顺序）

### 1~6 已全部完成 ✅

| 项 | 结果 |
|---|---|
| 屏幕 | 语言选择 → iOS 风格菜单，正常 |
| SD 卡 | 20MHz 挂载，391 个游戏（VCC 接错到 RX + 需要 5V + 缺 `.nes` 后缀，三个坑已解决） |
| 按键 | 8 个全部可读（含底部焊盘 B=14 / START=16 / SELECT=42） |
| 游戏启动 | 赤影战士（mapper 4）246ms 出第一帧 |
| 游戏退出 | START+SELECT 长按 0.6 秒 → 回菜单（约按 1 秒更稳） |
| 音频 | MAX98357A 立体声帧正常，喇叭不再发烫 |
| 游戏列表 | 动态分配、无固定上限，391 个全显示 |

### 7. 帧率与兼容性实测（剩下的主要工作）
- 日志里有 `[PERF] 启动游戏 -> 第一帧: xxx ms` 和 push 耗时；测几个不同类型游戏看帧率
- **mapper 兼容性**：卡上 391 个 ROM（大量中文 RPG / 汉化版），nofrendo 只支持一部分 mapper。
  跑不动的游戏会黑屏或花屏，属于正常，可换 ROM
- 若某游戏明显慢，先确认 `STREAM_ROWS` 没被改小、没有回退到 PSRAM 整帧缓冲

### 8. 音量 / 音质微调（按需）
如果游戏声音偏响或削顶，改 `.ino` 里 `-volume 100`，或 `osd.cpp` 的 `AUDIO_GAIN`。
开机音效音量在 `osd_boot_sound()` 里（幅度 9000/32767）。

### 9. 补文档
- `README.md` 接线表与实际焊好的对一遍（SD VCC 已更正为 5V）
- 截图存 `docs/`

---

## 七、代码结构（项目 B）

```
src/
  osd.cpp          ← 主改动文件：UI、多语言、SD ROM 读取、I2S 音频
  tft_driver.h     ← ST7789 驱动，所有 SPI 访问都包在 beginTransaction 里
  cn_font.h        ← 自动生成的中文字库（120KB）
  nes.c / nes_ppu.c / nes_apu.c / nes6502.c / nes_mmc.c / nes_rom.c
  mappers/*        ← 这些是 nofrendo 核心，【一行都不要改】
  nofrendo.c / bitmap.c → 同上
firmware/
  Dsn_nes_Emulator.ino   ← 主流程：SPI.begin → tft.init → SD.begin → 语言 → for(;;) 菜单
  hw_config.h            ← 所有引脚定义 + 编译期护栏（按键接错脚会 #error）
  controller.cpp         ← 按键读取（INPUT_PULLUP）
  partitions.csv         ← 项目 A 用；项目 B 不需要（用默认分区）
reference/
  osd_sdcard_original.cpp ← 上游原始 SD 卡实现，改 ROM 层时可参考
```

**关键接口**（换 ROM 来源只需要改这三个函数）：
```c
extern "C" int  osd_rom_open(const char *path);   // SD.open(path, FILE_READ)
extern "C" int  osd_rom_read(void *dst, int len); // 必须精确返回 len！循环读满
extern "C" void osd_rom_close(void);
```

---

## 八、编译与烧录

### 库环境（**注意**）
```
~\Documents\Arduino\libraries\Esp32NofrendobyDSN\   ← 当前用的库（项目 B 的 src）
~\Documents\Arduino\_unused_libs\Esp32-S3-nes-emulator-by-DSN\  ← 项目 A 的旧库（被移走避免同名头文件冲突）
```
**编译项目 A 时要把旧库移回来，编译项目 B 时用新库。两者不能同时在 libraries 里。**

### 编译命令

**项目 B（SuperMini）**
```powershell
$ide = "<Arduino IDE 1.8.19 安装目录>"   # 自带 arduino-builder.exe
$repo = (Get-Location).Path                     # 在项目根目录执行
$build = "$sketch\build"
Remove-Item $build -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $build -Force | Out-Null

# 同步 src → 库
$lib = "$env:USERPROFILE\Documents\Arduino\libraries\Esp32NofrendobyDSN"
Remove-Item "$lib\src" -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item "$repo\src" "$lib\src" -Recurse -Force

& "$ide\arduino-builder.exe" -compile `
  -hardware "$ide\hardware" -hardware "$env:LOCALAPPDATA\Arduino15\packages" `
  -tools "$ide\tools-builder" -tools "$ide\hardware\tools\avr" `
  -built-in-libraries "$ide\libraries" -libraries "$env:USERPROFILE\Documents\Arduino\libraries" `
  -fqbn "esp32:esp32:esp32s3:FlashSize=4M,PSRAM=enabled,FlashMode=qio,CPUFreq=240,USBMode=hwcdc,CDCOnBoot=cdc" `
  -ide-version 10819 -build-path $build -warnings default -logger=machine `
  "$sketch\Dsn_nes_Emulator.ino"
```

**项目 A（N16R8）** fqbn 换成：
```
esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,FlashMode=qio,CPUFreq=240,PartitionScheme=default,USBMode=hwcdc,CDCOnBoot=cdc
```

### 烧录
```powershell
$esptool = "$env:USERPROFILE\.espressif\python_env\idf6.1_py3.14_env\Scripts\esptool.exe"
# 整片烧（必须用 merged.bin，不能分开烧）
& $esptool --chip esp32s3 --port COM8 --baud 921600 write-flash 0x0 `
  "$repo\firmware\build\Dsn_nes_Emulator.ino.merged.bin"
```

### 读串口日志
```powershell
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$sp = New-Object System.IO.Ports.SerialPort('COM8',115200,[System.IO.Ports.Parity]::None,8,[System.IO.Ports.StopBits]::One)
$sp.DtrEnable = $false; $sp.RtsEnable = $false; $sp.ReadTimeout = 300
$sp.Open(); $sp.DiscardInBuffer()
$sp.RtsEnable = $true; Start-Sleep -Milliseconds 150; $sp.RtsEnable = $false   # 硬复位
# 然后循环 ReadExisting() 收集 15~30 秒
```

---

## 九、环境依赖

| 项 | 值 |
|---|---|
| Arduino IDE | **1.8.19**（带 arduino-builder.exe，装在任意目录） |
| ESP32 core | **3.3.12** |
| esptool | `$env:USERPROFILE\.espressif\python_env\idf6.1_py3.14_env\Scripts\esptool.exe` |
| Python | 3.14（esptool/字库脚本用）；系统还有 3.10 / 3.11 |
| ESP-IDF | v6.1 已装在 `G:\Espressif\esp-idf-v6.1`（非必须，仅提供 esptool） |
| 代理 | 本机 HTTP 代理（访问 GitHub 用；git 需配 `http.sslBackend=openssl`） |
| GitHub 账号 | `HarryXiaoCN`（凭据已存 Windows 凭据管理器） |

---

## 十、快速排障

| 现象 | 先查 |
|---|---|
| esptool 连不上 | 换数据线；`Get-PnpDevice` 看是否有 `VID_0000&PID_0002`（描述符失败=线不行） |
| 烧完反复重启 | 是不是分开烧的？必须 merged.bin 整片 |
| 屏幕全黑 | ①背光（BLK 接 3.3V）②SPI 引脚 ③降 `TFT_SPI_FREQ` |
| 串口无输出 | USB CDC 需 `CDCOnBoot=cdc`；或用 `Serial.setTxTimeoutMs(0)` 避免阻塞 |
| 按键一直读 0 | 该脚是 flash/PSRAM/strapping；查 `hw_config.h` 的禁用清单 |
| SD 挂载失败 | FAT32？CS=7/MISO=8 接对？降到 10MHz 试试 |
| 帧率低 | 是否退回了 PSRAM 整帧缓冲？`STREAM_ROWS` 是否被改小？ |

---

## 十一、用户偏好（重要）

- 中文交流，喜欢直接动手、看到结果，**不喜欢反复来回确认**
- 会让 AI 全权决定技术方案，但要**如实报告风险**
- 硬件动手能力强但焊接底部细焊盘有困难（已焊坏 2 个）
- 重视**代码来源的合法性**，明确要求不提交商业 ROM
- 项目目录习惯：工作项目放 `<工作目录>\<中文项目名>`；开源项目放独立的开源目录
