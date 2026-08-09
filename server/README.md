# MoveToPlay 云端训练服务

当前版本已经把玩家所选历史会话、官方基础数据合并、processed CSV 上传、完整性校验、异步训练、质量门禁、模型确认和产物下载串成一个闭环。API 与训练 worker 是两个独立容器，共享 SQLite 状态库和持久目录；worker 同一时间只执行一个任务，避免 8 GB ECS 被多个随机森林任务同时耗尽内存。

任务现在会按“准备数据、校验、状态模型训练/评估/导出、事件模型训练/评估/导出、质量门禁、产物校验”汇报进度、已用时间和预计剩余时间。已批准任务组成模型版本库，可重新生成固件或回滚；配置 `MOVETOPLAY_OSS_*` 与 `OSS_ACCESS_KEY_*` 后，批准动作会异步把完整模型目录压缩备份到私有 OSS Bucket。OSS 未配置或暂时失败不会影响本地批准和固件生成。

## 安全边界

- API 只绑定服务器回环地址 `127.0.0.1:8000`，当前通过 SSH 隧道访问，不开放公网端口。
- 除 `/`、`/health` 和接口文档外，业务接口都要求 Bearer Token。
- API 与 worker 均以 UID 1000 的非 root 用户运行，并启用 `no-new-privileges`。
- API 不挂载 Docker Socket，不能创建或控制其他容器。
- 上传文件最大 256 MiB，单个请求块最大 8 MiB；服务端校验声明大小、SHA-256 和 CSV 表头。
- 数据、任务状态、模型和 C 数组均保存在服务器持久目录，不进入 Git 或容器镜像。
- 持久目录权限为 `0700`，数据库和上传文件按 `0600` 创建。
- 训练通过质量门禁后仍不会自动更新固件，必须先调用人工审批接口。
- Python 基础镜像固定摘要；依赖固定版本并通过阿里云 PyPI 镜像下载。

完整接口和状态说明见 [API_PROTOCOL.md](API_PROTOCOL.md)。

## 本机测试

worker 测试会真正执行一次小型数据集校验，因此使用已安装训练依赖的隔离环境：

```powershell
.\.venv-training\Scripts\python.exe -m pip install -r server\requirements-dev.txt
Push-Location server
..\.venv-training\Scripts\python.exe -m pytest
Pop-Location
```

## 服务器配置

真实令牌只存于服务器：

```text
/home/movetoplay/MoveToPlay/shared/server.env
```

格式参考 `server.env.example`，文件权限应为 `0600`。部署时 Compose 会自动读取它。

`MOVETOPLAY_OFFICIAL_DATASET_ID` 必须指向已处于 `ready` 状态的官方完整数据集。客户端不会用玩家上一次训练结果替代它，确保官方数据不变、玩家只选择本次要加入的本地会话。

## 启动与检查

版本包由本机 `git archive` 生成并校验 SHA-256，再通过 SSH 上传。服务器运行：

```bash
/home/movetoplay/deploy-release.sh \
  /home/movetoplay/MoveToPlay/MoveToPlay-<提交ID>.tar.gz \
  <提交ID>

cd /home/movetoplay/MoveToPlay/current/server
docker compose ps
docker compose logs --tail 100 api worker
curl --fail --silent http://127.0.0.1:8000/health
```

## 从 Windows 上传

先保持 SSH 隧道窗口运行：

```powershell
ssh -L 8000:127.0.0.1:8000 movetoplay-server
```

在另一个 PowerShell 中设置令牌并上传。历史合并数据的 `event_id` 只在 session 内唯一，所以使用 `session`；新采集数据默认应使用全局唯一 ID。

`POST /api/v1/datasets` 可选传入 `base_dataset_id`。新数据仍按原大小和 SHA-256 续传，训练 worker 会递归展开已就绪的基础数据集链，在 Job 目录中合并 CSV，并给 `session_id`/`event_id` 添加来源数据集前缀。这样玩家只上传新动作会话，也能保留基础模型的全部旧动作。

```powershell
$env:MOVETOPLAY_API_TOKEN = '<服务器令牌>'
python tools\upload_training_dataset.py `
  --samples data\processed\event_samples_combined_slash_full.csv `
  --events data\processed\event_events_combined_slash_full.csv `
  --name 'movetoplay-latest-v2' `
  --event-id-scope session `
  --mode train `
  --wait
```

命令意外中断后，使用第一次输出的 dataset ID 加 `--dataset-id <ID>` 即可从服务器记录的字节偏移继续上传。
