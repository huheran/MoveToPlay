# MoveToPlay 云端训练 API v1

基础地址为 `http://127.0.0.1:8000`。当前应通过 SSH 隧道使用；所有 `/api/v1/` 请求都带：

```http
Authorization: Bearer <token>
```

## 数据集流程

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

`approved` 目前只记录人工决策，不会自动写入固件或下发设备。后续设备升级接口只能选择已经 `passed` 且存在 `approved_at` 的任务。

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
```

SQLite 使用 WAL 和 30 秒 busy timeout。API 负责写入上传与任务记录，单 worker 原子领取队列中最早的任务；worker 重启后会把中断的 `running` 任务重新放回队列。
