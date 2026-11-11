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
3. **iOS 风格 UI**：深色分组卡片 `#1C1C1E`、圆角、系统蓝 `#0A84FF` 选中条、chevron、Home 指示条。
   尺寸/配色统一在 `src/ui_theme.h`，三个页面共用 `ui_page/ui_group/ui_row/ui_page_footer` 组件（见第十二节）
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
| 23 | MP3 大音量下"偶尔高音毛刺、像共振、细节丢了" | MAX98357A 9dB 固定增益 + 5V，0dBFS 越过线性区（实测干净峰值 ≈ -11dBFS ≈ 音量 28%）；另 minimp3 默认 int16 输出会在乘音量**之前**硬削顶 | 把音量停在 30% 附近。浮点解码 + -11dBFS 限幅试过，小音量下与"手动调小音量"等价，**已回滚**；详见「音质：MAX98357A 的增益要留余量」 |

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
  osd.cpp          ← 主改动文件：UI 页面、多语言、SD ROM 读取、I2S 音频
  ui_theme.h       ← UI 设计 token：所有页面唯一的颜色/行高/内边距/圆角来源
  tft_driver.h     ← ST7789 驱动，所有 SPI 访问都包在 beginTransaction 里
  cn_font.h        ← 自动生成的中文字库（120KB，含 ASCII 的 16x16 版本，但 UI 不用它）
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
| 代理 | Clash Verge（verge-mihomo），混合端口 `127.0.0.1:7900`。`http.sslBackend=openssl` 已配。**git push 必须走 SOCKS5**（走 HTTP 代理会 `408` / `curl 52 Empty reply`）：<br>`git -c http.proxy=socks5h://127.0.0.1:7900 -c https.proxy=socks5h://127.0.0.1:7900 -c http.version=HTTP/1.1 push origin master` |
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

---

## 十二、UI 统一改造（2026-11，已完成，待上机确认）

### 改造前的问题

配色本身是对的（iOS 深色模式语义色，RGB565 转换全部校验过），问题在**结构**：
三个页面各自手写绘制，共享的只有 8 个颜色宏。

| 元素 | 游戏列表 | 设置页 | 语言选择页 |
|---|---|---|---|
| 行高 | 32 | 44 | 44 |
| accent 侧条 | y+7 高 18 | y+11 高 **16** | y+12 高 20 |
| 侧条占行高 | 56% | **36%** | 45% |
| 文字垂直偏移 | y+8 | y+9 | y+14 |
| 行内左内边距 | 16 | 18 | 20 |
| 卡片底 | 整卡 | **只有选中行** | 整卡 |
| 分隔线 | 有 | **无** | 有 |
| 标题 | 左上+计数 | 居中 | 居中 |
| Home 指示条 | 有 | **无** | 有 |

同一个「选中行」画出了三种样子。

### 一并修掉的 bug

1. `show_settings_page` 的 **UP 和 DOWN 完全一样**（都 `sel=(sel+1)%2`）——两项时凑巧能用，加第三项就废
2. **B 键三个页面一次都没读**（iOS 永远有返回，这里没有）
3. SELECT 同时是「进设置」和「从设置返回」两个相反语义
4. 长按 START 0.6s 切语言是**隐藏手势**，无任何提示
5. `draw_menu()` / `draw_settings()` 是死代码，用红条 `0xF800` / 蓝条 `0x001F` / 绿字 `0x07E0`
6. `menu_brightness` 是从不生效的孤变量
7. 无游戏页是裸 `fillScreen` + 一行字 + 3 秒，第四个"风格"
8. 方向键是恒定 150ms/格，391 个游戏从头翻到尾要按一分多钟

### 现在的结构

三层，从下到上：

```
ui_theme.h          颜色 + 几何 token（唯一来源）
  ↓
osd.cpp 组件层      ui_page / ui_group / ui_row / ui_page_footer / ui_scrollbar / ui_input_poll
  ↓
三个页面            游戏列表 / 设置页 / 语言选择页 / 无游戏页 —— 只调组件，不再自己算像素
```

**关键 token**（`src/ui_theme.h`）：
`UI_PAGE_PAD 10` / `UI_ROW_PAD 16` / `UI_ROW_END 14` / `UI_ROW_H 32`（紧凑）/ `UI_ROW_TALL 40`（标准）/
`UI_CARD_R 10` / `UI_ROW_R 8` / `UI_BAR_W 4` / `UI_BAR_H(h) = h*9/16`

**改页面时**：先改 token，不要在页面代码里写魔法数字。新增页面用 `ui_page` + `ui_group` + `ui_row` 拼。

### 顺带做的性能修复（`draw_cn_text`）

改造前：ASCII 走 `tft.drawChar`，而 `drawChar` 内部是**逐像素 `drawFilledRect`** —— 5×7 字模 = 35 次
完整 SPI 窗口事务/字符；汉字走 `draw_cn_glyph`，每字 16 次 `pushImage`。菜单一行 8 个汉字 = 128 次事务，
5 行 640 次 —— **这才是切页/换选项必须 `delay()` 遮丑的根本原因**。

现在：整行先渲染进一条 16px 高的内部 RAM 条带（`static uint16_t band[240]`，480 字节），再逐行
`pushImage`。无论这行多少字，固定 16 次事务。

⚠️ **注意一个坑**：ASCII 只占 7px 格宽的左边 5px，**整格都必须填**，否则字符右侧 2px 会残留上一帧内容。
现在的实现是按格宽（7px，空格 10px）整格填的，别改回去。

### 新增的 UI 行为

- **B = 返回**（设置页）；SELECT 保留为兼容别名
- **方向键连发**：按下沿立即响应，按住 400ms 后每 70ms 一格（`ui_input_poll`）
- **滚动条**：右侧 2px，391 个游戏终于有位置感
- **音量 10 格离散填充条**：比纯数字直观，左右各走一格
- **设置页 section footer**：`下次进入游戏时生效`（iOS 的 footer 说明）
- **footer 提示补全**：游戏列表现在写着 `A开始  SELECT设置`（改造前 SELECT 进设置是隐藏功能）
- `ui_input_reset()` 替代了原来"进设置页先空转 1500ms 等 SELECT 松手"的循环

### 上机验证清单

编译通过（758,344 B / 57%，RAM 88,528 / 27%），`osd.cpp` 零警告，几何用脚本复算过无越界/重叠。
已烧录到 COM8（Hash verified），启动日志干净：语言选择页 + 游戏列表页都真实跑过新组件层，
无 panic、无看门狗复位。**设置页需按 SELECT 才会执行到，视觉观感要靠眼睛确认。**

### 预览 UI（不用烧板子）

```powershell
python tools/mock_menu.py --mode all      # 生成 docs/ui-*.png
```

**这不是"参考固件画的示意图"，是逐像素重放。** 脚本直接解析 `src/cn_font.h` 的汉字
16×16 点阵、`src/tft_driver.h` 的 ASCII 5×7 字模、`src/ui_theme.h` 的 token，再按
`osd.cpp` 组件层的顺序重画一遍 —— 连 `fill_round_rect` 的四角圆弧算法、
`draw_cn_text` 的整格填充、`draw_chevron` 的 5 个像素都照抄固件。所以图和实机应当一致
（包括 RGB565 量化后的色偏，那是屏幕上真实的颜色，故意保留）。

`--mode` 可选 `menu` / `menu-en` / `settings` / `settings-en` / `lang-zh` / `lang-en` /
`empty`，默认 `all`（额外拼一张 3×2 对比图 `ui-sheet.png`）。

⚠️ 解析 `font_5x7` 时必须**先剥掉行尾注释**——每行 `// 0x20 ' '` 这种注释里也有 `0xNN`
形式，一起提取会让字模整体错位、ASCII 全变乱码。

**改 UI 的正确流程**：改 `osd.cpp` / `ui_theme.h` → 跑这个脚本看图 → 满意再烧板子。

### 已知视觉短板：ASCII 比汉字小一档

中文界面里 `A开始` / `SELECT设置` / `80%` 这些混排，英文只有 7px 高、汉字 16px，落差明显
（见 `docs/ui-menu.png` 底部的 `A开始`、`docs/ui-settings.png` 右侧的 `80%`）。
这是改造前就有的既有行为，不是这次引入的。

想做的话有两种路子，都需要重新验证全部文案宽度：

1. **按语言缩放 ASCII**：中文界面里 ASCII 画 2 倍（10×14，步进 14），英文界面保持 1 倍。
   但**游戏名必须是 1 倍**——ROM 文件名可能有 32 字节纯英文，2 倍会严重溢出。
2. **重新生成字库**：给 ASCII 出一套真正的 8×16 点阵（现有 `cn_font.h` 里的 ASCII 是 8×8
   放大成 16px 的等宽版，一行只放得下 15 个字符）。需要 `mc.ttf`。

### 没做的（评估过，故意跳过）

| 项 | 为什么不做 |
|---|---|
| push 转场动画 | 全屏 115KB / 40MHz SPI = 23ms 一帧，滑入要 6~8 次半屏重推，100ms+ 且撕裂 |
| 字号层级（34/28/17pt） | 字库只有 16×16 汉字，2 倍最近邻放大会把汉字糊成方块；层级改用留白和颜色做 |
| A–Z 索引条 | 391 个中文文件名要额外拼音首字母表，收益不抵成本 |
| 毛玻璃 / 半透明 | RGB565、无 DMA 双缓冲，做不了 |
| ASCII 换成 16px 点阵 | `cn_font.h` 里虽有 ASCII，但那是 8×8 字模 2 倍放大的**等宽 16px**，一行只能放 15 个字符，英文提示会溢出。保持 5×7 |

---

## 十三、顶层页面与 MP3（2026-11 起）

### 导航模型：三个并列页面，不是父子关系

**这一点在改造时理解错过一次，记下来避免再错**：SELECT 不是"进入设置页"，而是
**在三个顶层功能页之间循环切换**：

```
游戏  ──SELECT──▶  音乐  ──SELECT──▶  设置  ──SELECT──▶  游戏
  ◀──────B──────      ◀──────B──────      ◀──────B──────
```

- 三个页面**并列**，没有"返回上一层"的概念
- 页签画在**标题那一行**，不是 iOS 标准的底部 Tab Bar：240 高的屏上底部 tab bar
  至少要 40px，游戏列表会从 5 行掉到 4 行，391 个游戏的翻页成本直接上去
- 选中页签：主文字色 + 下方 2px 系统蓝指示条；未选中：次要文字色
- 想加新页面：在 `ui_page_t` 的 `PAGE_COUNT` 前插一项 + 补 `TXT_TABS` 一行 +
  写一个 `page_xxx_loop()` + 在 `show_menu()` 的 switch 里加一个 case

代码结构（`src/osd.cpp`）：

```
ui_page_t g_page                  当前顶层页
ui_page_tabs(active, trailing)    画页签 + 右上角计数
ui_page_switch(hw)                SELECT 向后 / B 向前，返回 true = 已切页
page_games_loop()                 游戏列表；返回 >=0 = 选中游戏
page_music_loop()                 音乐页
page_settings_loop()              设置页
show_menu()                       调度器，永不返回 -1（空状态也在页面内等切页）
```

每个 `xxx_loop()` 内部是死循环，切页时改 `g_page` 然后 `return`，由调度器重画下一页。
游戏列表的 `top_index` / `selected` 是 `static`，切页回来光标停在原处。

### 音乐页：已实现（2026-11）

播放链路打通：SD 读帧 → minimp3 解码（独立 32KB 栈任务）→ I2S 44100Hz 输出。

| 文件 | 作用 |
|---|---|
| `src/minimp3.h` | MP3 解码器，单头文件，编译后约 22KB flash |
| `src/mp3_player.h` | 播放器对外接口 |
| `src/mp3_player.cpp` | 曲目扫描 + 播放任务 + 音量 + 单声道转立体声 |
| `osd.cpp` 的 `page_music_loop()` | 曲目列表、当前播放高亮、状态行 |

**按键**：上下选曲（只移高亮）、A 播放/暂停、左右切歌、B/SELECT 切页。

**I2S 所有权**（这块最容易出错）：

- `osd_i2s_begin(rate)` 会比对采样率，**同频直接返回不重配**——否则每次进游戏
  都重建 DMA 通道，切换瞬间爆音
- 进游戏前 `osd_init_sound()` 先调 `mp3_suspend()`：停任务 → 等它退出 →
  `osd_i2s_end()`，然后用 22050Hz 重开给 NES
- 退出游戏后 I2S 保持 22050Hz，用户回音乐页按 A 时播放器自己重配回 44100Hz
- **44.1k ↔ 22.05k 必须真的 end+begin**，只做静音会让音调差一倍

**已验证**：真机出声正常；放歌 → 进游戏 → 退出 → 继续放，I2S 交接没问题。
（RAM 数字后来因为 32KB 读缓冲涨到 41%，见下面的音质一节。）

**踩坑**：拷完歌插回 SD 卡后第一次上电 `SD mount FAILED`（MISO 恒低），
复位一次就正常了——插卡接触问题，不是固件。碰到时先重插/复位再怀疑代码。

### 音乐页首版的两个 UI bug（已修，记下来别再犯）

**1. 长曲名换短曲名，右边留一截残影**

不是"忘了清屏"，是**文字画到了卡片外面**：

```
卡片范围        x = 10..230
长曲名画到      x = 26..239   ← 溢出卡片 9 像素
重绘卡片只覆盖  10..230        ← 239 那截永远清不掉
```

修法：新增 `draw_text_fit()`，按卡片可用宽度做 UTF-8 安全截断（末尾补 `..`）。
`ui_row` 内部和音乐页曲名都走它，**凡是变长内容（曲名 / ROM 名）都必须截断**。

**2. 翻选项时闪一下**

首版每次按键都 `draw_list()` 无条件重画 5 行；游戏列表早就是"只重画变化的 2 行"。
现在音乐页也改成：`top` 变了才整块重画，否则只刷换掉的两行。

**3. 漏了滚动条**

游戏列表有、音乐页忘了加。注意"只重画两行"的分支里滚动条也要单独更新
（滑块位置跟 `top` 走），否则翻页后滑块不动。

### 字库扩展：3849 → 7550 字

原来 `cn_font.h` 只有 GB2312 **一级**汉字 + ASCII。实测 SD 卡上 42 首歌名里
**13 首有方块**：

- 日文假名 30 种（`も` `な` `ヨ` `サ` `キ` `シ` `ョ` `レ` `ー`…）
- 二级/繁体汉字 14 种（`奕` `靓` `輪` `遠` `澤` `愛` `織` `紀` `風` `磯` `戸` `見` `琪` `蜢`）
- 全角标点 `（）：·～`，还有西里尔 `Звезда`

**做法**：原有 3849 个字的字形**原样保留**（一级汉字用的是像素字体，观感好），
只把缺的 3701 个字符用系统黑体（`simhei.ttf`）渲染后追加，按码点重排。

| | 之前 | 之后 |
|---|---|---|
| 字数 | 3849 | **7550** |
| 字模 | 120 KB | **236 KB** |
| Flash | 59% | **69%** |

原字库备份在 `src/cn_font.h.bak`（已在 `.gitignore` 里，不入库）。
重新生成见 `tools/gen_cn_font.py`。

⚠️ 新增字符是**黑体 16×16**，笔画多的字会比原来的像素字体糊一点——这是拿不到
`mc.ttf` 的代价。以后找回原字体的话，直接用 `tools/gen_cn_font.py` 重新生成全表。

**另一个代价**：Flash 从 59% 涨到 69%，留给未来的空间少了。真要省空间，可以只保留
实际用到的字符（扫描 SD 卡文件名取子集），但那样以后加歌又会缺字。

### 音量持久化：存了但不读（已修）

现象：设置页调完音量、重启后菜单里又变回 100%，用户以为"没存档"。

根因：

```c
save_volume_to_nvs(menu_volume);   // 设置页调值 → 确实写了 NVS，一直是对的
load_volume_from_nvs();            // 只在 osd_main() 里调用
//                                 ↑ 而 osd_main 只在「启动游戏」时跑
```

所以重启后菜单拿到的是全局初值 `int menu_volume = 100`，非要进一次游戏才会被
NVS 覆盖回来。修法：`show_menu()` 开头主动读一次（`static` 保证只读一次）。
启动日志现在会打 `[CFG] 音量恢复: 60%`。

**教训：写 NVS 和读 NVS 必须成对出现在启动路径上。** 只在一个入口（"进游戏"）读，
另一个入口（"开机进菜单"）就永远拿的是初值；而用户改设置恰恰是在菜单里改的。

### 日常迭代：只烧 app 分区（别整片烧）

改完 `src/` 里的代码这样烧：

```powershell
& $esptool --chip esp32s3 --port COM8 --baud 921600 write-flash 0x10000 `
  .\firmware\build\Dsn_nes_Emulator.ino.bin
```

**2.4 秒 vs 整片 8 秒，而且不擦 NVS**（语言/音量设置保留，不用每次重选语言）。
分区表里 `app0` 就在 `0x10000`（见 `firmware/build/partitions.csv`）。

只有改分区表、bootloader、或者换芯片时才需要 `merged.bin` 整片烧。

### 音质：MAX98357A 的增益要留余量（2026-11 实测结论，实现已回滚）

**症状**：MP3 大音量下"偶尔高音毛刺，像共振，细节丢了"。

**两步定位**（以后遇到音质问题先做这两步，比任何仪器都快）：
1. **同一段反复播放，毛刺位置固定** → 信号相关。SD/SPI 断流是随机的、和播放位置无关，
   这一步直接排掉"缓冲/DMA 不够"。
2. **音量降到 30% 明显干净** → 电平相关 → 失真在数字链路之后（功放/喇叭）。

**根因**：MAX98357A 是 9dB 固定增益 + 5V 供电，0dBFS 早就越过它的线性区，而这颗片子
**没有模拟音量可调**。实测干净峰值约 **-11dBFS**（≈ 音量 28%）。另外 minimp3 默认 int16
输出会在 ±32767 硬削顶，而且削顶发生在乘音量之前——不过实测降到 30% 后这部分听不出来。

**试过、已回滚、别再重复**：
| 方案 | 结果 |
|---|---|
| `MINIMP3_FLOAT_OUTPUT` + 浮点音量/高通/TPDF dither | 零代价，但也没有可闻收益 |
| -11dBFS 前视峰值限幅（32 帧块、2.9ms 前视、块内线性过渡、不过冲） | 小音量下与"手动调小音量"**逐样本等价**；只有推满时才变"响但被压"。用户评价："改了个寂寞" |
| 纯静态衰减 | 就是手动拧小音量，无价值 |
| 开机按住 SELECT 的扫频自检（400Hz~16kHz 阶梯正弦） | 定位共振有用，但"听单音判断哪里不对"对非专业耳朵不成立，已随回滚移除 |

**结论**：干净上限是硬件给的，软件只能决定这点余量怎么花，不能凭空造出来。
最实用的做法就是**把音量停在 30% 附近**；真想要大音量又干净，只能换功放+喇叭。

**回滚说明**：本节的浮点/限幅实现已回滚，`src/mp3_player.cpp` 仍是 int16 解码 + 180Hz
高通 + 软件音量。回滚原因见上表第一、二行。

### 板载 LED 与设置页新增项（2026-11）

**官方 wiki 的说法**（nologo.tech 的 ESP32S3-SuperMini 页）：
- 规格表：「板载 RGB、蓝色 LED：公用 GPIO48 引脚」
- Blink 示例：`int led = 48;`，注释 `digitalWrite(HIGH) // turn the LED off`、
  `digitalWrite(LOW) // turn the LED on` → 即**低电平点亮**

**实测（2026-11，设置页三档都试过）**：

| LED 脚设置 | 红色 LED | 蓝色 BATTERY LED |
|---|---|---|
| 拉低 | 灭 | 照闪 |
| 拉高 | 最亮 | 照闪 |
| 浮空（默认） | 微亮（高阻，靠弱上拉/漏电） | 照闪 |

结论（**原理图已核对**，官方 wiki 上的 EasyEDA 图，2026-11 下载）：

- **GP48 = 红色 LED 直驱 + 板载 WS2812 的数据脚，两者共用一根线**：
  `GP48 → R1(1K) → 红色 LED1(0402) → GND`，同一网络还接 `LE1(WS2812) 的 DI`，
  WS2812 的 DO 接 WSR1(10K) 且标注 **NC**（没连）。
  拉高 = 红灯最亮（WS2812 数据脚静态高，不会点亮它）；拉低 = 灭；浮空 = 微亮。
  wiki 示例注释里的 `HIGH=off / LOW=on` 是反的，照它写会把灯点亮当成关灯。
- **蓝色 LED 完全不在 GPIO 上**。原理图：`VBUS → PR6(1K) → 蓝色 LED(PD4) → TP4054 的
  CHRG 脚`（开漏状态输出）。它就是充电状态灯 —— 没有电池时 TP4054 一直在
  "检测→失败→重试"，所以闪。**固件碰不到它**，只能物理处理：贴住、或摘掉蓝灯/PR6。
- 顺带一个坑：原理图上 `BAT+ / BAT-` 两个焊盘标的是 **Pads (NC)**（未连接）。但
  Reddit 那条同名帖子（r/esp32 "esp32 s3 blue light stays on"）里多人实测反馈：电池焊盘
  在**板子背面、USB 口下方**，接上电池后充电电路就正常工作。
- **蓝灯的状态语义**（Reddit 帖里 ARRduinoPirate 的总结）：
  **闪烁 = 没接电池**；**常亮 = 正在充电**；**灭 = 电池已接（充满）**。
  所以"没电池时一直闪"是这块板的正常行为，不是故障。
- 因为 48 上还挂着 WS2812 的 DI，用 `analogWrite(48, ...)` 做 PWM 调光虽然能调红灯光，
  但 PWM 会被 WS2812 当垃圾数据（偶尔随机亮一下），所以固件里没做低亮档，只有三态。

**wiki 里那个 PWM 调光示例不能照抄**：它把 `ledPin` 写成 **8**，而本板 **GPIO8 是 SD 卡的
MISO**，`analogWrite(8, ...)` 会直接把 SD 读搞挂。就算改成 48，若那颗是可寻址 WS2818，
PWM 也不是它的协议（只会乱闪，调不了暗）。要"关掉"只需要把脚钉在确定电平或不驱动。

**设置页现在 4 行**（行高 38，卡片 40..196，footer 从 y=204 开始清屏）：

| 行 | 内容 | NVS 键 |
|---|---|---|
| 语言 | 中文 / English | `lang` |
| 音量 | 0~100%，10 格离散条，MP3 和游戏共用 | `vol` |
| 音色 | 原声 / 默认 / 柔和 —— **只作用于 MP3** | `tone` |
| LED 脚 | 浮空（默认）/ 拉低 / 拉高 —— 调试用，看哪颗灯归它管 | `led` |

**音色三档**（`src/mp3_player.cpp`，`mp3_set_tone`）：0 直通；1 = 180Hz 二阶高通（原行为）；
2 = 默认 + 8kHz 二阶低通。可以**边播边切**：音乐页按 A 播放后 SELECT 切到设置页，音乐不会
停，左右键直接 A/B，串口打 `[CFG] 音色 -> N`。

### MP3 解码性能与方案对比

**结论：自己做更合适。** 实测数据（`tools/mp3_bench/`，真板子跑的，不是估算）：

| 测试素材 | 码率 | 解码速度 | CPU 占用 |
|---|---|---|---|
| 5 秒正弦音 | 128kbps CBR | **10.41x 实时** | 10% |
| 真实歌曲片段（三輪学 - ヨスガノソラ） | 320kbps CBR | **9.66x 实时** | 10% |

解码 10 秒音频只要 1.04 秒，余量 9 倍多。码率从 128 涨到 320 几乎不影响速度 ——
瓶颈在 MDCT 和合成滤波，不在读数据。

**为什么选 minimp3 而不是 ESP32-audioI2S：**

| | minimp3（自己做） | ESP32-audioI2S |
|---|---|---|
| 代码体积 | 单头文件 `minimp3.h`，编译后约 30~40KB | 更大，还带网络流/格式转换 |
| core 3.x 兼容 | ✅ 纯 C 无依赖，GCC 14 直接编过 | ⚠️ 官方版历史上编不过，要社区补丁 |
| I2S 冲突 | ✅ 复用现有 `ESP_I2S` 类，只换采样率 | ❌ 自己 install I2S 驱动，得写所有权转移状态机 |
| 可控性 | ✅ 以后加 WAV/FLAC 就是加几十行 | ❌ 黑盒，出问题难查 |
| 工作量 | 帧读取、缓冲、播放控制都得自己写 | 现成 |

**必须知道的约束（已经踩过一次）：**

`mp3dec_decode_frame()` 内部有一个 `mp3dec_scratch_t` **局部变量**，约 14KB：

```
float grbuf[2][576]    // 4608 B
float syn[33][64]      // 8448 B
uint8_t maindata[1022]
L3_gr_info_t gr_info[4]
```

Arduino 的 `loopTask` 只有 8KB 栈，直接在 `setup()` / `loop()` 里调用会触发
`Stack canary watchpoint triggered (loopTask)` 然后无限重启。

**解码必须跑在独立任务里，栈 ≥ 32KB：**

```cpp
xTaskCreatePinnedToCore(mp3_decode_task, "mp3", 32768, NULL, 5, NULL, 0);
```

另外 `sizeof(mp3dec_t)`（解码器状态）只有 **6668 字节**，全局或放 PSRAM 都行。

**歌曲素材的实际情况**（本地歌库，42 首）：
**全部是 44100Hz 立体声**，码率 226~291kbps。而 NES 音频是 22050Hz ——
所以进游戏/出游戏时 I2S 必须重配采样率，不能只切静音。

### 万一还是要用 ESP32-audioI2S（三个硬问题）

**1. I2S 所有权冲突（最麻烦）**

现在 `osd_init_sound()` 用 core 3.x 的 `ESP_I2S` 类配置 MAX98357A（BCK=6 / WS=5 / DO=4，
22050Hz **立体声帧** —— 不能退回 `I2S_SLOT_MODE_MONO`，否则功放输出直流、音圈发烫，
见第四节坑 #19）。而 ESP32-audioI2S 自己要 `i2s_std_new` 同一组引脚，**两者不能同时持有**：

```
菜单 / 音乐页：audioI2S 持有 I2S        游戏运行中：ESP_I2S 持有 I2S
        │                                      │
        │  按 A 启动游戏前：                     │
        │    audio.stop() + 释放 I2S  ────────▶ │ osd_init_sound() 重新初始化
        │                                      │
        │ ◀──── 退出游戏：osd_stopsound()       │
        │       再让 audioI2S 重新 init          │
```

顺序错了两边都会崩（I2S 被占 / 引脚被抢），必须写成显式状态机。

**2. 库对 core 3.x 的兼容性**

Arduino core 3.x 用 GCC 14，ESP32-audioI2S 官方版本历史上编译不过，社区有补丁分支
（如 `PLSousa/ESP32-audioI2S` 的 `v2.0.6-gcc14` 变体，但那个是给"无 PSRAM"板子的）。
本机 `Documents/Arduino/libraries` 里**目前没装这个库**。
**动手第一步应该是"能不能编译过"的验证，不要先写业务代码。**

**3. SD 卡总线互斥**

audioI2S 从 SD 边读边解码，NES 的 `osd_rom_read()` 也走同一条 SPI。进游戏前必须
彻底 stop（含关文件句柄），否则会读到半截数据。

### 建议的按键映射（音乐页）

B / SELECT 在所有页面统一是切页，**不要为了播放控制破坏这个一致性**：

| 键 | 作用 |
|---|---|
| UP / DOWN | 选曲 |
| A | 播放 / 暂停 |
| LEFT / RIGHT | 上一首 / 下一首 |
| B / SELECT | 切页（全局统一） |

### 集成步骤（minimp3 路线）

1. ~~验证解码能力~~ ✅ 已完成：`tools/mp3_bench/` 实测 9.66x 实时 @320kbps
2. 把 `minimp3.h` 放进 `src/`，写一个带 32KB 栈的播放任务：
   SD 读帧 → `mp3dec_decode_frame` → PCM 写 I2S（44100Hz，**立体声帧**）
3. 扫 SD 根目录的 `.mp3` 建列表（复用 `scan_games()` 的结构，别另写一套）
4. 音乐页 UI：曲目列表 + 当前播放项 + 播放/暂停状态（`ui_row` / `ui_scrollbar` 现成）
5. 进游戏前：停播放、关文件、`i2s.end()`；`osd_init_sound()` 用 22050Hz 重开 I2S
   退出游戏后反向恢复。**注意 44.1k ↔ 22.05k 采样率不同，必须真的重配而不是静音**
6. 用带中文/日文名的歌曲验证文件名 UTF-8 显示（`draw_cn_text` 已支持）

**SD 卡在板子里，电脑上看不到**（没有可移动磁盘）——测试前需要把歌拷到卡根目录，
卡里现在一个 `.mp3` 都没有。

---

## 十四、翻页时"看得见从上到下刷新"的根因与优化（2026-11）

### 现象

页内上下移动光标完全正常，但**列表翻页**（滚屏）和**切页签**时，能肉眼看到
屏幕从上到下、从左到右被一块块刷出来。用户直觉会问"是不是渲染逻辑太落后"。

### 根因：不是算法，是没有帧缓冲

`tft_driver.h` 的每一次 `pushImage / drawFilledRect` 都是**同步直写 ST7789 的 GRAM**，
屏幕"写到哪显示到哪"，没有整帧缓冲可以一次性翻页。所以屏幕上看到的刷新顺序，
**就是固件的绘制顺序本身**：

- `ui_group()` 先画卡片中间大块 → 左右条 → 四角（都是从上往下逐行写）
- 每行文字按 16 条横带自上而下推
- 每条 SPI 数据流按 x 递增从左往右

用 `tools/mock_menu.py` 的组件层（逐像素复现固件）统计真实绘制成本，
按 40MHz 算传输下界：

| 操作 | 旧：事务数 | 旧：字节 | 旧：40MHz 下界 | 新：字节 | 新：下界 |
|---|---|---|---|---|---|
| 页内上下移动（2 行 + 滚动条） | 95 | 32.6KB | 6.5ms | 不变 | 6.5ms |
| **游戏列表翻一屏** | **217** | **154.0KB** | **30.8ms** | **51.4KB** | **10.3ms** |
| 切页签（整页 + fillScreen） | 319 | 294.7KB | 58.9ms | 264.5KB | 52.9ms |

**6.5ms 低于人眼感知阈值 → 页内切换看不出；30.8ms 以上 → 翻页必然可见。**
翻一屏的数据量（154KB）甚至比整屏（115KB）还多——因为卡片底（72.5KB）和
5 行行底（69KB）**同色重叠画了两遍**。

### 已做的优化（A+B）

**A. 重绘策略（`osd.cpp`）**

1. `ui_row()` 新增 `bg_covered` 参数：调用方保证"这行背后已经是卡片底色"时，
   非选中行只清 `216×16` 的文字带（6.7KB），不再整行填 `216×32`（13.8KB）。
2. 游戏页/音乐页翻页时**不重画卡片底**——翻页前后卡片位置、大小、颜色完全没变，
   重画它等于白写 72KB。只有滚到末尾、可见行数不足满屏时才必须重画。
3. 翻页时用 `last_selected - last_top` 算出"上次选中行在屏幕第几行"，
   只有它需要整行覆盖（高亮圆角有 8px 高，不在 16px 文字带里）。
   实测滚屏时选中项始终停在边界行、屏幕行号不变，所以这个分支通常是 -1。
4. `bg_covered` 且本行没有分隔线时，顺手把上一屏可能残留的 1px 分隔线擦掉。

**B. 驱动层批量写（`tft_driver.h` / `osd.cpp`）**

1. `drawFilledRect()` 不再逐像素 `SPI.write16()`（每个像素两次寄存器级写），
   改为 `fill_pixels()`：1KB 静态块按颜色缓存，`writeBytes()` 整块发。
2. `draw_cn_text()` 一次窗口写 16 行：原来每行一次 `pushImage` = 16 次
   CASET/RASET/RAMWR 事务，现在 1 次（新增 `pushImageBytes()`，按 SPI 字节序
   拼好的内部 RAM 条带直接发，零转换）。`band` 从 480B 增到 7680B。
3. `draw_chevron()` 从"每像素一次 1×1 窗口事务"（一行 10 次、5 行 50 次）
   改为一次推 5×9 小图，新增 `bg` 参数（推图会覆盖矩形，透明像素必须带底色）。

**代价**：BSS +7.8KB（7680B 文字带 + 1KB 填充块 - 旧的 480B/512B），
编译报告 145,296 / 327,680（44%），剩余 182KB，安全。

### 编译验证（无板子）

用 Arduino IDE 1.8.19 的 `arduino-builder.exe` 命令行编译通过：

```powershell
# 把 src 拷到临时库目录，避免改动用户的 Documents\Arduino\libraries
arduino-builder -compile -logger=machine `
  -hardware "$env:LOCALAPPDATA\Arduino15\packages" -hardware "G:\...\arduino-1.8.19\hardware" `
  -tools "$env:LOCALAPPDATA\Arduino15\packages" -tools "G:\...\arduino-1.8.19\tools-builder" `
  -tools "G:\...\arduino-1.8.19\hardware\tools\avr" `
  -built-in-libraries "G:\...\arduino-1.8.19\libraries" -libraries "<临时库里目录>" `
  -fqbn "esp32:esp32:esp32s3:FlashSize=4M,PSRAM=enabled,FlashMode=qio,CPUFreq=240,USBMode=hwcdc,CDCOnBoot=cdc" `
  -ide-version=10819 -build-path "<临时构建目录>" -warnings=none `
  firmware\Dsn_nes_Emulator.ino
```

产物 910,284B flash / 145,296B RAM，与改动前对比 text +516B、bss +7.8KB。

⚠️ **用户的 Arduino 库是 `src/` 的拷贝，不是符号链接**——
直接改 `D:\Code\...\src` 后 IDE 里编译的还是旧代码，必须把 `src/` 同步回
`Documents\Arduino\libraries\Esp32NofrendobyDSN\src` 再编译。

### 上机验证清单（待做）

- [ ] 游戏列表连续按 DOWN 翻页：应只看到文字替换，不应该再看到整卡重画
- [ ] 翻到列表末尾再翻回来：卡片底没有被重复刷的痕迹
- [ ] 音乐页 42 首翻页、播放态切换：颜色正常、无残影
- [ ] 切页签（SELECT/B）：仍然会整屏重绘（fillScreen 是必须的），应比之前快 10%
- [ ] 长按 START 切语言：所有文字宽度变化后无残影（清理带按 191px 整段算）
- [ ] 进游戏/退出游戏：菜单回到顶部时的整页重画正常

### 追加：屏幕时钟可切换 + 切页签局部清屏（2026-11，已上机）

**屏幕 SPI 时钟做成设置项**（设置页第 5 行「屏幕 / LCD SPI」）：

- 40M（默认，保守）/ 80M 两档，左右切换，**立即生效**并存 NVS（键 `spi`）
- 屏幕和 SD 卡共享 SCK/MOSI，但**从不同时占用总线**：各自用 `beginTransaction`
  设自己的时钟（屏幕 `spi_hz`、SD 20MHz），所以提屏幕时钟不影响 SD 稳定性
- 实际走线是**焊接**的（HANDOFF 早先写的"长杜邦线"不准确，已改注释），
  80MHz 有希望稳跑；是否花屏只能上机看
- 切换后强制整页重绘一次：万一 80M 花屏，切回 40M 时画面能自己恢复
- **防花屏兜底**：开机时按住 B → 强制 40MHz 并写回 NVS。这是不依赖屏幕的恢复路径

代码：`tft_driver.h` 的 `spi_hz` 成员 + `set_spi_freq()`（`bus_begin()` 从 static
改成成员函数，才能用运行时值）；`osd.cpp` 的 `spi_freq_apply/load_spi_from_nvs/
save_spi_to_nvs`，在 `osd_ui_language_setup()` 里按 NVS 应用。

**切页签不再整屏刷黑**：`ui_page_tabs()` 加了 `card_bottom` 参数，切页时用
`ui_clear_around_cards()` 只清「页签行 + 卡片下方」两条横带（40KB），
卡片矩形（72KB）随后由 `ui_group()` 覆盖。整屏 fillScreen 115KB → 40KB。

- 空状态页（卡片在 ey=84，位置和满页不同）仍然传 `-1` 走整屏清，否则残影
- 设置页从 4 行加到 5 行，行高从 38 收到 `UI_ROW_H`(32)：5×32+4 = 164，
  卡片 40..203，和游戏列表完全对齐，footer 从 204 开始清，刚好不重叠

**80MHz 下的理论时间**（字节数按 mock 重放统计）：

| 操作 | 40MHz | 80MHz |
|---|---|---|
| 游戏列表翻一屏（51.4KB） | 10.3ms | **5.1ms** |
| 切页签（40KB 清屏 + 内容 ≈ 200KB） | 40ms | **20ms** |
| 整屏（115KB） | 23ms | **11.5ms** |

**默认已改成 80MHz**（`TFT_SPI_FREQ` 和 NVS 缺省值都是 80；上机确认不花屏）。
如果换到别的板子出现噪点/错位：设置页再按一下切回 40M（会立刻重绘恢复），
或按住 B 开机强制回 40M。

**为什么不能上 100/120MHz**：ESP-IDF 的 SPI 主机频率常量最大就是
`SPI_MASTER_FREQ_80M`（`spi_master.h`：`80MHz / 分频 1`），时钟源是 APB 80MHz，
分频器最小 1 —— 请求 100M/120M 会被驱动钳回 80MHz，不会更快。
再往上只能减少数据量（见下面 C 方案），不能靠提频率。

**设置页上下切换也做了局部重绘**（和游戏列表同一套）：

- 原来每按一下上下都重画全部 5 行（5×13.8KB + 文字 ≈ 79KB / 80MHz ≈ 10ms）
- 现在只重画「旧选中行 + 新选中行」两行（≈28KB / 3.5ms）
- 选中行**上一行**的分隔线状态会变（选中行上方不画线），但内容没变，
  所以单独补一条 1px 线（`draw_sep_line()`），不整行重刷
- 左右调值只刷被调的那一行（音量行连进度条一起）

### 没做（评估过，故意跳过）

| 项 | 为什么 |
|---|---|
| ST7789 硬件垂直滚动（VSCRDEF/VSCSAD） | 唯一能彻底消除"看到刷新过程"的方案：翻页只画新进入的 1 行，再改滚动起始地址，数据量 154KB → ~14KB。但本模组 `MADCTL=0x60` 交换了 XY，滚动方向要实测；退出游戏/切页必须复位 VSCSAD，否则游戏画面整体偏移。风险高于收益，等这轮 A+B 上机看效果再决定 |
| 整帧缓冲（240×240×2 = 112.5KB） | 只能消除"绘制中间态"，推屏那 22.5ms 自上而下的扫描仍在；且挤占内部 RAM 会拖累游戏帧率 |
| SPI 提到 80MHz | 全屏 23ms → 11.5ms，但屏幕/SD 共享长杜邦线时 80MHz 容易花屏（见 `tft_driver.h` 顶部注释），需要硬件改线后再试 |
