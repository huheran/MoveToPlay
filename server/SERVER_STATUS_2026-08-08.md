# MoveToPlay ECS 状态记录（2026-08-08）

## 已完成

- 服务器：阿里云 ECS，Ubuntu 24.04.4 LTS，4 vCPU，约 8 GB 内存，40 GB 系统盘。
- Docker：29.1.3；Docker Compose：2.40.3。
- 普通用户 `movetoplay` 已加入 `docker` 用户组，日常部署不再需要 sudo。
- 当前应用版本：Git 提交 `f0e609c87eec01eae6b0f948428323a7864cbd05`。
- 当前容器：`movetoplay-server`，状态 `healthy`，容器用户为 `app`（非 root）。
- 安全选项：`no-new-privileges:true`。
- 监听地址：仅 `127.0.0.1:8000`，未开放公网 API 端口，也未修改 ECS 安全组。
- 根接口实测响应：`{"status":"MoveToPlay server running"}`。
- 健康接口实测响应：`{"status":"ok"}`。

## 部署与数据目录

- 应用根目录：`/home/movetoplay/MoveToPlay`
- 当前版本链接：`/home/movetoplay/MoveToPlay/current`
- 版本目录：`/home/movetoplay/MoveToPlay/releases/<Git提交ID>`
- 原始数据持久目录：`/home/movetoplay/MoveToPlay/shared/data`
- 模型和 C 数组产物目录：`/home/movetoplay/MoveToPlay/shared/artifacts`
- 当前部署包 SHA-256：`a8d51cccc7fbf8517bff2d66e9be1dead5b01468252899049b87c43f1e5bec10`（上传前后完全一致）

服务器没有配置私有 GitHub 仓库凭据。部署流程由本机对已提交版本执行 `git archive`，校验哈希后通过 SSH 上传，因此 GitHub 仍是源码事实来源，服务器不需要保存 GitHub 密钥。

## 日常检查命令

```bash
cd /home/movetoplay/MoveToPlay/current/server
docker compose ps
docker compose logs --tail 100
curl --fail --silent http://127.0.0.1:8000/
```

从 Windows 本机访问时建立 SSH 隧道：

```powershell
ssh -L 8000:127.0.0.1:8000 movetoplay-server
```

随后访问 `http://127.0.0.1:8000/` 或 `http://127.0.0.1:8000/docs`。

## 下一阶段尚未实现

当前只是安全可用的服务骨架。Dongle/电脑端软件的数据上传鉴权、断点续传、任务队列、云端训练执行、模型版本审批、C 数组下载和设备升级协议尚未接入；这些应在下一阶段逐项设计和实现。
