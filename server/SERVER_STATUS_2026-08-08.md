# MoveToPlay ECS 状态记录（2026-08-08）

## 服务器与运行版本

- 阿里云 ECS：Ubuntu 24.04.4 LTS，4 vCPU，约 8 GB 内存，40 GB 系统盘。
- Docker 29.1.3；Docker Compose 2.40.3。
- 当前部署版本：Git 提交 `5939f2cc26aca75fb08ba45fb9e3bf021ca02acc`。
- 部署包 SHA-256：`90431bfab0b5ca5263aa8534fc147db03a939008915ef0bcb36b306ac3bce8f6`。
- `movetoplay-server`：FastAPI，状态 `healthy`，容器用户 `app`。
- `movetoplay-training-worker`：训练队列 worker，容器用户 `worker`。
- 两个容器都启用 `no-new-privileges:true`，UID 为 1000；API 没有挂载 Docker Socket。
- API 仅监听 `127.0.0.1:8000`，ECS 安全组没有开放业务端口。

## 已上线的训练闭环

- Bearer Token 鉴权；真实令牌仅存于服务器 `shared/server.env`，权限 `0600`。
- processed samples/events CSV 分块上传，单块最大 8 MiB，支持按字节偏移续传。
- 声明大小、SHA-256、CSV 必需列、四节点、事件/样本 session 关系和流水线数据校验。
- SQLite WAL 持久任务队列，API 与单 worker 分离。
- `validate` 和 `train` 两种任务，状态可查询。
- 双随机森林训练、质量门禁、joblib 模型和 C 数组生成。
- 运行产物列表与单文件下载。
- 训练通过后的人工审批记录；没有自动写入固件或下发设备。
- 持久目录权限 `0700`，数据库、数据和产物文件按 `0600` 创建。

接口协议见 `server/API_PROTOCOL.md`，参考上传客户端为 `tools/upload_training_dataset.py`。

## ECS 冒烟测试

- 数据集 ID：`1477c0342d064c1b8bbaf02c001777b8`
- 校验任务 ID：`856d870b2acb45a286711ba0a67bebed`
- 状态变化：`queued -> running -> validated`
- 产物通过鉴权 API 下载后与服务器文件逐字节一致。
- 运行清单记录 Python 3.13.9 和源提交 `5939f2cc26aca75fb08ba45fb9e3bf021ca02acc`。

## 完整历史数据云端训练

- 数据集 ID：`309794a8bc3e4a1b951321153260f3db`
- 训练任务 ID：`9968a258eb204188b18682471f30a4d8`
- 输入：1,218,686 条 samples，1,722 条 events。
- 时间：2026-08-08 18:55:31 至 19:16:55（北京时间），约 21 分 24 秒。
- 状态：`passed`，尚未人工审批。
- `state_rf_15_full`：accuracy `0.990346127`，macro F1 `0.990847717`，质量门禁通过。
- `event_rf`：accuracy `0.991435245`，macro F1 `0.988198508`，质量门禁通过。
- 四个生成的 `.c/.h` 文件与本机 `local-full-v1b` 的 SHA-256 全部一致，证明云端复现了本机结果。
- 训练峰值内存低于 3 GiB；任务结束后 API/worker 常驻内存约 41/46 MiB。
- 运行目录约 17 MiB；系统盘验收时使用率约 12%。

完整云端运行目录已备份到：

```text
D:\git_fork_sure_mine\MoveToPlay_backups\2026-08-08-server-deploy\cloud-training-run-9968a258eb204188b18682471f30a4d8.tar.gz
SHA-256: b012e0e5ed3a57ce52ca3dc4740962fc48a81b50cb444bebaaf2f8ee1d7cb1a4
```

上传使用的服务器临时中转副本已经删除；API 持久目录中的正式数据保留并重新核验了 SHA-256。

## 持久目录

```text
/home/movetoplay/MoveToPlay/shared/
|-- server.env
|-- state/movetoplay.sqlite3
|-- datasets/<dataset-id>/
|-- jobs/<job-id>/
`-- artifacts/training-runs/<job-id>/
```

## 日常检查

```bash
cd /home/movetoplay/MoveToPlay/current/server
docker compose ps
docker compose logs --tail 100 api worker
curl --fail --silent http://127.0.0.1:8000/health
```

## 下一步边界

服务器训练闭环已经可用。下一阶段是把分块上传客户端接入 Windows Companion 的“数据采集训练模式”，并选择公网接入方式。正式开放公网前必须配置域名、HTTPS、限流和令牌轮换；固件更新接口只能选择已经通过质量门禁且人工审批的任务。
