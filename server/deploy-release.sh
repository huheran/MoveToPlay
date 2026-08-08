#!/usr/bin/env bash
set -Eeuo pipefail

if [[ "$#" -ne 2 ]]; then
  echo "用法：$0 <部署包.tar.gz> <Git提交ID>" >&2
  exit 1
fi

ARCHIVE="$1"
RELEASE_ID="$2"

if [[ ! "${RELEASE_ID}" =~ ^[0-9a-f]{7,40}$ ]]; then
  echo "错误：Git 提交 ID 格式不正确。" >&2
  exit 1
fi

if [[ ! -f "${ARCHIVE}" ]]; then
  echo "错误：部署包不存在：${ARCHIVE}" >&2
  exit 1
fi

APP_ROOT="${HOME}/MoveToPlay"
RELEASE_DIR="${APP_ROOT}/releases/${RELEASE_ID}"

if [[ -e "${RELEASE_DIR}" ]]; then
  echo "错误：版本目录已存在，拒绝覆盖：${RELEASE_DIR}" >&2
  exit 1
fi

mkdir -p "${RELEASE_DIR}"
tar -xzf "${ARCHIVE}" -C "${RELEASE_DIR}"

if [[ ! -f "${RELEASE_DIR}/server/compose.yaml" ]]; then
  echo "错误：部署包中缺少 server/compose.yaml。" >&2
  exit 1
fi

cd "${RELEASE_DIR}/server"
export MOVETOPLAY_SOURCE_COMMIT="${RELEASE_ID}"

if docker compose version >/dev/null 2>&1; then
  docker compose up -d --build
  docker compose ps
else
  docker-compose up -d --build
  docker-compose ps
fi

ln -sfn "${RELEASE_DIR}" "${APP_ROOT}/current"
echo "部署完成：${RELEASE_ID}"
