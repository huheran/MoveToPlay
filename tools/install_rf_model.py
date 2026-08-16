#!/usr/bin/env python3
"""把通过质量门禁的随机森林 C 数组安装到 Dongle 固件目录。"""

from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
GENERATED_FILES = [
    "rf_state_model_generated.c",
    "rf_state_model_generated.h",
    "rf_model_generated.c",
    "rf_model_generated.h",
]


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="安装 MoveToPlay 随机森林固件数组")
    parser.add_argument("--run-dir", required=True, type=Path, help="output/training-runs 下的运行目录")
    parser.add_argument("--dry-run", action="store_true", help="只检查，不复制文件")
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()
    run_dir = args.run_dir.resolve()
    generated_dir = run_dir / "generated"
    target_dir = PROJECT_ROOT / "main" / "generated"

    missing = [name for name in GENERATED_FILES if not (generated_dir / name).is_file()]
    if missing:
        print(f"[error] 训练输出缺少文件：{missing}")
        return 1
    manifest_path = run_dir / "run_manifest.json"
    if not (run_dir / "quality").is_dir() or not manifest_path.is_file():
        print("[error] 运行目录缺少质量报告或 run_manifest.json")
        return 1
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"[error] 无法读取 run_manifest.json：{exc}")
        return 1
    if manifest.get("status") != "passed":
        print(f"[error] 训练运行未通过质量门禁：status={manifest.get('status')!r}")
        return 1

    for name in GENERATED_FILES:
        source = generated_dir / name
        target = target_dir / name
        print(f"[copy] {source} -> {target}")
        if not args.dry_run:
            target_dir.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)

    print("[ok] dry-run 检查通过" if args.dry_run else "[ok] 随机森林数组已安装到 main/generated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
