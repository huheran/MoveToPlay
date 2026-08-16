# MoveToPlay 3D 模型复刻说明

本目录存放 MoveToPlay 外壳与结构件的 SolidWorks 源文件及通用交换格式。模型已经按照硬件模块和版本分类，复刻时请根据所选 PCB 版本进入对应文件夹。

## 文件格式

每个结构件通常提供三种格式：

- `.SLDPRT`：SolidWorks 可编辑源文件，适合修改结构设计。
- `.STEP`：通用 CAD 交换格式，适合导入其他建模软件、检查装配和继续编辑。
- `.STL`：三角网格模型，适合直接导入切片软件进行 3D 打印。

如果只需要打印外壳，通常下载 STL 即可；如果需要修改尺寸或适配其他元件，建议使用 SLDPRT 或 STEP。

## 模型目录

| 模型目录 | 用途 | 对应 PCB |
| --- | --- | --- |
| `solidworks建模/Blade剑身V1.0/` | 不带心率检测开孔的 Blade V1.0 剑身 | `../pcb/Blade_V1.0_Nromal/` |
| `solidworks建模/Blade剑身V2.0（心率检测）/` | 带心率检测结构的 Blade V2.0 剑身 | `../pcb/Blade_V2.0_HR(Heart Rate)/` + `../pcb/Heart_Rate_V2.0/` |
| `solidworks建模/Blade剑柄/` | Blade 剑柄结构件 | Blade V1.0 或 V2.0 |
| `solidworks建模/Blade上盖/` | Blade 上盖结构件 | Blade V1.0 或 V2.0 |
| `solidworks建模/Tracker上半面V1.0/` | Tracker V1.0 外壳上半部分 | 两种 Tracker V1.0 PCB |
| `solidworks建模/Tracker下半面V1.0/` | Tracker V1.0 外壳下半部分 | 两种 Tracker V1.0 PCB |
| `solidworks建模/Tracker上半面V2.0（电池下置亚克力版本）/` | Tracker V2.0 电池下置外壳上半部分 | 两种 Tracker V2.0 PCB |
| `solidworks建模/Tracker下半面V2.0（电池下置亚克力版）/` | Tracker V2.0 电池下置外壳下半部分 | 两种 Tracker V2.0 PCB |
| `solidworks建模/充电底座/` | Tracker 充电底座外壳 | `../pcb/充电底座/` |

## Blade 版本选择

- Blade V1.0 不带心率传感器。只体验 MoveToPlay 的体感控制和游玩原神时，建议选择该版本。
- Blade V2.0 带心率检测结构，需要同时制作 `Blade_V2.0_HR(Heart Rate)` 主 PCB 和独立的 `Heart_Rate_V2.0` 心率传感器 PCB。
- 心率传感器采用独立 PCB 堆叠安装，使传感器能够升高并从外壳开孔中露出。

Blade V1.0 PCB 必须搭配 `Blade剑身V1.0`，Blade V2.0 PCB 必须搭配 `Blade剑身V2.0（心率检测）`。剑柄和上盖为两种版本都会使用的结构件。

## Tracker 版本选择

Tracker 的 PCB 与外壳按版本一一对应：

- Tracker V1.0 PCB 对应 `Tracker上半面V1.0` 和 `Tracker下半面V1.0`。
- Tracker V2.0 PCB 对应 `Tracker上半面V2.0（电池下置亚克力版本）` 和 `Tracker下半面V2.0（电池下置亚克力版）`。

V1.0 与 V2.0 的 LED 发光方向、电池位置和外壳结构不同，请勿将 V1.0 PCB 与 V2.0 外壳混用，反之亦然。

每个 Tracker 版本均有手焊布局和 SMT 标准化布局。是否带 `_SMT` 后缀不会改变 3D 模型的版本：同为 V1.0 就使用 V1.0 外壳，同为 V2.0 就使用 V2.0 外壳。

## 制作建议

1. 先确定 Blade 和 Tracker 的 PCB 版本，再下载相同版本的模型。
2. 打印前在切片软件中检查模型单位和实际尺寸，确认模型未被自动缩放。
3. 根据打印机、材料和装配松紧度设置合理公差；首次制作建议先打印关键连接位置进行试装。
4. 安装前检查按键、LED、USB 接口、心率传感器和充电触点的开孔方向。
5. V2.0 Tracker 使用亚克力结构时，请根据实际板材厚度和加工公差检查装配。

## 许可证

本目录中的 3D 模型使用 CC0 1.0 Universal 发布，可以复制、修改、分发和使用，包括商业用途，无需事先申请许可或署名。详情见本目录的 [`LICENSE`](LICENSE)。
