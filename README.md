# MoveToPlay

MoveToPlay 是一个把真实肢体运动转换为游戏输入的软硬件一体项目。系统由 ESP32-S3 设备、Windows 桌面端、动作模型训练流水线和云端训练服务组成：Tracker 采集身体节点的 IMU 数据，Dongle 汇总无线数据并执行动作识别，Companion 在电脑端展示运动状态、游戏悬浮层、数据采集和模型更新流程。

本仓库面向项目源码公开与二次开发，包含固件、桌面端、训练脚本、服务端和安装包构建脚本。训练数据、服务器密钥、API Token、安装包产物和本地构建输出不应提交到仓库。

## 功能概览

- 多节点动作采集：胸部、左右手和腿部 Tracker 通过 ESP-NOW 向 Dongle 发送 IMU 数据。
- 实时动作识别：Dongle 侧集成随机森林动作模型，可输出键盘/鼠标事件。
- 游戏悬浮层：Windows Companion 提供透明置顶悬浮层，展示动作、强度、心率、卡路里和激励提示。
- 数据采集闭环：Companion 支持动作事件标记、历史采集会话管理、上传训练和固件回滚。
- 模型训练流水线：本地或容器内复现 processed CSV 到模型产物、C 数组和质量报告的流程。
- 云端训练服务：API 与 worker 分离，负责数据校验、异步训练、质量门禁、固件编译与模型版本管理。
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
├── server/                       # 云端训练 API、worker 与部署脚本
├── installer/                    # Windows 安装包构建脚本
├── tools/                        # 数据采集、分析、训练和导出工具
├── analysis/                     # 动作数据诊断脚本
├── partitions*.csv               # ESP32-S3 分区表
└── sdkconfig.defaults*           # ESP-IDF 默认配置
```

## 环境要求

- ESP-IDF 5.5.x，用于构建 ESP32-S3 固件。
- .NET 8 SDK，用于运行和构建 Windows Companion。
- Python 3.11，用于训练、分析和服务端开发。
- Docker，用于复现训练环境或运行云端训练服务。
- Windows 10/11，用于 Companion、游戏悬浮层和安装包验证。

不同模块可以独立开发；只调试 Companion 时不需要安装 ESP-IDF，只运行训练脚本时也不需要连接硬件。

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

## Windows Companion

Companion 是 Windows 桌面端，源码位于 `companion/MoveToPlay.Companion/`。

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

训练数据默认不进入 Git。数据集 manifest 会记录路径、大小、行数、标签分布和 SHA-256，便于复现和审计。详细说明见 [training/README.md](training/README.md)。

## 云端训练服务

服务端代码位于 `server/`，由 API 容器和 worker 容器组成，共享 SQLite 状态库和持久化目录。服务负责数据上传校验、异步训练、质量门禁、模型审批、固件编译和产物下载。

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

- 不提交 `data/`、`output/`、构建目录、训练产物、安装包产物和本地密钥。
- 不提交真实 API Token、SSH 私钥、OSS Access Key 或服务器环境文件。
- 不把个人采集数据、未脱敏日志或客户/队友内部配置放入仓库。
- 发布前检查 `.gitignore` 与 Git 状态，确认只包含可公开源码和文档。

## 许可证

本仓库当前尚未声明开源许可证。公开前请根据预期授权方式补充 `LICENSE` 文件；在许可证明确前，默认保留所有权利。
