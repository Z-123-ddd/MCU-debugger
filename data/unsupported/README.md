# data/unsupported —— 暂不支持的厂商数据（归档）

这里放的是**框架能处理、但目前不做验证与支持**的厂商数据。

当前状态（v1.1.1.0）：

| 厂商 | 状态 | 说明 |
| --- | --- | --- |
| Nationstech N32 | ✅ 支持 | `data/device_ids.json` + `data/packages/`，已用 N32H473CE 真机实测 |
| STM32 | ⏸ 归档 | 识别条目与封装表在此目录，未附带 SVD，**不做验证** |

## 为什么保留而不是删掉

代码路径本身是通用框架，不写死任何厂商：

- `DeviceIdEntry` 有 `vendor` / `family` / `devId` / `idcodeAddress` 字段；
- `GpioCodec` 同时支持三套 GPIO 寄存器布局（CRL/CRH、MODER/OTYPER/PUPDR、BSRR/BRR），
  N32G 系列走的正是 STM32F1 风格的 CRL/CRH；
- 型号 → SVD 走 `svd/manifest.json`，与厂商无关；
- 封装引脚表是 `data/packages/*.json`，schema 通用。

所以将来要支持 STM32，只需要补数据，不需要改代码。

## 如何重新启用

```bat
:: 1) 识别条目：拷回补充表目录（主表优先，不会被覆盖）
mkdir data\device_ids.d
copy data\unsupported\device_ids_stm32.json data\device_ids.d\

:: 2) 封装引脚表：拷回 packages 目录
copy data\unsupported\packages\STM32F103C8_LQFP48.json data\packages\

:: 3) SVD：补齐 STM32 的 .svd 并登记到 svd/manifest.json
```

`data/device_ids.d/*.json` 是主表之外的自定义识别表，启动时按文件名顺序合并，
schema 与 `data/device_ids.json` 完全一致。主表里的条目优先，
同一 `devId` + 型号不会被补充表重复定义。

注意：归档的封装表里 `PD0`/`PD1` 标为 `type: clock`（实为 OSC_IN/OSC_OUT），
按现行规则整个 PD 端口会归为"仅专用功能"，不显示为 GPIO 组；
如果将来 STM32F103C8 实际要用 PD0/PD1 作 GPIO，需要按数据手册改这一处的 `type`。
