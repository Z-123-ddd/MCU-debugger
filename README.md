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

## 5. 界面说明

| 区域 | 作用 |
|---|---|
| 工具栏 | 后端/接口/速度/芯片名、连接与断开、暂停/运行/复位、撤销写入 |
| 左侧「外设」 | 按 `groupName` 分组；勾选 = 加入轮询集合 |
| 「封装 / 引脚」 | 芯片体 + 四边引脚，颜色=引脚类型/实时 GPIO 模式，点 GPIO 跳到端口 |
| 「端口矩阵 (IO)」 | 选中端口 PA0..PA15 的 4×4 矩阵，颜色=输入/输出/复用/模拟 |
| 「寄存器全景」 | 选中外设的**分块寄存器图**（名称/偏移/当前值/位域带/RO-RW），下方位域条 |
| 右侧「寄存器检查器」 | 位域表、枚举下拉、字段 hex 编辑与置 1/清 0/写入（读改写）、整寄存器写、重读 |
| 底部「日志」 | 解析与探针日志，同时写入 `%APPDATA%\MCUTools\McuRegDebugger\logs\` |

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

探测寄存器不是随便挑的：优先 `*ID` > `*CFG/*CTRL/*CR` > `*MODER` > `*STS/*SR`，
并跳过 `*CLR/*ICR/*FLAG/*DAT/*RX/*TX/PBSC/POD/...` 这类写侧可能有副作用的寄存器
（本 SVD 没有 `modifiedWriteValues`/`readAction` 可依据，所以用名字规避）。
实测 N32H473CE 的 GPIOA 选中了只读的 `GPIOx_PID`。

外设树新增「时钟」列与筛选器 `显示: 全部 / 仅本芯片存在 / 仅时钟已开启`；
分组标题会显示 `GPIO (8/8)` 这样的计数。读不到设备树时（未连接/失败）自动回退为
SVD 全集，不会把列表清空。

离线可回归的断言（无需硬件）：

```bat
mcuRegDebugger.exe --selftest --svd <n32h473.svd> --device N32H473CE --render out
::  [PASS] 从 RCC 解析出时钟门控位        (65 个外设可查时钟 / 75)
::  [PASS] IOPAEN 正确映射到 GPIOA        (RCC_AHB1PCLKEN[0])
::  [PASS] USBFSEN 正确映射到 USB_FS      (RCC_APB1PCLKEN[18])
::  [PASS] 探测寄存器避开写侧影响寄存器   (GPIOx_PID)
::  [PASS] 门控开 + 读成功 -> 存在且已使能
::  [PASS] 门控开 + 读失败 -> 判定缺失
::  [PASS] 门控关 + 读失败 -> 不误判为缺失
::  [PASS] 无门控信息 + 读成功 -> 存在但时钟未知
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
