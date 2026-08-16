# MoveToPlay

MoveToPlay 是一个把真实肢体运动转换为游戏输入的软硬件一体项目。核心系统由 ESP32-S3 设备组成：Tracker 采集身体节点的 IMU 数据，Dongle 汇总无线数据、执行动作识别并输出游戏操作。仓库还提供可选的 Windows Companion、本地随机森林训练工具和云端训练服务，用于状态展示、数据采集和模型更新等扩展功能。

本仓库面向项目源码公开与二次开发，包含固件、桌面端、随机森林训练数据与脚本、服务端和安装包构建脚本。服务器密钥、API Token、安装包产物、本地个人采集会话和构建输出不应提交到仓库。

## 功能概览

- 多节点动作采集：胸部、左右手和腿部 Tracker 通过 ESP-NOW 向 Dongle 发送 IMU 数据。
- 实时动作识别：Dongle 侧集成随机森林动作模型，可输出键盘/鼠标事件。
- 可选游戏悬浮层：Windows Companion 提供透明置顶悬浮层，展示动作、强度、心率、卡路里和激励提示。
- 可选数据采集闭环：Companion 支持动作事件标记、历史采集会话管理、上传训练和固件回滚。
- 模型训练流水线：本地或容器内复现 processed CSV 到模型产物、C 数组和质量报告的流程。
- 可选云端训练服务：API 与 worker 分离，负责数据校验、异步训练、质量门禁、固件编译与模型版本管理。
- Dongle 配置页：维护模式下可通过 Wi-Fi 页面调整动作到键鼠输出的映射、阈值和冷却时间。

## 目录结构

```text
MoveToPlay/
├── main/                         # ESP-IDF 固件源码
│   ├── app_main.c                # Dongle / Blade / Tracker 统一入口
│   └── generated/                # 模型导出的 C 数组
├── companion/
│   ├── MoveToPlay.Companion/     # Windows WPF 桌面端
│   └── MoveToPlay.Companion.Smoke/ # Companion 冒烟测试
├── training/                     # 可复现训练流水线和 Docker 环境
├── data/                         # Git LFS 管理的公开随机森林训练数据
├── server/                       # 云端训练 API、worker 与部署脚本
├── installer/                    # Windows 安装包构建脚本
├── hardware/pcb/                 # 嘉立创 EDA PCB 源工程与硬件版本说明
├── hardware/3d-models/           # SolidWorks、STEP 与 STL 结构模型
├── tools/                        # 数据采集、分析、训练和导出工具
├── analysis/                     # 动作数据诊断脚本
├── partitions*.csv               # ESP32-S3 分区表
└── sdkconfig.defaults*           # ESP-IDF 默认配置
```

## 环境要求

- ESP-IDF 5.5.x：自行复刻时必需，用于编译并烧录 Dongle、Blade 和 Tracker 固件；可以使用 VS Code 的 ESP-IDF 插件或 ESP-IDF 命令行环境。
- Python 3.11：需要在本地重新训练或分析随机森林模型时安装。
- .NET 8 SDK 与 Windows 10/11：仅在使用或开发 Windows Companion 时需要。
- Docker：仅在希望用容器复现训练环境或部署云端训练服务时需要。

不同模块可以独立使用。当前 Windows Companion 只实现了 Dongle 的代码烧录支持，无法代替完整的 ESP-IDF 开发环境；自行复刻 Blade 和 Tracker 时，仍需安装 ESP-IDF 并在本地完成编译与烧录。

## PCB 与 3D 模型

嘉立创 EDA PCB 源工程与详细版本说明位于 [`hardware/pcb/`](hardware/pcb/README.md)，SolidWorks、STEP 和 STL 结构模型位于 [`hardware/3d-models/`](hardware/3d-models/README.md)。目前包含：

- MoveToPlay Blade Standard 与 Blade HR
- Top-Battery 与 Bottom-Battery 两种 Motion Tracker，并分别提供手焊布局和 SMT 标准化布局
- 三种不同按键的 USB Dongle
- Charging Dock
- 独立 Heart-Rate Sensor

Blade 与 Tracker 的 PCB 和 3D 模型均按版本对应：V1.0 PCB 使用 V1.0 模型，V2.0 PCB 使用 V2.0 模型。Tracker 同一版本的手焊版和 SMT 版使用同一套外壳。

公开的 `.eprj2` 文件已移除本地账号元数据，并使用 Git LFS 管理。正式投板前仍应核对 Gerber、钻孔文件、BOM、贴片坐标和原理图 PDF；打印外壳前也应检查模型尺寸、公差和接口方向。

### 官方推荐复刻组合

第一次复刻建议使用功能够用、结构更简单的 V1.0 组合：

- `Blade_V1.0_Nromal`（仓库保留了早期文件名中的 `Nromal` 拼写）：不带心率传感器。
- Tracker V1.0：手工焊接选择 `Tracker_V1.0_Top-Battery`，SMT 贴片生产选择 `Tracker_V1.0_Top-Battery_SMT`；每个佩戴位置各制作一块。
- `Dongle_SW1`、`Dongle_SW2` 或 `Dongle_SW3`：三者只有按键不同，任选一个即可。
- 充电底座。

这套组合不需要 `Blade_V2.0_HR(Heart Rate)` 和 `Heart_Rate_V2.0`。选择 PCB 后，请使用相同版本的 3D 模型，具体对应关系见 [PCB 复刻说明](hardware/pcb/README.md)。

## 固件构建

固件工程位于仓库根目录，使用 ESP-IDF 构建：

```bash
idf.py build
idf.py -p <串口> flash monitor
```

设备角色在 `main/app_main.c` 顶部通过 `M2P_BOARD_PROFILE` 选择：

| 值 | 角色 |
| --- | --- |
| `1` | Dongle |
| `2` | Blade |
| `3` | 胸部 Tracker |
| `4` | 右手 Tracker |
| `5` | 左手 Tracker |
| `6` | 腿部 Tracker |

目前需要在烧录每种设备前手动修改该数字。例如：

```c
#define M2P_BOARD_PROFILE 1
```

将其依次改为上表中的对应值，再分别执行编译和烧录。推荐组合通常需要烧录 1 个 Dongle、1 个 Blade，以及胸部、右手、左手和腿部共 4 个 Tracker。后续可能会简化角色选择和烧录流程，但当前复刻请以这种方式操作。

默认配置按 8MB flash 构建，可兼容 16MB Dongle。如果需要显式使用不同 flash 配置：

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

Dongle 固件支持 Play、数据采集和 Wi-Fi 维护状态切换。进入 Wi-Fi 维护状态后连接 `MoveToPlay-Dongle` 热点，访问 `http://192.168.4.1/` 可查看设备状态并编辑动作输出配置。

## Windows Companion（可选）

Companion 是非必需的 Windows 桌面端，主要面向购买 MoveToPlay 成品的用户；自行复刻并体验体感控制时，可以不安装。它用于显示已识别动作、运动强度、心率等状态，并提供数据采集、云端训练和模型管理等扩展功能。选择不带心率传感器的 Blade V1.0 时，心率显示自然不会产生数据，也不影响基本体感控制。

当前 Companion 的固件烧录功能仅支持 Dongle，不支持完整烧录 Blade 和各个 Tracker。自行复刻仍应安装 ESP-IDF，在 `main/app_main.c` 中选择设备角色后分别编译和烧录。源码位于 `companion/MoveToPlay.Companion/`。

```powershell
dotnet run --project companion/MoveToPlay.Companion/MoveToPlay.Companion.csproj
```

构建 Release：

```powershell
dotnet build companion/MoveToPlay.Companion/MoveToPlay.Companion.csproj -c Release
```

默认会自动查找 USB VID `303A`、PID `4005` 的 MoveToPlay Dongle。没有硬件时可以使用模拟模式预览界面：

```powershell
dotnet run --project companion/MoveToPlay.Companion/MoveToPlay.Companion.csproj -- --demo
```

更多使用说明见 [companion/MoveToPlay.Companion/README.md](companion/MoveToPlay.Companion/README.md)。

## 训练流水线

训练模块位于 `training/`，目标是把 processed CSV 数据转成可审计的模型产物、质量报告和固件 C 数组。每次运行会生成独立输出目录，不会直接覆盖固件源码中的模型文件。

如果需要用自己的数据训练动作模型，官方目前推荐优先使用本地训练：流程最直接，也不受服务器排队和网络状态影响。训练完成后，按照 [训练说明](training/README.md) 将生成的模型集成到固件，再使用 ESP-IDF 重新编译并烧录 Dongle。

Windows 本地运行：

```powershell
powershell -ExecutionPolicy Bypass -File training/run-local.ps1 -ValidateOnly -Reinstall
powershell -ExecutionPolicy Bypass -File training/run-local.ps1
```

Docker 运行：

```powershell
docker build -f training/Dockerfile -t movetoplay-training:1 .
docker run --rm `
  --mount "type=bind,src=$PWD/data,dst=/workspace/data,readonly" `
  --mount "type=bind,src=$PWD/output,dst=/workspace/output" `
  movetoplay-training:1
```

公开的规范训练数据由 Git LFS 管理。数据集 manifest 会记录路径、大小、行数、标签分布和 SHA-256，便于复现和审计；个人采集会话默认仍不进入 Git。完整的采集、合并、训练和固件集成说明见 [training/README.md](training/README.md)。

## 云端训练服务（可选）

云端训练不是复刻项目的必需步骤。服务端代码位于 `server/`，由 API 容器和 worker 容器组成，共享 SQLite 状态库和持久化目录。服务负责数据上传校验、异步训练、质量门禁、模型审批、固件编译和产物下载。

当同时训练的人较多时，云端任务可能排队较久，也可能受到服务器或网络状态影响而不够稳定，因此现阶段更建议使用本地训练。云端流程后续可能继续优化。

开发测试：

```powershell
python -m pip install -r server/requirements-dev.txt
Push-Location server
python -m pytest
Pop-Location
```

运行服务前复制并配置 `server/server.env.example` 中的环境变量。真实令牌、私钥、OSS 凭据和持久化数据目录应只保存在部署环境，不应提交到 Git。

接口协议见 [server/API_PROTOCOL.md](server/API_PROTOCOL.md)，服务说明见 [server/README.md](server/README.md)。

## 安装包

安装包脚本位于 `installer/`：

```powershell
pwsh -File installer/build-team-installer.ps1
```

该脚本面向受控分发场景，会注入本机外部保存的凭据并生成 Windows 安装程序。公开发布前应替换为独立的用户认证和 HTTPS API，不要把团队凭据或安装包产物提交到公开仓库。

## 公开仓库注意事项

- 只提交 `data/README.md` 中列出的规范公开数据；不提交个人会话、`output/`、构建目录、训练产物、安装包产物和本地密钥。
- 不提交真实 API Token、SSH 私钥、OSS Access Key 或服务器环境文件。
- 不把个人采集数据、未脱敏日志或客户/队友内部配置放入仓库。
- 发布前检查 `.gitignore` 与 Git 状态，确认只包含可公开源码和文档。

## 许可证

除子目录或第三方文件另有明确声明外：

- 软件、固件、训练工具、服务端和文档：Apache License 2.0，见 [`LICENSE`](LICENSE) 与 [`NOTICE`](NOTICE)。
- PCB 设计文件：CERN Open Hardware Licence Version 2 - Permissive，见 [`hardware/pcb/LICENSE`](hardware/pcb/LICENSE)。
- 3D 模型：CC0 1.0 Universal，见 [`hardware/3d-models/LICENSE`](hardware/3d-models/LICENSE)。
- 公开训练数据：CC0 1.0 Universal，见 [`data/LICENSE`](data/LICENSE)。

除许可证明确授予的权利外，MoveToPlay 的项目名称和标识不因代码开源而自动授予商标许可。
