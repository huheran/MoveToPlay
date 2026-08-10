# MoveToPlay 云端训练 API v1

基础地址为 `http://127.0.0.1:8000`。当前应通过 SSH 隧道使用；所有 `/api/v1/` 请求都带：

```http
Authorization: Bearer <token>
```

## 数据集流程

`GET /api/v1/system-config` 返回服务器固定的 `official_dataset_id`。Companion 每次只把玩家在“我的采集数据”中勾选的会话叠加到这个只读官方数据集，不会把上一次玩家训练结果继续作为基础数据，因而不会重复累计历史会话。

1. `POST /api/v1/datasets`：声明 samples/events 的文件名、字节数和 SHA-256，获得 dataset ID。
2. `PUT /api/v1/datasets/{id}/files/samples`：上传原始二进制块，并提供 `X-Upload-Offset`。
3. `PUT /api/v1/datasets/{id}/files/events`：同上。
4. `GET /api/v1/datasets/{id}`：查询两个文件各自已接收的字节数，用于断点续传。
5. `POST /api/v1/datasets/{id}/complete`：核对总长度、SHA-256 和必需 CSV 列；成功后状态变为 `ready`。

创建请求示例：

```json
{
  "name": "collection-2026-08-08",
  "samples": {
    "filename": "samples.csv",
    "bytes": 123456,
    "sha256": "64位十六进制SHA-256"
  },
  "events": {
    "filename": "events.csv",
    "bytes": 1234,
    "sha256": "64位十六进制SHA-256"
  },
  "event_id_scope": "global",
  "base_dataset_id": "可选的32位基础数据集ID"
}
```

上传是严格顺序追加的。若客户端偏移与服务器不一致，服务器返回 HTTP 409 和正确的 `expected_offset`；客户端应查询数据集后从该位置继续。完成后的数据集不可修改，哈希错误的数据集会标记为 `rejected`，需要重新创建。

`base_dataset_id` 省略或传 `null` 时是独立数据集；传入一个已处于 `ready` 状态的数据集后，worker 会递归合并基础链和本次 CSV。合并时会为 `session_id`、`event_id` 增加来源数据集前缀，避免不同采集会话发生 ID 冲突。

## 训练任务流程

- `POST /api/v1/jobs`：为 `ready` 数据集创建任务，`mode` 为 `validate` 或 `train`。
- `GET /api/v1/jobs`：列出任务。
- `GET /api/v1/jobs/{id}`：查询单个任务。
- `GET /api/v1/jobs/{id}/artifacts`：列出运行产物。
- `GET /api/v1/jobs/{id}/artifacts/{path}`：下载单个产物。
- `POST /api/v1/jobs/{id}/approve`：人工批准状态为 `passed` 的训练任务。
- `GET /api/v1/models`：列出全部已批准模型版本及名称、来源、当前采用、固件和 OSS 备份状态。
- `PATCH /api/v1/models/{id}`：修改模型的人类可读名称，请求体为 `{ "name": "评委演示模型" }`；传 `null` 恢复默认名称，技术版本号不变。
- `POST /api/v1/models/{id}/activate`：把已批准历史版本设为当前采用模型。

任务响应同时返回 `progress_stage`、`progress_detail`、`progress_percent`、
`elapsed_seconds` 和 `estimated_remaining_seconds`。训练未结束前客户端不展示旧模型指标；
完成后再读取 `run_manifest.json` 显示准确率与质量门禁。

任务状态变化：

```text
queued -> running -> validated   （仅校验）
                  -> passed      （训练和质量门禁通过）
                  -> failed
```

训练请求：

```json
{
  "dataset_id": "32位十六进制ID",
  "mode": "train"
}
```

审批请求：

```json
{
  "approved_by": "操作者名称"
}
```

`approved` 记录人工决策。训练通过后 worker 自动把已校验 C 数组集成进 Dongle 工程，在固定 ESP-IDF 环境中编译并生成 `firmware/firmware-bundle.zip`。Companion 只允许对 `passed` 且存在 `approved_at` 的任务下载并校验完整固件包，并在用户再次确认串口后调用内置烧录工具。
批准时服务器分配不可变的 `model_version`，将它设为当前版本，并在配置 OSS 后异步归档完整运行目录。

历史任务可通过 `POST /api/v1/jobs/{job_id}/firmware` 请求固件；请求体 `{ "force": true }` 会让云端重新编译。任务响应中的 `firmware_status`、`firmware_progress_percent`、`firmware_detail` 和 `firmware_error` 分别表示固件状态、进度、当前阶段和错误。

## 旧任务清理规则

- 已批准模型：本地永久保留，并自动备份到 OSS；不会被清理任务删除。
- 失败任务：默认保留 7 天。
- 仅校验任务：默认保留 14 天。
- 通过但未批准任务：默认保留 30 天。
- 排队中和运行中任务：永不清理。

保留天数均可通过 `MOVETOPLAY_CLEANUP_*_DAYS` 调整；可先运行
`python -m app.maintenance --dry-run` 预览清理范围。

## 持久目录

```text
/home/movetoplay/MoveToPlay/shared/
|-- server.env
|-- state/movetoplay.sqlite3
|-- datasets/<dataset-id>/samples.csv
|-- datasets/<dataset-id>/events.csv
|-- jobs/<job-id>/dataset_manifest.json
|-- jobs/<job-id>/pipeline.log
`-- artifacts/training-runs/<job-id>/
    `-- firmware/{firmware-manifest.json,firmware-bundle.zip,*.bin}
```

SQLite 使用 WAL 和 30 秒 busy timeout。API 负责写入上传与任务记录，单 worker 原子领取队列中最早的任务；worker 重启后会把中断的 `running` 任务重新放回队列。
