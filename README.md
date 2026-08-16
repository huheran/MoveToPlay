<p align="center">
  <img src="docs/images/poster.png" alt="MoveToPlay" width="800" />
</p>

# MoveToPlay

> 把身体变成手柄。

[English](README.en.md) · 简体中文

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)
[![SoC](https://img.shields.io/badge/SoC-ESP32--S3-8b5cf6.svg)]()
[![Python](https://img.shields.io/badge/Python-3.11%2B-3776AB.svg)]()
[![.NET](https://img.shields.io/badge/.NET-8-512BD4.svg)]()

MoveToPlay 是一套基于 ESP32-S3 的开源体感外设。你在胸口、两只手、腿上各绑一个 Tracker，手里拿一把剑形的 Blade，插在电脑上的 Dongle 就能看懂你在跳、在挥剑、在奔跑，然后替你按下对应的按键。不用碰键盘和手柄，用身体直接打游戏。

## 目录

- [它能干什么](#它能干什么)
- [三个设备](#三个设备)
- [怎么玩](#怎么玩)
- [硬件](#硬件)
- [固件](#固件)
- [Windows Companion](#windows-companion)
- [训练自己的动作](#训练自己的动作)
- [云端训练服务](#云端训练服务)
- [目录结构](#目录结构)
- [许可证](#许可证)

## 它能干什么

对着屏幕挥一下剑，游戏里的角色就砍一刀；原地跳一下，角色就跳起来；迈步走路，角色就开始走。识别到的动作会变成真实的键盘、鼠标输入，所以理论上任何 PC 游戏都能用，只要把动作映射到对应的键。

默认键位是照着《原神》调的，开箱就能玩：

| 动作 | 默认输出 |
| --- | --- |
| 空闲 / 动作噪声 | 无 |
| 行走 | 按住 W |
| 奔跑 | 按住 Shift + W |
| 右手挥砍 | 鼠标左键连点 |
| 跳跃 | 空格 |
| 踢 | E |
| 左手抬起 | M |
| 右手抬起 | X |
| 双手交叉额头 | 角色键 1–4 循环 |
| 下压 | F |
| 射击 | 鼠标右键定时按住 |
| 左转 / 右转 | 视角左转 / 右转 |
| 奥特曼光线 | Q |

这套映射不是写死在固件里的。Dongle 有个 Wi-Fi 配置页，连上之后每个动作都能单独调输出、触发方式和阈值，改完保存在设备上，具体见[固件](#固件)。

## 三个设备

整套系统就三种硬件，各司其职：

- **Tracker × 4** —— 绑在胸口、右手、左手、腿上。里面是一颗 LSM6DSV 六轴 IMU，以 100 Hz 采样，通过 ESP-NOW 无线发给 Dongle。
- **Blade** —— 剑形控制器，一颗按键加一块 MAX30102 心率传感器。按住按键转动身体，胸口 Tracker 的陀螺仪就把转身和俯仰转成鼠标视角；采集数据时，这按键用来给动作打点。
- **Dongle** —— 插在电脑上的接收器。汇总所有节点的数据，在设备上跑随机森林识别动作，再伪装成 USB 键盘 + 鼠标输出。同时也是 USB 虚拟串口，供 Companion 读遥测、采数据、烧固件。

识别分两个随机森林：一个负责「持续状态」（空闲 / 走路 / 跑步 / 挥剑 / 动作噪声），50 棵树、5 个类别；另一个负责「瞬间动作」（跳、踢、抬手这些），180 棵树、15 个类别。特征都是 812 维，全部在 ESP32-S3 上本地跑，不依赖电脑端做任何识别。

## 怎么玩

最快的一条路：

1. **做硬件**。按下面的推荐组合打样 PCB、打印外壳、焊接。图纸在 `hardware/`。
2. **烧固件**。装好 ESP-IDF，把 6 个设备——1 个 Dongle、1 个 Blade、4 个 Tracker——依次编译烧录。
3. **绑上就玩**。Dongle 插电脑，Tracker 绑身上，开游戏。

只用官方公开的数据和模型的话，不用碰任何训练脚本。想用自己的动作重新训练，才需要 Python 和训练那套东西。

## 硬件

PCB 源工程和 3D 模型都在 `hardware/` 下，PCB 用的是嘉立创 EDA（`.eprj2`），外壳提供 SolidWorks / STEP / STL 三种格式。首次复刻推荐功能够用、结构最简单的 V1.0 组合：

- **Blade**：`Blade_V1.0_Nromal`（不带心率，文件名里的 `Nromal` 是早期拼写，保留原样）
- **Tracker**：手焊选 `Tracker_V1.0_Top-Battery`，贴片选 `Tracker_V1.0_Top-Battery_SMT`
- **Dongle**：`Dongle_SW1` / `Dongle_SW2` / `Dongle_SW3` 任选一个，只有按键不一样
- 充电底座，可选

这套组合不需要 `Blade_V2.0_HR(Heart Rate)` 和 `Heart_Rate_V2.0`。PCB 和外壳要按版本对应：V1.0 PCB 配 V1.0 模型，V2.0 配 V2.0，别混用。具体的工程清单、版本差异和对应关系见 [`hardware/pcb/README.md`](hardware/pcb/README.md) 和 [`hardware/3d-models/README.md`](hardware/3d-models/README.md)。

大文件走 Git LFS，克隆完先拉一下：

```bash
git lfs install
git lfs pull
```

投板前记得自己在嘉立创 EDA 里核对一遍 Gerber、钻孔、BOM、坐标和原理图，打印外壳前也核对尺寸和公差。

## 固件

固件工程在仓库根目录，ESP-IDF 5.5.x 就能编译：

```bash
idf.py build
idf.py -p <串口> flash monitor
```

三种设备共用同一份代码，靠 `main/app_main.c` 顶部的 `M2P_BOARD_PROFILE` 区分。改一次数字、烧一次：

| 值 | 设备 |
| --- | --- |
| `1` | Dongle |
| `2` | Blade |
| `3` | 胸口 Tracker |
| `4` | 右手 Tracker |
| `5` | 左手 Tracker |
| `6` | 腿部 Tracker |

```c
#define M2P_BOARD_PROFILE 1
```

推荐组合一共要烧 6 个设备，就是把上面这个数字从 1 到 6 各烧一遍。默认按 8 MB flash 构建（16 MB 的 Dongle 也能用），要显式指定的话：

```bash
# 8MB
idf.py -B build-tracker8mb \
  -D "SDKCONFIG=build-tracker8mb/sdkconfig" \
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults.8mb" \
  build

# 16MB
idf.py -B build-dongle16mb \
  -D "SDKCONFIG=build-dongle16mb/sdkconfig" \
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults.16mb" \
  build
```

Dongle 上长按按键 2 秒，会在三种运行状态之间循环，靠 LED 颜色区分：**绿 = 游戏**、**橙 = 采集**、**蓝 = Wi-Fi 维护**。进了蓝色状态后，手机或电脑连上 `MoveToPlay-Dongle` 这个热点（无密码），打开 `http://192.168.4.1/` 就能看设备状态、改动作映射。

配置页里每个动作可以设成：按一下键盘、按住键盘、角色键 1–4 循环、鼠标左键点击、鼠标右键定时按住、鼠标左键按住、鼠标左移 / 右移、视角左转 / 右转。触发方式有四种：冷却、刚出现、持续帧、重复。还能单独调每个动作的置信度阈值（30–95%）和冷却时间，配置存在设备 NVS 里，断电不丢。

## Windows Companion

Companion 是可选的 Windows 桌面端，面向买成品的人；自己复刻、只想体验体感控制的话可以不用装。它干这些事：

- 一个透明置顶、鼠标穿透的游戏悬浮层，显示当前动作、强度、心率、卡路里和连击
- 8 套主题：原神、鸣潮、我的世界、艾尔登法环、GTA V、赛博朋克 2077、通用、科幻竞技
- 动作事件标记、采集会话管理、上传训练、固件下载与回滚

需要 .NET 8 SDK 和 Windows 10/11：

```powershell
dotnet run --project companion/MoveToPlay.Companion/MoveToPlay.Companion.csproj
```

没有硬件时加 `--demo` 用模拟数据预览界面：

```powershell
dotnet run --project companion/MoveToPlay.Companion/MoveToPlay.Companion.csproj -- --demo
```

它会自动找 USB VID `303A`、PID `4005` 的 Dongle，设备插拔后自动重连。运行中 Dongle 每 100 ms 发一行 JSON 遥测——当前动作、置信度、强度、四个 Tracker 的在线和电量——Companion 拿来做悬浮层和统计。

点「结束运动并测量心率」会唤醒 Blade，用 MAX30102 测 10 秒心率；卡路里按动作 MET、强度和体重（默认 68 kg，`--weight` 可改）估算，不是医疗级数据。目前 Companion 只能烧 Dongle 固件，Blade 和 Tracker 还是得用 ESP-IDF 自己编译烧录。更多细节见 [`companion/MoveToPlay.Companion/README.md`](companion/MoveToPlay.Companion/README.md)。

## 训练自己的动作

想用自己的动作数据重新训练模型，流程是：采集 → 合并 → 训练 → 装进固件。用官方公开数据的话，跳过前两步，直接训练。

先准备环境。本地训练要 Python 3.11+，Windows 上一键搞定（第一次会建 `.venv-training` 并装好固定版本依赖）：

```powershell
# 先验证数据和环境
powershell -ExecutionPolicy Bypass -File training/run-local.ps1 -ValidateOnly -Reinstall
# 再训练
powershell -ExecutionPolicy Bypass -File training/run-local.ps1
```

也可以用 Docker，结果一致：

```powershell
docker build -f training/Dockerfile -t movetoplay-training:1 .
docker run --rm `
  --mount "type=bind,src=$PWD/data,dst=/workspace/data,readonly" `
  --mount "type=bind,src=$PWD/output,dst=/workspace/output" `
  movetoplay-training:1
```

每次训练都会生成一个独立目录，不会直接覆盖固件里的模型。训练通过质量门禁后，把产物装进固件、重新编译 Dongle：

```powershell
python tools/install_rf_model.py --run-dir output/training-runs/<run-id> --dry-run
python tools/install_rf_model.py --run-dir output/training-runs/<run-id>
idf.py -B build-dongle \
  -D "SDKCONFIG=build-dongle/sdkconfig" \
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults.8mb" \
  build
```

**采集自己的数据**：用 Companion 采集，或者命令行：

```powershell
python -m pip install -r training/requirements-collection.txt
python tools/collect_imu_events.py --port COM6 --baud 115200 --output data/my-sessions/session_001.csv --autostart
```

**合并并生成 manifest**：

```powershell
python tools/prepare_rf_dataset.py --input-dir data/my-sessions --output-dir data/processed/my-dataset --dataset-id my-dataset
```

**训练自己的数据集**：

```powershell
powershell -ExecutionPolicy Bypass -File training/run-local.ps1 -DatasetManifest data/processed/my-dataset/dataset_manifest.json
```

数据格式很简单：samples 一行是 `pc_timestamp_ms,board_timestamp_ms,node_id,ax,ay,az,gx,gy,gz,state_label,session_id`，events 一行是 `event_id,event_group,event_type,pc_timestamp_ms,state_label,session_id`。`node_id` 固定 1=胸、2=右手、3=左手、4=腿，四个节点都得有数据。

公开数据由 Git LFS 管理，manifest 记录了大小、行数、标签分布和 SHA-256，训练前会逐项校验。两个模型都得过质量门禁才算通过：状态模型准确率 ≥ 0.98，事件模型准确率 ≥ 0.97、macro-F1 ≥ 0.96。还有个关键点：复现已部署模型必须用 `requirements.txt` 里固定的 scikit-learn 1.7.2，换个版本即使准确率一样，生成的数组内部结构也可能不同。完整的采集、合并、验证、Docker 复现和格式说明都在 [`training/README.md`](training/README.md)。

## 云端训练服务

不是复刻的必需步骤。`server/` 是一个 API + worker 分离的云端训练服务，共享 SQLite 状态库，负责数据校验、异步训练、质量门禁、固件编译和版本管理。API 只绑定 `127.0.0.1:8000`，走 SSH 隧道，业务接口要 Bearer Token。

人一多任务会排队，网络不稳也容易失败，所以现阶段建议本地训练。本地起一个做开发测试：

```powershell
python -m pip install -r server/requirements-dev.txt
Push-Location server
python -m pytest
Pop-Location
```

接口说明见 [`server/API_PROTOCOL.md`](server/API_PROTOCOL.md)，部署说明见 [`server/README.md`](server/README.md)。真实令牌、私钥、OSS 凭据都只放在服务器上，不要进 Git。

## 目录结构

```text
MoveToPlay/
├── main/             # ESP-IDF 固件（三种设备共用，generated/ 放模型 C 数组）
├── companion/        # Windows WPF 桌面端 + 冒烟测试
├── training/         # 训练流水线、Docker 环境和公开数据 manifest
├── data/             # Git LFS 管理的公开随机森林训练数据
├── server/           # 云端训练 API 与 worker
├── installer/        # Windows 安装包构建脚本（仅内部分发用）
├── hardware/
│   ├── pcb/          # 嘉立创 EDA PCB 源工程
│   └── 3d-models/    # SolidWorks / STEP / STL 结构模型
├── tools/            # 采集、分析、训练、导出脚本
├── analysis/         # 动作数据诊断脚本
├── partitions*.csv   # ESP32-S3 分区表
└── sdkconfig.defaults*   # ESP-IDF 默认配置
```

## 许可证

分目录授权：

- 软件、固件、训练工具、服务端和文档：Apache License 2.0（[`LICENSE`](LICENSE)、[`NOTICE`](NOTICE)）
- PCB 设计：CERN Open Hardware Licence v2 – Permissive（[`hardware/pcb/LICENSE`](hardware/pcb/LICENSE)）
- 3D 模型：CC0 1.0（[`hardware/3d-models/LICENSE`](hardware/3d-models/LICENSE)）
- 公开训练数据：CC0 1.0（[`data/LICENSE`](data/LICENSE)）