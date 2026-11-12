# MCU Register Debugger

MCU 寄存器级 UI 调试器：通过 **J-Link** 连接调试器（保留 SEGGER 官方 USB 驱动，**无需换 WinUSB**），
读取 `DBGMCU_IDCODE` / `CPUID` 自动识别 MCU 型号，再用 **CMSIS-SVD** 描述文件图形化重建芯片的
寄存器布局与 IO 布局。寄存器视图参考 CubeMX 的配置页面，但内部以 **分块寄存器图** 展示。

- UI：Qt 6 Widgets + QGraphicsView 自绘（分块寄存器图、位域条、封装引脚图、端口矩阵）
- 描述文件：CMSIS-SVD 1.0 / 1.1 / 1.3
- 主后端：**JLinkGDBServerCL.exe + GDB Remote Serial Protocol**（持久 TCP 会话，毫秒级读写）
- 备选后端：**probe-rs CLI**（需 WinUSB 驱动，可切换）

---

## 1. 为什么是 GDB Server 而不是 J-Link DLL / Commander

实测结论（本机 SEGGER J-Link V8.26 + N32H473CE）：

| 方案 | 结果 |
|---|---|
| 直接调 `JLink_x64.dll` | `JLINKARM_GetDLLVersion` 等符号的返回类型/缓冲区语义与自带头文件不匹配，未连接时部分调用直接崩溃 → 放弃 |
| `JLink.exe`（Commander）持久会话 | 管道下 **stdout 被完全缓冲**（只有进程退出才 flush），无法交互 → 放弃 |
| `JLinkGDBServerCL.exe` + GDB RSP | 持久 TCP、无缓冲、官方驱动；`m/M` 读写、`monitor halt/go/reset` 全部可用 → **采用** |

每次读写都是一个 GDB 包，轮询 200 ms 全外设刷新实测无压力。

## 2. 目录结构

```
CMakeLists.txt               构建定义（Qt6 Widgets + Network，C++20）
src/core/                    SVD 解析、寄存器会话、J-Link GDB 后端、GPIO 解码
  GdbRspClient.*             GDB RSP 最小客户端（$..#xx 帧、转义、分包读写）
  JLinkGdbProbe.*            IProbe 实现：拉起 JLinkGDBServerCL 并驱动 GDB RSP
  JLinkRunner.*              JLink.exe 定位 + 一次性脚本（版本/仿真器信息/器件名校验）
  ProbeRsRunner/ProbeRsProbe 备选 probe-rs 后端
  ProbeController.*          探针线程隔离（所有阻塞调用在 worker 线程）
  DeviceIdentifier.*         CPUID/IDCODE → MCU → SVD 识别
  GpioCodec.*                GPIO 端口解码（F1 / F4-G4 / N32 三种布局）
  RegisterSession.*          寄存器快照、读改写、撤销、轮询
src/ui/                      主窗口、寄存器分块图、位域条、检查器、封装图、端口矩阵、主题
src/app/                     main、命令行、离屏渲染自检
data/device_ids.json         器件识别表（DEV_ID → MCU → 器件名 → SVD）
data/packages/*.json         封装引脚图数据
scripts/build.bat            编译
scripts/deploy.bat           windeployqt 打包到 deploy/
scripts/fetch-probe-rs.ps1   下载官方 probe-rs（仅在使用 probe-rs 后端时需要）
packaging/installer.nsi      NSIS 安装包脚本
packaging/pack.bat           调用 makensis 生成 dist\McuRegDebugger-<ver>-setup.exe
```

## 3. 依赖

| 组件 | 版本 | 说明 |
|---|---|---|
| Qt | 6.5+（本机 6.8.3 mingw_64） | Core / Gui / Widgets / Network |
| CMake + Ninja | 3.21+ | |
| MinGW | GCC 13（`D:\Qt\Tools\mingw1310_64`） | |
| SEGGER J-Link 软件 | V7.96 / V8.26 | 提供 `JLink.exe` 与 `JLinkGDBServerCL.exe` |
| NSIS | 3.x（`D:\NSIS`） | 仅打包时需要 |

J-Link 软件按以下顺序自动发现（设置项可覆盖）：注册表 `HKLM\SOFTWARE\SEGGER\J-Link`
→ `%ProgramFiles%\SEGGER\JLink_V*`（版本号降序）→ `<exe>/tools/jlink` → `PATH`。

## 4. 编译与运行

```bat
scripts\build.bat            :: 增量编译
scripts\build.bat --clean    :: 全量重建
build\bin\mcuRegDebugger.exe
```

### 命令行

```
mcuRegDebugger                      启动图形界面
  --backend <jlink|probe-rs>        探针后端（默认 jlink）
  --interface <swd|jtag>            连接协议（默认 swd）
  --speed <kHz>                     时钟（默认 4000）
  --device <chip>                   指定器件名（默认自动识别）
  --svd <path>                      启动时加载 SVD
  --autoconnect                     GUI 启动后自动连接
  --probe-info                      探针信息 + 连接 + 识别 + 实时寄存器读取 + 写入回环
  --selftest --svd <path>           SVD 解析自检（退出码 0 = 通过）
  --render <dir>                    自检时离屏渲染 PNG 快照
  --port <PA|PB|...>                指定端口（配合 --probe-info / --render）
  --log <path>                      日志文件位置
```

真机验证示例（本机实测通过）：

```bat
build\bin\mcuRegDebugger.exe --probe-info
```

输出包含：Commander/DLL/固件/硬件/SN、`CPUID`、`IDCODE`（DEV_ID/REV_ID）、
识别出的 MCU 与 SVD、逐引脚 GPIO 解码、SRAM 写入回环校验。

### 用 VSCode 开发

仓库自带 `.vscode/` 工程配置（已取消对 `.vscode/` 的忽略，机器本地文件仍排除）。
装好 **CMake Tools** + **C/C++** 两个扩展后直接打开仓库根目录即可。

| 文件 | 作用 |
|---|---|
| `settings.json` | Qt / MinGW / Ninja 路径、CMake 预设、IntelliSense 数据源、搜索排除 |
| `c_cpp_properties.json` | 走 `build/compile_commands.json`；CMake Tools 未启用时用内置 include 兜底 |
| `tasks.json` | `build` / `build (clean)` / `build (debug)` / `selftest` / `selftest + render` / `run` / `probe-info` / `deploy` / `pack (installer)` |
| `launch.json` | gdb 调试 GUI 与自检两种入口 |
| `cmake-kits.json` | Qt 6.8.3 MinGW 64-bit kit |
| `extensions.json` | 扩展推荐 |

常用：`Ctrl+Shift+B` 默认构建；`Ctrl+Shift+P → Tasks: Run Task` 选 `selftest`。

两条注意：

1. **路径含空格**（`...\MCU debugger`）。VSCode 的 shell 任务不会自动给
   `command`/`args` 加引号，所以任务里一律用 `options.cwd` + 相对路径，
   不要改成 `${workspaceFolder}/build/bin/...` 这种绝对路径，否则会得到
   `ERRORLEVEL=9009`（找不到命令）。
2. **换机器**要改 4 个路径：`settings.json` 里的 `CMAKE_PREFIX_PATH`、
   `CMAKE_MAKE_PROGRAM`、`compilerPath`、`miDebuggerPath`，
   与 `scripts\build.bat` 头部的 `QT` / `MINGW` / `CMAKEDIR` / `NINJA` 一一对应。

`build (debug)` 任务单独产出 `build-debug/`（已 gitignore），供断点单步；
默认的 `build/` 是 Release，无调试符号。

## 5. 界面说明

| 区域 | 作用 |
|---|---|
| 工具栏 | 后端/接口/速度/芯片名、连接与断开、暂停/运行/复位、撤销写入 |
| 左侧「外设」 | 按 `groupName` 分组；勾选 = 加入轮询集合；「实时」列显示定时器计数值 |
| 「封装 / 引脚」 | 芯片体 + 四边引脚，颜色=引脚类型/实时 GPIO 模式，点 GPIO 跳到端口 |
| 「端口矩阵 (IO)」 | 选中端口 PA0..PA15 的 4×4 矩阵，颜色=输入/输出/复用/模拟 |
| 「寄存器全景」 | 选中外设的**分块寄存器图**（名称/偏移/当前值/位域带/RO-RW），下方位域条 |
| 右侧「寄存器检查器」 | 位域表、枚举下拉、字段 hex 编辑与置 1/清 0/写入（读改写）、整寄存器写、重读 |
| 底部「日志」 | 解析与探针日志，同时写入 `%APPDATA%\MCUTools\McuRegDebugger\logs\` |
| 状态栏右侧 | **CPU 状态**（正常 / 已暂停 / 有故障标志 / **HardFault**，可点击看详情）、当前器件的 **Flash / RAM 容量**（悬停显示 SRAM+CCM 拆分）、轮询状态、连接状态 |

左/右/下三个停靠面板的宽度与高度都可自由拖动（分隔条无锁定）。位域条**始终画满 32 位**：
面板变窄时位格按 `可用宽度/32` 缩小（小于 14px 时隐藏 0/1 数字、小于 22px 时隐藏字段名），
高度固定，永不裁切；`--selftest` 会在 96…904px 七档宽度上断言绘制区不越界。

GDB 后端的运行控制：`暂停` = `monitor halt`，`运行` = `monitor go`，`复位` = `monitor reset`；
CPU 是否停机由 GDB 停止应答（`S05`/`T05`）与 `DHCSR` 判定。读写寄存器**不会**改变 CPU 运行状态。

## 6. 自动识别流程（本机实测）

```
CPUID  0xE000ED00 = 0x410FC241  -> Cortex-M4
IDCODE 0xE0042000 = 0xA5384712  -> DEV_ID=0x712 REV_ID=0xA538
DEV_ID 0x712 -> data/device_ids.json -> N32H473CE (Nationstech / N32H47x)
器件名 N32H473CE -> SVD 库评分匹配 -> N32H473.svd（75 外设 / 1454 寄存器 / 9153 字段）
```

- 未指定 `--device` 时：先用通用核名 `Cortex-M4` 连接 → 读 IDCODE → 识别 → **以识别出的器件名重连**。
- 识别失败可手动指定：界面下拉框、`--device`，或 SVD 库搜索（文件 → 在 SVD 库中搜索）。
- SVD 库根目录：`<exe>/svd`、`%APPDATA%/MCUTools/McuRegDebugger/svd`、`C:\Keil_v5\ARM\PACK`。

## 7. 数据文件格式

### data/device_ids.json

```json
{
  "idcodeAddresses": ["0xE0042000", "0x5C001000", "0x40015800"],
  "devices": [
    { "mcu": "N32H473CE", "family": "N32H47x", "vendor": "Nationstech",
      "devId": "0x712", "svdHint": "N32H473", "probeRsChip": "N32H473CE",
      "verified": true, "idcodeAddress": "0xE0042000" }
  ]
}
```

- `devId`：`DBGMCU_IDCODE` 低 12 位；`verified:true` 表示已在真实硬件上实测。
- `probeRsChip`：传给 J-Link / probe-rs 的器件名（J-Link 用精确料号，如 `N32H473CE`）。
- `svdHint`：SVD 的 `<name>` 前缀（如 `N32H473`）。
- 未实测的器件（`devId: "0x0"`, `verified:false`）需手动选择。

已实测：**N32H473CE**（DEV_ID 0x712）；其余条目为手册值，首次使用建议核对。

> **支持范围（v1.1.1.0）**：只收录 Nationstech **N32** 系列（11 条，随包附带 SVD 与封装表）。
> STM32 已**归档**到 `data/unsupported/`，暂不做验证与支持；代码本身不写死厂商，
> 恢复方式见 `data/unsupported/README.md`。

### data/packages/*.json（封装引脚表，从数据手册生成）

引脚信号名**只**存在于数据手册的管脚定义表里：SVD 是 die 级、`.pdsc` 只有数量、
原理图符号不可靠（LCEDA 符号里残留 `STM32G431CBU6` 就是教训）。所以这条链路是：

```bat
python scriptsetch-n32-datasheets.py      :: 从厂商下载中心抓 12 个系列手册
python scripts\build-package-tables.py       :: 解析管脚定义表 -> data/packages/
```

- **下载**：厂商站是 JS 应用，但下载列表有 JSON 接口
  （`/api/dow?keyword=N32G455`），脚本据此自动发现 URL，不硬编码、不会过期失效。
- **解析**：各系列手册的表格形状完全不同（10/14/17/18/21 列，封装组合也不同，
  有的还是英文表头），所以**列布局是自动发现的**——按表头找"管脚名称"列，
  按封装名模式找封装列，其余列按相对位置推断。
- **校验**（不通过就不落盘，宁可退回结构图也不写错）：
  1. 引脚号必须**恰好 1..N** 连续（无重复、无缺失）；
  2. **N 必须等于封装名里的数字**（LQFP64 就得 64 脚）——这条能抓住整表/半表误选；
  3. 与 `pdsc` 声明的脚数交叉核对。
- **命名**：按**系列组 + 封装**一张表（`N32H47x_LQFP48.json`），因为一份手册覆盖
  同组多个型号（`N32G455.pdf` 同时描述 G451/452/455/457）。组名里的 `x` 是通配，
  `N32G473CE` → `N32H47x` 靠前缀匹配。
- 同组两份手册（`N32H48x` 有 482 和 487 两本）对同一封装取**并集**：引脚名总是
  一致，但复用功能列表会因列识别差异而长短不一，缺条目比多条目危险。

当前收获：**30 张封装表 / 5 个系列组 / 1944 个引脚**，全部经上述校验。

> 覆盖边界要说清：`N32G033`、`N32G05x` 的手册用的是另一种排版的表，`N32G401` /
> `N32H492` 只解析出碎片，`N32H730` 的下载链接在厂商站上已失效——这几个系列暂时
> 走 `synthesize()` 兜底（按 IO 数/封装脚数画结构图，引脚名为通用编号，界面上明确
> 标注"占位引脚表"）。再跑一次脚本即可增量补齐，不需要改代码。

### data/parts.json（料号表，脚本生成）

`data/device_ids.json` 按 **DEV_ID** 索引，只能到系列（`N32H473`）——同一个 DEV_ID 被所有封装/容量
变体共用，所以它记不下"这颗到底多大 Flash"。厂商 CMSIS-Pack 是按**精确料号**描述
（`Dname="N32H473CE"`）的，料号表就是把它摊平成运行时可直接查的一份：

```json
"N32H473CE": {
  "pack": "N32H47x_DFP.1.2.0",
  "package": "UQFN48/LQFP48",
  "flashBytes": 524288, "ramBytes": 196608,
  "flash": "512KB", "sram": "160KB", "ccmSram": "32KB",
  "ioCount": 42,
  "capabilities": { "SPI": 5, "FDCAN": 2, "I2C": 4, "DMA": 2, ... }
}
```

- `flashBytes` / `ramBytes` 取 pack 里的链接器 `<memory>` 行（`IROM1` / `IRAM1`），
  这是"实际有多大"，不受描述文字影响；`sram`/`ccmSram` 只是 pack 散文里的拆分说明。
- 查询用**最长前缀匹配**：订货号 `N32H473CEU7` 会落到 `N32H473CE`。
  系列名 `N32H473` 匹配到多个料号时**不猜**，直接返回无效——
  同系列 `N32H473KC` 是 256 KB、`N32H473CE` 是 512 KB，猜错就报错容量。
- 当前覆盖 **680 个料号 / 28 个 DFP**，其中 474 个有 Flash+RAM、155 个带能力清单。
  缺失时对应功能自动降级（状态栏不显示尺寸、不按数量裁剪），不会报错。

```bat
python scripts\sync-part-table.py             :: 重新生成（读 svd/ 与 Keil PACK）
python scripts\sync-part-table.py --dry-run   :: 只看统计
```

### data/packages/*.json

```json
{
  "name": "LQFP48",
  "chip": "STM32F103C8",
  "pins": [
    { "number": 10, "name": "PA0", "type": "io", "port": "PA", "pin": 0,
      "functions": ["PA0", "ADC12_IN0", "TIM2_CH1"] }
  ]
}
```

`type` 取 `power/ground/clock/reset/boot/io`（决定颜色）；`port`+`pin` 关联 GPIO 位。
SVD 不含封装与引脚复用信息，这一层必须由本文件提供（文件 → 打开封装目录可切换目录）。

**`numberVerified`**：引脚号是否已按厂商数据手册核对过。

- `true` / 缺省 → 正常显示。
- `false` → `number` 是**占位编号**。封装页顶部会用橙字提示原因（读 `pinCountNote`，
  缺省读 `comment`），每个引脚的 tooltip 也标注"待数据手册核对"——界面不会假装权威。

#### 引脚数据能从哪来

| 来源 | 给什么 | 结论 |
|---|---|---|
| **CMSIS-Pack `.pdsc`**（本机已有） | `<feature type="QFN" count="48" name="UQFN48"/>` + `<feature type="IOs" n="42"/>` | ✅ **封装类型、引脚总数、IO 数**（权威、可机读） |
| `*.svd` | GPIOA..GPIOH 的寄存器地址空间 | ❌ 不含引脚号，也不告知哪些 IO 被键合出来 |
| 板级原理图 | 引脚名与板级连接 | ⚠️ 可用，但符号 Value 残留 `STM32G431CBU6`，**引脚号不可沿用** |
| 固件源码 | 每个引脚的**实际用途** | ✅ 最可靠（实现代码 ≠ 移植文档） |
| 数据手册 / N32Cube | 逐脚 `number → 复用功能` 表 | ✅ 唯一能给出真实引脚号 |

`scripts/gen-package-json.py` 会把 `.pdsc` 与 SVD 当作**校验器**：GPIO 数必须等于
`<feature type="IOs">`，总脚数必须等于 `<feature type="QFN" count>`，任一不符直接 `[FAIL]` 退出。

#### N32H473CEU7 引脚数据提取过程（已完成，numberVerified=true）

从**嘉立创（LCEDA）该料号的原理图符号**逐脚读出，方法可复现：

1. 符号体内标注了 `N32H473CEU7` → 确认是这颗料而不是移植前的 STM32G431 符号；
2. 把左侧/右侧引脚列按 8 倍放大后做 OCR（Windows `Windows.Media.Ocr`），
   读出「引脚号 + 引脚名」；对可疑字符（`C→3`、`C→I`）用局部放大复核；
3. 与 CMSIS-Pack 的 `pdsc` 声明交叉校验：

   | 校验项 | pdsc | 提取结果 | 结论 |
   |---|---|---|---|
   | 封装 | `QFN count=48 name=UQFN48` | 48 项 | ✅ |
   | IO 数 | `IOs n=42` | 16×PA + 16×PB + 7×PC(4,6,10,11,13,14,15) + 3×PH(0,1,6) = **42** | ✅ |
   | 电源数 | 48 − 42 = 6 | VBAT、VREF+、VDDA、VDD×3 = **6** | ✅ |

4. 用渲染结果做**像素级回验**：把 6 个电源脚（应为主题红 `#C03A3A`）、
   `PH0/PH1`（时钟蓝）、`PH6`（复位紫）按编号算出坐标取样，颜色全部落在预期位置。

> 注意两个与直觉不同的点：**OSC 是 `PH0/PH1`（不是 PF0/PF1）**，**NRST 是 `PH6`（不是 PG10）**，
> `PB8 = BOOT0`。这也解释了为什么 `pdsc` 的 42 个 IO 里包含 PH6 —— NRST 引脚在 N32 上同时是 GPIOH6。

QFN48 物理排列：**每边 12 脚，pin 1 在左上角，逆时针** ——
左侧 1..12（上→下）、下侧 13..24（左→右）、右侧 25..36（下→上）、上侧 37..48（右→左）。

#### 引脚布局是"连接后实时生成"的

封装图**不预渲染**。启动时该页只显示一句提示：

```
连接目标后自动识别并渲染引脚布局
```

判定顺序（`PackagePage::renderForChip()`）：

1. J-Link 连接成功 → 读 `DBGMCU_IDCODE` → 识别出料号；
2. 从 `data/device_ids.json` 取得该料号的 `ioCount` / `pinCount` / `packageName`
   （数值源自 CMSIS-Pack `.pdsc`，不是猜的）；
3. 若 `data/packages/` 里有 chip 名匹配的 JSON → **用真实逐脚定义**渲染；
4. 否则 → **按 IO 数与封装脚数实时合成**网格：`pinCount − ioCount` 个电源/地 +
   `ioCount` 个 IO，引脚名用通用编号 `PWR1..PWRm` / `IO1..IOn`，
   并在页面顶部橙字标注"尚无该器件的逐脚定义"，**不会编造引脚名**；
5. 两者都拿不到（无 IO/脚数）→ 保持等待态并说明原因。

断开连接后回到等待态。

回归验证（离屏，`--device` 触发与 GUI 完全相同的代码路径）：

```bat
:: 识别前：覆盖率必须 < 2%（证明没有预渲染）
mcuRegDebugger.exe --selftest --svd <svd> --render out
::   -> out\package-waiting.png  非背景像素 0.15%

:: 有 JSON：按逐脚定义渲染
mcuRegDebugger.exe --selftest --svd <n32h473.svd> --device N32H473CE --render out
::   -> out\package-live-N32H473CE.png   48 脚 / 每边 12 / 覆盖率 40.5%

:: 无 JSON：按 IO 数合成
mcuRegDebugger.exe --selftest --svd <stm32f407.svd> --device STM32F407VE --render out
::   -> out\package-live-STM32F407VE.png  100 脚 / 每边 25 / 覆盖率 41.5%
```

真机日志对照：

```
INFO  J-Link GDB Server 就绪: port=52007, device=N32H473CE
INFO  识别到 N32H473CE (Cortex-M4), SVD=N32H473.svd
INFO  引脚布局已按识别结果渲染: N32H473CE (IO 42 / 脚 48)
```

#### 封装图几何（按 LCEDA 实测比例复现）

从 LCEDA 引脚图量得三组比例，代码里以**比例**而非固定像素实现，随窗口自适应缩放：

| 特征 | LCEDA 实测 | 本项目 |
|---|---|---|
| 间距 / 芯片体 | 476 px 体 / 39.4 px 间距 → 体 = 12.08 × 间距 | `body = (slotCount + 1) × pitch` |
| 引脚厚度 / 间距 | 22 / 39.4 = **0.56** | `thickness = 0.56 × pitch` |
| 引脚长度 / 间距 | 62 / 39.4 = **1.57** | `length = 1.57 × pitch` |
| 引脚名 | 沿引脚轴向排布（竖引脚旋转 90°） | 同 |
| 引脚号 | 引脚外侧、灰色小字 | 同 |
| 引脚 1 标记 | 体内左上角圆点 | 同 |

`metricsFor()` 会把 `body + 2×length` 一起算进可用宽度反解 `pitch`，并有二次收缩保护，
保证任何窗口尺寸下都不会溢出。

### 引脚数据：从厂商数据手册提取（numberVerified=true）

`data/packages/N32H473CEU7_UQFN48.json` 不再依赖原理图符号，而是从数据手册
`N32H473.pdf` 的 **表 3-1 管脚定义** 直接提取：

```bat
python scripts\parse-datasheet-pins.py                       :: 默认 UQFN48 / N32H473CE
python scripts\parse-datasheet-pins.py --package LQFP48       :: 换封装（10 种可选）
python scripts\parse-datasheet-pins.py --pdf <路径> --part N32H473KE --package QFN32
```

该表每种封装一列（QFN32 / QFN48 / UQFN48 / UQFN48-1 / LQFP48 / QFN60 / LQFP64 /
LQFP80 / LQFP100 / LQFP128），脚本按列取出 **引脚号 + 引脚名 + 类型 + 复用功能**，
并做三重校验（任一不符直接失败退出）：

| 校验 | 结果 |
|---|---|
| 引脚号连续 1..N | 48 |
| GPIO 数 == `.pdsc` 的 `<feature type="IOs">` | 42 == 42 |
| 引脚数 == `.pdsc` 的 `<feature type="QFN" count>` | 48 == 48 |

**UQFN48 实测结果**（这正是之前 GPIO 组错误的根源）：

```
PA=16  PB=16  PC=7  PH=3          共 42 个 IO
PC 实际引出位: 4, 6, 10, 11, 13, 14, 15
PH 实际引出位: 0(OSC_IN), 1(OSC_OUT), 6(NRST)
未引出（SVD 有寄存器但无引脚）: GPIOD, GPIOE, GPIOF, GPIOG
```

所以 `--probe-info` 现在会打印：

```
--- 封装键合 ---
  UQFN48 (N32H473CEU7_UQFN48.json, 引脚号已核实=是): PA=16  PB=16  PC=7  PH=3
    未引出（寄存器存在但无引脚）: GPIOD, GPIOE, GPIOF, GPIOG
[PASS] 封装键合表已应用   (UQFN48)
[PASS] 端口矩阵只列出已引出的端口   (PA/PB/PC/PH)
```

外设树里 `GPIOD/E/F/G` 显示为灰字 **(本封装未引出)**，端口矩阵的下拉框只剩
`PA / PB / PC / PH`。即便这些端口的寄存器在总线上仍能读（die 上确实有），也不会再被
当成可用 IO。

`functions` 字段是手册的复用功能列表，带 `板级/` 前缀的条目来自本板固件，不是手册内容。
信号名的上划线在文本提取时会丢失，脚本已去掉。

> `scripts/gen-package-json.py` 不会覆盖这份文件（检测到 `source: datasheet` 就跳过）。

### 下载 / 解压全系列 N32 SVD

N32 的 DFP 是**每个系列一个包**，不是一个大包；包内 SVD 又按**子系列**给
（`N32H47x_DFP` 里只有 `N32H473/474/475.svd`，其中 N32H473 的 13 个封装型号共用一份）。

```bat
python scripts\fetch-n32-svds.py --list           :: 列出全部系列的下载链接
python scripts\fetch-n32-svds.py                  :: 全部下载并解出 SVD
python scripts\extract-svd-packs.py               :: 解压已放进 svd\ 的 .pack
python scripts\extract-svd-packs.py --clean       :: 先清掉旧 .svd 再解压
```

下载地址取自每个包自带 `.pdsc` 里的 `<url>`（Keil 在 `PACK\.Web\NSING.N32*_DFP.pdsc`
留有索引），版本号取 `<release version>`。也可以像现在这样手动把 `.pack` 放进 `svd\`
再跑 `extract-svd-packs.py`。

**解压后的目录**：

```
svd/
  N32H47x_DFP.1.2.0/
    N32H473.svd  N32H474.svd  N32H475.svd
    package.pdsc
  N32G45x_DFP.1.3.0/  N32H48x_DFP.1.2.0/  ...   共 10 个系列 20 份 SVD
  manifest.json        型号 → SVD 精确映射（157 条）
```

`svd/` 已被 `.gitignore` 排除（二进制资产不入库），脚本按机器跑一次即可；
程序启动时会扫描仓库的 `svd/` 目录并加载 `manifest.json`。

#### manifest 解决的是「型号 → 该用哪份 SVD」

厂商清单是**唯一**能把精确料号绑到 SVD 的地方，程序优先用它，只有查不到才退回
按名字模糊评分：

```
[PASS] 加载 svd/manifest.json   (157 条)
    N32H473CE   -> svd/N32H47x_DFP.1.2.0/N32H473.svd  IO=42  脚=48  封装=UQFN48/LQFP48
    N32H473KE   -> svd/N32H47x_DFP.1.2.0/N32H473.svd  IO=26  脚=32  封装=QFN32
    N32G452CB   -> svd/N32G45x_DFP.1.3.0/N32G452.svd  IO=37  脚=48  封装=LQFP48
[PASS] 清单查询 N32H999ZZ   (未命中)
```

几个细节：

- **两种 pdsc 格式都支持**：老版用结构化 `<feature type="QFN" count="48"/>` +
  `<feature type="IOs" n="42"/>`，新版只写进 `<description>` 散文
  （`- Package:UQFN48/LQFP48` / `- I/O: 42`），脚本两种都解析。
- **多封装不猜**：`- Package:UQFN48/LQFP48` 两个名字都是 48 脚 → 引脚数确定为 48；
  `LQFP100/LQFP128` 引脚数不一致 → 置空而不是取第一个（否则会出现「100 脚配 107 IO」
  这种自相矛盾的记录）。
- **短料号可前缀匹配**：J-Link 报 `N32G452CB`，包里写的是完整订货号 `N32G452CBL7`，
  manifest 查询会先精确匹配、再唯一前缀匹配，多义时明确报出来而不瞎选。

识别流程因此变成：`CPUID/IDCODE → 型号 → manifest 查 SVD 与封装事实 → 加载该 SVD`。
`ioCount` / `pinCount` 改用 manifest 的值（比 `data/device_ids.json` 更贴近具体料号）。

### 左侧外设树也只在连接后更新

外设树和封装图遵循同一条规则：**未连接时不展示内容**，只显示一句提示——

```
连接目标后自动读取设备树
```

即使启动时已经通过 `--svd` 或上次会话加载了 SVD，树也保持等待态：SVD 描述的是整个
家族，未确认目标型号前把它铺开会让人误以为这些外设都在这颗芯片上。填充时机：

| 时机 | 行为 |
|---|---|
| 启动 / 打开 SVD（未连接） | 树 = 等待提示；寄存器全景、检查器、端口矩阵一并清空 |
| 连接 + 识别 + 设备树扫描成功 | `populateDeviceUi()` 填充树、端口矩阵并选中首个有意义的外设 |
| 已连接但扫描失败 | 回退为 SVD 全集（仍可用），状态栏提示 |
| 断开 | 回到等待提示，清空设备树状态与「已配置引脚」白名单 |
| 识别失败且未连接 | 保持等待提示 |

中心三个页签（封装 / 端口矩阵 / 寄存器全景）与外设树因此是同步的：都只在真正知道
目标之后才有内容。

回归断言：`--render` 除 `peripheral-tree.png` 外还会输出 `peripheral-tree-waiting.png`，
并校验等待态**只有 1 条禁用提示项、无子项**（不一致直接判失败），防止以后改造时又把
SVD 全集铺出来。

### 左侧外设树：按目标真实设备树分配

SVD 描述的是**整个家族**（`N32H473.svd` 覆盖 473/474/475/482/487），直接铺开会把本芯片
没有的外设也列出来。现在识别完成后会向目标读一遍真实设备树，两个独立来源交叉判定
（`DeviceTreeScanner`）：

| 来源 | 读什么 | 说明什么 |
|---|---|---|
| **RCC 时钟门控位** | `RCC_AHB1PCLKEN` / `APB1PCLKEN` / `APB2PCLKEN` / `AHBPCLKEN`，每个外设一位 | 固件有没有真的给这个外设开时钟 |
| **总线访问测试** | 每个外设挑一个寄存器读 4 字节 | 芯片在总线上有没有这个地址 |

单个读结果的歧义由两者组合消除 —— 总线报错**只有在时钟确定开启时**才判"本芯片未实现"，
因为时钟关闭时访问同样会报错：

| 时钟位 | 总线读 | 判定 | 界面显示 |
|---|---|---|---|
| 开 | 成功 | 存在·已使能 | 正常，时钟列 `开` |
| 开 | 报错 | **本芯片未实现** | 灰字 + `(本芯片未实现)` |
| 关 | 报错 | 无法访问（**不**判缺失） | 灰字 + `(无法访问)`，时钟列 `关` |
| 无门控信息 | 成功 | 存在·时钟未知 | 正常，时钟列 `—` |

### 本型号没有的外设：静态判定，不碰目标

SVD 描述的是**芯片家族**：`N32H473.svd` 声明 `SPI1..SPI6`，但 48 脚 N32H473CE 只有 **5 条 SPI**。
判断"这台设备到底有哪些"有两份**静态厂商数据**可用，完全不需要读目标：

| 来源 | 内容 | 作用 |
|---|---|---|
| CMSIS-Pack `.pdsc` 的 `<description>` | 厂商按**精确型号**写的能力清单（`Dname="N32H473CE"`）：`SPI/I2S:5/2`、`FDCAN:2`、`I2C:4`、`I/O:42` … | 给出"有几个"的上限 |
| `data/packages/*.json` 引脚表 | 每个实例在本封装里真正引出的信号 | 决定"是哪几个" |

算法：按类取 SVD 实例 → 用引脚信号完整性筛（SPI 需 `NSS/SCK/MISO/MOSI` 齐全）→
若仍多于厂商声明数，按编号从大到小裁剪。**可任意 IO 映射**的类（数据手册注 5/6：
`USART3`、`UART5`、`UART8` 的 TX/RX，`FDCAN1/2` 的 TX/RX）没有固定引脚是正常的，
不参与引脚判据。

实测结果与厂商声明**逐一吻合**：

```
类      SVD 实例   厂商声明   可用    剔除原因
SPI       6         5        5     SPI5 缺 MISO（仅 PF8/PE13，100/144 脚才有）
I2C       4         4        4     —
USART     4         4        4     USART3 可任意映射
UART      4         4        4     UART5/UART8 可任意映射
FDCAN     3         2        2     FDCAN3 超出声明（FDCAN1/2 可任意映射）
```

> **为什么不靠读取来证明"不存在"**：外设的时钟门控串在总线应答路径上，门控关着时
> 寄存器不通电，访问必然失败 —— 而"外设不存在"表现完全相同，无法区分。要区分就得
> 先把它的时钟打开，但那会扰动目标：被门控挂起但仍保存着配置的外设（定时器 `CCxE`、
> SPI 的 `SPE`）会**恢复输出**，`DMA1/DMA2` 的时钟一旦恢复可能**重启挂起的传输**。
> 因此这里只用静态数据，`scan()` 保持纯只读，一个字节都不写。

> 顺带说明：**Keil/MDK 同样不做型号裁剪**。它的 Debug → Peripherals 只按
> `<debug svd="...">` 指向的 SVD 列外设，所以 Keil 面对这份 SVD 也会显示 6 条 SPI。
> 我们比它多做一步，依据的就是它自己 pack 里那段没人解析的描述文字。

能力清单用脚本从 pdsc 同步到 `data/device_ids.json`（`data/` 是随包发布的，
`svd/` 不入库，所以运行时只依赖前者）：

```bat
python scripts\sync-part-capabilities.py           :: 从 svd/ 或 Keil PACK 读取并写入
python scripts\sync-part-capabilities.py --dry-run :: 只看结果
```

未登记 `capabilities` 的型号**不做任何裁剪**（显示 SVD 全集），不会凭猜测隐藏。

### 连接后的刷新开销（为什么以前会卡）

每轮轮询会产生**十几次读取完成**（pinned 外设 + 每个定时器一个 CNT），而原先**每次读取完成
都触发一次完整 UI 刷新**：重建位域表、重画分块图、重画端口矩阵、重画封装图 16 次。

`--bench` 把这个路径单独计时（不需要硬件）：

```bat
mcuRegDebugger.exe --bench --svd <svd> --device N32H473CE
```

改动前后（同一台机器，FDCAN1 外设）：

| 项目 | 改前 | 改后 |
|---|---|---|
| `InspectorPanel::setRegister`（值在变） | 283 µs | 10.9 µs |
| 单次刷新合计 | 287 µs | 12.8 µs |
| 每轮刷新次数 | ≈ 8～20 次（每次读取一次） | **1 次**（合并） |
| 估算每轮 GUI 开销 | 2.3 ms | **0.013 ms** |

具体做法：

1. **合并刷新**：读取完成只置一个标志，30 ms 单次定时器统一刷一次 —— UI 开销不再随
   「外设数量 × 读取次数」增长。
2. **同寄存器走快路径**：轮询时选中的寄存器不变，只更新值变了的单元格；不再重建整张表
   （`resizeRowsToContents` + 每格重新 new 一个 `QTableWidgetItem` 是主要成本）。字段编辑区
   也只在**选中字段的位**真的变化时才重建（它会重建枚举下拉）。
3. **定时器计数降频**：CNT 每 `max(500, 2×轮询间隔)` 采样一次，而不是每轮都读。
4. **封装图批量重绘**：喂 16 个引脚由「16 次 `update()`」改为 1 次。
5. **读取回填用二分查找**：一次窗口读取完成后，只遍历落在窗口内的寄存器；原先每次都扫全
   设备 1455 个寄存器。

> 顺带修掉一个真正的内存越界：`composeFromBytes()` 会把**整个窗口长度**（几十字节）的
> `memcpy` 进一个 `quint32`，手动读取大窗口时破坏栈。这也是 `--bench` 一开始直接崩溃的原因。
> 现在按目标宽度截断。

自检里有性能预算断言（40 次刷新平均 < 100 µs），防止以后又退化成整表重建。

### 型号到底能不能从 Flash 读出来？

结论：**"具体型号"读不出来，"是不是同一颗模具"读得出来**，而后者正好够用。

数据手册（N32G455 第 17 页「唯一设备序列号」）写明 Flash 的系统配置块里有：

| 内容 | 长度 | 用途 |
|---|---|---|
| UID | 96 bit | 单颗芯片的序列号，做密钥/序列号 |
| UCID | 128 bit | "遵守国民技术芯片序列号定义，包含芯片生产及版本相关信息" |

- 这两个都是**单颗芯片的唯一标识**，不是型号。UCID 名义上含版本信息，但手册
  **没有公布位定义**，无法据此反推"N32G455"。
- 存储器映射里 `0x1FFF_F800–0x1FFF_F811` 是 OptionBytes，`0x1FFF_4000–0x1FFF_F7FF`
  是保留区，SVD 里也**没有** `0x1FFFxxxx` 段的任何寄存器（我核对过全部 54 个 baseAddress）。

而型号本来就有一个权威来源：**`DBGMCU_IDCODE` 的 DEV_ID**。之前读不出来不是读不到，
而是库里没有这一条。你这块板实测：

```
IDCODE @0xE0042000 = 0x8F544511  ->  DEV_ID = 0x511  REV_ID = 0x8F54
```

`0x511` 已写进内置识别表（标记为实测），于是现在整条链路自动打通：

```
DEV_ID 0x511 -> N32G455 -> N32G455.svd   (SVD 由系列内料号反查)
```

剩下"同容量不同封装"的部分才需要你选一次，见上一节。

### 连接时不再重复识别

日志里 `CPUID` / `IDCODE` / `清单` 各出现两次：`doConnect()` 为了用正确器件名重新挂接，
本来就要识别一次，UI 又完整识别了一次。现在**复用连接时的结果**（`earlyIdentified` 信号），
只在手动指定芯片、或那次提前识别失败时才重新识别。

### 断线后不再刷屏（`GDB Server 未连接`）

现象：连 G455 后日志每 200 ms 刷一批

```
WARN 读取 0xE000ED28 失败: GDB Server 未连接
```

三个叠加的原因，逐一修掉：

1. **重连失败被当成成功**。`doConnect()` 自动识别后会用真实器件名 `setDevice()` 重挂接，
   而 `setDevice()` 会先 `stopServer()`；重连失败时却只打了条 WARN，`st` 仍是成功，
   于是界面以为连着、继续轮询一个**已经停掉的服务器**。
   现在重连失败会**退回通用 `Cortex-M4`** 再试一次（会话仍可读寄存器），
   两次都失败才如实上报连接失败。
   > 为什么偏偏 G455 触发：J-Link 的 `-device` 要**精确料号**，而家族级识别给出的是
   > `N32G455`（15 个料号共用一个 DEV_ID），这个名字 J-Link 不认。

2. **失败信息没被识别成"断线"**。错误文本里已经写着"未连接"，但判断用的是
   `QLatin1String("未连接")` —— QLatin1String 把字节按 Latin-1 解释，**永远匹配不到中文**，
   所以断线检测静默失效。已改成 `QStringLiteral`，并加了**源码级断言**：
   扫描 `src/` 下所有 `QLatin1String("…")`，出现非 ASCII 就 FAIL。

3. **每条失败都单独记日志**。现在同一次断线只记一次，连续 3 次后自动
   `stopPolling()` + 标记断开 + 视图回到等待态，状态栏提示"连接已丢失"。

**单次总线错误不会被误判成断线**（只有"未连接/探针未打开"这类文本才算），
这一点也有断言覆盖。

### 真机实测：N32G455 已能自动定到 N32G455CCL7

在真实板子上跑出来的完整链路（`--probe-info --verbose`）：

```
IDCODE  0xE0042000 = 0x8F544511   -> DEV_ID=0x511
内存探测: Flash 256 KB / RAM 144 KB
端口探测: 存在 3，不存在 4，未知 0
按 GPIO 端口存在性排除 3 个料号: N32G455MCL7, N32G455RCL7, N32G455VCL7
由端口探测确定为 N32G455CCL7
识别结果 : N32G455CCL7   (LQFP48, 256KB/144KB)
```

**关键实测发现**：die 上不存在的 GPIO 端口**不会总线报错**，而是整个寄存器块读回全 0。
直接读内存就能看到：

```
0x40010800 (GPIOA) = B0000008 18800BBB 0000D000 0000A000   ← 有值
0x40011400 (GPIOD) = 00000000 00000000 00000000 00000000   ← 全 0
0x40011800 (GPIOE) = 00000000 ...                          ← 全 0
```

而 `RCC_APB2PCLKEN = 0x0000283D`（bit5 `IOPDEN`=1）说明 **GPIOD 时钟是开着的**，
所以"全 0"不是没使能，而是这个端口根本不在 die 上。

因此判据改为：**时钟已开 + 读成功但全 0，或总线拒绝 → 端口不存在**。
只靠"总线报错"会把 7 个端口全判成存在，四个封装也就排除不掉（这就是你日志里
`仍匹配到 4 个料号` 的原因）。

### 只剩一个候选却还要你选封装（已修复）

现象：端口探测已经把 N32G455 收到唯一料号 `N32G455CCL7`，但中间的引脚图仍然要求你
选择封装才肯绘制。

根因是**步骤顺序错了**：

```
步骤 3  按料号查 SVD  ->  此时候选还有 4 个，于是"封装未知"（故意的，不猜）
步骤 4  端口探测      ->  候选收窄到 1 个
```

封装/脚数/IO 数在第 3 步写死跳过，第 4 步收窄后又没有补做，于是绘图页拿到的是
`封装=? / 脚=?`，只能要求选择。

修复：新增 **4b 步**，在候选列表最终确定之后再补一次精确料号信息（封装 / IO / 脚数），
来源仍是料号表与厂商清单，不是猜测。

```
识别结果: N32G455CCL7 / 候选 1 / 封装 LQFP48 / IO 37 / 脚 48
渲染    : 已绘制, 当前封装 LQFP48 (48 脚)      ← 不再要求选择
```

### 连接日志：INFO 只剩一条

连接一次只在 INFO 输出**一条**摘要：

```
已连接: 器件 N32G455CCL7  |  Cortex-M4  |  DEV_ID 0x511  |  LQFP48  |  N32G455.svd
```

以下全部降为 **DEBUG**（`--verbose` 才显示，实测仍有 37 条，调试不缺证据）：

| 原 INFO | 说明 |
|---|---|
| `端口探测: 存在 3，不存在 4，未知 0` | 端口探测汇总 |
| `die 上没有的 GPIO 端口: PG, PF, PE, PD` | 缺失端口清单 |
| `J-Link 器件名: N32G455CC（…后缀不接受）` | 名字映射说明 |
| `按 DEV_ID 识别为 N32G455，以 N32G455CC 重新连接` | 重挂接过程 |
| `已加载 N32G455 的 SVD；连接后将按目标回读设备树` | SVD 加载 |
| `按端口探测隐藏 4 个本 die 不存在的 GPIO 端口` | 隐藏计数 |
| `GPIOD 的寄存器可读，但 LQFP48 封装未引出该端口 -> 视为不可用`（×4） | 逐端口说明 |
| `按封装键合数据修正了 4 个 GPIO 端口` | 键合修正计数 |
| `Flash/RAM 探测: … 可读 -> …` / `内存探测: …` | 内存探测细节 |

> 顺带修掉一条**误报**：工具栏里的 `N32G455CC` 与识别库行 `N32G455` 其实是同一个器件
> （只差封装字母），以前每次重连都会 WARN 一次"界面上的芯片名与实际不符"。现在按
> J-Link 器件名做前缀比较，同器件不再报警；真机日志因此**零 INFO/WARN**。

### probe-info 输出只留结论

去掉了几类噪音：

- 日志文件 / JLink.exe / probe-rs 的**完整安装路径**（现在只报版本）
- 候选可执行文件的逐条路径列表
- 器件识别库的路径（只报条数）
- SVD 的完整路径（只报文件名）

现在是：

```
=== 探针后端: jlink ===
[OK] J-Link V8.26 | DLL V8.26 | 固件 ... | S/N 602717758 | J-Link PLUS
[PASS] 器件识别库   (11 条)
--- 连接目标 ---
[PASS] 重连后会话可用   (N32G455CC)
识别结果  : N32G455CCL7  家族=N32G45x  厂商=Nationstech  核=Cortex-M4
后端器件名: N32G455CC（器件名 N32G455）
SVD       : N32G455.svd
```

### 换了同系列另一颗 H473 就识别不了（已修复）

现象：换成另一块 H473，报 `DEV_ID 0x711 不在识别库中`，整个工具无法使用。

实测这颗板子：`DEV_ID=0x711`、`REV_ID=0xA538`、Flash 256 KB、RAM 144 KB、
GPIO 只有 PA/PB/PC/PH。同系列里 48 脚的只有两个：

| 料号 | Flash | RAM | DEV_ID |
|---|---|---|---|
| `N32H473CC` | 256 KB | 144 KB | **0x711**（本次实测） |
| `N32H473CE` | 512 KB | 192 KB | 0x712（之前实测） |

之前库里只登记了 `0x712`，所以这颗直接判失败。已把 **0x711 → N32H473CC** 登记为实测值。

同时修了两个**通用性**问题，避免下次再“换个料号就废”：

1. **未知 DEV_ID 不再硬失败**。以前查不到就直接报错、什么都不显示。现在会读内存签名，
   在 `data/parts.json` 里跨系列匹配，给出候选列表让你选一次（选中后记住 DEV_ID）。
   实测该场景能给出 30 个候选而不是死路。

2. **内存探测的上界判据修正**。以前“读成功”就算该尺寸存在，但：
   - 擦除的 Flash 读回 `0xFFFFFFFF`（512K 边界在 256K 芯片上也是这个值）
   - 保留/别名区读回 `0x00000000`（192K 边界在 144K RAM 芯片上也是这个值）

   于是 256K/144K 的芯片被误判成 512K/192K。现在这两种值都视为**无法作为依据**，
   继续向下找。实测已能正确判为 256K/144K。

### SEGGER 器件名的真正来源：厂商 XML，不是 DLL

上一节说“J-Link 不认识 N32G455”仍只对了一半：`JLinkARM.dll` 里确实只有 124 个
**旧名字**，但厂商另外安装了完整定义：

```
%APPDATA%\SEGGER\JlinkDevices\Nations-JLinkDevices.xml   ← 197 个 ChipInfo
%APPDATA%\SEGGER\JlinkDevices\Nsing-JLinkDevices.xml
```

这里面**包含 N32H473CC / H473CE 等**（DLL 里没有）。这就是 J-Flash 能选到这些型号的
原因，也解释了为什么 `-device N32H473CC` 能连上而 `-device N32G455` 不能
（后者只是系列名，XML 里没有这个条目）。

`scripts/sync-jlink-devices.py` 现在同时扫描 XML 与 DLL，收集到 **228 个器件名 / 41 个系列**
（之前只扫 DLL 得 124 个，还因此误判 N32H473 不受支持）。

### DEV_ID 是靠人一条条加的？——不是，靠测量

一个合理的怀疑：每换一颗芯片就要往 `data/device_ids.json` 里加一条，那这个表早晚
会比芯片还长。结论是**不需要**——下面说清楚为什么，以及人工登记到底还剩多少。

#### 先分清两件事

| 环节 | 数据来源 | 会不会碰目标？ |
|---|---|---|
| **设备树：有哪些外设** | SVD + `parts.json` + `packages/*.json` | **完全不碰**，纯静态 |
| **设备树：哪个外设时钟开着** | 读 RCC 寄存器 | 只读，不写 |
| **型号识别：这是哪颗芯片** | DEV_ID，查不到时靠测量 | 会（临时开 GPIO 时钟） |

第一行是关键：`PeripheralPresence::analyse(const SvdDevice&, const Inputs&)` 的签名里
**没有 `IProbe`**，它拿不到探针，只能靠静态数据算。`DeviceTreeScanner::scan` 里只有
两处 `readU32`（读 RCC 取时钟状态、读外设首寄存器看可访问性），**没有任何 `writeU32`**。
所以「扫描设备树」不会开任何时钟。

全工程只有一处会写 RCC 去开 GPIO 时钟：`DeviceIdentifier::probeGpioPorts`。它只回答
一个问题——**「这颗芯片是哪个料号」**，不回答「它有哪些外设」。型号一旦确定，设备树
立刻回到纯静态。

#### 为什么 DEV_ID 无法查表

真的去翻过所有厂商文件，**没有任何一处记录 DEV_ID → 型号**：

| 文件 | 里面有什么 | 有 DEV_ID 吗 |
|---|---|---|
| CMSIS-Pack `.pdsc` | description / memory / algorithm / debug | 无 |
| SVD（`.svd`） | 寄存器与字段，`DBG_ID` 的 resetValue 是 `0x00000000` | 无 |
| SEGGER `JlinkDevices.xml` | Name / Core / WorkRAM / Flash | 无 |

`DBGMCU_IDCODE` 的 DEV_ID 是硅片在掩膜时定下的，只存在于芯片里。所以新型号没法"查"，
只能"测"。

#### 三级测量：测量代替登记

`DeviceIdentifier` 在 DEV_ID 未登记时依次测三样，每一样都是通用物理事实，与具体型号无关：

1. **Flash / RAM 大小** → 定容量。Cortex-M 的地址映射是统一的，直接往上界读。
2. **GPIO 块基地址** → 定系列。各系列的 GPIO 不在同一地址（这就是为什么能分开）：

   ```
   0x40010800   N32G05x / G432 / G435 / G451..G457
   0x40021400   N32G41x
   0x40023400   N32G401
   0x4002C000   N32H473..H499
   0x58032800   N32H7xx
   ```

   这张表由 `scripts/sync-svd-gpio.py` 从 SVD 里扫出来，写进 `data/svd_gpio.json`，
   **不需要人工维护**：以后丢进新的 SVD，重跑脚本即可。

3. **哪些 GPIO 端口应答** → 定封装。同一颗 die 的不同封装引出的端口不同
   （LQFP48 有 PA/PB/PC/PH，LQFP64 多 PD，LQFP100 多 PE/PF），数一数就知道是哪个封装。

实测一颗没登记的 `DEV_ID 0x711`：27 个候选 → 基地址筛掉 15 个 → 端口筛掉 8 个 →
外设差异再筛掉 2 个 → **剩 2 个**（`N32H473CC` / `N32H473KC`，唯一的差别是 48 脚 vs
QFN32，硅片上没有信息可区分，这属于"该问人"的边界）。

#### 人工登记还剩多少

只剩**用户自己点选的那一次**：候选不唯一时弹一次列表，选完按 DEV_ID 记住
（`AppSettings::setBindingForSn`）。也就是说这张表是**用着用着自己长出来的**，不是
开发时一条条填的。已有 DEV_ID 的芯片（比如 0x711→N32H473CC）连一次都不会问。

#### 测量过程中踩到的四个坑（都已修正）

1. **擦除的 Flash 读回 `0xFFFFFFFF`**，512K 边界在 256K 芯片上也是这个值 → 曾把
   256K 判成 512K。现在 `0xFFFFFFFF` 和 `0x00000000` 都视为"无法作为依据"，继续往下找。
2. **RAM 上界会读到邻居/别名区的数据**。实测在 144K 的 H473 上，`0x20027FFC`
   （160K 边界）读回 `557E40E1`——非零，但它不是 RAM。所以 **RAM 只能做辅助**，
   判断以 Flash 为准（`MemoryProbe::flashMatches`）。
3. **`parts.json` 里有占位料号**（`N32H785xIx7` 这类，Flash/RAM 都是 0）。"两边都
   未知"曾被认为"不冲突"，于是每次宽扫都被它们灌满候选。现在没有容量数据的条目
   **直接不当候选**。
4. **开了时钟之后，缺失的端口读回 `0xFFFFFFFF`，而不是 0**。实测 H473 上把 GPIO
   时钟全开，缺失的 GPIOD..GPIOH 在 `+0x00` 读回 `0xFFFFFFFF`——那恰好等于真实端口
   PMODE 的复位值，所以"非零即存在"是错的。判据改成：`0x00000000`（空白）和
   `0xFFFFFFFF`（桩值）都算"没有信息"，真端口总会有一处给出真实值。
   不过 `GPIOH` 的 `PID` 是 0、只有 `PMODE=0x3FFF`，所以最终判据是"任一寄存器既非 0
   也非 `0xFFFFFFFF`"，单看某一个寄存器会漏。

#### 姊妹型号怎么分

`N32H473CC` 和 `N32H474CC` 容量、封装、端口全一样，静态数据分不开。但外设不同：
H474 有 8 个 SHRTIM 块，H473 没有，而缺失的外设读回空白。于是从"只在对方 SVD 里
出现"的外设中挑一个探测即可（`4a` 步）。实测用 `SHRTIM_TIME` 判出这是 H473。

### 导入真实 C++ 工程只有 3 个变量（已修复，含四个根因）

现象：导入 ODrive 的 `BX8115.axf`，只解析出 3 个变量。而这个文件里其实有
**6557 个 `DW_TAG_variable`、107 个编译单元、951 个带固定地址的变量**。

`--symbols <文件>` 就是为这类问题加的：不需要硬件，直接看解析结果和诊断。

四个都是真实缺陷，而且第一个就足以让 107 个编译单元里的 106 个被丢掉：

**1. 只读了第一个编译单元。**

```cpp
// 错的：readUnit 已经把游标推到本单元末尾了
if (next <= info.pos) break;      // → 第一个单元之后立刻跳出
```

DWARF 把整个程序切成一个个编译单元（这里 107 个），每个都有独立的 `unit_length`。
这个条件在读完第一个单元后恒成立，于是后面 106 个单元从来没被读过。修成按
`unit_length` 推进后：**3 → 781**。

**2. C++ 的 `class` 型不被识别。**

原来只认 `DW_TAG_structure_type` / `DW_TAG_union_type`，漏了 `DW_TAG_class_type`。
C++ 里几乎所有对象都是 class，于是它们的类型一律解析失败，变量被丢弃。

**3. 前置声明不会跳到真正的定义。**

ODrive 的 `controllers`（14848 字节，整个工程的核心数据）类型是 `Controller[1]`，而在
那个编译单元里 `Controller` 只是一句 `DW_AT_declaration: 1` 的**前置声明**——没有
size、没有成员。定义在别的单元里。

原来一边读单元一边解析变量，跨单元的引用根本还读不到。改成**两阶段**：先把所有单元
的 DIE 全收下来，建立「类型名 → 定义」索引，再统一解析变量。

顺带说明：`Controller` 在这个 .axf 里**只有声明**（所有同名 DIE 都带
`DW_AT_declaration: 1`），编译器没有输出它的成员布局。所以 `controllers` 本身仍然
无法展开成 `controllers[0].axis…`——这是文件的信息量限制，不是解析器的问题。
`Config_t`（14628 字节）这类有定义的 struct 则能正常展开。

**4. 结构体数组一行都不出。**

`expandComposite` 只走 `struct → member` 这条路，遇到 `TAG_ARRAY_TYPE` 直接 return，
所以「结构体数组」既不算标量也不能展开，结果是**零行**。修好后：

```
error_mapping_table_axis[0].internal_error_code   0x08038560
error_mapping_table_axis[0].cia402_error_code     0x08038568
error_mapping_table_axis[0].err_reg_bit           0x0803856C
error_mapping_table_axis[1].internal_error_code   0x08038570
```

**另外两处改进**（不修也能跑，但结果会很难看）：

- **名称限定**：C++ 里两个命名空间可以有同名静态变量。现在按祖先链拼出
  `ns::Class::name`，避免混在一起。
- **跨单元去重**：同一个全局变量会被每个引用它的编译单元重复描述一次（`nm` 只有一个
  符号）。原来同一地址同一名字会出现好几行，现在按「名字@地址」去重（8 条）。
- **`--symbols` 的类型分布**能一眼看出「所有变量都退化成无符号」这类故障：

```
类型分布: 未知=27  指针=645  无符号=1659  有符号=101  浮点=18
地址来源: DWARF 2446 / 符号表 4
```

最终：**3 个 → 2450 个变量**，地址与 `arm-none-eabi-nm` 逐一核对一致。

#### 变量有三种，不是一种：全局 / 局部 / 类成员

这是一个反复踩到的问题，值得写清楚——报「这个变量没有」的，多数不是解析失败，而是
它压根不属于「全局变量」这一类：

| 类别 | DWARF 表示 | 地址 | 出现在树里 |
|---|---|---|---|
| 全局 / 静态 | `DW_TAG_variable`（文件作用域） | 链接时固定 | ✓ |
| **函数局部** | `DW_TAG_variable` 挂在 `DW_TAG_subprogram` 下 | 帧基址 + 偏移，停机才有 | ✓（在函数下） |
| **类成员** | `DW_TAG_member` 挂在 `class`/`struct` 下 | 只能 `实例 + 偏移` | ✓（在类型下） |

##### 函数局部变量（`match_indx` 这类）

`match_indx` 是 `update_sec_enc_offset()` 里的局部变量。它之前完全没被收进来，因为
**9285 个局部变量用的是 location list，只有 1226 个是内联 `DW_OP_fbreg`**，而我只处理
了后者。

location list 必须**在停机那个 PC 上求值**——同一个变量在不同代码区间可能在寄存器里，
也可能在栈上。所以不能导入时算好，要等读到 SP/PC 再解：

```
encoder.cpp
  update_sec_enc_offset       函数 · 12 个局部变量
    match_indx                int32_t     0x2000A11A
```

顺带修了两件事：
- **参数也是局部变量**，但它们是 `DW_TAG_formal_parameter`，不是 `DW_TAG_variable`；
  漏了这条会让「参数全在栈上」的函数看起来空着。
- **函数名缺失**：armclang 在 22942/49480 个 subprogram 上不写 `DW_AT_name`。现在按
  `linkage_name`（做了 Itanium demangle）→ `specification` → `abstract_origin` 回退，
  实在没有才显示 `(匿名函数 @地址)`。否则全都挤在一个「(匿名函数)」里。

##### 类成员（`index_found_` / `is_ready_` 这类）

它们是 `class Encoder` 的成员，既不是全局也不是局部。**成员没有独立地址**，
只有 `实例 + 偏移` 才有意义。

原来的实现只在展开某个全局实例时才看得到它们（`encoders[0].index_found_`），
所以打开 `encoder.cpp` 找一个 `Encoder` 的成员是找不到的——它跟着**实例**走，
而实例声明在别的文件里。

现在按文件额外列出**该文件定义的类型**：

```
encoder.hpp
  class Encoder               class · 63 个成员
    index_found_              bool        +1098      1 B
    is_ready_                 bool        +1099      1 B
    shadow_count_             int32_t     +1100      4 B
```

成员显示的是**实例内偏移**而不是地址，因为地址取决于你关心哪个实例。它们不可勾选
（勾一个没有地址的东西没有意义）。如果你要采样，仍然走实例那条路
（`encoders[0].index_found_` 是真实地址 `0x2000A11A`）。

两个实现细节：
- `typedef struct { ... } Point;` 的**匿名结构体没有 `DW_AT_name`**，只有 typedef 名。
  这个写法在 C 里到处都是，不处理就会整类丢失。
- 文件行的列表取**并集**（有变量的 ∪ 有函数的 ∪ 有类型的）。之前只取有变量的文件，
  于是 `encoder.hpp` 这种「只定义类型」的头文件根本没有行，`Encoder` 自然无处可见。

##### 文件顺序

有变量的文件在前，其次有函数的，最后只定义类型的头文件——否则字母序会把
`__bit_reference` 这类 libc++ 内部头文件排到 `adc.c` 前面。

#### 变量按源文件分组；选择面板独立成窗

一列 4000 个变量名没法用——你不知道该去哪找。现在树的顶层是**源文件**，文件下面是
声明在它里面的变量：

```
ds402.c                              380 个变量
  ObjectDict                          ds402_Objdict_t[...]   0x2000...
    ...
controller.cpp                         2 个变量
  controllers                         Controller[1]          0x2000620C
```

实现上要解 `.debug_line`：`DW_AT_decl_file` 只是该编译单元文件表里的**序号**，名字在
行号程序头里。这里踩到一个坑——第一个编译单元的行表偏移就是 **0**：

```cpp
if (spec.name == AT_STMT_LIST && value.u != 0)   // ← 0 是合法偏移，被当成“没有”丢掉了
```

结果第一个文件的变量全落到「(未知文件)」。改成只判断**属性是否存在**后，ODrive 那份
52 个文件全部归位（`comm_modbus.cpp` 就是第一个单元）。

文件表只解析 prologue（目录 + 文件名），不解析行号程序本身：我们只需要"序号 N 是哪个
文件"。DWARF v2–v4 的 prologue 格式已支持；v5 换了自描述格式，这里**不支持**——遇到
v5 就不分组，而不是猜。

**变量选择面板挪到独立的 dock**（`QDockWidget`，可拖动/浮动/关闭），和左边的外设树
叠成标签页，并加进了「视图」菜单可以随时开关。以前它和曲线挤在同一个 tab 里，变量列表
一展开就把波形压没了。首次成功导入 .axf 时自动显示。

#### 树为什么一开始看不出来（显示 bug）+ 卡顿的成因

三个问题一起修了，**第一个是我上一轮自己引入的**：

**1. 树是平的，因为顶层项的展开箭头被关掉了。**

```cpp
m_variableTree->setRootIsDecorated(false);   // ← 顶层项没有箭头
```

结构体和数组**全部是顶层项**，所以一个都展不开，看起来就是一列平铺的名字。数据模型
明明是树，界面却是平的——而我的自检只断言了 `childCount()`（模型层），没断言
`rootIsDecorated`，于是测试全绿、界面全错。已修，并补了断言。

**2. 卡顿：5692 个节点全部提前创建。**

一份 4 MB 的 .axf 展开后有 5692 个节点，每个节点 4 列 + 多行 tooltip。全部建出来就是
一次性几百毫秒的界面冻结，之后滚动也沉。

改成**懒加载**：只建顶层（637 个），每个聚合节点挂一个占位子节点好让 Qt 画出箭头，
真正展开时才用模型数据填充。实测：

```
解析 DWARF      : 907 ms
导入面板(懒加载): 1051 ms（实际创建 637 个顶层行）
全展开(旧行为)  : 979 ms        ← 以前每次导入都要付这个代价
过滤重建        : 10 ms         ← 以前每敲一个字符都重建
```

另外两处：
- 过滤框加了 220 ms 防抖，不再每个按键重建整棵树；
- 勾选三态改成自己传播（Qt 的 auto-tristate 只看已存在的子项，对懒加载树不成立）。

**3. 勾选聚合节点必须记录在模型层。**

懒加载下，勾一个没展开的结构体时它的叶子还不存在。所以选择用一个 `QSet<QString>`
记录叶子路径（`m_selectedLeaves`），控件上的勾选状态只是它的**视图**。否则「勾一个
折叠的 struct」会采样不到任何东西，而用户以为勾上了。

**4. 临时变量（局部变量）**

局部变量和全局变量不是一回事，必须说清楚限制：

| | 全局/静态 | 局部变量 |
|---|---|---|
| 地址 | 链接时固定 | **帧基址 + 偏移**，运行时就变 |
| 何时可读 | 任何时刻 | **只能在停机上、且 PC 在该函数内** |
| 能否画曲线 | 能 | 不建议（地址会变） |

面板上有「读取局部变量」按钮：读了 SP/PC，找到 PC 所在的函数，把 DWARF 里的
`DW_OP_fbreg` 偏移加上帧基址，得到真实地址后显示当前值。

**工具不会自己让目标停机**——停掉一个正在跑电机控制环的核心是用户的决定，不是调试器
该悄悄做的事。

两种编译器的写法不同，都支持：

- **armclang（Keil，用户实际用的）**：`DW_AT_frame_base = DW_OP_reg13`，帧基址就是
  SP，直接可用。ODrive 那份 436 个函数 / 2007 个局部变量全部解析成功。
- **GCC**：`DW_AT_frame_base = DW_OP_call_frame_cfa`，意思是「这个 PC 的 CFA」。它必须
  从 `.debug_frame` 里求值，而且**规则在函数内部会变**（prologue 每压一次寄存器就
  `DW_CFA_def_cfa_offset` 一次）。所以在**停机的那个 PC** 上求值，而不是函数入口——
  用入口值配当前 SP 会算错。实测在 `lowPc+2` 处解出 `r13 + 24`（入口是 `r13 + 0`），
  差别就是这样来的。

地址算不出来时（缺 `.debug_frame`）明确报错，不猜。

#### 变量列表按源码层级显示（不是一列平铺的 `a.b.c`）

第一版把所有东西压成一行一个名字：`g_points[0].x`、`g_points[0].y`……用户看不出这是
一个结构体数组，也看不出 `g_points` 本身有几个元素。

现在数据模型就是**树**（`VariableNode`），UI 直接照树的形状显示：

```
g_points                     Point[2]        0x08001040   24 B
  [0]                        Point           0x08001040   12 B
    x                        float           0x08001040    4 B
    y                        float           0x08001044    4 B
    flags                    uint16_t        0x08001048    2 B
  [1]                        Point           0x0800104C   12 B
    ...
g_adc_raw                    uint16_t[4]     0x0800105C    8 B
```

几条规则：

- **结构体/联合体/类** → 节点 + 成员子节点，成员按源码顺序，地址是父地址 + 成员偏移。
- **数组** → 节点，类型显示成 `Point[2]` / `uint16_t[4]`，元素数写在类型里。
  - 结构体数组会展开出 `[0]`、`[1]` 子节点（这才是真层级）；
  - **标量数组不展开**：`uint16_t[4]` 一行就够，画图时按元素拆成 4 条曲线，
    没必要在树里造 4 个节点（`uint8_t[39406]` 那种更是不能造）。
- **指针** 不追进去：那需要读目标内存，属于采样的事，不是符号解析能回答的。
- 位域成员保留但标记为不可读（没有独立字节地址）。
- 默认**折叠**：ODrive 那份有 5692 个节点，全展开会卡住。
- 勾选是**三态**：勾一个结构体等于勾它下面所有标量，父节点显示半选/全选。
  一次勾选会触发很多次 `itemChanged`，所以用 0 延时的单次定时器合并成一次重算。

过滤也改成**剪枝**而不是打平：搜 `flags` 会保留 `g_points → [0] → flags` 整条路径，
否则搜出来的东西又变成平铺列表了。

同时扁平视图仍然保留（`SymbolTable::variables`），因为采样、帧布局和生成的固件代码
都要按叶子算。两套视图来自同一棵树，不会不一致。

**顺带解决了上一轮留下的限制**：之前说 `controllers` 因为 `Controller` 只有前置声明
而无法展开。改成两阶段解析 + 按名字找定义之后，现在能一路展开到

```
controllers                  Controller[1]    0x2000620C   14848 B
  [0]                        Controller       0x2000620C   14848 B
    config_                  Config_t         0x20006230   14628 B
      control_mode           ControlMode      0x20006230       1 B
      ...
        vel_gain             float            0x20006248       4 B
```

也就是说 `controllers[0].config_.vel_gain` 现在是可以直接勾选采样的。

#### 回归夹具改成两单元

原来的夹具只有一个编译单元，上面第 1、3、4 个 bug **一个都测不出来**。现在
`data/test/symbol_test.c` + `symbol_test_b.c` 两个单元链接成一个镜像，并断言：

```
[PASS] g_points[0].x = 浮点 @ 0x08001040      ← 结构体数组逐元素展开
[PASS] g_points[1].x = 浮点 @ 0x0800104C
[PASS] g_tu_b_counter = 有符号 @ 0x08001074   ← 第二个编译单元被读到
[PASS] g_tu_b_point.x = 浮点 @ 0x08001078
[PASS] 跨编译单元的同一变量只出现一次          ← 去重
[PASS] 所有变量的类型都被解析（无 Unknown）
```

#### 为什么放心临时开 GPIO 时钟

只开 GPIO 端口**自身寄存器块**的时钟。引脚的电平由 `PMODE`（模式）、`AFSEL`（复用）、
`ODR`（输出数据）决定，这三个寄存器一个字节都不改，所以时钟一起来引脚也不会开始
驱动。写之前先读原值，写完读回校验，析构时（`GateRestorer`）**按原值写回**；真机上
已验证 RCC 复位到 `0x607`（与进入前一致）。定时器 / DMA 的时钟**绝对不碰**——那些
一起来就真的会对外输出。

### J-Flash 为什么能支持 G455 —— 查清了，并已复用

之前结论"J-Link 不认识 N32G455"是**错的**，原因是我喂错了名字。

`JLinkARM.dll` 里其实**内置了 124 个 N32 器件名**（17 个系列），但 SEGGER 存的是
**去掉订货物号后缀**的名字：

```
JLinkARM.dll 里的 N32G455 系列（13 个）:
  N32G455CB  N32G455CC  N32G455CE  N32G455MB  N32G455MC  N32G455ME
  N32G455RB  N32G455RC  N32G455RE  N32G455TB  N32G455VB  N32G455VC  N32G455VE
```

实测对比（`JLinkGDBServerCL -device …`）：

| 器件名 | 结果 |
|---|---|
| `N32G455CC` | **成功**，J-Link 按真实器件配置 |
| `N32G455CCL7` | `Failed to get index for device name` |
| `N32G455` | `Failed to get index for device name` |

J-Flash 之所以"能支持 G455"，是因为它的界面里你选的是 `N32G455CC`（从库里挑），
**永远不会把带 `L7` 的订货物号递给 J-Link**。我们的工具拿的是物号，所以一直失败、
一直退回通用 `Cortex-M4`。

现在做了两件事：

1. **`scripts/sync-jlink-devices.py`** —— 从本机 `JLinkARM.dll` 提取全部器件名，
   连同 `JLinkDevices.xml` 的厂商扩展，生成 `data/jlink_devices.json`
   （当前 124 个名字 / 17 个系列，随包发布）。
2. **`core/JLinkDeviceName`** —— 把订货物号转成 J-Link 接受的名字：
   剥掉 `L7`/`Q7`/`S7` 这类"封装涂层+温度等级"两字符后缀，再用器件名表校验；
   查不到就返回候选列表，绝不编造。

真机结果（`--probe-info`）：

```
按识别结果重新以 N32G455CC 连接...
[PASS] 重连后会话可用   (N32G455CC)
J-Link 解析器件: N32G455CC
识别结果 : N32G455CCL7
后端器件名: N32G455CC（器件名 N32G455）
```

**不再退回 Cortex-M4** —— J-Link 现在按真实器件型号工作（Flash 算法、AP 配置都对）。

> 顺带说明：`N32H473` 系列**确实不在** SEGGER 库里（表里没有），所以它仍然走通用
> `Cortex-M4`。这条也写成了断言，避免以后误以为"所有 N32 都能直接识别"。

### J-Link 不认识 N32 的器件名（已确认）

实测 `JLinkGDBServerCL` 对任何 N32G455 名字都直接退出：

```
-device N32G455CCL7 → Failed to get index for device name 'N32G455CCL7'. GDBServer will be closed.
-device N32G455     → Failed to get index for device name 'N32G455'.
```

连**精确料号**也不行 —— SEGGER 的器件库里没有这个系列。所以：

- 会话始终以通用 `Cortex-M4` 运行（读寄存器完全够用）；
- 识别出的料号只用于**选 SVD / 识别型号**，不再拿去重连；
- 万一某个环境的 J-Link 库更新支持了，脚本仍会尝试一次，失败就退回 Cortex-M4 并说明原因。

界面与日志现在会同时显示两者，不再把料号冒充成"J-Link 已识别"：

```
后端器件名: Cortex-M4（器件名 N32G455）
```

### 内存相同怎么办：再按 GPIO 端口集合定位

你的日志：

```
N32G455 系列内存为 Flash 256 KB / RAM 144 KB，仍匹配到 4 个料号:
  N32G455CCL7, N32G455MCL7, N32G455RCL7, N32G455VCL7
```

这 4 个同容量但封装不同（LQFP48/80/64/100）。**它们的 GPIO 端口集合不一样**：

| 封装 | 端口 | IO |
|---|---|---|
| LQFP48 | PA PB PC | 37 |
| LQFP64 | PA PB PC PD | 51 |
| LQFP80 | PA PB PC PD PE | 67 |
| LQFP100 | PA PB PC PD PE | 80 |

所以再读一次"每个 GPIO 端口在不在"，就能排除掉需要 die 上不存在的端口的封装。

实现（`DeviceIdentifier::probeGpioPorts`）：

1. 只临时开启 **GPIO 的时钟门控**（读完写回原值）。这是安全子集：GPIO 时钟只供电给端口自己的
   寄存器块，**不会让任何引脚对外输出**——引脚状态由 `PMODE/AFSEL/ODR` 决定，一个都不碰。
   定时器/DMA 的时钟**绝不**碰（那才会恢复真实输出）。写入后读回校验，被写保护就退回保守判断。
2. 读每个端口的探测寄存器：读成功=存在；**时钟确认已开但仍失败=die 上没这个端口**；时钟关着=
   未知（不排除任何候选）。

端到端验证（假探针模拟"只有 PA/PB/PC"的 die）：

```
端口探测: 存在 PA,PB,PC / 不存在 PD,PE,PF,PG
识别结果: N32G455CCL7 (1 个候选)      ← LQFP48，唯一正确
```

> **方法的诚实边界**：只有当厂商为小封装用了**端口更少的 die** 时才有效。如果所有封装共用同一颗
> 全端口 die，这一步排除不了任何候选（80/100 脚仍二义），那就只能选一次——但选中后会按 DEV_ID
> 记住，以后自动。这个上限也写成了断言，避免以后误以为它能包治百病。

### 还有别的办法吗

查过、确认**不可用**的：

| 途径 | 结论 |
|---|---|
| Flash 里的 UID / UCID | UID 是单颗序列号；UCID 手册说"含生产及版本信息"但**未公布位定义**，单样本无法反推型号 |
| `0x1FFFxxxx` 系统区寄存器 | 手册映射里是 OptionBytes + 保留区；SVD 里该段**没有任何寄存器**（核对过全部 54 个 baseAddress） |
| `DBGMCU_IDCODE` | 只有 die 级 DEV_ID（0x511），所有封装共用 |
| J-Link 器件库 | 只能"你告诉它型号"，不能反查 |

真正准确的只有两样：**DEV_ID（已用）+ 内存/端口实测（已用）**，剩下的靠**芯片丝印**——所以最后
一个候选仍不唯一时，界面上用下拉让你点一次，然后记住。

### 连接日志只留一行

以前连接一次要刷十几行（CPUID、原始 IDCODE、清单加载、能力清单 14 项、引脚渲染…），
真正有用的信息被埋掉了。现在 INFO 只剩两行：

```
探针 jlink 8.26 SN 602717758
已连接: 器件 N32G455  |  Cortex-M4  |  DEV_ID 0x511  |  LQFP48  |  N32G455.svd
```

其余步骤（CPUID / 原始 IDCODE / 厂商清单 / 内存探测 / 能力清单 / 引脚渲染明细）
降到 **DEBUG**，需要排查时加 `--verbose` 就能看到：

```bat
mcuRegDebugger.exe --autoconnect --verbose
```

> 顺带修了个崩溃：那个"收集 INFO 行"的测试 lambda 捕获了局部变量的引用，而连接在语句块
> 结束后仍然有效，后续日志会写进已销毁的 `QStringList`。默认级别下看不出来（块结束后没有
> INFO 了），一开 `--verbose` 就段错误。现在 lambda 挂在**局部**接收者上，随作用域销毁。

### 工具栏芯片栏只显示型号

工具栏的芯片栏不写说明性长句，只显示型号：

| 时机 | 显示 |
|---|---|
| 启动、未连接 | `自动` |
| 识别完成后 | `N32G455CE`（就这一个型号，无后缀） |
| 手动选/填 | 你填的料号 |

显示的是型号，但**模式仍是自动**：下拉项的数据为空，`effectiveChip()` 返回空，
连接照旧走"通用 Cortex-M4 挂上 → 读 DBGMCU_IDCODE → 用真实器件名重挂接"。
这样既满足"只想看到型号"，又不会把这一块板的名字粘成下一块板的手动覆盖。
悬停可看到"已按 DEV_ID 自动识别为 …"的说明。

### 工具栏芯片名默认是“自动识别”

以前工具栏的芯片名会显示上一次识别到的料号（比如 H473），而且这个值被当成**手动指定**
传给识别，于是换芯片后仍按旧型号解释。现在：

- 下拉框第一项是 **`自动识别（推荐）`**（数据为空 = 不是器件名），默认就是它；
  连接时先用通用 Cortex-M4 挂上，再按 `DBGMCU_IDCODE` 认型号并重新挂接；
- 自动识别成功后，下拉框**保持**在自动模式，只在标题里显示 `自动识别（当前 N32H473CE）`，
  不再把料号写进去 —— 否则下一次连接又变成"手动指定上一块板"；
- 保存设置时**只记显式选择**，自动模式存空值，所以旧料号不会在下次启动时回来；
- 老版本保存过料号的用户会被**一次性迁移**清空（`probe/chipModeVersion` 标记），
  升级后第一次启动就恢复成自动。

只有自动识别不出来时（DEV_ID 未登记），才需要在这里选料号 —— 而那种情况现在多半会
先用内存探测缩小到几个候选，再让你从下拉里挑，见下一节。

### 为什么换芯片后日志还是旧型号（已修复，含根因）

现象：J-Link 接的是 N32G455，日志里读的却还是 N32H473。

根因不在识别库，而在**工具栏芯片名被当成了"手动指定"**：

1. 第一次识别成功后，程序把识别到的料号写进工具栏输入框（方便 J-Link 的 `setDevice`）；
2. 这个值从此一直留着，而 `effectiveChip()` 直接把它当 `overrideDevice` 传入识别；
3. `identify()` 原本一见 `overrideDevice` 非空就**短路**，完全不看 DEV_ID ——
   于是插上 G455，程序仍按 H473 解释，日志自然也是 H473。

修复原则：**DEV_ID 来自硅片，必须优先于界面上的名字**。

- 只要 DEV_ID 能查到，就用它；界面的名字与实测不符时打 WARN 并说明"已按实际器件识别"。
- 界面名字只在 **DEV_ID 查不到**时才作为答案（此时标记 `manuallySelected`，界面需提示）。
- 识别成功后**总是**把工具栏更新成实际料号，这样下一次连接 J-Link 拿到的也是对的。

回归测试用假探针驱动真实 `identify()`：

```
界面填 N32G455 / 实际 DEV_ID 0x712 -> 识别为 N32H473CE     ← 覆盖旧名字
DEV_ID 读不到 / 界面填 N32G455     -> 识别为 N32G455        ← 退回手动
```

### 为什么还要选料号？—— 现在大多不用了

你下载的软件包里**没有任何 DEV_ID 映射**（我逐个查过：`N32G45x_DFP` 的每个 `<device>`
只有 `description` / `memory` / `algorithm` / `debug` 四项），12 份数据手册里也没有 ID 表。
所以"DEV_ID → 料号"无法从文件推出，只能从硬件读。

于是改成**读容量**来缩小范围（`core/MemoryProbe`）：读每个候选容量的**最后一个字**，
能读到就说明地址存在。这是纯读操作 —— 不写任何寄存器、不开时钟门控。

以 N32G455 为例，15 个料号被容量区分成 3 组：

| 容量 | 料号数 |
|---|---|
| Flash 128K / RAM 80K | 5 |
| Flash 256K / RAM 144K | 4 |
| **Flash 512K / RAM 144K** | **6** |

- **唯一命中** → 直接确定料号，不用你选；
- **同容量多封装**（上表 6 个）→ 封装图页的下拉框会列出这 6 个封装供你选一次，
  **不需要手打型号**，选中后按 DEV_ID 记住，下次自动。

> 为什么同容量不能继续自动判：DEV_ID 只标识 die，封装大小在电气上读不出来
> （35/48/64/80/100 脚的同一颗芯片，寄存器行为完全一致）。硬猜封装的代价是
> 每个信号都标在错误的引脚上，比让人选一次糟得多。

### 换芯片后界面没跟着变（已修复）

现象：换成 N32G455 后点连接，界面还是上一颗 N32H473 的样子。

根因不在渲染，而在**识别"成功"了却没有型号**：识别库里只有 `N32H473CE` 登记了真实
DEV_ID（0x712），其余 10 个条目都是 `devId 0x0 / verified false`。G455 的 DEV_ID
查不到时，`identify()` 仍返回成功、`mcuName` 与 `svdPath` 却是空的，于是
`onIdentifyFinished` 既不报警、也不卸载旧 SVD —— 旧 H473 的寄存器表和引脚图就留在了屏幕上。

三处修复：

1. **识别不出型号不再冒充成功**：`mcuName`/`svdPath` 任一为空即视为未确定，明确警告、
   卸载旧 SVD、清空能力/Flash/RAM，并 `parkDeviceViews()` 把所有视图退回等待态
   （而不是继续显示上一颗芯片）。
2. **DEV_ID 自动学习**：厂商数据手册里没有 DEV_ID 表（12 份手册全查过），所以识别库只能
   从真实硬件长出来。现在既便手动指定芯片也会读 DEV_ID，一旦该 DEV_ID 未登记，就把
   `DEV_ID -> 料号` 写进可写的 `<AppData>/data/device_ids.d/learned.json`；下次连接自动识别。
   已登记的条目**不会被覆盖**（内置表权威）。
3. **多封装家族不猜封装**：DEV_ID 只标识 die，`N32G455` 有 QFN36/48/64、LQFP48/64/80/100
   七种封装，仅凭系列名或引脚数都可能多义（如 64 脚同时有 LQFP64 与 QFN64）。现在只有
   **唯一最高分候选**才绘制引脚图，否则提示"请在工具栏选择精确料号（例：N32G455REL7）"。
   画错封装的代价是每个信号都标在错误的引脚上，比不画更糟。

### 断开连接后右侧检查器复位

断开时除了探针，界面也要回到"没有目标"的状态。新增 `parkDeviceViews(reason)` 统一处理：
检查器清空（不再持有寄存器、不保留选中字段）、位域条清空、寄存器全景清空、端口矩阵清空、
封装图与外设树回到等待提示，并重置 `m_currentPeripheral` / `m_currentRegister`。
初次加载 SVD 与断开连接走同一条路径，避免两处逻辑漂移。

### G 系列 GPIO 布局（PL_CFG / PH_CFG）

切到 G455 后端口矩阵原本是空的：`GpioCodec` 只认 CRL/CRH、MODER、PMODE 三种布局，
而 N32G4x 用的是 `GPIOx_PL_CFG` / `GPIOx_PH_CFG` —— **位布局与 STM32F1 的 CRL/CRH 完全相同**
（每脚 4 bit：MODE[1:0] 在低两位、CNF[1:0] 在高两位），只是名字不同。现在：

- `isGpioPort()` 认这对寄存器，数据寄存器兼容 `IDR/ODR` 与 `PID/POD`；
- 解码时按 kind 生成模式文案（原来只看 MODE 位，会把复用脚和模拟脚都写成"输出"）；
- `encodePinConfig()` 同时接受两套命名（否则右键改引脚模式在 G 系列上静默失效）。

断言也随之改成按实际布局测试，同一个自检现在能跑三个系列：

| 系列 | 自检结果 |
|---|---|
| N32H473CE（PMODE + UQFN48） | 171 PASS / 0 FAIL |
| N32G455（PL_CFG，家族级） | 125 PASS / 0 FAIL |
| N32G452CBL7（PL_CFG，料号级） | 131 PASS / 0 FAIL |

### 点击字段导致崩溃（已修复）

现象：连接后点右侧「寄存器检查器」里的任意字段，程序直接退出。

根因是**双向转发没有终止条件**，与寄存器内容无关：

```
点击字段 → InspectorPanel::selectField
  → emit fieldSelected
    → MainWindow::onFieldSelected
      → m_inspector->selectField(index)     ← 又回到自己
        → emit fieldSelected → ...         无限递归 → 栈溢出
```

`selectField()` 当时没有"同一个字段就返回"的保护，所以连重复点击同一行也会崩。

两处修复，互为保险：

1. **`MainWindow::onFieldSelected` 不再回灌发信者**：它只把选择同步给「寄存器全景」页下方
   的位域条，并且只在检查器的选中项确实不同时才转发；整段用 `QScopedValueRollback`
   置位重入标志，物理上断开回环。
2. **`InspectorPanel::selectField` 加幂等早退**：选中项未变时只刷新视图、**不再发信号**。

回归测试有两条（都不需要硬件）：

- `重复选择同一字段只发一次信号` / `切换字段只发一次信号` / `越界字段被忽略` ——
  模拟旧的"收到信号就再选一次"的监听者，断言它会收敛。
- `连续切换/重复选择字段不崩溃` —— 构造**真实的 `MainWindow`**，从
  `findChild<InspectorPanel*>()` 拿到检查器，逐字段切换并把每个字段连点两次，
  直接覆盖用户报的那条路径。

### MCU 状态与 HardFault 可见性

状态栏右侧常驻一个 CPU 状态指示，连上目标后按轮询间隔采样 Armv7-M 的 SCB 故障寄存器：

| 显示 | 含义 |
|---|---|
| `CPU 正常` | 没有锁存的故障标志 |
| `CPU 已暂停` | 调试器暂停了内核，且无故障 |
| `CPU 有故障标志` | CFSR 有标志但未升级为 HardFault（如除零、非对齐） |
| `CPU HardFault` | **HFSR.FORCED 或 VECTTBL 置位 —— 已进 HardFault 且未清除** |
| `CPU —` | 还没读到（未连接 / 后端不支持） |

HardFault 用红色加粗显示。点击指示打开详情对话框，列出：

- 置位的标志名（`IACCVIOL` / `PRECISERR` / `UNDEFINSTR` / `DIVBYZERO` / `FORCED` …）
- `CFSR` / `HFSR` / `DFSR` 原始值
- `MMFAR` / `BFAR` **出错地址**（仅在 `MMARVALID` / `BFARVALID` 置位时显示 ——
  不精确的总线错误没有记录地址，这时不显示而不是给个假地址）
- 「清除故障标志」按钮：向 CFSR/HFSR 写回当前值（写 1 清零），`DEBUGEVT` 位不碰

不用管寄存器读协议：这些都走普通内存读取，所以 J-Link/GDB 与 probe-rs 两个后端都支持。
判定逻辑在 `core/FaultStatus`，**纯函数**，自检里覆盖了 FORCED / VECTTBL / 仅 CFSR /
不精确总线错误 / 未读到等分支，无需硬件。

### 外设树的「实时」列：定时器计数值

要判断"这个定时器到底在不在跑"，光看时钟门控位不够（门控开着只说明它被供电）。
树里因此多了一列**实时**：

```
外设          基地址       时钟   实时
ATIM1        0x40012C00   开     12345 ↑
GTIM1        0x40010000   开     900   ·
LPTIM1       0x40007C00   关     —
```

- 值来自定时器的计数寄存器（`TIMx_CNT` / `LPTIM_CNT`；`TIMx_REPCNT` 是重复计数器，**不会**被选中）。
- `↑` = 与上次轮询相比**变了**（在跑）；`·` = 没变（被停住、或在等触发）；`—` = 没读到。
- 悬停显示寄存器名与十六进制值。
- 每轮只读**一个寄存器**（不是整窗外设），且**只读时钟确认开启的定时器**——
  门控关着的定时器读不到，也没必要去读（避免制造总线错误）。
- 判定规则统一放在 `core/PeripheralRoles`，自检可以脱离硬件断言（17 个定时器全部命中计数寄存器）。

探测寄存器不是随便挑的：优先 `*ID` > `*CFG/*CTRL/*CR` > `*MODER` > `*STS/*SR`，
并跳过 `*CLR/*ICR/*FLAG/*DAT/*RX/*TX/PBSC/POD/...` 这类写侧可能有副作用的寄存器
（本 SVD 没有 `modifiedWriteValues`/`readAction` 可依据，所以用名字规避）。
实测 N32H473CE 的 GPIOA 选中了只读的 `GPIOx_PID`。

外设树有「时钟」列与筛选器 `显示: 全部 / 仅本芯片存在 / 仅时钟已开启`，
默认停在 `仅本芯片存在`；分组标题会显示 `GPIO (8/8)` 这样的计数。读不到设备树时
（未连接/失败）自动回退为 SVD 全集，不会把列表清空。

离线可回归的断言（无需硬件）：

```bat
mcuRegDebugger.exe --selftest --svd <n32h473.svd> --device N32H473CE --render out
::  [PASS] 从 RCC 解析出时钟门控位        (68 个外设可查时钟 / 75)
::  [PASS] IOPAEN 正确映射到 GPIOA        (RCC_AHB1PCLKEN[0])
::  [PASS] USBFSEN 正确映射到 USB_FS      (RCC_APB1PCLKEN[18])
::  [PASS] 探测寄存器避开写侧影响寄存器   (GPIOx_PID)
::  [PASS] 门控开 + 读成功 -> 存在且已使能
::  [PASS] 门控开 + 读失败 -> 判定缺失
::  [PASS] 门控关 + 读失败 -> 不误判为缺失
::  [PASS] 无门控信息 + 读成功 -> 存在但时钟未知
::  [PASS] 读失败（时钟未知）仍算作存在
::  [PASS] 静态判定不存在 -> 不计入本芯片
::  [PASS] 型号能力清单已从 pdsc 载入   (12 项)
::  [PASS] SPI5 被剔除（缺 MISO，仅 100/144 脚有）
::  [PASS] SPI6 保留（本封装信号齐全）
::  [PASS] FDCAN3 被剔除（pdsc 声明只有 2 条）
::  [PASS] 可任意 IO 映射的实例保留
::  [PASS] SPI 裁剪到厂商声明数         (可用 5 / 期望 5)
::  [PASS] 无能力数据时不隐藏任何外设
::  [PASS] 无引脚数据时不隐藏任何实例
::  [PASS] 加载 data/parts.json          (680 个料号)
::  [PASS] N32H473CE Flash = 512 KB / RAM = 192 KB
::  [PASS] 订货号 N32H473CEU7 归到 N32H473CE
::  [PASS] 同 DEV_ID 的不同容量确实不同   (N32H473KC 是 256 KB)
::  [PASS] G 系列料号也有 Flash/RAM      (N32G451CBL7: 128 KB / 48 KB)
::  [PASS] 状态栏 Flash/RAM 文案         (Flash 512 KB  /  RAM 192 KB)
::  [PASS] ATIM1 选中真实计数寄存器      (TIMx_CNT)
::  [PASS] 至少一个定时器有计数寄存器    (17 / 17 个)
::  [PASS] 刷新同一寄存器的开销在预算内  (< 100 us)
::  [PASS] HFSR.FORCED -> HardFault
::  [PASS] 未读到寄存器 -> 状态未知（不谎报正常）
::  [PASS] IMPRECISERR 不算 BFAR 有效
::  [PASS] 重复选择同一字段只发一次信号   (回环保护)
::  [PASS] 连续切换/重复选择字段不崩溃   (真实窗口连接图)
::  [PASS] 断开后检查器不再持有寄存器 / 端口矩阵清空
::  [PASS] 未登记的 DEV_ID 查不到型号（不继承上一颗）
::  [PASS] 新 DEV_ID 可被记住 / 不会覆盖已登记的
::  [PASS] 家族级识别且多封装时不擅自选一个封装
::  [PASS] 多义时以下拉列表给出候选（无需手打型号）
::  [PASS] DEV_ID 覆盖界面上的旧芯片名
::  [PASS] 内存探测确实缩小了范围   (15 -> 6)
::  [PASS] 芯片下拉框第一项是「自动识别」
::  [PASS] 保存的芯片名为空即表示自动识别
::  [PASS] DEV_ID 0x511 识别为 N32G455
::  [PASS] 0x511 匹配到 N32G455 的 SVD
::  [PASS] 提示“未连接”时会话自动标记断开（停止后续轮询）
::  [PASS] 单次总线错误不误判断线
::  [PASS] 源码中 QLatin1String 不含非 ASCII（否则永远匹配不到）
::  -> out\peripheral-tree.png（树结构另有 75 行 / 缺失标记 / 时钟开关的逐项校验）
```

真机上 `--probe-info` 会打印整棵树：

```
--- 设备树（真实外设，时钟门控 + 总线可访问性） ---
  合计 75: 本芯片存在 75 / 缺失 0 / 无法访问 0 / 时钟已开 24
  外设              时钟  访问
  GPIOA              开  GPIOx_IDR = 0x0000D600
  ...
```

### 改变输出电平：写哪个寄存器、写多少

以 **PA1 = 数字推挽 + 上拉输出** 为例（`PMODE[PA1]=01`、`POTYPE[PA1]=0`、`PUPD[PA1]=01`），
电平由**输出数据**决定，与上面三个配置寄存器无关：

| 目的 | 寄存器 | 偏移 | 绝对地址 | 写入值 |
|---|---|---|---|---|
| **置高** | `GPIOA_PBSC` | `0x24` | `0x4002C024` | `0x00000002` |
| **置低** | `GPIOA_PBSC` | `0x24` | `0x4002C024` | `0x00020000` |

`PBSC`（Port Bit Set/Clear）是**原子双半字寄存器**，一次写只影响目标脚：

```
bits[15:0]  = PBS0..PBS15   写 1 → 该脚输出高
bits[31:16] = PBC0..PBC15   写 1 → 该脚输出低
```

所以 PA1 置高 = `bit1 = 1` → `0x00000002`；置低 = `bit(16+1) = bit17 = 1` → `0x00020000`。
**不要**写 `0` 去"置低"——写 0 是"无动作"。

等价写法（都可，但不推荐做读改写）：

| 写法 | 置高 | 置低 |
|---|---|---|
| `GPIOA_PBC` (0x28) | — | `0x00000002` |
| `GPIOA_POD` (0x30, R/W, 复位 `0x0000A000`) | `0x0000A002` | `0x0000A000` |

（`POD` 是纯输出数据寄存器，需读改写，多脚操作时有竞争风险，故实现里优先用 `PBSC`。）

对应到 STM32 家族：`BSRR` 同样是低半字置高 / 高半字置低；F1 另有 `BRR` 只做置低。

界面上：端口矩阵里对**输出模式**的脚右键 → `输出电平 ▸ 置高 / 置低 / 翻转`，
每次只发一个写操作并进写历史；日志会打出完整寄存器与数值，例如

```
INFO  PA1 -> 高电平: GPIOA.GPIOx_PBSC = 0x00000002
```

### 端口矩阵：未初始化引脚与右键重配

**未初始化是怎么判定的。** N32H47x 的 `PMODE` 复位值是 `0xFFFFFFFF`，即每脚都是 `11`（模拟）——
所以"固件没碰过"和"固件配成模拟"在寄存器上完全同值。判定规则因此是：
**PMODE 位 = `11` 且 `PUPD`/`POTYPE`/`AFSEL` 对应位都是复位值** → 显示 `未初始化`（灰色），
不再冒充"模拟量"。本会话通过右键显式配置过的引脚会记入白名单，仍按所选模式显示。

实测快照 `PMODE=0x028BFFFD, PUPD=0x64100002` 的解码结果：

| 引脚 | PMODE | 显示 | 颜色 |
|---|---|---|---|
| PA0 | `01` | 输出 推挽 | 橙 |
| PA1..PA8 | `11` 全复位 | **未初始化** | 灰 |
| PA9/PA11/PA12 | `10` | 复用 AF8 / AF10 | 紫 |
| PA10 | `00` + 上拉 | 输入 上拉 | 绿 |
| PA13..PA15 | `00` | 输入 | 绿 |

**右键重配。** 在端口矩阵任意引脚上右键，可切换该脚配置，写入走 GDB RSP 读改写：

```
模拟量输入
数字输入  ▸ 浮空 / 上拉 / 下拉
数字输出  ▸ 推挽 / 开漏 / 推挽+上拉 / 推挽+下拉
```

编码按端口实际布局自动选路径（`GpioCodec::encodePinConfig`）：

| 布局 | 涉及的寄存器 |
|---|---|
| N32H47x | `PMODE`（2 bit/脚）、`PUPD`（2 bit/脚）、`POTYPE`（1 bit/脚）、`AFSEL0-3`（5 bit/脚） |
| STM32F4/G4/L4 | `MODER`、`OTYPER`、`PUPDR`、`AFRL/AFRH` |
| STM32F1 | `CRL`/`CRH`（4 bit/脚：CNF+MODE） |

每次只写真正变化的寄存器，并进写历史（可撤销）。全部为离线可回归的确定性断言：

```bat
mcuRegDebugger.exe --selftest --svd <n32h473.svd> --device N32H473CE --render out
::  [PASS] 复位值端口全部显示为未初始化 (16/16 个)
::  [PASS] PA0(PMODE=01) 识别为输出 / PA9(PMODE=10) 识别为复用
::  [PASS] PA1(PMODE=11 且无上下拉/类型/AF) 识别为未初始化
::  [PASS] 已显式配置的 PA1 显示为模拟而非未初始化
::  [PASS] PMODE[PA1] 被编码为 01(输出)  /  PUPD[PA1] 被编码为 01(上拉)
::  -> out\port-matrix-PA.png（可肉眼核对颜色与文字）
```

### 变量曲线（J-Scope 风格）：从 Keil .axf 到实时波形

面板是第 4 个标签页「变量曲线 (J-Scope)」。它回答一个寄存器视图回答不了的问题：
*这个变量现在是多少，过去 10 秒怎么变的。*

#### 为什么用 .axf 而不是 .map

`.map` 只有名字 + 地址 + 大小。这三样足够**找到**一个变量，但不够**解释**它——
四字节可能是 `float`、`int32`、枚举，或 `uint16_t[2]`。画图必须知道该按哪种类型
解码，否则曲线是错的。这些信息在 DWARF 里，也就是 `.axf` 里：

| 来源 | 名字 | 地址 | 大小 | 类型 |
|---|---|---|---|---|
| `.map` | ✓ | ✓ | ✓ | ✗ |
| `.axf`（DWARF） | ✓ | ✓ | ✓ | ✓ |

所以只支持 `.axf`（任何带 DWARF 的 ELF 都行，GCC 的 `.elf` 也可以）。Keil 里需要
在 `Options → Output` 勾上 **Debug Information**；GCC 加 `-g`。

导入后会列出**所有静态分配的变量**：全局、`static`、`const`（在 `.rodata` 也照样有
地址）。局部变量不在列表里——它们在栈上，没有固定地址，任何同类工具（J-Scope 也是）
都受这个限制。

结构体会被**展开成成员**（`g_point.x`、`g_point.y`、`g_point.flags`），每个成员都有自己的
地址和类型；数组保持一条记录并带元素个数，画图时按元素分成多条曲线
（`g_adc_raw[0]`…`g_adc_raw[3]`）。

#### 数据来源：RTT，而不是轮询寄存器

RTT（SEGGER Real-Time Transfer）的模型是**目标写、上位机读**：固件往 RAM 里的环形
缓冲区写，上位机只是把已经写好的数据取走。这里没有任何"挂起内核读一次寄存器"的动作。

采集循环也不是定时器驱动的：**一次读取完成后立刻发起下一次**。所以

* 采样速率只受链路限制，不受 UI 刷新率限制；
* 窗口卡顿、被遮挡、正在做重排，都不会丢样本；
* 渲染只是读环形缓冲并重绘，**永远不触发一次读**。

上位机每次只做三件事：读 `WrOff`（4 字节）→ 读这段数据 → 回写 `RdOff`。回写是 RTT
协议要求的（不回写目标会以为缓冲满而开始丢数据），且只动**读方自己的字段**。

#### 帧协议与固件端代码

RTT 是字节流，传什么是固件决定的。为了让两端不会悄悄错位，格式由上位机定义：

```
每帧 = [0xA5][0x5A] + 各变量原始字节（小端），按变量地址升序排列，长度固定
```

勾选变量后点**「复制固件端代码」**，会得到可直接粘贴的 C 文件片段：

```c
typedef struct {
    unsigned char sync[2];
    unsigned char data[16];
} MrdScopeFrame;

void MrdScope_Send(void)
{
    MrdScopeFrame f;
    f.sync[0] = 0xA5;
    f.sync[1] = 0x5A;
    memcpy(f.data + 0, (const void *)0x08001018u, 4);   /* g_temperature : float */
    ...
    SEGGER_RTT_Write(0, &f, sizeof(f));
}
```

在定时器中断或主循环里调用 `MrdScope_Send()` 即可。因为按**地址**拷贝，结构体成员
也能发，不需要在固件里写 `g_point.x` 这种非法名字。

帧长由变量列表决定，所以**改了勾选就要重新生成代码并重编译**——这是 RTT 方案的固有
约束（J-Scope 也一样），`样本帧` 状态行会显示当前帧长。

前置条件：固件要链接 SEGGER RTT（`SEGGER_RTT.c`）并至少写过一次数据，否则 RAM 里没有
控制块。上位机优先用 `.axf` 里的 `_SEGGER_RTT` 符号定位它，没有符号文件时才扫 RAM。

#### 顺带修掉的日志噪音

`FDCAN3 判定为本型号不具备: …` 和 `静态判定剔除 2 个…` 这类提示每条都打 INFO，
连接一次能刷十几行，把唯一的连接摘要埋了。已降为 DEBUG（`--verbose` 可见），
剔除原因在设备树的 tooltip 里本来就写着。

#### 自检覆盖

夹具 `data/test/symbol_test.axf` 是真实编译器产物（`scripts/gen-symbol-fixture.py`
可重建），所以断言的是确定值而不是猜测：

```
[PASS] g_temperature = 浮点 @ 0x08001018          (4 字节)
[PASS] g_adc_raw     = 无符号 @ 0x0800101C        (2 字节 x4)
[PASS] g_point.x/y/flags 展开到 +0/+4/+8
[PASS] float 按 IEEE-754 解码（0x41CC0000 -> 25.5）
[PASS] 有符号字节正确符号扩展（0xFF -> -1）
[PASS] 跨两次读取的半帧能拼接
[PASS] 垃圾字节后能重新对齐同步头
[PASS] 环形缓冲溢出后保留最新 32 条
[PASS] 全平曲线不除零
```

## 8. GPIO 解码支持的三种寄存器布局

| 家族 | 寄存器 |
|---|---|
| STM32F1 | `CRL`/`CRH` + `IDR`/`ODR`/`BSRR`/`BRR` |
| STM32F4/G4/L4 | `MODER`/`OTYPER`/`OSPEEDR`/`PUPDR`/`IDR`/`ODR`/`AFRL`/`AFRH` |
| 国民技术 N32H47x | `PMODE`/`PUPD`/`POTYPE`/`AFSEL0-3`/`DS`/`PID`/`POD` |

布局按外设实际声明的寄存器自动判定，不需要按家族维护表。实测 N32H473CE：
`PMODE=0x028BFFFD` → PA0 输出推挽、PA1..PA8 模拟、PA9 AF8、PA10 上拉输入、PA11/PA12 AF10。

## 9. 自检与回归

```bat
:: SVD 解析 + 一致性断言 + 离屏渲染（无硬件也可跑）
build\bin\mcuRegDebugger.exe --selftest ^
  --svd "C:\Keil_v5\ARM\PACK\NSING\N32H47x_DFP\1.0.1\svd\N32H473.svd" --render out
```

断言包括：SVD 可解析、外设/寄存器/字段数量 > 0、字段位区间在宽度内、
`derivedFrom` 外设均解析出寄存器、SVD 库可索引、按器件名可匹配、渲染结果非空（>2% 非背景像素）。

## 10. 打包安装程序

```bat
scripts\build.bat
scripts\deploy.bat
packaging\pack.bat
```

产物 `dist\McuRegDebugger-0.1.0-setup.exe`（NSIS，含卸载项与开始菜单/桌面快捷方式）。

- `deploy\` 含 Qt6 运行库、`platforms\`、`data\`；
- J-Link 软件 **不打包**（安装后使用本机已装的 SEGGER 软件，避免驱动/许可问题）；
- 若已下载 `tools\probe-rs\probe-rs.exe`，`deploy.bat` 会一并带上（仅 probe-rs 后端需要）。

## 11. 已知限制

1. **J-Link 独占**：同一时刻只能有一个程序占用探针；本工具运行期间关闭 J-Flash/Keil 调试会话。
2. GDB Server 由本工具作为子进程管理；进程被强杀留下的孤儿会在**下次启动时自动回收**
   （PID 记录在 `%APPDATA%\MCUTools\McuRegDebugger\jlink-gdb.pid`，回收前校验映像名）。
3. 轮询会周期性读整个外设窗口（默认 200 ms，可在设置中调整）；目标处于低功耗模式时读取可能失败并记录日志。
4. 当前随包只带一份封装引脚图：**N32H473CEU7 UQFN48（引脚号已逐脚核实，取自数据手册表 3-1 + 第 37 页封装图）**；
   STM32F103C8 LQFP48 已归档到 `data/unsupported/packages/`。
   若数据手册的 Pin definition 与 LCEDA 符号不一致，以数据手册为准并改
   `scripts/gen-package-json.py` 的 `PINS`。
   注：48 脚 N32H473CE 的 PH0/PH1/PH6 分别是 `OSC_IN`/`OSC_OUT`/`NRST`，
   整组 PH 因此不显示为 GPIO 端口，只按功能名出现在封装图上。
5. N32H47x 的厂商 SVD 把 GPIOB..GPIOH 的 `PMODE` 只声明了 0..6 位（GPIOA 才是 0..15），
   属于 SVD 缺陷；寄存器解码按整字提取，不受影响，但位域列表只显示 7 个脚。
6. N32H47x 的 `PMODE` 枚举语义按参考手册（0 输入 / 1 输出 / 2 复用 / 3 模拟）解释；
   若实测与手册不符，改 `src/core/GpioCodec.cpp` 的 N32 分支即可。
