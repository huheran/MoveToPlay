#!/usr/bin/env bash
set -Eeuo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "错误：请使用 sudo bash 执行本脚本。" >&2
  exit 1
fi

if [[ ! -r /etc/os-release ]]; then
  echo "错误：无法识别服务器操作系统。" >&2
  exit 1
fi

# shellcheck disable=SC1091
source /etc/os-release
if [[ "${ID:-}" != "ubuntu" ]]; then
  echo "错误：当前仅支持 Ubuntu，检测到 ${ID:-unknown}。" >&2
  exit 1
fi

TARGET_USER="${SUDO_USER:-movetoplay}"
if [[ "${TARGET_USER}" == "root" ]] || ! id "${TARGET_USER}" >/dev/null 2>&1; then
  echo "错误：无法确定需要配置的普通用户。" >&2
  exit 1
fi

echo "[1/5] 更新阿里云 Ubuntu 软件索引"
export DEBIAN_FRONTEND=noninteractive
apt-get update

echo "[2/5] 安装 Docker、Git 与基础工具"
apt-get install -y --no-install-recommends ca-certificates curl git docker.io

if apt-cache show docker-compose-v2 >/dev/null 2>&1; then
  apt-get install -y --no-install-recommends docker-compose-v2
else
  apt-get install -y --no-install-recommends docker-compose
fi

echo "[3/5] 启动 Docker，并授权普通用户管理容器"
systemctl enable --now docker
getent group docker >/dev/null || groupadd --system docker
usermod -aG docker "${TARGET_USER}"

echo "[4/5] 创建 MoveToPlay 的持久目录"
TARGET_HOME="$(getent passwd "${TARGET_USER}" | cut -d: -f6)"
install -d -m 0755 -o "${TARGET_USER}" -g "${TARGET_USER}" \
  "${TARGET_HOME}/MoveToPlay" \
  "${TARGET_HOME}/MoveToPlay/releases" \
  "${TARGET_HOME}/MoveToPlay/shared" \
  "${TARGET_HOME}/MoveToPlay/shared/data" \
  "${TARGET_HOME}/MoveToPlay/shared/artifacts"

echo "[5/5] 验证安装结果"
docker --version
if docker compose version >/dev/null 2>&1; then
  docker compose version
else
  docker-compose --version
fi
git --version

echo
echo "初始化完成。Docker 用户组权限会在下一次 SSH 登录时生效。"
echo "本脚本没有开放公网端口，也没有修改云服务器安全组。"
