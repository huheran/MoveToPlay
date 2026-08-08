#!/usr/bin/env python3
"""分块、可续传地把 processed CSV 数据集上传到 MoveToPlay 服务。"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


class ApiClient:
    def __init__(self, base_url: str, token: str):
        self.base_url = base_url.rstrip("/")
        self.token = token

    def request(self, method: str, path: str, payload: Any | None = None, headers: dict[str, str] | None = None) -> Any:
        body = None
        request_headers = {"Authorization": f"Bearer {self.token}"}
        if payload is not None:
            if isinstance(payload, bytes):
                body = payload
                request_headers["Content-Type"] = "application/octet-stream"
            else:
                body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
                request_headers["Content-Type"] = "application/json"
        request_headers.update(headers or {})
        request = urllib.request.Request(
            self.base_url + path,
            data=body,
            headers=request_headers,
            method=method,
        )
        try:
            with urllib.request.urlopen(request, timeout=120) as response:
                content = response.read()
        except urllib.error.HTTPError as exc:
            content = exc.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"API {exc.code} {method} {path}: {content}") from exc
        return json.loads(content) if content else None


def file_spec(path: Path) -> dict[str, Any]:
    return {"filename": path.name, "bytes": path.stat().st_size, "sha256": sha256_file(path)}


def verify_resume_file(kind: str, path: Path, remote: dict[str, Any]) -> None:
    expected = remote["files"][kind]
    if path.stat().st_size != expected["expected_bytes"]:
        raise RuntimeError(f"本地 {kind} 文件大小与服务器登记值不一致")
    if sha256_file(path) != expected["sha256"]:
        raise RuntimeError(f"本地 {kind} 文件 SHA-256 与服务器登记值不一致")


def upload_file(client: ApiClient, dataset: dict[str, Any], kind: str, path: Path, chunk_size: int) -> dict[str, Any]:
    offset = int(dataset["files"][kind]["received_bytes"])
    total = path.stat().st_size
    with path.open("rb") as handle:
        handle.seek(offset)
        while offset < total:
            chunk = handle.read(min(chunk_size, total - offset))
            dataset = client.request(
                "PUT",
                f"/api/v1/datasets/{dataset['id']}/files/{kind}",
                chunk,
                {"X-Upload-Offset": str(offset)},
            )
            offset = int(dataset["files"][kind]["received_bytes"])
            percent = offset * 100 / total
            print(f"[upload] {kind}: {offset}/{total} bytes ({percent:.1f}%)", flush=True)
    return dataset


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="上传 MoveToPlay processed CSV 并创建训练任务")
    parser.add_argument("--url", default="http://127.0.0.1:8000")
    parser.add_argument("--samples", required=True, type=Path)
    parser.add_argument("--events", required=True, type=Path)
    parser.add_argument("--name", default="MoveToPlay uploaded dataset")
    parser.add_argument("--event-id-scope", choices=["global", "session"], default="global")
    parser.add_argument("--dataset-id", help="继续上传已经建立的数据集")
    parser.add_argument("--mode", choices=["validate", "train"], default="train")
    parser.add_argument("--chunk-mib", type=int, default=8)
    parser.add_argument("--wait", action="store_true", help="等待任务结束并输出最终状态")
    parser.add_argument("--poll-seconds", type=float, default=5.0)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    token = os.getenv("MOVETOPLAY_API_TOKEN", "").strip()
    if not token:
        print("错误：请通过 MOVETOPLAY_API_TOKEN 环境变量提供访问令牌。", file=sys.stderr)
        return 2
    samples = args.samples.resolve()
    events = args.events.resolve()
    for path in (samples, events):
        if not path.is_file():
            print(f"错误：文件不存在：{path}", file=sys.stderr)
            return 2
    if args.chunk_mib < 1 or args.chunk_mib > 8:
        print("错误：--chunk-mib 必须在 1 到 8 之间。", file=sys.stderr)
        return 2

    client = ApiClient(args.url, token)
    if args.dataset_id:
        dataset = client.request("GET", f"/api/v1/datasets/{args.dataset_id}")
        verify_resume_file("samples", samples, dataset)
        verify_resume_file("events", events, dataset)
    else:
        print("[prepare] 正在计算文件 SHA-256", flush=True)
        dataset = client.request(
            "POST",
            "/api/v1/datasets",
            {
                "name": args.name,
                "samples": file_spec(samples),
                "events": file_spec(events),
                "event_id_scope": args.event_id_scope,
            },
        )
        print(f"[dataset] id={dataset['id']}", flush=True)

    for kind, path in (("samples", samples), ("events", events)):
        dataset = upload_file(client, dataset, kind, path, args.chunk_mib * 1024 * 1024)
    dataset = client.request("POST", f"/api/v1/datasets/{dataset['id']}/complete")
    print(f"[dataset] status={dataset['status']}", flush=True)
    job = client.request(
        "POST", "/api/v1/jobs", {"dataset_id": dataset["id"], "mode": args.mode}
    )
    print(f"[job] id={job['id']} status={job['status']} mode={job['mode']}", flush=True)
    if args.wait:
        previous = None
        while job["status"] in {"queued", "running"}:
            if job["status"] != previous:
                print(f"[job] status={job['status']}", flush=True)
                previous = job["status"]
            time.sleep(args.poll_seconds)
            job = client.request("GET", f"/api/v1/jobs/{job['id']}")
        print(json.dumps(job, ensure_ascii=False, indent=2))
        return 0 if job["status"] in {"validated", "passed"} else 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
