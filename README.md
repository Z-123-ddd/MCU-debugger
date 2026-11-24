# MCU Register Debugger

> 许可证：**GPL v2**。程序链接了 GPL v2 的 [KFR](https://github.com/kfrlib/kfr)（频谱分析），
> 因此整体以 GPL v2 发布。详见 [LICENSE](LICENSE) 与 `third_party/kfr/LICENSE.txt`。

MCU 寄存器级 UI 调试器：通过 **J-Link** 连接调试器（保留 SEGGER 官方 USB 驱动，**无需换 WinUSB**），
读取 `DBGMCU_IDCODE` / `CPUID` 自动识别 MCU 型号，再用 **CMSIS-SVD** 描述文件图形化重建芯片的
寄存器布局与 IO 布局。寄存器视图参考 CubeMX 的配置页面，但内部以 **分块寄存器图** 展示。

- UI：Qt 6 Widgets + QGraphicsView 自绘（分块寄存器图、位域条、封装引脚图、端口矩阵）
- 描述文件：CMSIS-SVD 1.0 / 1.1 / 1.3
- 连接方式：**进程内加载 `JLink_x64.dll`**（单一后端；只有它能拿到 HSS 实时采样）

---

## 1. 连接方式：只用 J-Link DLL 一条路

**当前只有一条连接路径：进程内加载 `JLink_x64.dll`。** 没有后端下拉框，没有命令行切换，
也没有 GDB Server 后端。理由有三条，按重要性排：

1. **HSS 只有 DLL 能拿到。** 实时采样引擎不暴露在 GDB 协议上，走 GDB 就永远只能是停机快照。
2. **效率差一个数量级。** DLL 是一次进程内 C 调用（`JLINK_ReadMem`，二进制缓冲）；GDB 要
   起一个子进程、过 TCP、把请求和回复都编成 ASCII 十六进制（`$m<addr>,<len>#xx`），
   每字节线上翻倍，外加每 1 KB 一次往返。
3. **少一套要同步的代码。** 两个后端意味着每加一个能力都要写两遍、测两遍。

### 走过的弯路（已删除，留档避免重走）

| 方案 | 当年的结果 |
|---|---|
| 直接调 `JLink_x64.dll` | 最初的封装用错了符号语义（把 `JLINKARM_*` 当成返回字节数），未连接时崩溃 → 一度放弃。**后来按 SEGGER 的实际约定重写**（`JLINK_ReadMem` 成功返回 **0**，失败返回正数错误码且缓冲区填 `0xAAAAAAAA`），成为今天的唯一后端 |
| `JLink.exe`（Commander）持久会话 | 管道下 **stdout 被完全缓冲**（只有进程退出才 flush），无法交互 → 放弃；但**一次性脚本**仍用于烧录 |
| `JLinkGDBServerCL.exe` + GDB RSP | 曾经采用过（持久 TCP、无缓冲）；因上面前两条被移除，`JLinkGdbProbe` / `GdbRspClient` 已删除 |

> 32 位兼容的代价：旧版 J-Link 只提供 32 位 `JLinkARM.dll`，无法载入 64 位进程，过去靠 GDB
> 后端兜底。删掉 GDB 后端后这种机器不再支持 —— 实测 V6.0 以后都自带 `JLink_x64.dll`
> （本机 V7.96c 与 V8.26 都有），所以只影响 V5.x 时代的安装。

## 2. 目录结构

```
CMakeLists.txt               构建定义（Qt6 Widgets + Network，C++20）
src/core/                    SVD 解析、寄存器会话、J-Link DLL 后端、GPIO 解码
  JLinkGdbProbe.*            IProbe 实现：拉起 JLinkGDBServerCL 并驱动 GDB RSP
  JLinkRunner.*              JLink.exe 定位 + 一次性脚本（版本/仿真器信息/器件名校验）
  ProbeController.*          探针线程隔离（所有阻塞调用在 worker 线程）
  DeviceIdentifier.*         CPUID/IDCODE → MCU → SVD 识别
  GpioCodec.*                GPIO 端口解码（F1 / F4-G4 / N32 三种布局）
  RegisterSession.*          寄存器快照、读改写、撤销、轮询
src/ui/                      主窗口、寄存器分块图、位域条、检查器、封装图、端口矩阵、主题
src/app/                     main、命令行、离屏渲染自检
data/parts.json              器件表（SVD 路径 / 封装 / 引脚 / 外设能力）
data/packages/*.json         封装引脚图数据
data/bootloader/*.hex        内置 bootloader（勾选时自动填入）
scripts/build.bat            编译
scripts/deploy.bat           windeployqt 打包到 deploy/（含 data 与 svd 支持包）
scripts/installer.nsi        NSIS 安装包脚本
scripts/package.bat          部署 + makensis → dist\McuRegDebugger-<ver>-setup.exe
scripts/fetch-nsis.ps1       下载 NSIS 3.11 便携版到 tools\nsis（无需管理员）
src/core/FirmwareImage.*     固件起止地址识别与容量校验（纯逻辑，可自检）
src/core/ImageBlob.*         Intel HEX / ELF 解析与镜像合并（供 bootloader 合并用）
src/core/FirmwareBurner.*    经 J-Link Commander 擦除/写入/校验固件
src/ui/BurnDialog.*          烧录对话框：bootloader 选项 + 三个可编辑地址
src/core/ZipArchive.*        最小 ZIP 读取（供 .fmu 解包，不依赖 Qt 私有 API）
src/core/SimModel.*          仿真模型接口 + 按扩展名分发
src/core/FmuModel.*          FMI 2.0 Co-Simulation 后端（加载 FMU 的平台 DLL）
src/core/PythonModel.*       Python 模型后端（子进程 + 逐行 JSON 协议）
src/ui/SimulationPage.*      模型仿真页：映射、步进、实测 vs 模型出图
src/ui/ChipPickerDialog.*    系列 / 料号 / 封装 三级选择对话框（含「无 SVD」提示）
data/test/sim_lag.fmu        FMI 2.0 测试夹具（scripts/build-sim-fixture.py 可重建）
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
  --backend <jlink-dll|jlink>
                                   探针后端（默认 jlink-dll，支持 HSS 实时采样）
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
  --symbols <axf>                   打印变量清单（不需要硬件）
  --read-var <name>                 配合 --probe-info + --symbols：从目标读取该变量
  --bench-scope                     测变量面板耗时
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
| 「变量曲线」 | 导入 `.axf`；「开始实时采样」用 HSS 画实时曲线；「暂停/清空」控制会话；「频谱」切 FFT；「导出 CSV…」落盘 |
| 左侧「变量选择」 | 与「外设」同一 dock 的第二个标签：按源文件分组的变量树、过滤、勾选 |
| 右侧「寄存器检查器」 | 位域表、枚举下拉、字段 hex 编辑与置 1/清 0/写入（读改写）、整寄存器写、重读 |
| 底部「日志」 | 解析与探针日志，同时写入 `%APPDATA%\MCUTools\McuRegDebugger\logs\` |
| 状态栏右侧 | **CPU 状态**（正常 / 已暂停 / 有故障标志 / **HardFault**，可点击看详情）、当前器件的 **Flash / RAM 容量**（悬停显示 SRAM+CCM 拆分）、轮询状态、连接状态 |

左/右/下三个停靠面板的宽度与高度都可自由拖动（分隔条无锁定）。位域条**始终画满 32 位**：
面板变窄时位格按 `可用宽度/32` 缩小（小于 14px 时隐藏 0/1 数字、小于 22px 时隐藏字段名），
高度固定，永不裁切；`--selftest` 会在 96…904px 七档宽度上断言绘制区不越界。

运行控制直接走 DLL：`JLINK_Halt` / `JLINK_Go` / `JLINK_Reset`；CPU 是否停机由
`JLINK_IsHalted` 与 `DHCSR` 共同判定。读写寄存器**不会**改变 CPU 运行状态。

## 6. 型号选择：系列 → 料号 → 封装，连接时校验

**识别部分已完全移除。** 不再读 `DEV_ID`、不探测容量、不探测 GPIO 端口来推断型号。
现在只有一条路径：

```
选择 系列 → 料号 → 封装     （工具栏「选择型号…」）
        ↓  全部来自 data/parts.json
生成 设备树 + MCU 引脚图 + 容量
        ↓
点击连接 → 逐个访问设备树里的每个外设 → 校验对不对得上 → 对不上就拒绝
```

### 为什么不能自动推断

原来那套"聪明"的流程是：`DEV_ID` 得家族 → 探测 Flash/RAM 收窄料号 → 探测 GPIO 端口
收窄封装。它有一个**不可修复**的缺陷：

- 擦除态的 Flash 读出来是 `0xFFFFFFFF`
- 未映射窗口读出来也是 `0xFFFFFFFF`

两者无法区分。于是一颗 **512 KB 的 N32G455CE**、固件只占 235 KB（上半 256 KB 全空）时，
512 KB 边界读到 0xFF 就被判为"没有" → 落到 256 KB 边界 → **判成 N32G455CC**。

后果不只是显示错：**封装错 → 引脚图错**，LQFP48 与 LQFP64 的引脚完全不同，按错图接线
会烧硬件。该芯片的 SVD 里也没有容量寄存器（只有 `BLKSIZE` 之类的控制器字段），读数无法
给出容量。所以改成"问一句 + 连接时验一遍"。

### 三级选择

```
系列:  [ N32H473 ▾ ]
料号:  [ N32H473CE ▾ ]
封装:  [ LQFP48 ▾ ]
       N32H473CE · LQFP48
       Flash 512 KB · RAM 192 KB
       42 个 IO
       主频 200MHz
       [ 使用该型号 ]  [ 取消 ]
```

### 连接时的校验

#### 第一道门：内存容量对不对得上

选好型号后，连接的**第一步**是核对 RAM / Flash，因为它只读几个字，几毫秒就能否掉一个
选错的型号，不用等整个外设扫描。

| 探测到的 | 判定 |
|---|---|
| 所选范围内某点**读失败** | **拒绝** —— 目标的容量比所选型号小 |
| 所选范围内全部可读 | 通过（只是"一致"，不是"已证明相等"） |
| 所选容量本就比目标小 | 通过 —— 读数无法证明这件事 |
| 参考地址（Flash 基址）读不到 | **不判为型号错误**，标记为"无法验证" |

为什么只有"读失败"能当证据：擦除态 Flash 和未映射窗口**都读回 `0xFF`**，所以"读成功"
说明不了容量。反过来，"所选范围内读失败"是硬证据 —— 那个地址根本不存在。

Flash 会采样**三个点**（中点、3/4、末字）而不是只看最后两个字，因为某些 die 会把末尾
几个字节别名成 `0xFF`，只看边界会漏掉"上半区整块不存在"这种情况。

RAM 略有不同：上界读回 `0` 不算证据（SRAM 里合法地存 0 很常见），只记一条"无法确认"，
不据此拒绝。

拒绝时弹出具体地址，例如：

```
所选型号与目标内存不符：
· 所选 Flash 为 512 KB，但 0x08040000, 0x08060000, 0x0807fffc 读不到 —— 目标的 Flash 比所选型号小。

请确认板上型号（当前选的是 512 KB Flash / 144 KB RAM），用「选择型号…」改成实际型号后重连。
```

#### 第二道门：外设对不对得上



点击连接后不再"连上就算成功"：

1. 按料号加载 SVD、容量、引脚图；
2. **逐个访问设备树里该型号应有的每个外设**（`DeviceTreeScanner`）；
3. 用 `validatePeripherals()` 判定：

| 读结果 | 含义 | 是否算"对不上" |
|---|---|---|
| **可访问** | 寄存器应答了，块确实存在 | 不算（硬证据） |
| **对不上** | 访问失败，**但时钟门控读出来是开着的** | **算** |
| 未验证 | 访问失败，时钟关着/未知 | 不算 —— 目标运行时大多数时钟本来就关着 |

- **对不上超过 `max(3, 应有数/4)`** → **拒绝连接**：断开、卸载设备模型、弹窗列出前几个
  读不到的外设、并在日志里记 ERROR。理由要具体：时钟已开却访问失败，只能说明该型号没有
  这些外设。
- **全都没有响应** → 不判为型号错误（可能是停机/总线无响应），但明确提示"设备树无法验证"，
  而不是假装成功。

拒绝是**真断开**：留着会话会让轮询、端口矩阵和写入路径继续操作一份已被硬件否定的设备模型。

### 一个入口负责所有"按料号"的操作

`MainWindow::applyChipSelection()` 是唯一入口，因为它同时决定五件必须一致的事：

| 依赖料号/封装的东西 | 由谁产出 |
|---|---|
| SVD 设备模型（寄存器列表） | 厂商 manifest → 该料号的 SVD 文件 |
| 外设能力计数（过滤设备树） | `parts.json` 的 `capabilities` |
| Flash / RAM 数字 | `parts.json` |
| 引脚图 · 端口矩阵 | 封装引脚表（同时决定 `m_bondedBits`） |
| J-Link 器件名 | `JLinkDeviceName`（去掉 SEGGER 不认识的尾缀） |

调用方只有两个：**选择对话框**（连接中切换型号/封装）和**连接流程**。手动「打开 SVD 文件」
是诊断通道，会提示"下次连接以所选型号为准"，避免寄存器图和引脚图描述不同芯片。

### 会话里描述的两个现象

**"连的是 CEU7，识别成 CC"** —— 就是上面的容量误判。

### 命令行

```
mcuRegDebugger.exe --svd ... --device N32G455CCL7
```

`--device` 给完整料号等价于界面选择（封装取该料号的第一个）。`--probe-info` 仍保留原有的
诊断式识别，用于调试探针本身，不参与界面流程。

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
- `probeRsChip`：器件名的可读别名，仅用于匹配用户输入的型号（不是 J-Link 器件名）。
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

### 选中的寄存器走 HSS 实时读，其余走轮询

```
未选中的寄存器  → 每 200 ms 整外设扫描（原样不动）
选中的那一个    → HSS 后台采样，探针自己读，1 ms 一个样本
```

**为什么用 HSS 而不是把轮询调快**：轮询是"主机发起一次读、等它回来、再发下一次"。把选中的
寄存器提到 20 ms 意味着主线程每 20 ms 挤进一次 GDB/DLL 往返，而且每个显示值都对应一次
总线访问。HSS 反过来——采样节奏在**探针侧**，主机只是定期从缓冲区取走已经采好的样本：

| | 轮询 | HSS |
|---|---|---|
| 谁决定采样时刻 | 主机定时器 | 探针自己的时基 |
| 每个显示值 | 1 次总线读 | 摊薄：一次搬运几十个样本 |
| 目标是否被拖慢 | 是（每次读都占用总线） | 否（采样不抢执行时间） |

### 一个 HSS 会话，两个页面抢——按可见页面仲裁

HSS 在探针里是**唯一**的全局会话：`JLINK_HSS_Start` 一次只挂一张块地址表，改地址必须
`stop()`+`start()`，DLL 还硬限 8 块。而变量曲线页早就占着它了（勾选变化时 stop/start 重建）。

原代码里**没有任何互斥**：两边都 start 的话，后启动的会悄悄把块表换掉，前一个还以为自己在
采，只是永远收不到数据。

按你的要求，归属跟**可见页面**走：

| 当前标签页 | HSS 归谁 | 寄存器怎么更新 |
|---|---|---|
| 寄存器全景 | 寄存器 | HSS 1 ms |
| 变量曲线 | 曲线 | 退回 20 ms 快轮询 |
| 其它 | 空闲 | 退回 20 ms 快轮询 |

切到寄存器页时：若曲线在采，先让它 `releaseHss()`（走和「暂停」同一条停止路径，已画数据保留），
再启寄存器会话。切走时寄存器会话停掉，把会话还回去。

判定抽成纯函数 `MainWindow::shouldUseRegisterHss()`，自检直接断言真值表——这种仲裁一旦写错，
现象是"值不动"或"总线被读两遍"，都很难从界面上看出来。

### HSS 不可用时自动退回快速轮询

HSS 有三个前提，缺一个就走上一轮实现的 20 ms 单寄存器轮询，并在状态栏说明：

| 条件 | 不满足时 |
|---|---|
| 后端是 J-Link DLL | 已是唯一后端，此项恒成立 |
| DLL 导出 `JLINK_HSS_*` | 旧 DLL 没有（`hasHss()` 为假）|
| CPU 运行中 | 暂停后寄存器全冻结，采一样也没意义 |

状态栏会显示当前实际用的是哪一种：`轮询: 1 个外设 / 200 ms（选中寄存器 HSS 1 ms）`
或 `…（选中寄存器 20 ms）`。

### 左键选中、右键取消

| 操作 | 结果 |
|---|---|
| **左键**点寄存器 | 选中：HSS 1 ms 实时读，检查器/位域条跟着变 |
| **右键**（任意位置，含块间空白） | 取消选中：该寄存器回到 200 ms 轮询 |

取消选中后 `m_currentRegister = -1`，两个加速通道**自动**停掉——`shouldUseRegisterHss()`
和 `shouldFastPoll()` 都以"有选中"为前提，所以 HSS 会话被交还（曲线页可以拿走）、
单寄存器定时器停摆、检查器与位域条清空，而**寄存器本身仍在 200 ms 整外设扫描里更新**，
只是回到共享速率。

> 原先右键和左键走同一个 `clicked()` 路径，所以右键是**选中**——正好和"取消"手势相反。
> 现在右键在 `RegisterBlockView::mousePressEvent()` 里拦下，左键才转发给场景；这样点在
> 块与块的空白处也能取消，不必精确命中某个矩形。

### 两个异步竞态（都修了）

**1. `start()` 是异步的。** 它返回成功只代表"请求已下发"，探针确认前 `m_running` 仍是 false。
快速切换寄存器时，第二次仲裁会看到"没在跑"于是再 start 一次，被 `HssSampler` 以"采样已在
进行中"拒掉，然后退回轮询——而 HSS 其实随后就起来了，于是**两边同时在跑**。
修法：新增 `HssSampler::isStarting()`，"运行中或启动中"都算会话已被占用。

**2. 异步拒绝会变成无限重试。** 探针可能在 start() 已返回成功之后才回 `started(false, ...)`。
失败回调里再跑一次仲裁，就又申请一次、又被拒一次，转成定时器速度的循环。
修法：`m_registerHssRefused` 记住这次拒绝，回到轮询；下一次**人为动作**（换寄存器、换外设、
重新连接）才清掉重试。

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

- 日志文件 / JLink.exe 的**完整安装路径**（现在只报版本）
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

#### 变量清单的完整性：三类漏收集 + 一类噪音

面板要回答的是「固件里有哪些静态变量 / 数组 / 对象」，所以漏一个都算错。实测对
BX8115.axf 做了一次对照（DWARF 的真实清单 vs 面板里的树），修掉四个问题：

| 问题 | 后果 | 修法 |
|---|---|---|
| `DW_OP_addr: 0` 被当成"没地址"丢弃 | `Weights_128`（`float32_t[256]`）、`adc_full_scale`、`SaveState` 整类消失 | 保留并标记「不可读」 |
| 函数局部变量被当成全局收集 | `count_fwd`/`error_fwd` 出现在顶层；3536 个因无地址被丢弃 | 按父链判定作用域，局部只挂到函数下 |
| 静态类成员被当成实例成员 | `Encoder::ports_to_sample` 显示在实例基址 `0x20009CD0` | 识别 `DW_AT_declaration`，按符号名独立寻址 |
| 芯片包文件混在树里 | CMSIS 查找表、libc++ 内部量淹没项目变量 | 按 line table 的**目录**分类，默认隐藏 |

**`DW_OP_addr: 0`** 是 armclang 对 link-time 解析符号的写法——地址由链接器填，DWARF
里写 0。原来的 `hasAddress = (value != 0)` 把它当失败，再查 `.symtab` 也查不到（那些
变量确实没进镜像，或被优化掉了）就默默删掉。现在保留、标「无地址」，不可勾选。

**作用域**靠父链判定：往上走，遇到 `DW_TAG_subprogram` / `DW_TAG_lexical_block` 就是
局部，遇到 `DW_TAG_namespace` 仍是全局。之前没有这个判断，所有 depth 的 `DW_TAG_variable`
都变成根。

**静态成员**：`DW_TAG_member` 带 `DW_AT_declaration: 1`、没有 `DW_AT_data_member_location`
的就是静态成员。它有独立存储（`ports_to_sample` 在 `0x200002F0`，12 字节 = 3 个指针），
不该按 `实例+偏移` 算。armclang 不给它 `DW_AT_linkage_name`，所以按 Itanium 形状
（`...7Encoder15ports_to_sampleE`）去 `.symtab` 里找。找到后它是**可勾选的真实变量**。

**芯片包判定**看 line table 的目录字段（之前只保留文件名，把目录丢了）：

```
../MotorControl                                    ← 项目
C:/Keil_v5/ARM/ARMCLANG/Bin/../include/libcxx      ← 编译器
../ThirdParty/CMSIS/core                           ← 芯片包
```

含 `armclang` / `armcc` / `keil_v5` / `libcxx` / `cmsis` / `thirdparty` / `arm-none-eabi`
等标记的目录算芯片包。面板上有「含芯片包」勾选框，默认关闭。

**最终对照结果**：DWARF 里 746 个文件作用域变量，树里 745 个。差的 1 个是命名空间前缀
差异（`fibre::embedded_json` vs `embedded_json`）。另有 17 个**没有出现在树里也不需要
出现**——它们是 `DW_AT_const_value`（`constexpr` / `PROTOCOL_VERSION` 这类），编译期
常量，没有 RAM 存储，永远不变，列进变量树只会是噪音。

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
叠成标签页，并加进了「视图」菜单可以随时开关。变量列表一展开就有上千行，放在主标签页
里会把别的视图挤没。首次成功导入 .axf 时自动切到该标签。

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

不用管寄存器读协议：这些都走普通内存读取，两个后端（DLL / GDB）都支持。
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

### 变量曲线：只有一个来源

第 4 个标签页只做一件事：把勾选的变量画成随时间变化的曲线。

| 出口 | 数据来源 |
|---|---|
| **开始实时采样** → 实时曲线 / 频谱 | HSS 后台采样，目标不停机 |

**「读取快照」已删除**（连同它后面那两个微调框和整套 snapshot 采集逻辑），**变量表的
数值列也已删除**。原因：这两样都在"读一次内存然后把数字摆出来"，而数字本身在寄存器视图
和左侧变量树（地址/大小/层级）里已经能定位；留下的 HSS 才是这个页面独有的能力——
按探针硬件时基连续采样，且不停机。

变量树的列因此回到 4 列：**变量 / 类型 / 地址 / 大小**。

#### 地址必须以链接符号表为准（已修，26% → 1.2%）

做上面那个实测时发现的：`xTickCount` 解析出的地址是 `0x20002230`，而 ELF 符号表里是
`0x20002248`。顺着查下去，`pos_actual` / `torque_actual` / `vel_actual` 三个变量拿到了
**同一个地址**——读 `pos_actual` 实际读到的是别人的内存。

原因：armclang 把同一编译单元里的多个静态变量塞进一个 `.L_MergedGlobals` 块，DWARF 里
把它们都描述成**块基址**，每个变量自己的偏移只在链接符号表里。原来的代码只在"DWARF 完全
没给地址"时才查符号表，于是这种"给了个错地址"的情况被无条件信任了。

修法：DWARF 有地址时也做一次交叉核对，**冲突时以符号表为准**（链接器是权威），并连带
把整棵子树的偏移一起修正。为避免同名变量误伤，还要求符号大小与变量一致才接受覆盖。

用 ELF 符号表逐条核对 586 个变量：

| | 地址错的数量 |
|---|---|
| 修前 | 154（26.3%） |
| 修后 | 7（1.2%） |

剩下 7 个是跨编译单元的同名变量（CANopen 对象字典那类），本质歧义，不影响结构体内成员
（`error_table` 这类是"根地址 + 成员偏移"算出来的，根地址对了它们就都对）。

`--symbols` 现在会把校正数量打出来，便于确认：

```
地址校正: 156 个变量的 DWARF 地址与符号表不一致，已按符号表修正
```

注意这**不只是显示问题**：勾选 `pos_actual` 去看曲线，读的是 `torque_actual` 的内存，
得到的曲线看起来合理却是错的——这类错误比崩溃更难发现。

#### 为什么不读所有变量（这条结论仍成立，只是不再有值列消费它）

一轮"把所有变量都读一遍"在真实固件上是 **700 次请求 / 692 ms**（3777 个可采样叶子，
相邻地址已经合并过）。瓶颈是每次请求的协议往返（约 0.22 ms），不是字节数——200 ms
只够约 900 次请求。这就是为什么本程序不提供"全量变量实时刷新"：不是没实现，是做不到。

读取本身是**非侵入**的（实测 `DHCSR.C_HALT` 全程为 0，目标的 `DWT_CYCCNT` 持续递增），
所以寄存器轮询是安全的；这条结论仍然支撑着寄存器视图的 200 ms 轮询。

#### 采样越久越挤、满buff后变空白：时间轴锚错了

现象：采样时间一长，全部数据挤在一起、看不到细节；环形缓冲填满后，图表直接空白。

根因是 `visibleWindow()` 把**时长**当成了**绝对时间**：

```cpp
const double total = (lastTime() - firstTime()) / 1000.0;  // 时长，不是时刻
const double end = span - m_xOffsetSec;                    // 右边缘锚在 0 秒
```

于是窗口永远是 `[0, 总时长]`，两个阶段各自出问题：

| 阶段 | 发生了什么 |
|---|---|
| 数据还在累积 | 总时长一直变大，窗口跟着撑开 → 全部数据压缩进同一屏（"挤占初始数据"） |
| 缓冲填满（10000 点） | `firstTime()` 开始前移，总时长稳定在 ~10 s，但样本的绝对时间已到 100 s+ → 窗口 `[0,10]` 落在过去，**一个点都画不出来** |

修法：右边缘锚定**最新样本的绝对时间**，并且给实时模式一个**固定可见窗口**（默认 2 秒）：

```
toSec   = lastTime（最新样本）+ 拖动偏移
fromSec = toSec - 窗口 / X 缩放
```

这样图表随时间**行进**：新数据从右边进来，旧数据从左边滚出，缓冲区满了也不影响——
只是滚出去的不再回来。

界面上的对应控件：

| 控件 | 作用 |
|---|---|
| **窗口(s)** | 可见时间窗口，默认 2 秒。整份数据一起显示会把细节压没 |
| **Y 拉伸** | 纵向（幅度）倍数，把小幅波动拉开看。1.0 = 自适应（按可见数据算） |
| 滚轮 / **Shift+滚轮** / 右键 | 纵向缩放 / 横向缩放 / 恢复整段适配 |
| 拖动 | 往右拖进入过去，同时**停止跟随**（否则视图会从光标下溜走） |

自检用 900 点数据 + 200 点缓冲模拟"采样很久、已经回绕"的场景，断言三件事：缓冲回绕后
**仍画出曲线**（计数像素，不是只看控件存在）、时间戳是绝对时间（139.9 s）、重置视图
恢复跟随与 1 倍拉伸。

#### 实时曲线上的"大尖刺"：三个真实缺陷

用户在稳定信号上看到偶发的巨大尖刺。查下来不是信号问题，而是三个叠加的读取缺陷，
其中一个是内存越界写——它同时也造成了退出时的崩溃。

**1. DLL 无视传入的缓冲区大小**

`JLINK_HSS_Read(buffer, size)` 的 `size` 不是"最多写这么多"，而是被忽略：它把上次读取
之后积压的**全部**记录写进缓冲区。实测一次 700 ms 的间隔产生 5552 条记录 = 44 KB，
而当时只给了 32 KB —— 越界部分落在堆上，把相邻内存改成了垃圾值。

```
缓冲区清零  : got=5552 → 需要 44416 字节
缓冲区填 0xAA: got=2464 → 需要 19712 字节
当时给的大小 : 32768 字节
```

被改坏的那块内存随后被当作样本读出，就是那个"偶发的大尖刺"。修法：缓冲区按积压量
分配（`max(1 MiB, 记录宽度 × 32768)`），并把完整大小传给 DLL。

**2. 时间戳 0 被误判成"填充"**

HSS 启动后**第一条记录的时间戳就是 0**（它是相对于启动时刻的）。我原来的校验写成
"timestamp == 0 即结束"，于是每次读取都在第一条就中断，一个有有效样本都没采到。
正确的判据是**严格递增**：只有"没有变大"才表示到了零填充的尾部。

**3. 最后一条记录可能正在被写入**

探针可以在数据写入之前先更新记录的时间戳，读到那一刻就得到一个混合值——单条尖刺的
另一种来源。每次读取丢弃最新一条记录。

顺带修的第四个问题：解码时按**地址**回查变量。本项目里 armclang 的合并块会让不同变量
共享地址（26% 的变量曾经地址相同），按地址查可能拿到别的变量的类型——把 `int` 当
`float` 解就是 1e-40 这种量级的假值。现在每个通道直接持有自己的 `WatchVariable`。

修复后连续三次实测：

```
run1 共 697 个样本，实测间隔 1001.6 µs，FAIL=0
run2 共 693 个样本，实测间隔 1001.6 µs，FAIL=0
run3 共 695 个样本，实测间隔 1001.6 µs，FAIL=0
```

700 ms @ 1 kHz 期望 700 条，全部命中；量程最小值不再出现 0（之前垃圾值会把它拉到 0）。

#### Y 轴自动缩放：忽略极值，但如果忽略了要告诉你

原来的自动量程取可见数据的**绝对 min/max**。只要有一个坏点，整条曲线就被压成底部
的一条直线——这正是"没法和分辨率细节对上"的原因。

现在取 0.5% / 99.5% 分位数作为量程（样本数 ≥ 40 时生效），单个尖刺不再左右坐标轴。
两个约束保证不欺骗用户：

- **原始数据不动**：只是不画在视窗内，`SampleHistory` 里仍然是完整值；
- **裁掉了多少要显示**：状态行会写 `Y 轴已忽略 N 个极值点`，与别的工具对照时能知道
  极值确实存在。

自检用 1000 点稳定数据 + 1 个 1e6 的尖刺验证：极值被排除在量程外，同时原始最大值
仍然是 1e6（没被丢弃）。

### 变量树：4 列，数组不可绘图

变量列表回答"这份固件里有什么、在哪个地址"。它**不显示数值**——那一列已经删除，数值的
定位靠地址与类型，取值靠勾选后看曲线。

| 列 | 内容 |
|---|---|
| 变量 | 源码里的名字（结构体成员按层级展开） |
| 类型 | `float` / `uint16_t[4]` / `Point` |
| 地址 | RAM/Flash 绝对地址，或聚合行的 `+offset` |
| 大小 | 字节数 |

#### 读取不会停 MCU——这是实测的

这条结论是寄存器轮询安全的基础，用真板子量过（J-Link V9，SWD 50 MHz）：

```
DHCSR.C_HALT: 轮询前 0，轮询后 0（0=运行）
单次小请求: 0.220 ms
```

**`DHCSR.C_HALT` 全程为 0**，目标的 `DWT_CYCCNT` 在 1 秒内涨了一亿多，说明读内存走的是
调试端口的后台访问，内核一直在跑。

注意这和 GDB RSP 后端完全不同：那条路径的读是**停机快照**语义，实测读一次目标就停一次。

#### 为什么不提供"所有变量实时刷新"

真实固件（3777 个可采样叶子 / 117 KB）实测：

| 读法 | 请求数 | 耗时 |
|---|---|---|
| 逐变量读 | 3777 | 1740 ms |
| 合并连续区段读 | 700 | **692 ms** |

单次小请求 0.220 ms，200 ms 只能买 **~900 次请求**。合并后仍要 700 次，差距约 3.5 倍。
所以"勾一个就全量刷新"这条路在这套硬件上是走不通的，本程序不提供它——能力上限如此，
不是没做。

#### 数组：一行，不可勾选，不展开

`float error_accum[256]` 这类标量数组是一行：

```
error_accum        float[256]       0x20000100      1024 B
```

- **没有复选框**：复选框承诺一条曲线，而 256 条曲线既画不进图表，也会瞬间占满 HSS 的
  8 个变量名额；
- **不展开**：标量数组在 DWARF 里没有元素节点，而展开出来的 `[i]` 行在删掉数值列之后
  没有任何内容可显示；
- **搜索结果里的数组同样不可勾选**（这一条曾经是漏的，自检抓出来了：扁平结果行无条件
  加了复选框）。

**结构体数组**是另一回事：`Point[2]` 的成员来自 DWARF，仍然可以展开成
`[0].x` / `[0].y` 这样的标量行，这些叶子照常可以勾选画曲线。

#### .axf 被重新编译后自动重载

Keil 每次编译都会重写 `.axf`。不重载的话，图表和变量列表用的还是**旧地址**——界面看起来
正常，读的却是别的东西，这是最难发现的一类错误。

现在监听文件**和它所在目录**：链接器通常是"写临时文件 + 改名替换"，纯文件监听会失效。
事件有 400 ms 防抖（一次编译会触发多次），解析失败会自动重试两次（文件可能正在写）。

重载后：

1. 按**变量名**重新解析地址，勾选的变量继续有效；
2. 地址变了的、以及新文件里消失的，都在状态栏报出来；
3. 正在跑的实时采样会**用新地址重建会话**（旧曲线的地址已经错了，缓冲区一并清空）；
4. 变量树重新按新地址构建。

```
已自动重新导入 firmware.axf（3777 个变量，3777 → 3781），勾选的 4 个变量中 1 个地址已变化
```

#### 实时曲线的暂停、清空与导出

按钮行下方那行实时读数（`9597 样本 | vel=… [min..max] avg … | Y 轴已忽略 40 个极值点`）已经去掉：它每秒重画一次、挤占图表高度，而数值本身鼠标悬停就能看到。样本数与「Y 轴已忽略极值点」的提示改到**状态栏**，最多每秒报一次，不再占图表空间。

四个新行为：

**拖动即冻结时间轴。** 采样进行中，曲线原本每帧都把右沿重新锚到最新样本，于是拖动
设置的偏移量立刻被抵消，轨迹仍在鼠标底下滑动。现在按下鼠标时记住当时右沿位置，
指针移动超过 3 px 才真正接管坐标轴，之后新样本不再推动视图。只点一下（用十字线）
不会冻结。想回到实时跟随：双击／点「跟随最新」。

**暂停**。停掉采样定时器和 HSS 会话，**已画出的数据与当前缩放全部保留**。继续时
新会话的时间戳从 0 重新开始，所以恢复时会带上一个偏移量（`HssSampler::start` 的
`timeOffsetUs`），接在原时间轴末尾之后——否则续采的样本会落到已有曲线的**左边**，
看起来像曲线折返。

**清空**。丢弃已采集数据，图表回到空白。若正在采样，会用偏移 0 重开会话，让新数据
从 t=0 开始。

**导出 CSV**。列是：

```
index,timestamp_us,timestamp_ms,<变量名>...
0,1000,1.000,1.5,2.25
1,2500,2.500,3,
```

- 三个时间列：原始索引、微秒（存储精度）、毫秒（好看）。索引能反推采样率。
- 变量名含逗号或引号会自动加引号转义。
- 采样缺口写成**空单元格**，不是 0，否则在 Excel 里画出来的图和屏幕上看到的不一致。
- 文件带 UTF-8 BOM，Excel 直接双击不会把中文变量名显示成乱码。

#### `OutputPort` 为什么勾不上：它不是数值

实测一份真实固件（BX8115.axf）：**19 个 `OutputPort` 实例，19 个都不可勾选**。这不是
缺陷，是设计：复选框的含义是"勾了能得到一条曲线"，而曲线是一串数字，结构体不是数字。

值在里面：

```
pos_estimate_  OutputPort<float>      0x2000A104  8 B      ← 不可勾选（聚合）
  age_         uint32_t               0x2000A104  4 B      ← 时间戳
  content_     float                  0x2000A108  4 B      ← 数值在这里，可勾选
```

`OutputPort<std::pair<float,float>>` 再深一层：

```
Vdq_setpoint_  OutputPort<pair<float,float>>  0x2000A690  12 B
  age_         uint32_t                       0x2000A690  4 B
  content_     pair<float,float>              0x2000A694  8 B
    first      float                          0x2000A694  4 B  ← 可勾选
    second     float                          0x2000A698  4 B  ← 可勾选
```

**所以“不可用于实时测量”是误判**：`content_` 完全支持（实测 `elementSize=4`、
`samplable=1`、`usable=1`）。真正的问题是**找不到它**，为此修了两处：

1. **搜索结果行名必须可分辨**。原来只显示最后一段，于是 34 个 `content_` 全叫
   `content_`、`g_points[0].x` 和 `g_points[1].x` 都叫 `x`。现在取最后**两段**
   （`pos_estimate_.content_`、`g_points[1].x`），并且**检测重名**——仍有冲突的回退到
   完整路径。这是自检抓出来的：第一版只对几个"通用成员名"做了特判，`x`/`y` 漏了。
2. **聚合行的 tooltip 直接指出值在哪**：有 `content_` 子节点就写"数值在 content_ 里：
   展开后勾选它"，有 `first`/`second` 就写"数值是 pair：展开后勾选 first / second"。

**另一个真实限制**：实时采样最多 **8 个变量**（DLL 硬限）。勾选超过 8 个时后续被忽略，
状态行会提示"超出 8 个的部分已忽略"。

#### 取消勾选后曲线还在：勾选没有驱动采集

现象：取消一个参数，曲线仍在；换一个参数，图表不变。而且——**图表本身支持多参数，问题
是勾选变化没有传到采样端**。

根因在 HSS 的工作方式：块的地址表在 `start()` 时一次性下发给探针，之后**固定不变**。
而勾选处理器只更新了状态文字，代码里甚至写着"选中变化不需要重启"，这是错的。

修法：勾选停稳后（150 ms 防抖，避免勾一个大结构体时逐行重启）判断是否正在采样，
是则重建会话：

```cpp
m_sampler->stop();                       // HSS_Stop：PC 侧操作，不停目标
m_history->clear();                      // 通道变了，旧样本不再有意义
m_history->setTraceNames(names);         // 新的通道集合
m_sampler->start(chosen, period);        // HSS_Start：新的块表
```

**为什么必须清空缓冲**：`setTraceNames()` 本来就会清空（不同通道集合下旧样本对不上），
这是对的——留着会把上一个参数的曲线画在新名字下。

**全部取消时的行为**：停止采样，而不是继续拉一份过期的集合。

#### 多参数能力与上限

| 项 | 值 |
|---|---|
| 同时实时采样的变量数 | **最多 8 个**（DLL 硬限，`JLINK_HSS_Start` 的块数） |
| 超出时 | 只取前 8 个，并在状态行提示「超出 8 个的部分已忽略」 |
| 数组 | 只采第 0 个元素（一个块 = 一个地址）|
| 单个数组的内容 | 曲线只能看第 0 个元素；数组不再提供逐元素读数 |

8 个是探针/固件侧的限制，不是本程序的取舍：J-Link Commander 的 `HSSStart` 也明写
"can only handle up to 8 blocks"。要看更多变量只能分批勾选，或换成 J-Scope 的
RTT 模式（上限 12）。

验证（无需硬件，检查可观察的契约）：

```
[PASS] HSS 会话最多 8 个变量   (8)
[PASS] 两个通道各 10 个样本   (10 个样本 / 2 通道)
[PASS] 换通道后只保留新通道   (1 个)
[PASS] 换通道会清空旧样本（避免画出上一个参数的曲线）   (0 个样本)
[PASS] 新通道能画出曲线   (30 个顶点)
```

#### 频谱分析改用 KFR

手写的 radix-2 FFT 换成 [KFR](https://github.com/kfrlib/kfr)。理由不是"手写的不能用"
——它验证过 125 Hz 命中、-6 dB 幅度、直流无泄漏——而是**这类数值代码不该由本项目管理
一辈子**：KFR 有 SIMD 内核、缓存过的 plan、经过验证的窗函数和归一化。

接口刻意保留 `QVector<double>` / Hz / dB，调用方不接触 KFR 类型：

```cpp
QVector<double> Fft::magnitudeSpectrum(const QVector<double> &input);   // dB, 长度 N/2
```

**结果与旧实现逐位一致**（同一个 125 Hz 正弦）：

| | 手写 FFT | KFR |
|---|---|---|
| 峰值频点 | 125.0 Hz | **125.0 Hz** |
| 幅度 | -6.0 dB | **-6.03 dB** |
| 直流主瓣外 | -64 dB | **-64 dB** |

集成方式：`third_party/kfr/` 只保留 `include/` 与 `src/dft/`（audio/io/dsp/capi 删掉，
省 1.3 MB）。KFR 需要编译，不是纯头文件——CMake 里显式列出 10 个 `dft/*.cpp`。

**许可证变了**：KFR 是 **GPL v2**，链接它使整个程序成为 GPL v2。本项目的 `LICENSE`
已从 MIT 换成 GPL v2，并在开头说明了这一变更的原因与时间点。若将来要闭源分发，需要
向 KFR 作者购买商业授权（代码无需改动）。

#### 频谱缩放的三个 bug

"手动缩放时频谱不太正常"，查下来是三处：

1. **跨越距离用错了单位**：`bridgeDistanceX()` 从 `visibleWindow()` 取跨度——那是
   **秒**，而频谱的 X 轴是 **Hz**。把这个数当频率阈值用，曲线被切成一个个点。
   改为显式传入跨度，时间轴传秒、频谱传 Hz。
2. **频谱完全不响应滚轮**：`buildSpectrumSeries` 永远画 0..Nyquist，`m_xZoom` 根本没被读。
   现在 X 缩放以 0 Hz 为锚点（频谱的有用信息都在低频），平移按可见跨度比例。
3. **自检报的范围是假的**：`prepareForTest` 在频谱分支里硬编码返回 `0..Nyquist`，
   于是"曲线窄了但报告的范围没变"，测试永远是绿的。改为与 `paintGL` 共用同一套计算。

验证：

```
全览: 0..500 Hz，512 个顶点
放大后: 0..400 Hz，411 个顶点
[PASS] Ctrl+滚轮放大后频率范围收窄   (上界 400 Hz)
[PASS] 放大后提交的 bin 数减少（只画可见频段）   (512 -> 411)
[PASS] 频率轴从 0 Hz 开始（不出现负频率）   (0.0 Hz)
```

#### 删掉的东西

- **数据表格页**：与曲线重复，且每次快照都要重建整个 QTableWidget。现在只有图表。
- **窗口(s) / Y 拉伸 两个输入框**：用滚轮就够了，两个 spinbox 占地方且与滚轮抢控制权。
- **死代码**：`ScopePage::currentSelection`、`selectAllForTest`、`snapshotForTest`、
  `GlPlotWidget::hoverSummary`、`gpuPointCount`、`lastPrepareMs`、`liveWindow`、
  `setTraceVisible`。每个都用 grep 确认过零调用；顺带省掉每帧一次 `QElapsedTimer`
  和一个 `QStringList::join`。

#### 更新滞后、线条断点、起始位置：三个独立问题

**1. 数据变了图表要等一会儿才动**

原来是"一个 33 ms 定时器 tick 只取一次数据，取不到就算了"。一次渲染慢（或 tick 被跳过）
就会积压，图表于是显示几秒前的数据。改成**取到空为止**：一次 drain 若把读取上限填满，
说明探针里还有没读走的记录，立刻再取（上限 64 轮，防止探针卡住时死循环）。定时器缩到
20 ms，只负责重绘节奏。

**2. 线条之间的断点**

两个来源：

- `if (delta == 0) continue;` —— 时间戳相同的记录被当成"撕裂读"丢弃。同一微秒内的两个
  样本是正常数据，丢掉就在曲线上打洞。现在保留。
- 跨越阈值原来按"中位采样间隔 × 4"算，一次调度抖动就会超过它而断开。改成按**像素**：
  3 像素以内的空洞直接连线——屏幕上根本看不出来，断线才是误导。

**3. 起始位置与滚动**

原来窗口钉在最新样本上，所以刚开始采样时曲线出现在**最右边缘**，左边全空。现在：

```
数据未填满窗口 -> 左边从 0 开始，向右生长
填满之后       -> 跟随最新样本滚动
```

X 轴刻度**不随数据量自动变化**，只由用户的缩放决定——自动适配会不停和滚轮打架。

**4. 线条加粗到 2 像素**

1 px 在高 DPI 屏上是一根发丝，读起来像噪声。2 px 仍然硬边（不抗锯齿），但明显是曲线。

#### 顺带修掉一个堆越界（自检崩溃的真正原因）

改这几项时自检开始段错误。gdb 给出的是堆检查报告，不是崩溃点：

```
Heap block at ... modified at ... past requested size of 44
#10 mrd::GlPlotWidget::buildTimeSeries (...) at GlPlotWidget.cpp:616
```

`buildRuns` 最坏情况**每个点自成一段**（数据稀疏、每步都超过跨越阈值），而 run 数组只分配了
`count/2 + 2`。稀疏数据下直接写越界 40 字节。修法：按 `count + 1` 分配，并让 `buildRuns`
在达到上限时返回而不是继续写。

#### "只有右侧一小段有曲线"：时间戳被重复累加

现象：曲线只出现在最右边约 25 像素里，其余全是黑的（对截图统计：黑色绘图区
1165 列中只有 **6 列**有轨迹像素，全部在 x=1125..1174）。

根因在跨 drain 的时间戳解绕。原来是：

```cpp
qint64 base = m_unwrappedUs;      // 上一轮的【最后一个时间戳】
timestampUs = base + raw;         // 又加了一次 raw
```

`raw` 本身就已经从那一点继续了，于是每轮把上一轮的总量再叠一遍——**时间轴二次增长**。
采一分钟之后，"最近 2 秒"的窗口里只剩最新一轮的尾巴，画出来就是右边缘那几列。

改成真正的解绕状态：`m_wrapBaseUs`（累加多少倍的 2³²）+ `m_lastRawTs`（上一条原始
时间戳，用来识别**跨 drain 发生**的回绕）：

```cpp
timestamp = wrapBase + raw;       // raw 不再被加两次
```

回绕与零填充的区分也写清了：**回退接近整个量程**（`prev - raw > 2³¹`）是 32 位计数器
回绕；**小幅回退**是 DLL 缓冲区的零填充尾部，到此为止。

**验证用的是"时间轴必须线性"这个不变量**，而不是"看着像连续"：

```
[PASS] 跨 drain 的时间步长恒定（不重复累加）   (0 步异常，最大偏差 0 µs)
[PASS] 总跨度等于样本数 × 采样周期             (999000 µs)
[PASS] 32 位时间戳回绕后继续推进（不归零）      (4294966796 -> 4294967796)
[PASS] 回绕处步长仍然正确                      (1000 µs)
[PASS] 零填充尾部被截断                        (3 个有效)
    窗口 [0.499, 0.999] s，提交顶点 502
[PASS] 窗口内顶点数与采样率匹配（曲线铺满窗口）   (502 个)
```

最后一条是端到端：500 ms 窗口 × 1 kHz = 500 点，实测 502（两端各多取一个点接边）。
旧代码在这个测试里只有个位数顶点。

#### 竖向条状曲线：时间轴精度不够

现象：实时曲线看起来是一根根竖线，不是连续的折线。

根因不在绘制，在**时间基准的精度**。`SampleHistory` 原来按**毫秒整数**存时间，而 1 kHz
采样下相邻样本正好差 1 ms —— 任何抖动都会让多个样本截断到同一个毫秒值：

```
样本 A: t=1000 µs -> 存 1 ms
样本 B: t=1999 µs -> 存 1 ms   ← 同一 X，不同 Y
```

同一 X 上两点不同 Y，GL 画出来就是一条竖线。修复：整条链路改用**微秒**。

```cpp
history->append(timestampUs, row);   // 不再 /1000
```

#### 绘图跟不上采样：只提交可见窗口

另一个真实浪费：原来每帧都把**整个缓冲**（10000 点 × 8 通道 = 8 万顶点）重建并上传，
而 900 px 的窗口只能显示其中几百个 —— GPU 随后把它们全部裁掉。

改成只生成可见范围内的顶点（两端各多取一个点保证接边）：

| | 改前 | 改后 |
|---|---|---|
| 顶点/帧 | 80000 | **8016** |
| CPU 准备耗时 | 6.01 ms | **0.61 ms** |

这才是"让绘图速度与采样速率匹配"的正解：**绘制成本与采样量脱钩**，只与屏幕宽度有关。
缓冲仍在，采样仍是 1 kHz，但画一帧不再随历史长度增长。

#### 日志只留结论

启动时那几行（`SVD 库: 130 个文件`、`料号表: 680 个型号`、`已加载 J-Link DLL`、
`J-Link DLL 会话已建立`）描述的是内部记账，会淹没真正有用的那一行
`已连接: 器件 N32G455CC ...`。全部降为 DEBUG，`--verbose` 仍可看到。

#### 曲线断续、锯齿：两个把好数据丢掉的地方

现象：实时曲线上有断口，看着一段一段，还带竖直跳变。

对截图做了量化（逐列统计轨迹像素）后结论很明确：**90% 的相邻列跳变只有 1 像素**
——曲线本身是平滑的；问题是有 12 段列空隙、最长 45 列，也就是约 128 ms 的数据直接
缺了。所以不是"画得抖"，是"数据被扔了"。

**1. 采样端把整轮缓冲丢掉**

```cpp
const qint64 maxGapUs = qMax<qint64>(1000, qint64(m_periodUs) * 100);  // ✗
```

1 kHz 采样下这是 100 ms 的上限。只要 UI 卡一下、USB 停一下，记录间的时间戳就会超过它，
于是 `break` —— **这一轮剩下的记录全部丢弃**。断流一次就丢一大片，正好对应 45 列的空隙。
判据的本意只是"这是垃圾数据还是真记录"，用绝对时间就够了：

```cpp
const qint64 maxGapUs = qMax<qint64>(5000000, qint64(m_periodUs) * 1000);  // ✓ 5 s
```

**2. 每轮无条件丢掉最新一条记录**

原来是"最新一条可能正在被写入，所以丢掉"。但 33 ms 抽帧 × 1 kHz 下，这等于每秒扔 30 条
（3%），表现为**持续的小断口**。现在只在它自己的时间戳没有推进时才丢——那才是"正在写"
的特征。

**3. 绘图端不再为单个缺失点断线**

`buildRuns` 原来遇到任何 NaN 就结束一段。改成**跨越短缺口**：用相邻有效点间距的
**中位数 × 4** 作为阈值，比它短的空洞直接连线（像示波器那样），比它长的才分段。
用中位数而不是均值，是为了让一个长断流不会把阈值抬高到吞掉所有其他缺口。

**实测（真机，1 kHz）**

```
连续性: 702 个样本，0 处缺口，最长 0 ms
[PASS] 缺口不超过样本数的 5%（曲线连续）   (0 / 702)
    共 702 个样本，请求周期 1000 µs，实测间隔 1000.1 µs
```

之前同样的测试会看到成片的缺口。自检里也留了三条针对性断言：单个缺失样本被跨越、
真正的断流仍然分段、跨越距离随采样间隔自适应。

#### 复现 J-Scope 的渲染：从截图反推而不是凭印象

用一张真实 J-Scope 截图和一张本程序的截图做了像素级比对，差异是可量化的：

| 特征 | J-Scope（实测） | 本程序（改前） |
|---|---|---|
| 网格间距 | **固定 100 px** | 按数据算 1-2-5 步长，位置随数据漂移 |
| 网格色 | `#3C3C3C` | 主题浅灰 `#C8CFD8` |
| 轴线 | `#C0C0C0`（零轴加亮） | 无 |
| 背景 | `#000000` | `#F5F6F8` |
| 轨迹线宽 | 1 px 占 72%，**硬边**（AA 比 0.12） | 1.4 px，抗锯齿（AA 比 1.05） |
| 轨迹颜色 | 纯饱和 `#FF0000` / `#FFA500` | 柔和 `#E63C3C` |

**关键差异是网格的生成方式**，不是颜色。SEGGER 手册写得很直白：
"One div is defined as **100px**（与背景黑格同尺寸）"。J-Scope 的格子是**固定像素
晶格**，每格代表多少由工具栏的分辨率（s/div、Y 单位/div）决定；而按数据算 1-2-5 步长
会让格线落在任意像素、每帧位置都在变——这正是"看着不像示波器"的根源。

改法：

```cpp
constexpr int kPixelsPerDivision = 100;   // 固定晶格
painter.setPen(QPen(scope::grid(), 1));   // #3C3C3C
glLineWidth(1.0f);                        // 硬边 1px，关掉 GL_LINE_SMOOTH / BLEND
```

**怎么验证"真的像了"**

自检不只比对颜色，还从离屏渲染的像素里**数出网格线的间距**：

```
离屏配色: 黑底 97.4% · 纯红轨迹 3768 像素
网格: 4 条横线，间距 100..100 px
[PASS] 网格间距固定 100px（与 J-Scope 一致）
```

这一条能抓住"改回 1-2-5 阶梯"的退化——那种情况下间距会变成 97/103 这类不等值。
离屏图现在也会把 QPainter 叠加层（网格、坐标）合成进去，否则测试只覆盖曲线、
悄悄漏掉网格。

#### GPU 绘图的三个真实故障（都不是"看起来那样"）

从 QPainter 换到 OpenGL 后连续踩了三个坑，每一个的表现都指向错误的方向：

**1. 片元着色器里的 `precision mediump float;`**

那是 **GLSL ES** 语法（WebGL 用）。桌面 GL 的 GLSL 120 直接拒绝：

```
ERROR: 0:2: '' :  syntax error, unexpected IDENTIFIER
ERROR: 1 compilation errors.  No code generated.
```

着色器链接失败 → 控件退化成"尚无数据"。而采样、符号、地址全都正常，所以现象完全
不像 GL 的问题。修法：去掉 precision 语句，加 `#version 120`。

自检里留了**负向对照**：把带 `precision` 的版本再编译一次，没被拒绝就说明这台驱动的
行为变了，断言会失效——这比"我记得应该这样"可靠。

**2. `mat2` uniform 被当成 `vec4` 上传**

```cpp
m_program->setUniformValueArray(loc, scale, 1, 4);  // ✗ 内部走 glUniform4fv
glUniformMatrix2fv(loc, 1, GL_FALSE, scale);         // ✓ 矩阵必须用矩阵接口
```

Qt 的 `setUniformValueArray` 没有矩阵重载，它会按 tupleSize 调 `glUniformNfv`。类型
不匹配导致矩阵没被设置：**所有顶点塌缩到同一点**。坐标轴和数值（QPainter 画的）正常
显示，曲线却完全不可见——这正是"数值存在但曲线不可见"。

**3. `QOpenGLFunctions` 必须绑定到当前上下文**

`glViewport`/`glClear`/`glDrawArrays` 是成员函数指针。离屏渲染时没有 `initializeGL()`
回调，必须先 `initializeOpenGLFunctions()`，否则函数指针为空、第一次 GL 调用跳到 0
地址。栈回溯给出的位置非常明确：

```
#0 mrd::GlPlotWidget::renderOffscreenForTest(int, int, int*)
#1 mrd::cli::runSelftest(...)
```

**怎么保证这些不再复发**

`QOpenGLWidget` 的内容无法用 `QWidget::render()` 抓取，所以自检改用**离屏 FBO 渲染 +
读回像素**：

```
离屏渲染: 500 个顶点 → 2081 个非背景像素
[PASS] 曲线真的被画出来了（离屏像素验证）
```

这一条是唯一能抓住"矩阵未设置"的断言——顶点数、坐标范围、量程全都正确，只有像素是
空的。加上着色器编译与负向对照，四个断言覆盖了整条渲染链。

#### 绘图改为 GPU：移植 webgl-plot 的渲染方式

原来的曲线是 QPainter 在 CPU 上逐点光栅化。数据量一大（8 条曲线 × 10000 点），
每帧的代价随点数线性增长，这才是"卡"的真正来源。

现在照 [webgl-plot](https://github.com/danchitnis/webgl-plot) 的做法改成 GPU 渲染：
每条曲线是一块 `(x, y)` float 顶点缓冲，一次 `GL_LINE_STRIP` 画完，数据空间到裁剪
空间的映射放在**顶点着色器**里做：

```glsl
attribute vec2 coordinates;
uniform mat2 uscale;  uniform vec2 uoffset;
void main(void) {
    gl_Position = vec4(uscale * coordinates + uoffset, 0.0, 1.0);
}
```

这是 webgl-plot 的 `initThinLineProgram` 原样对应的实现——它那个 `uscale`/`uoffset`
就是"缩放 + 平移"，和这里完全一致。CPU 只负责把 `double` 数据摊成 `float` 顶点并检测
NaN 断点，**不再参与光栅化**。

实测（8 条曲线 × 10000 点 = 80000 顶点/帧）：

```
CPU 准备顶点: 1.15 ms/帧（含 double→float 与 NaN 断点检测）
```

对比旧实现同一量级的 CPU 光栅化约 80 ms，降了两个数量级。

**为什么不是直接嵌入 webgl-plot**

本机 Qt 6.8.3 只有 mingw_64 套件，而 **Qt WebEngine 在 Windows 上只支持 MSVC**，
且模块未安装。要跑 webgl-plot 的 JS 得先装 MSVC 版 Qt + WebEngine 模块，并把整个项目
换工具链。既然诉求是"用 GPU 而不是 CPU 硬算"，移植同一套 GL 技术更直接，也不引入
上百 MB 的 Chromium 依赖。

**保留的部分**：频谱分析仍是 C++ 的 `Fft`（Cooley-Tukey + Hanning 窗），只是把算出来的
dB 序列同样交给 GPU 画成一条 `LINE_STRIP`。

#### 波形显示：移植自 Clion-Waveform-Plotter 的设计

绘图是自己画的（QPainter），但交互与频谱的**设计**参考了开源项目
[Clion-Waveform-Plotter](https://github.com/Szturin/Clion-Waveform-Plotter)
（Kotlin 的 CLion 插件，不能直接搬代码，所以按它的行为在 C++ 里重写）。
它做的这几件事恰好是原实现缺的：

| 能力 | 说明 |
|---|---|
| **真实时间轴** | 最新样本在 0，旧样本为负值（示波器习惯）；单位自动 ms/µs/ns |
| **1-2-5 网格** | 刻度落在 1/2/5×10ⁿ，不会出现 `0.0371 s` 这种读数 |
| **滚轮缩放 Y** | 手动缩放叠加在自动量程之上，所以幅度大小不影响手感 |
| **Shift+滚轮缩放 X** | 以右边缘（新数据到达处）为锚点，放大不会丢掉最新值 |
| **左键拖动平移** | 双轴任意方向；**右键单击**恢复整段适配 |
| **悬停十字线 + 数值框** | 框里列出该时刻每条曲线的值和时间差，靠右侧时自动翻转 |
| **NaN 自动断线** | 缺失样本存为 NaN，曲线断开而不是掉到 0 |
| **FFT 频谱** | 一键切时域/频域，Hanning 窗 + 补零到 2 的幂，纵轴 dB |
| **CSV 导出** | 时间 + 每通道一列，可直接被表格软件绘图 |

**交互**：实时图表开放缩放/拖动，因为连续数据确实需要放大某段来看。拖动会冻结时间轴
（双击恢复跟随），这样正在滚动的曲线不会在鼠标底下跑掉。

#### FFT 频谱：为什么电机调试需要它

电流环纹波、编码器插值误差、机械谐振，这些东西在时域里只能靠肉眼数峰。切到频谱
直接读频率。取**最新 1024 点**（不是整个缓冲）——整个缓冲会把用户刚改的参数平均掉。
在 1 kHz 采样下是 ~1 Hz 分辨率。

自检用**已知频率的合成正弦**验证，不是拿录好的数据"看起来对"：

```
[PASS] 已知频率的正弦落在正确频点   (峰值 125.0 Hz，期望 125.0 Hz)
[PASS] 幅度以 dB 表示且量级正确     (-6.0 dB)
[PASS] 直流只留在低频（Hanning 主瓣之外无泄漏）  (主瓣外最高 -64 dB)
```

范围 1..2 是刻意的：Hanning 窗是升余弦，会把直流摊到基频和两个相邻 bin，所以
"直流不能泄漏"只能在主瓣之外判定。第一版断言写成"除 bin 0 外都要 < -100 dB"，
结果 8 dB 失败——**校验了窗函数的性质，而不是代码的 bug**。

#### 实时曲线：直连 J-Link DLL 的 HSS 引擎

上面测出的"停机读取"限制有一个官方解法：**HSS（High-Speed Sampling）**。采样由探针
自己按硬件时基完成，主机只负责取缓冲区，所以目标一直在跑，曲线是活的。

这是 `JLinkDllProbe` 的能力，也是 DLL 成为唯一后端的首要原因（见 §1）。

| | 直连 DLL（唯一后端） | 已删除的 GDB RSP 后端 |
|---|---|---|
| 连接方式 | 进程内加载 `JLink_x64.dll` | 起 GDB Server 子进程，走 RSP 协议 |
| 读内存 | 停机快照 | 停机快照 |
| **实时采样** | **HSS，目标继续运行** | 不支持 |
| 依赖 | 只需要 DLL | 需要 JLinkGDBServerCL.exe |

用法：

```
mcuRegDebugger.exe                    # 勾选变量 → 「开始实时采样」
mcuRegDebugger.exe --probe-info       # 命令行自检（含 HSS 一节）
```

**HSS 的接口是反推出来的，不是文档**

HSS 的结构体只存在于付费的 J-Link SDK（`JLinkARMDLL.h`），公开渠道查不到。本机的
SEGGER 自家的可视化工具也是动态 `GetProcAddress` 加载，没有静态导入表。所以布局是从
它的包装代码反汇编出来的，再用真机验证：

```c
// 块数组：每块 16 字节，末尾必须有一个全零块作为终止符
struct HssBlock { U32 address; U32 numBytes; U32 reserved0; U32 reserved1; };
int JLINK_HSS_Start(HssBlock *blocks, U32 numBlocks, U32 periodUs, U32 flags); // flags = 1
int JLINK_HSS_Read(void *buffer, U32 sizeBytes);  // 返回样本数
int JLINK_HSS_Stop(void);
```

实测确认的事实（J-Link PLUS V9.70 / DLL V8.26）：

| 事实 | 依据 |
|---|---|
| `Period` 单位是**微秒**，且准确 | 请求 1000 / 8000 / 16000 µs，实测间隔 1000.1 / 8000.2 / 16000.3 µs |
| 每条记录 = `[4B 时间戳][各块数据…]` | 时间戳单调递增，间隔与请求周期一致 |
| 返回值是**样本数**，不是字节数 | 8 字节/样本 × 699 = 5592 字节 |
| 返回值**可能超过缓冲区容量** | 请求 4096 条却返回 5608 条 → 会写越界 |

最后两条各自的后果都很实际：

- 若把返回值当字节数 → 少读 8 倍；
- 若不检查越界 → 堆被写坏。第一次实现就是这样段错误的。修法是给 DLL 双倍缓冲、
  只申报一半，然后**用时间戳判定有效数据**（时间戳不再递增就是零填充尾巴）。修好后
  700 ms 恰好 699 个样本。

**块数上限 8**：DLL 自己拒绝更多（Commander 原文 "This command can only handle up to
8 blocks"）。所以实时采样限 8 个变量，数组只采第 0 个元素。

**采样率上限与探针型号有关**：官方 Model comparison 表给出非 Pro/Ultra 型号 **1 kHz**。
实测 8 变量、请求 100 µs 时实际约 999 样本/秒，与官方数字吻合；把周期设更小只会增加
丢样本风险，不会更快。界面上的周期就是对此的直接映射。

**这个限制和"拖慢 MCU"的关系**：HSS 的读取由探针发起，不占 CPU 指令周期，所以轮询
不再像 GDB 那样让内核停机。真正的代价是总线和内存访问的争用——117 KB/次的流量对
AHB 带宽不到 0.1%，但每次访问仍会短暂抢占总线，对 12 kHz 的 FOC 这类硬实时循环需要
实际观察是否引起 deadline 丢失。

#### 轮询全部静态变量会不会拖慢 MCU（实测）

这是能用板子直接回答的问题，所以用 `--bench-bus` 量了一遍（J-Link V9，SWD 50 MHz，
BX8115.axf，3777 个可采样叶子 / 117,336 字节）。

**单次全量读取要多久**

| 读法 | 请求数 | 耗时 | 等效刷新率 |
|---|---|---|---|
| 逐变量读 | 3777 | **1740 ms** | 0.57 次/秒 |
| 合并连续区段读 | 700 | **692 ms** | 1.4 次/秒 |

瓶颈不是字节数而是**每次请求的协议往返**（1640 ms / 3777 ≈ 0.46 ms 一次，与块大小
几乎无关）。所以把相邻变量合并成连续区段读，字节数不变而快 2.5 倍——这是唯一有意义
的优化方向。

**`--bench-bus` 的用法**：它读目标自己的 FreeRTOS 节拍计数器 `xTickCount`，先在空载测
一次速率，再在持续读取下测一次。如果目标真的在被拖慢，第二次的 tick/s 会降。

```
mcuRegDebugger.exe --probe-info --symbols firmware.axf --bench-bus 4
```

#### 命令行也能做同一件事

```
mcuRegDebugger.exe --probe-info --symbols firmware.axf --read-var error_table.__elems_
```

会连上目标、按符号解析地址、读回全部元素并打印索引/值/十六进制。这条命令是本地
表格功能的端到端证明：同一套地址解析与解码，只是输出到终端而不是表格控件。
不带探针时用 `--symbols` 单独确认变量是否被解析出来。

#### 为什么用 .axf 而不是 .map

`.map` 只有名字 + 地址 + 大小。这三样够**找到**一个变量，但不够**解释**它——四字节
可能是 `float`、`int32`、枚举，或 `uint16_t[2]`。解码需要知道按哪种类型，否则曲线
是错的。这些信息在 DWARF 里，也就是 `.axf` 里：

| 来源 | 名字 | 地址 | 大小 | 类型 |
|---|---|---|---|---|
| `.map` | ✓ | ✓ | ✓ | ✗ |
| `.axf`（DWARF） | ✓ | ✓ | ✓ | ✓ |

任何带 DWARF 的 ELF 都行（GCC 的 `.elf` 也可以）。导入后会列出**所有静态分配的
变量**：全局、`static`、`const`（在 `.rodata` 也照样有地址）。

结构体会被**展开成成员**（`g_point.x`、`g_point.y`、`g_point.flags`），每个成员有自己的
地址和类型；数组保持一条记录并带元素个数（`uint16_t[4]` 一行，不拆成 4 行噪音）。

#### 为什么局部变量不在列表里

局部变量在栈上，地址随调用层级变。快照和 HSS 都按**绝对地址**读取，所以两者都无法
采局部变量——这不是本程序的取舍，是"按地址读内存"这条路的下限。DWARF 里的局部变量
信息仍然被解析（自检会验证 `frameBaseAt` / `functionContaining` 的算术），只是不作为
可选行呈现，因为没有固定地址可读。

#### 命令行确认变量清单

不需要 GUI 也不需要目标板，只检查「为什么某个变量没出现在列表里」：

```
mcuRegDebugger.exe --symbols firmware.axf
```

输出按文件分组的变量、类型分布、地址来源，以及（如果发生了）`地址校正: N 个变量…`
——那表示 DWARF 给的地址与链接符号表冲突、已按符号表修正。

#### 自检覆盖

夹具 `data/test/symbol_test.axf` 是真实编译器产物（`scripts/gen-symbol-fixture.py`
可重建），所以断言的是确定值而不是猜测：

```
[PASS] g_temperature = 浮点 @ 0x08001018          (4 字节)
[PASS] g_adc_raw     = 无符号 @ 0x0800101C        (2 字节 x4)
[PASS] g_point.x/y/flags 展开到 +0/+4/+8
[PASS] 变量树按 文件 → 类型/函数 → 成员 分层
[PASS] 勾选标量成员后进入导出列表
[PASS] 类型成员可勾选（已通过实例解析出地址）
[PASS] 符号名用叶子名，完整路径写 NameWithParents
[PASS] 地址写成 0x + 大写十六进制
[PASS] 时间轴按秒/格写入（100 ms/格 -> 100000 us）
```

## 7.1 烧录固件：起止地址怎么定

**未连接也能打开这个对话框。** 分析镜像、合并 bootloader、另存结果都是纯文件操作，不需要
探针；只有真正擦写才需要。所以门禁放在「烧录」按钮上，而不是放在开窗口上：

| | 已连接 | 未连接 |
|---|---|---|
| 打开对话框 | ✅ | ✅（标题变为「固件合并（未连接：不执行烧录）」）|
| 分析 / 预览起止地址 | ✅ | ✅ |
| 合并另存到桌面 | ✅ | ✅ |
| 烧录按钮 | 可用 | **禁用**，文字变「烧录（需连接）」|

这样做是因为实际流程往往是"先合并好镜像，再去接板子"。以前未连接时直接弹一句
"请先连接目标再烧录固件"，把合并功能一并挡在门外了。

`.bin` 文件本身不含地址信息，所以"烧到哪"必须**推导**出来而不是猜。规则不针对某个
型号，而是对照器件表里的内存窗口做校验，换型号时同一套逻辑照用。

### 识别规则

| 项 | 怎么得来 | 校验 |
|---|---|---|
| **起始地址** | Cortex-M 的 Flash 基址 `0x08000000` | 镜像前两个字是 `[初始 SP][复位向量]`：SP 必须落在本型号 SRAM 内、复位向量必须落在本型号 Flash 内，否则拒绝 |
| **结束地址** | 最后一个不是 `0xFF`（擦除态）的字节 | 不必等于文件长度（文件可能带尾部填充），也不必等于 Flash 容量（固件通常比芯片小） |
| **写入长度** | 结束地址按 **4 KB 擦除扇区**上对齐 | 避免写入停在扇区中间，留下半擦除状态 |
| **容量校验** | 写入长度 vs 器件表的 `flashBytes` | 装不下就**拒绝烧录**并说明超出多少——先擦后写再发现装不下，目标就变砖了 |

### 实测（SG6010C 固件 + N32G455CCL7）

```
--- 固件烧录: SG6010C_3.12.3_0.6.4_261118.bin ---
    型号      : N32G455CCL7
    文件大小  : 235024 字节
    起始地址  : 0x08000000
    结束地址  : 0x0803a000
    写入长度  : 237568 字节（232 KB）
    向量表    : SP=0x200123d8  Reset=0x08000320
    说明      : 按型号 N32G455CCL7 校验：Flash 256 KB / RAM 144 KB
    说明      : 结束地址按 4 KB 擦除粒度上对齐：0x08039610 -> 0x0803a000
[PASS] 固件容得下这个型号的 Flash   (需要 232 KB / 可用 256 KB)
```

### 为什么用 J-Link Commander 而不是 DLL

烧 Flash 需要厂商的 flash 算法（扇区布局、编程时序、RMW 处理），`JLink.exe` 自带。
从 DLL 直接 `JLINK_WriteMem` 写 Flash 地址只有在 flash bank 配置正确时才成立，配错
的后果就是半个扇区写坏。所以烧录走 Commander，和 J-Flash 用的是同一条路径：

```
device N32G455CC
si SWD
speed 4000
connect
h
erase 0x08000000 0x0803a000        ; 仅 --burn-force 时插入
loadbin "…\SG6010C_….bin", 0x08000000
verifybin "…\SG6010C_….bin", 0x08000000
r
g
```

### 一个重要的实测行为：内容相同时 J-Link 会跳过写入

设备里已经是同一份固件时，`loadbin` 打印：

```
J-Link: Flash download: Bank 0 @ 0x08000000: Skipped. Contents already match
```

这是 J-Link 的比较优化（省时间和 Flash 寿命），但意味着"烧录"可能什么都没写。要强制
真正擦写，用 `--burn-force`（或界面里勾选）——它插入一条 `erase`，**只擦固件覆盖的
范围**，不会整片擦除。实测强制路径：

```
J-Link>erase 0x08000000 0x0803a000
Erasing done.
J-Link>loadbin …
J-Link: Flash download: Bank 0 @ 0x08000000: 1 range affected (235520 bytes)
J-Link: Flash download: Program speed: 26 KB/s
O.K.
Verify successful.
```

### 配置区不会被擦掉

该固件的映像是 `0x08000000..0x08039610`，而设备在 `0x0803D000..0x08040000` 存着
**运行时配置/校准数据**（不属于固件）。因为擦除范围精确到固件覆盖的扇区，实测烧录后：

| 项 | 结果 |
|---|---|
| 固件区前 235024 字节 | 与文件**完全一致** |
| 配置区 0x3D000..0x40000 非 0xFF 字节 | **9059 字节，与烧录前相同**（未被破坏） |

想连配置一起清掉，用 J-Link Commander 的整片 `erase` 即可，本工具不做。

### 合并 bootloader

带 bootloader 的板子需要**一次写入两份镜像**：bootloader 在 flash 开头，应用在它后面。
界面里勾选「合并 bootloader」后选择 bootloader 文件（`.axf` / `.elf` / `.hex`），工具会把
两者合成一个镜像写下去——一次擦除、一次编程、一次校验，中间没有"新 bootloader 配旧应用"
的窗口。

**三个地址都可由用户输入**（这正是勾选后出现的那三个框）：

| 输入框 | 默认 | 含义 |
|---|---|---|
| 固件起始地址 | 勾选 bootloader 时 `0x08008000`，否则 `0x08000000` | 应用放在哪里 |
| 整体起始地址 | 留空 = 按内容推导（取所有镜像最小地址） | 擦除/写入范围的起点，可以往前放宽以清掉旧残留 |
| 整体结束地址 | 留空 = 按内容推导并按 4 KB 扇区上对齐 | 擦除/写入范围的终点 |

点「**分析**」后才允许「烧录」：分析会显示推导出的范围、写入长度、向量表和容量校验，
确认无误再擦写。

Intel HEX 解析支持记录类型 00/01/02/04/05，**校验和错误直接拒绝**——一个损坏的
bootloader 是唯一能把板子彻底写废的东西，不能静默跳过。

### 内置 bootloader

应用自带一份 bootloader：`data/bootloader/LION_H473_Bootloader.hex`
（`0x08000000 .. 0x08002ED8`，11992 字节，SP=`0x20002968`、Reset=`0x08000231`）。

- 勾选「使用 bootloader」后**自动填入**这个文件，不需要手动找；
- 路径仍可编辑，换别的 bootloader 没问题；
- **取消勾选会清空路径** —— 这点是必要的：`plan()` 用"路径非空"判断是否合并，
  留着路径会让刚取消勾选的 bootloader 继续被合并进去（自检抓到过）。

### 烧录不再让窗口"无响应"

原来的烧录在 UI 线程里同步等 J-Link Commander 退出，一次完整烧写十几秒，Windows 就会
在标题栏画上"（无响应）"。现在：

- `JLinkRunner::runScriptAsync()` 用 `QProcess` + 信号驱动，内部**没有任何 `waitFor*`**，
  事件循环照常跑，窗口可以拖动、状态栏持续显示进度；
- `FirmwareBurner::burnAsync()` 走同一套命令列表，结果经回调返回；
- 成功与失败的判定抽成 `FirmwareBurner::interpret()`，阻塞版与异步版**共用**，
  避免两条路径对"成功"的定义不一致（非零退出码、`Could not connect`、超时都算失败）；
- 阻塞版 `burn()` 保留给命令行 —— 那里等待才是目的。

### 临时镜像的清理

合并后的缓冲区要落成一个 temp 文件（`loadbin` 只能指向文件，不能指向内存），所以**每次
分析**都会写一个 `%TEMP%\mrd_merged_<文件名>_<pid>.bin`。三个泄漏点都已堵上：

| 场景 | 谁负责删 |
|---|---|
| 分析后又改参数、再分析 | `analyse()` 在覆盖镜像前先删上一轮的 |
| 关掉对话框（取消 / 另存） | `~BurnDialog()` |
| 真正烧录 | `takeImage()` **移交所有权**，由 `MainWindow` 在四条退出路径上删 |

> `takeImage()` 是必要的：烧录是异步的，`burnAsync` 拿到命令就返回，J-Link 进程之后才去
> 读那个文件。如果对话框析构时把它删掉，带 bootloader 的烧录会随机失败。
> 这条修复前 `%TEMP%` 里堆了 **472 个**残留文件。

### 手动合并另存（不烧录）

「烧录固件」对话框里除「烧录」外还有一个「**合并另存到桌面**」：把同样的合并结果按
**导入固件的名字**另存成 `.bin` 到桌面，**不改动原文件**。

| | 自动回写 | 手动另存 |
|---|---|---|
| 什么时候发生 | 烧录并**校验通过**之后 | 点按钮时，随时 |
| 写到哪里 | 覆盖你选的那个 `.bin`（同名） | 桌面，`<原名>_with_bootloader.bin` |
| 原文件 | 被覆盖（留 `.bak`） | **不动**，也不产生 `.bak` |
| 用途 | 出货前把 bootloader 固化进 release 文件 | 先检查合并结果、或板子还没接上时 |

例：导入 `SG6010C_3.12.3.bin` → 桌面得到 `SG6010C_3.12.3_with_bootloader.bin`。

按钮只在**分析成功且确实是合并镜像**后可用 —— 没勾 bootloader 时没有"合并结果"可存。

### 烧录成功后的三步

1. **重连并确认应用在跑**：烧写完成会自动重连，走完型号校验后报告目标是否在运行。
   合并 bootloader 的意义就是"板子能自己起来"，所以这一步要验证，而不是只看写入成功。
2. **回写覆盖原文件**：若烧的是**合并镜像**且校验通过，把合并后的字节**同名覆盖**回
   你选择的那个 `.bin`。下一次烧这个文件就是单镜像烧录，不再需要 bootloader 选项。
3. **保留 `.bak`**：覆盖前把原内容复制成 `<文件名>.bak`。覆盖的是准备出货的文件，
   必须可回退。

回写有两条硬性限制（都有自检）：

| 情况 | 行为 |
|---|---|
| 只烧了应用（未合并） | 不回写 —— 没有新内容 |
| 校验未通过 | 不回写 |
| 源文件不是 `.bin`（如 `.hex`） | 不回写 —— 原始字节写进 `.hex` 会让下游工具全部解析失败 |

### 两个必须拒绝而不是"尽力而为"的情况

**(1) 应用被链接在别的地址**

裸 `.bin` 里的绝对地址是链接时定死的：向量表第一个字是栈顶、第二个字是复位向量，都是
绝对地址。把链接在 `0x08000000` 的镜像搬到 `0x08008000`，镜像里的复位向量仍然指向
`0x080003A0`——MCU 会跳进 bootloader 里。所以这种情况**拒绝**并说明原因：

```
固件被链接在 0x08000320，但要求放到 0x08008000：它的复位向量指向 0x08000320，不在放置范围内。
裸 .bin 里的绝对地址是链接时定死的，直接搬到别的地址会让 MCU 跳到错误的位置（这里会跳进 bootloader）。
请用 0x08008000 重新链接后再烧录。
```

要合并 bootloader，应用必须**按 `0x08008000` 重新链接**后再导出 `.bin`。

**(2) 两份镜像落在同一地址**

```
两个镜像在地址 0x08000000 处内容冲突（0x68 vs 0xd8）
```

同地址、不同字节 → 拒绝。相同字节的重叠是允许的（重复记录无害）。

### 实测：这台设备的布局装不下

用桌面上的两个真实文件实测（bootloader 来自附件 HEX，应用来自 `SG6010C_...bin`）：

| 镜像 | 范围 | 大小 |
|---|---|---|
| bootloader | `0x08000000 .. 0x08002ED8` | 11.7 KB |
| 应用（链接在 0x08000000） | `0x08000000 .. 0x08039610` | 229.5 KB |

- 应用**没有按 `0x08008000` 链接**，所以合并被拒绝（情况 1）。
- 即使重新链接，`32 KB + 229.5 KB = 261.5 KB` 仍然超过这颗 N32G455CC 的 **256 KB**
  ——已经超出，不是因为擦除对齐。用通用 `Cortex-M4` 设备名读 `0x08040000` 也失败，
  说明物理上就到 256 KB，不是 J-Link 的型号配置限制。

结论：这颗板子要做 bootloader 分区，需要一个更大的 512 KB 版本（同系列 N32G455CE 等），
或者把 bootloader 与应用的边界压到更小（应用仍需重新链接）。

### 用法

界面：**固件 → 烧录固件…**，选文件后会先显示识别出的起止地址、写入长度、向量表和
容量校验结果，确认后才擦写。烧录前会自动断开本工具占用的 J-Link，完成后重连。

命令行（默认只报告，不写入）：

```bat
mcuRegDebugger.exe --probe-info --burn firmware.bin                  :: 只分析
mcuRegDebugger.exe --probe-info --burn firmware.bin --burn-commit    :: 真正烧录
mcuRegDebugger.exe --probe-info --burn firmware.bin --burn-commit --burn-force  :: 强制擦写

:: 合并 bootloader（地址可覆盖）
mcuRegDebugger.exe --probe-info --burn app.bin --bootloader boot.hex ^
    --app-base 0x08008000 --image-start 0x08000000 --image-end 0x08042000
```

烧录前建议先备份当前 Flash（只读操作）：

```bat
mcuRegDebugger.exe --probe-info --dump-flash backup.bin
```

## 7.2 模型仿真：用真实设备数据驱动模型

中间窗口新增「**模型仿真**」页。它做的事只有一件：把**探针从 .axf 变量地址读回来的真实
数据**逐帧喂给一个模型，再画出模型输出。

### 为什么不模拟数据源

模型的用处在于回答"真实输入下它会怎样"。喂一段合成信号只能验证模型自洽，验证不了它
面对真实工况（噪声、饱和、边界值）时的行为。所以这里**没有信号发生器**：输入就是
「变量曲线」页正在采的那批样本，同一个 `HssSampler`、同一批地址。采样没跑就直说，不编数。

数据通路：

```
HSS 采样（探针按自己的时基读 axf 里的地址）
  → ScopePage::liveSample(timeUs, {变量名: 值})     每个样本一次，时间戳来自目标
  → SimulationPage：查输入映射 → 模型 step(dt = 真实时间差)
  → 模型输出 → 图表（x = 模型时间）
```

`dt` 用的是**相邻样本的真实时间差**（来自目标自己的时间戳），不是假设的周期。所以采样
被拖慢时模型看到的是正确的时间跨度，而不是按名义周期硬推。

### 支持的模型

| 格式 | 是什么 | 怎么来 |
|---|---|---|
| **`.fmu`** | FMI 2.0 Co-Simulation 包（ZIP + DLL + modelDescription.xml） | Simulink 的 **Export to FMU**；也可以是任何符合 FMI 2.0 的工具导出 |
| **`.py`** | Python 文件，定义 `class Model` 与 `step(inputs) -> dict` | 手写，或把方块图重新实现一遍 |

**`.slx` / `.mdl` 不做解析**，这是有意的取舍。正确仿真一个方块图需要求解器、代数环、
子系统、Stateflow、查表与库块——那就是 Simulink 本身。做一个"能跑简单块"的子集会让人
以为拿到了真结果，而数字是错的。要 Simulink 的模型，请导出 FMU。

### FMU 是怎么跑起来的

`.fmu` 是普通 ZIP，里面有 `modelDescription.xml` 和 `binaries/win64/<model>.dll`。加载
步骤与任何 FMI 工具一致：

1. 自己解 ZIP（不依赖 Qt 私有的 `QZipReader`，它明确写着"随时可能移除"）；
2. 解析 `modelDescription.xml`：`fmiVersion`、`modelName`、`guid`、`CoSimulation`、
   以及每个 `ScalarVariable` 的 `valueReference` 与 `causality`；
3. 把二进制解到临时目录、`QLibrary` 加载（DLL 不能从内存加载）；
4. 按 FMI 2.0 的 ABI 调 `fmi2Instantiate` → `fmi2SetupExperiment` →
   `fmi2EnterInitializationMode` → `fmi2ExitInitializationMode`，然后每步
   `fmi2SetReal` → `fmi2DoStep` → `fmi2GetReal`。

**只支持 FMI 2.0**。3.0 把所有入口改名成 `fmi3*` 并换了类型系统，混用会静默产出垃圾
数字，所以检测到非 2.0 直接拒绝并说明。

`fmi2CallbackFunctions` 的字段顺序是 ABI 的一部分：顺序写错会让 FMU 用 logger 指针去
调 `free()`。结构体在代码里逐字段写死，不靠猜。

### Python 模型怎么写

```python
# model.py
INPUTS = ["u"]                 # 声明输入名，页面才能建映射表
OUTPUTS = ["y", "steps"]       # 声明输出名，供图表选择

class Model:
    def __init__(self, sample_time=0.001):
        self.y = 0.0
        self.tau = 0.1

    def step(self, inputs):
        u = float(inputs.get("u", 0.0))
        dt = 0.001
        alpha = dt / (self.tau + dt)
        self.y += alpha * (u - self.y)
        return {"y": self.y}        # 返回 {输出名: 数}
```

- 也可以用模块级 `def step(inputs)` 代替 `class Model`；
- `Model(sample_time=dt)`、`Model(dt)`、`Model()` 三种构造都会被尝试；
- **`INPUTS` / `OUTPUTS` 建议声明**。不声明时输入名无从得知（页面上没有可映射的输入行），
  输出名会在第一次真实步进后学到。

模型跑在**子进程**里（`python` / `python3` / `py`，会实际执行验证而非只看文件是否存在）。
理由有两个：不把 CPython 链进 GPL 二进制、也不把它钉在某一个解释器版本上；而且用户模型
崩溃不会拖垮调试器。模型自己的 `print()` 被重定向到 stderr，不会污染协议流。

协议是逐行 JSON。一次 `step` 超时（默认 10 s）即判定模型不可用并停止仿真——继续跑会画出
一条"部分更新"的假曲线。

### 界面与用法

1. 选模型 →「加载模型」，会显示模型名、FMI 版本、输入/输出数量；
2. 在「输入映射」里把每个模型输入接到一个**设备变量**（下拉项来自当前采样通道）；
3. 选要绘制的输出 → 「开始仿真」；
4. 若「变量曲线」页还没在采样，会自动启动它（同一批地址，不需要重复勾选）。

状态行显示步进次数、模型时间、失败次数。任何一个步进失败都会**立即停止**并记录原因。

### 实测

自检用一个真实的 FMI 2.0 夹具（`data/test/sim_lag.fmu`，一阶滞后 `y' = (u-y)/0.1`）
和一份行为相同的 Python 模型，两个后端给出**完全一致**的结果：

```
滞后模型 60 步后 y = 0.44955
Python 模型 60 步后 y = 0.44955，steps = 60
每个真实样本步进一次模型   (150 步)
图表点数等于步数（中途没有被清空）   (150 点)
曲线在上升（真实输入驱动了模型）   (0.00990099 -> 0.775201)
没有映射变量入参时不步进   (150 -> 150)
```

夹具由 `scripts/build-sim-fixture.py` 重新生成（mingw 编译 `scripts/sim_fixture/testlag.c`
再打成 ZIP），与 `data/test/symbol_test.axf` 一样作为二进制夹具提交，这样测试不需要编译器。

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

采样策略另有专门断言：快轮询周期确实远小于 200 ms 且不小于 10 ms、`shouldFastPoll()` 与
`shouldUseRegisterHss()` 两张真值表（未连接 / 未选中 / CPU 暂停 / 后端不支持 HSS / 本页不在前
都必须退回轮询）、无在途读取时不被误判为阻塞、空闲采样器"既不运行也不在启动中"。

三个系列各跑一遍，均 0 失败：

| 器件 | 断言数 |
|---|---|
| N32H473CE | 580 |
| N32G455CCL7 | 541 |
| N32G452CBL7 | 541 |

## 10. 打包安装程序

```bat
scripts\build.bat                        :: 编译
scripts\package.bat                      :: 部署 + 打包（一条命令就够）
```

产物 `dist\McuRegDebugger-<版本>-setup.exe`，当前版本 **1.1.1.0**，约 **10 MB**
（65 MB 的部署目录经 NSIS 的 solid LZMA 压缩）。

第一次打包前取一次打包工具；它是便携版，不装东西、不需要管理员：

```bat
powershell -ExecutionPolicy Bypass -File scripts\fetch-nsis.ps1
```

### 安装包里有什么

| 内容 | 说明 |
|---|---|
| `mcuRegDebugger.exe` | 主程序 |
| Qt6 运行时 | `windeployqt` 生成：DLL、`platforms\`、`styles\`、`tls\`、`imageformats\` 等 |
| `svd\` | **11 套 Nationstech SVD 支持包** + `manifest.json`（157 个型号）；离线也能识别芯片 |
| `data\` | 器件识别表、封装引脚图、J-Link 器件名映射 |
| `LICENSE` / `README.md` | GPL v2 许可与说明 |

**不打包的两样，各有原因**：

- `data\datasheets\`（31 MB 厂商 PDF）：它们只是 `scripts/build-package-tables.py` 的输入，
  已经被固化成 `data/parts.json`，运行时没有任何代码会打开 PDF。打进去只会让安装包大四倍。
- `svd\*.pack`（1.4 MB 原始厂商压缩包）：程序读的是解压后的 `.svd` 文件，`.pack` 只是
  下载原件。`deploy.bat` 用 `robocopy /XF *.pack` 排除。
- J-Link 软件同理**不打包**：用本机已装的 SEGGER 驱动与 `JLink_x64.dll`，避免驱动与许可问题。

### 安装行为

- **按用户安装**，装到 `%LOCALAPPDATA%\Programs\McuRegDebugger`，**不触发 UAC**。
  这与程序自身的行为一致——它本来就把学到的 DEV_ID 写进 `%APPDATA%\MCUTools\McuRegDebugger`。
- 创建开始菜单组（程序 + 卸载）与桌面快捷方式；
- 控制面板「程序和功能」里有正式条目（DisplayName / 版本 / 发布者 / EstimatedSize / 静默卸载命令）；
- 卸载会移除程序目录、快捷方式和注册表项，但**保留 `%APPDATA%` 里的设置与学到的 DEV_ID**
  ——卸载不该悄悄丢掉用户教给它的板子信息；
- 静默安装：`McuRegDebugger-<版本>-setup.exe /S /D=<目录>`（`/D` 必须最后、且不加引号）。

### 自检安装包

```bat
dist\McuRegDebugger-1.1.1.0-setup.exe /S /D=C:\Temp\mrd_inst
C:\Temp\mrd_inst\mcuRegDebugger.exe --selftest --svd svd\N32H47x_DFP.1.2.0\N32H473.svd --device N32H473CE
```

实测：安装后目录 65 MB，自检 **419 PASS / 0 FAIL**，SVD 清单全部解析到安装目录内
（`<安装目录>\svd\...`），不依赖源码树。卸载后目录、快捷方式、注册表项均消失。

## 11. 已知限制

1. **J-Link 独占**：同一时刻只能有一个程序占用探针；本工具运行期间关闭 J-Flash/Keil 调试会话。
2. 本工具**不再启动 GDB Server**（该后端已删除），因此也不会去清理
   `JLinkGDBServerCL.exe` 进程 —— 机器上正在跑的那个多半属于 Keil / IAR。
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
