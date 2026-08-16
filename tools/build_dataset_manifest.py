#!/usr/bin/env python3
"""为 MoveToPlay 随机森林训练数据生成可复现的数据集 manifest。"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from collections import Counter
from pathlib import Path
from typing import Any


PROJECT_ROOT = Path(__file__).resolve().parents[1]
SAMPLE_COLUMNS = [
    "pc_timestamp_ms",
    "board_timestamp_ms",
    "node_id",
    "ax",
    "ay",
    "az",
    "gx",
    "gy",
    "gz",
    "state_label",
    "session_id",
]
EVENT_COLUMNS = [
    "event_id",
    "event_group",
    "event_type",
    "pc_timestamp_ms",
    "state_label",
    "session_id",
]
NUMERIC_SAMPLE_COLUMNS = [
    "pc_timestamp_ms",
    "node_id",
    "ax",
    "ay",
    "az",
    "gx",
    "gy",
    "gz",
]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def manifest_path(path: Path) -> str:
    resolved = path.resolve()
    try:
        return resolved.relative_to(PROJECT_ROOT).as_posix()
    except ValueError:
        return str(resolved)


def require_columns(path: Path, fieldnames: list[str] | None, required: list[str]) -> None:
    if not fieldnames:
        raise ValueError(f"CSV 缺少表头：{path}")
    missing = sorted(set(required) - set(fieldnames))
    if missing:
        raise ValueError(f"CSV 缺少必需列 {missing}：{path}")


def inspect_samples(path: Path) -> dict[str, Any]:
    rows = 0
    sessions: set[str] = set()
    node_ids: set[int] = set()
    state_counts: Counter[str] = Counter()

    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        require_columns(path, reader.fieldnames, SAMPLE_COLUMNS)
        for line_number, row in enumerate(reader, start=2):
            rows += 1
            session_id = (row.get("session_id") or "").strip()
            if not session_id:
                raise ValueError(f"samples 第 {line_number} 行缺少 session_id")
            sessions.add(session_id)

            for column in NUMERIC_SAMPLE_COLUMNS:
                try:
                    float(row.get(column, ""))
                except (TypeError, ValueError) as exc:
                    raise ValueError(
                        f"samples 第 {line_number} 行的 {column} 不是有效数字"
                    ) from exc

            node_id = int(float(row["node_id"]))
            node_ids.add(node_id)
            state_counts[(row.get("state_label") or "<missing>").strip() or "<missing>"] += 1

    if rows == 0:
        raise ValueError("samples CSV 不能为空")
    if node_ids != {1, 2, 3, 4}:
        raise ValueError(f"samples 必须包含 node_id 1、2、3、4，实际为 {sorted(node_ids)}")

    return {
        "path": manifest_path(path),
        "bytes": path.stat().st_size,
        "sha256": sha256_file(path),
        "rows": rows,
        "session_count": len(sessions),
        "node_ids": sorted(node_ids),
        "required_columns": SAMPLE_COLUMNS,
        "state_label_counts": dict(sorted(state_counts.items())),
        "sessions": sessions,
    }


def inspect_events(path: Path, event_id_scope: str) -> dict[str, Any]:
    rows = 0
    sessions: set[str] = set()
    event_counts: Counter[str] = Counter()
    event_ids: set[str] = set()
    session_event_ids: set[tuple[str, str]] = set()

    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        require_columns(path, reader.fieldnames, EVENT_COLUMNS)
        for line_number, row in enumerate(reader, start=2):
            rows += 1
            session_id = (row.get("session_id") or "").strip()
            event_id = (row.get("event_id") or "").strip()
            if not session_id or not event_id:
                raise ValueError(f"events 第 {line_number} 行缺少 session_id 或 event_id")
            try:
                float(row.get("pc_timestamp_ms", ""))
            except (TypeError, ValueError) as exc:
                raise ValueError(
                    f"events 第 {line_number} 行的 pc_timestamp_ms 不是有效数字"
                ) from exc

            pair = (session_id, event_id)
            if pair in session_event_ids:
                raise ValueError(f"events 存在重复的 (session_id, event_id)：{pair}")
            if event_id_scope == "global" and event_id in event_ids:
                raise ValueError(f"events 存在重复的全局 event_id：{event_id}")

            sessions.add(session_id)
            event_ids.add(event_id)
            session_event_ids.add(pair)
            event_counts[(row.get("event_type") or "<missing>").strip() or "<missing>"] += 1

    if rows == 0:
        raise ValueError("events CSV 不能为空")

    return {
        "path": manifest_path(path),
        "event_id_scope": event_id_scope,
        "bytes": path.stat().st_size,
        "sha256": sha256_file(path),
        "rows": rows,
        "session_count": len(sessions),
        "required_columns": EVENT_COLUMNS,
        "event_type_counts": dict(sorted(event_counts.items())),
        "sessions": sessions,
    }


def build_manifest(
    samples_path: Path,
    events_path: Path,
    output_path: Path,
    dataset_id: str,
    description: str,
    producer: str,
    event_id_scope: str,
    force: bool = False,
) -> dict[str, Any]:
    samples_path = samples_path.resolve()
    events_path = events_path.resolve()
    output_path = output_path.resolve()

    if not samples_path.is_file():
        raise FileNotFoundError(f"找不到 samples CSV：{samples_path}")
    if not events_path.is_file():
        raise FileNotFoundError(f"找不到 events CSV：{events_path}")
    if output_path.exists() and not force:
        raise FileExistsError(f"manifest 已存在；如需覆盖请使用 --force：{output_path}")

    samples = inspect_samples(samples_path)
    events = inspect_events(events_path, event_id_scope)
    unknown_sessions = sorted(events.pop("sessions") - samples.pop("sessions"))
    if unknown_sessions:
        raise ValueError(f"events 引用了 samples 中不存在的 session_id：{unknown_sessions}")

    manifest = {
        "schema_version": 1,
        "dataset_id": dataset_id,
        "description": description,
        "ingestion": {"kind": "processed_csv", "producer": producer},
        "samples": samples,
        "events": events,
    }
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    return manifest


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="生成 MoveToPlay 随机森林数据集 manifest")
    parser.add_argument("--samples", required=True, type=Path, help="合并后的 samples CSV")
    parser.add_argument("--events", required=True, type=Path, help="合并后的 events CSV")
    parser.add_argument("--dataset-id", required=True, help="数据集唯一名称")
    parser.add_argument("--output", type=Path, help="manifest 输出路径")
    parser.add_argument("--description", help="数据集说明")
    parser.add_argument("--producer", default="movetoplay-local-collection-v1")
    parser.add_argument("--event-id-scope", choices=["global", "session"], default="session")
    parser.add_argument("--force", action="store_true", help="允许覆盖已有 manifest")
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()
    output = args.output or PROJECT_ROOT / "training" / "datasets" / f"{args.dataset_id}.json"
    try:
        manifest = build_manifest(
            samples_path=args.samples,
            events_path=args.events,
            output_path=output,
            dataset_id=args.dataset_id,
            description=args.description or f"MoveToPlay 自定义数据集 {args.dataset_id}",
            producer=args.producer,
            event_id_scope=args.event_id_scope,
            force=args.force,
        )
    except (FileExistsError, FileNotFoundError, ValueError) as exc:
        print(f"[error] {exc}")
        return 1

    print(f"[ok] manifest={output.resolve()}")
    print(f"[ok] samples={manifest['samples']['rows']} rows")
    print(f"[ok] events={manifest['events']['rows']} rows")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
