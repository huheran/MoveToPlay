#!/usr/bin/env python3
"""合并 MoveToPlay 采集会话，并生成随机森林训练 manifest。"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

from build_dataset_manifest import (
    EVENT_COLUMNS,
    SAMPLE_COLUMNS,
    build_manifest,
    require_columns,
)


def discover_csv_files(input_dir: Path, excluded: set[Path]) -> tuple[list[Path], list[Path]]:
    samples: list[Path] = []
    events: list[Path] = []
    for path in sorted(input_dir.rglob("*.csv")):
        resolved = path.resolve()
        if resolved in excluded:
            continue
        name = path.name.lower()
        if name == "events.csv" or name.endswith("_events.csv"):
            events.append(path)
        else:
            samples.append(path)
    return samples, events


def collect_headers(paths: list[Path], required: list[str]) -> list[str]:
    columns: list[str] = []
    for path in paths:
        with path.open("r", encoding="utf-8-sig", newline="") as handle:
            reader = csv.DictReader(handle)
            require_columns(path, reader.fieldnames, required)
            for column in reader.fieldnames or []:
                if column not in columns:
                    columns.append(column)
    return columns


def merge_csv_files(paths: list[Path], output: Path, required: list[str]) -> int:
    if not paths:
        raise ValueError(f"没有找到包含 {required} 的 CSV")
    fieldnames = collect_headers(paths, required)
    output.parent.mkdir(parents=True, exist_ok=True)
    rows = 0
    with output.open("w", encoding="utf-8", newline="") as destination:
        writer = csv.DictWriter(destination, fieldnames=fieldnames, lineterminator="\n")
        writer.writeheader()
        for path in paths:
            with path.open("r", encoding="utf-8-sig", newline="") as source:
                reader = csv.DictReader(source)
                for row in reader:
                    writer.writerow(row)
                    rows += 1
    return rows


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="合并采集会话并准备随机森林训练数据")
    parser.add_argument("--input-dir", required=True, type=Path, help="包含会话 CSV 的目录")
    parser.add_argument("--output-dir", required=True, type=Path, help="处理后数据输出目录")
    parser.add_argument("--dataset-id", required=True, help="数据集唯一名称")
    parser.add_argument("--description", help="数据集说明")
    parser.add_argument("--force", action="store_true", help="允许覆盖已有输出")
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()
    input_dir = args.input_dir.resolve()
    output_dir = args.output_dir.resolve()
    samples_output = output_dir / "samples.csv"
    events_output = output_dir / "events.csv"
    manifest_output = output_dir / "dataset_manifest.json"

    if not input_dir.is_dir():
        print(f"[error] 找不到输入目录：{input_dir}")
        return 1
    existing = [path for path in (samples_output, events_output, manifest_output) if path.exists()]
    if existing and not args.force:
        print(f"[error] 输出已存在；如需覆盖请使用 --force：{existing}")
        return 1

    try:
        samples_files, events_files = discover_csv_files(
            input_dir,
            {samples_output.resolve(), events_output.resolve()},
        )
        print(f"[info] samples files={len(samples_files)} events files={len(events_files)}")
        sample_rows = merge_csv_files(samples_files, samples_output, SAMPLE_COLUMNS)
        event_rows = merge_csv_files(events_files, events_output, EVENT_COLUMNS)
        build_manifest(
            samples_path=samples_output,
            events_path=events_output,
            output_path=manifest_output,
            dataset_id=args.dataset_id,
            description=args.description or f"MoveToPlay 自定义数据集 {args.dataset_id}",
            producer="movetoplay-prepare-rf-dataset-v1",
            event_id_scope="session",
            force=args.force,
        )
    except (FileExistsError, FileNotFoundError, ValueError) as exc:
        print(f"[error] {exc}")
        return 1

    print(f"[ok] samples={samples_output} ({sample_rows} rows)")
    print(f"[ok] events={events_output} ({event_rows} rows)")
    print(f"[ok] manifest={manifest_output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
