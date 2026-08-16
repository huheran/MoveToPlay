# MoveToPlay 随机森林训练数据

本目录公开 MoveToPlay 双随机森林流水线使用的规范化 CSV 数据。大文件由 Git LFS 管理，克隆仓库后请先运行：

```bash
git lfs install
git lfs pull
```

## 已公开数据集

### movetoplay-latest-v2

用于新实验的最新恢复数据，包含后续追加的采集会话：

- `processed/event_samples_combined_slash_full.csv`
- `processed/event_events_combined_slash_full.csv`
- manifest：[`../training/datasets/movetoplay-latest-v2.json`](../training/datasets/movetoplay-latest-v2.json)

### movetoplay-deployed-v1

用于复现当前固件中已部署随机森林数组的数据快照：

- `processed/event_samples_combined_slash_full.csv.pre_zhq10_zhq11_20260808_140431.bak`
- `processed/event_events_combined_slash_full.csv.pre_zhq10_zhq11_20260808_140431.bak`
- manifest：[`../training/datasets/movetoplay-deployed-v1.json`](../training/datasets/movetoplay-deployed-v1.json)

manifest 记录文件路径、字节数、SHA-256、行数、会话数、节点编号和标签分布。训练开始前，流水线会逐项验证，防止数据损坏或误用其他版本。

## CSV 格式

samples CSV 至少包含：

```text
pc_timestamp_ms,board_timestamp_ms,node_id,ax,ay,az,gx,gy,gz,state_label,session_id
```

events CSV 至少包含：

```text
event_id,event_group,event_type,pc_timestamp_ms,state_label,session_id
```

`node_id` 必须覆盖四个 Tracker：

| node_id | 佩戴位置 |
| --- | --- |
| `1` | 胸部 |
| `2` | 右手 |
| `3` | 左手 |
| `4` | 腿部 |

## 使用自己的数据

请勿直接修改上述官方快照。使用 Companion 或 `tools/collect_imu_events.py` 创建新的会话目录，然后运行：

```powershell
python tools/prepare_rf_dataset.py `
  --input-dir data/my-sessions `
  --output-dir data/processed/my-dataset `
  --dataset-id my-dataset
```

该工具会合并 samples/events CSV，并自动生成与训练流水线兼容的 `dataset_manifest.json`。完整步骤见 [`../training/README.md`](../training/README.md)。

## 许可证

本目录中明确纳入 Git 的训练数据使用 CC0 1.0 Universal 发布，可以复制、修改、重新训练、分发和用于商业目的，无需申请许可或署名。详情见 [`LICENSE`](LICENSE)。

未纳入 Git 的本地个人会话、临时备份和恢复文件不属于公开数据发布内容。
