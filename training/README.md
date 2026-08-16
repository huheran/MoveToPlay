# MoveToPlay 随机森林训练指南

本目录提供从 IMU 采集数据到 Dongle 固件随机森林 C 数组的完整可复现流水线。当前正式模型由两个随机森林组成：

- `state_rf_15_full`：识别 idle、walk、run、move_noise、right_hand_slash 等持续状态。
- `event_rf`：识别 jump、kick、抬手、转向等离散动作事件。

模型参数和质量门禁统一定义在 [`pipeline.json`](pipeline.json)。CNN 试验脚本不属于本指南，也不是当前固件的标准训练流程。

## 1. 环境要求

- Windows 10/11 与 PowerShell，或支持 Docker 的系统。
- Python 3.11 及以上版本。
- 采集真实硬件数据时需要 MoveToPlay Dongle 和四个 Tracker。
- 重新编译 Dongle 固件时需要 ESP-IDF 5.5.x。

克隆仓库后先下载 Git LFS 管理的数据和 PCB 工程：

```powershell
git lfs install
git lfs pull
```

## 2. 使用公开数据训练

仓库公开两套数据快照，说明见 [`../data/README.md`](../data/README.md)：

- `movetoplay-latest-v2`：默认数据集，用于新实验。
- `movetoplay-deployed-v1`：当前固件已部署模型使用的数据快照。

### Windows 一键验证

第一次运行会创建 `.venv-training` 并安装固定版本依赖：

```powershell
powershell -ExecutionPolicy Bypass -File training/run-local.ps1 -ValidateOnly -Reinstall
```

验证会检查 CSV 表头、文件大小、SHA-256、行数、会话数、四节点覆盖、标签分布、数值完整性和事件 ID 唯一性。

### Windows 一键训练

```powershell
powershell -ExecutionPolicy Bypass -File training/run-local.ps1
```

### 精确复现当前已部署模型

```powershell
powershell -ExecutionPolicy Bypass -File training/run-local.ps1 `
  -DatasetManifest training/datasets/movetoplay-deployed-v1.json `
  -RequireReferenceMatch
```

`-RequireReferenceMatch` 会要求新生成的 C 数组与仓库当前已部署数组一致，适合发布前审计。

精确复现必须使用 `training/requirements.txt` 固定的 scikit-learn 1.7.2。请通过 `run-local.ps1` 创建的 `.venv-training` 运行，不要直接使用安装了其他 scikit-learn 版本的系统 Python；即使准确率相同，不同版本也可能生成内部结构不同的随机森林数组。

## 3. 采集自己的动作数据

### 方法 A：使用 Windows Companion

运行桌面端：

```powershell
dotnet run --project companion/MoveToPlay.Companion/MoveToPlay.Companion.csproj
```

在 Dongle 上长按按钮约 2 秒切换到橙色数据采集模式，然后进入 Companion 的“数据采集与云端训练”页面。选择动作、开始会话并使用 Blade 或界面标记事件。每次会话会保存 samples CSV 和 events CSV。

### 方法 B：使用命令行采集

安装采集依赖：

```powershell
python -m pip install -r training/requirements-collection.txt
```

查看串口号后开始采集：

```powershell
python tools/collect_imu_events.py `
  --port COM6 `
  --baud 115200 `
  --output data/my-sessions/session_001.csv `
  --autostart
```

工具会同时生成 `session_001.csv` 和 `session_001_events.csv`。运行时会打印状态标签和事件标记快捷键；采集过程中应保证四个 Tracker 都在线。

建议每个动作采集多个人、多次会话，并保留 idle、walk、run 和 move_noise 等负样本。训练/测试划分按 `session_id` 分组，避免同一会话同时进入训练集和测试集造成数据泄漏。

## 4. 合并会话并生成 manifest

将准备训练的会话放入一个独立目录，然后运行：

```powershell
python tools/prepare_rf_dataset.py `
  --input-dir data/my-sessions `
  --output-dir data/processed/my-dataset `
  --dataset-id my-dataset
```

该命令会递归查找 samples/events CSV，输出：

```text
data/processed/my-dataset/
├── samples.csv
├── events.csv
└── dataset_manifest.json
```

如果已经自行合并好两份 CSV，也可以只生成 manifest：

```powershell
python tools/build_dataset_manifest.py `
  --samples data/processed/my-dataset/samples.csv `
  --events data/processed/my-dataset/events.csv `
  --dataset-id my-dataset `
  --output data/processed/my-dataset/dataset_manifest.json
```

manifest 会记录输入路径、字节数、SHA-256、行数、节点和标签分布。数据发生任何变化后都必须重新生成 manifest，不能手工沿用旧哈希。

## 5. 训练自己的数据集

先验证：

```powershell
powershell -ExecutionPolicy Bypass -File training/run-local.ps1 `
  -DatasetManifest data/processed/my-dataset/dataset_manifest.json `
  -ValidateOnly
```

验证通过后训练：

```powershell
powershell -ExecutionPolicy Bypass -File training/run-local.ps1 `
  -DatasetManifest data/processed/my-dataset/dataset_manifest.json
```

默认 `pipeline.json` 要求公开基线中的全部状态和事件类别。如果自定义数据只包含部分动作，需要同时调整 `required_classes` 和相关训练参数；不要为了通过门禁而简单降低指标，应先检查采集数量、标签时序和类别平衡。

## 6. 训练输出

每次执行都会创建独立目录，不会直接覆盖固件：

```text
output/training-runs/<run-id>/
├── dataset_validation.json
├── run_manifest.json
├── logs/
├── models/
│   ├── state_rf_15_full/
│   └── event_rf/
├── generated/
└── quality/
```

`run_manifest.json` 记录 Git 提交、工作区状态、Python 与依赖版本、数据/配置哈希、模型指标、质量门禁和所有产物哈希。只有两个模型都通过 `pipeline.json` 中的门禁，运行状态才会是 `passed`。

## 7. 将模型安装到 Dongle 固件

先使用 dry-run 检查训练产物：

```powershell
python tools/install_rf_model.py `
  --run-dir output/training-runs/<run-id> `
  --dry-run
```

确认后安装四个 C/H 数组：

```powershell
python tools/install_rf_model.py `
  --run-dir output/training-runs/<run-id>
```

脚本会把状态模型和事件模型复制到 `main/generated/`。随后重新编译 Dongle 固件：

```powershell
idf.py -B build-dongle `
  -D "SDKCONFIG=build-dongle/sdkconfig" `
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults.8mb" `
  build
```

正式使用前应在未参与训练的新会话上验证误触发、漏识别和实时性能，并先保留上一版可回滚固件。

## 8. Docker 复现

在仓库根目录构建镜像：

```powershell
docker build -f training/Dockerfile -t movetoplay-training:1 .
```

使用公开默认数据训练：

```powershell
docker run --rm `
  --mount "type=bind,src=$PWD/data,dst=/workspace/data,readonly" `
  --mount "type=bind,src=$PWD/output,dst=/workspace/output" `
  movetoplay-training:1
```

训练数据不会被打包进 Docker 镜像，必须在运行时只读挂载；输出目录单独可写挂载。

## 9. 数据格式

samples CSV 必需列：

```text
pc_timestamp_ms,board_timestamp_ms,node_id,ax,ay,az,gx,gy,gz,state_label,session_id
```

events CSV 必需列：

```text
event_id,event_group,event_type,pc_timestamp_ms,state_label,session_id
```

`node_id` 固定为 `1=胸部`、`2=右手`、`3=左手`、`4=腿部`。新采集事件推荐使用全局唯一 `event_id`；历史数据允许在 `session_id` 范围内唯一。

## 10. 许可证

- 训练代码、脚本和文档使用 Apache License 2.0。
- `data/README.md` 明确列出的公开数据使用 CC0 1.0 Universal。

许可证详情见仓库根目录 [`LICENSE`](../LICENSE) 和数据目录 [`../data/LICENSE`](../data/LICENSE)。
