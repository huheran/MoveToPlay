# MoveToPlay PCB 复刻说明

本目录存放 MoveToPlay 各硬件模块的嘉立创 EDA 可编辑源工程和 PCB 预览图。每个 PCB 使用独立文件夹存放，文件夹名称同时表示模块、硬件版本及布局类型。

> 文件名中的 `Nromal` 和 `Botton` 是早期工程沿用的名称，含义分别为 `Normal`（标准版）和 `Bottom`（下置电池版）。为避免工程引用失效，仓库保留原文件名。

## 推荐复刻组合

如果只想体验 MoveToPlay 的体感控制和游玩原神，推荐先制作以下版本：

- `Blade_V1.0_Nromal`：不带心率检测，功能已经足够。
- Tracker V1.0：手工焊接选择 `Tracker_V1.0_Top-Battery`，使用 SMT 贴片生产选择 `Tracker_V1.0_Top-Battery_SMT`。
- `Dongle_SW1`、`Dongle_SW2` 或 `Dongle_SW3`：三者仅按键不同，任选一个即可。
- `充电底座`：需要集中充电时再制作。

采用这套基础组合时，不需要制作 `Blade_V2.0_HR(Heart Rate)` 和 `Heart_Rate_V2.0`。

## PCB 工程列表

| PCB 工程 | 用途与区别 |
| --- | --- |
| `Blade_V1.0_Nromal/Blade_V1.0_Nromal.eprj2` | Blade V1.0 标准版，不带心率传感器，推荐基础复刻使用 |
| `Blade_V2.0_HR(Heart Rate)/Blade_V2.0_HR(Heart Rate).eprj2` | Blade V2.0 心率版，增加心率传感器相关接口 |
| `Heart_Rate_V2.0/Heart_Rate_V2.0.eprj2` | Blade V2.0 使用的独立 MAX30102 心率传感器板 |
| `Tracker_V1.0_Top-Battery/Tracker_V1.0_Top-Battery.eprj2` | Tracker V1.0 上置电池版，为方便手工焊接而设计的器件布局 |
| `Tracker_V1.0_Top-Battery_SMT/Tracker_V1.0_SMT_Top-Battery.eprj2` | Tracker V1.0 上置电池 SMT 版，器件经过标准化，方便贴片生产 |
| `Tracker_V2.0_Botton-Battery/Tracker_V2.0_Botton-Battery.eprj2` | Tracker V2.0 下置电池版，为方便手工焊接而设计的器件布局 |
| `Tracker_V2.0_Botton-Battery_SMT/Tracker_V2.0_SMT_Botton-Battery.eprj2` | Tracker V2.0 下置电池 SMT 版，器件经过标准化，方便贴片生产 |
| `Dongle_SW1/Dongle_SW1.eprj2` | USB Dongle 第一种按键版本 |
| `Dongle_SW2/Dongle_SW2.eprj2` | USB Dongle 第二种按键版本 |
| `Dongle_SW3/Dongle_SW3.eprj2` | USB Dongle 第三种按键版本 |
| `充电底座/充电底座.eprj2` | Tracker 集中充电底座 |

## Blade V1.0 与 V2.0

Blade V2.0 在 V1.0 的基础上增加了心率传感器功能。心率检测不是体感控制原神所必需的功能，因此只体验游戏控制时使用 Blade V1.0 即可。

为了让心率传感器能够从 Blade 外壳开孔中露出，传感器被单独放在 `Heart_Rate_V2.0` PCB 上，并通过堆叠安装提高传感器位置。只有制作 Blade V2.0 心率版时才需要该 PCB；选择 Blade V1.0 时可忽略整个 `Heart_Rate_V2.0` 文件夹。

## Tracker V1.0 与 V2.0

Tracker V1.0 与 V2.0 的区别主要是 LED 发光方向和电池安装位置：

- V1.0 为 `Top-Battery`（电池上置）版本。
- V2.0 为 `Botton-Battery`（电池下置）版本。

两种版本的结构尺寸和安装位置不同，必须搭配相同版本的 3D 模型：V1.0 PCB 对应 V1.0 模型，V2.0 PCB 对应 V2.0 模型，不建议跨版本混装。

每个 Tracker 版本又提供两种 PCB 布局：

- 不带 `_SMT` 后缀：为方便手工焊接而调整的布局，适合个人复刻和调试。
- 带 `_SMT` 后缀：器件经过标准化，便于 SMT 贴片生产。

同一版本的手焊版和 SMT 版功能相同，并使用同一版本的 3D 外壳。选择哪一种只取决于焊接与生产方式。

## Dongle SW1、SW2 与 SW3

三个 Dongle 工程仅使用的按键不同，其他电路和功能没有区别。复刻时可以根据手头已有的按键或个人偏好任选一个版本。

## PCB 与 3D 模型对应关系

| PCB | 对应 3D 模型目录 |
| --- | --- |
| `Blade_V1.0_Nromal` | `../3d-models/solidworks建模/Blade剑身V1.0/`，并搭配 `Blade剑柄/` 和 `Blade上盖/` |
| `Blade_V2.0_HR(Heart Rate)` + `Heart_Rate_V2.0` | `../3d-models/solidworks建模/Blade剑身V2.0（心率检测）/`，并搭配 `Blade剑柄/` 和 `Blade上盖/` |
| `Tracker_V1.0_Top-Battery` 或 `Tracker_V1.0_Top-Battery_SMT` | `../3d-models/solidworks建模/Tracker上半面V1.0/` + `Tracker下半面V1.0/` |
| `Tracker_V2.0_Botton-Battery` 或 `Tracker_V2.0_Botton-Battery_SMT` | `../3d-models/solidworks建模/Tracker上半面V2.0（电池下置亚克力版本）/` + `Tracker下半面V2.0（电池下置亚克力版）/` |
| `充电底座` | `../3d-models/solidworks建模/充电底座/` |

更详细的模型文件说明请参阅 [`../3d-models/README.md`](../3d-models/README.md)。

## 打开与生产

`.eprj2` 是嘉立创 EDA 可编辑工程。克隆仓库后如果文件内容不完整，请先获取 Git LFS 文件：

```bash
git lfs install
git lfs pull
```

仓库中的 PNG 可用于快速查看 PCB 正反面或布局，但不能代替生产文件。正式投板前，请在嘉立创 EDA 中重新检查原理图、板层、板厚、铜厚、封装和设计规则，并导出及核对 Gerber、钻孔文件、BOM、贴片坐标和原理图 PDF。建议先少量打样并完成通电测试，再进行批量生产。
