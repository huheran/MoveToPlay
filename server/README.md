# MoveToPlay 云端服务

当前阶段先建立一个最小、可验证的 FastAPI 服务，后续再接入设备数据上传、训练任务、模型版本管理和 C 数组产物下载。

## 安全边界

- API 容器只绑定服务器回环地址 `127.0.0.1:8000`，不会直接暴露到公网。
- 服务器初始化脚本只安装 Docker、Compose、Git 和基础证书工具，创建应用目录；不会修改 ECS 安全组。
- 应用在容器内使用非 root 用户运行，并启用 `no-new-privileges`。
- 原始训练数据和模型产物放在服务器持久目录中，不提交到 Git。
- Python 基础镜像通过 ECS 实测可达的 ECR Public 获取，并固定镜像摘要，避免 Docker Hub 网络超时和镜像漂移。
- 容器构建通过阿里云 PyPI 镜像下载已锁定版本的 Python 包，以适应中国大陆 ECS 的网络环境。

## 本机测试

在仓库根目录运行：

```powershell
python -m venv .venv-server
.\.venv-server\Scripts\python.exe -m pip install -r server\requirements-dev.txt
Push-Location server
..\.venv-server\Scripts\python.exe -m pytest
Pop-Location
```

## ECS 一次性初始化

先检查脚本内容，再在 Remote SSH 终端中手动输入 sudo 密码：

```bash
sed -n '1,240p' /home/movetoplay/bootstrap-server.sh
sudo bash /home/movetoplay/bootstrap-server.sh
```

执行完成后重新建立 SSH 连接，让 Docker 用户组权限生效。

## 启动与验证

把由 `git archive` 生成的部署包上传到服务器后运行：

```bash
chmod +x /home/movetoplay/deploy-release.sh
/home/movetoplay/deploy-release.sh \
  /home/movetoplay/MoveToPlay/MoveToPlay-<提交ID>.tar.gz \
  <提交ID>
curl --fail --silent http://127.0.0.1:8000/
```

预期响应：

```json
{"status":"MoveToPlay server running"}
```

如需从本机浏览器访问，建立 SSH 隧道：

```powershell
ssh -L 8000:127.0.0.1:8000 movetoplay-server
```

然后打开 `http://127.0.0.1:8000/` 或接口文档 `http://127.0.0.1:8000/docs`。
