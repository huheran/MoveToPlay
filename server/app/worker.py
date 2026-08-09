"""串行执行云端数据校验和随机森林训练任务。"""

from __future__ import annotations

import argparse
import csv
import json
import os
import signal
import shutil
import subprocess
import sys
import time
import traceback
from collections import Counter
from pathlib import Path
from typing import Any

import pandas as pd

from .config import Settings
from .database import Database
from .maintenance import cleanup_old_jobs
from .storage import EVENT_COLUMNS, SAMPLE_COLUMNS, dataset_file, sha256_file


PROJECT_ROOT = Path(__file__).resolve().parents[2]


def _progress_from_line(line: str, current_model: str | None) -> tuple[str, str | None, float] | None:
    text = line.strip()
    if "[pipeline] validating dataset=" in text:
        return "validating", None, 12
    if "[pipeline] dataset valid:" in text:
        return "validated", None, 18
    if "[pipeline] training model=state" in text:
        return "training_state", "state", 22
    if "[pipeline] training model=event" in text:
        return "training_event", "event", 52
    if "[info] source events=" in text:
        return ("training_state", current_model, 26) if current_model == "state" else ("training_event", current_model, 56)
    if "[info] windows total=" in text:
        return ("training_state", current_model, 36) if current_model == "state" else ("training_event", current_model, 70)
    if text == "=== Metrics ===":
        return ("evaluating_state", current_model, 42) if current_model == "state" else ("evaluating_event", current_model, 78)
    if "[info] model saved:" in text:
        return ("exporting_state", current_model, 44) if current_model == "state" else ("exporting_event", current_model, 83)
    if "[pipeline] model=state" in text and "quality=PASS" in text:
        return "state_complete", current_model, 48
    if "[pipeline] model=event" in text and "quality=PASS" in text:
        return "quality_gate", current_model, 90
    if "[pipeline] complete:" in text:
        return "finalizing", current_model, 98
    return None


def _progress_detail(stage: str) -> str:
    return {
        "validating": "正在校验合并数据集和 CSV 完整性",
        "validated": "数据校验完成，正在构建训练窗口",
        "training_state": "正在训练连续状态随机森林",
        "evaluating_state": "正在评估状态模型准确率",
        "exporting_state": "正在导出状态模型 C 数组",
        "state_complete": "状态模型已通过质量门禁",
        "training_event": "正在训练动作事件随机森林",
        "evaluating_event": "正在评估动作事件模型",
        "exporting_event": "正在导出动作模型 C 数组",
        "quality_gate": "两个模型已训练完成，正在执行质量门禁",
        "finalizing": "正在生成清单并校验全部产物",
    }.get(stage, "云端正在训练模型")


def counter_dict(counter: Counter[str]) -> dict[str, int]:
    return {key: int(value) for key, value in sorted(counter.items())}


def dataset_lineage(database: Database, dataset: dict) -> list[dict]:
    lineage: list[dict] = []
    seen: set[str] = set()
    current: dict | None = dataset
    while current is not None:
        dataset_id = str(current["id"])
        if dataset_id in seen:
            raise ValueError("dataset base chain contains a cycle")
        seen.add(dataset_id)
        if current["status"] != "ready":
            raise ValueError(f"dataset in base chain is not ready: {dataset_id}")
        lineage.append(current)
        base_id = current.get("base_dataset_id")
        current = database.get_dataset(str(base_id)) if base_id else None
        if base_id and current is None:
            raise ValueError(f"base dataset is missing: {base_id}")
    lineage.reverse()
    return lineage


def merge_csv_files(
    sources: list[tuple[Path, str]],
    destination: Path,
    identity_columns: tuple[str, ...],
) -> None:
    fieldnames: list[str] = []
    for source, _ in sources:
        with source.open("r", encoding="utf-8-sig", newline="") as handle:
            reader = csv.reader(handle)
            header = next(reader, None)
            if not header:
                raise ValueError(f"CSV is empty: {source}")
            for name in header:
                if name not in fieldnames:
                    fieldnames.append(name)

    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        for source, source_id in sources:
            with source.open("r", encoding="utf-8-sig", newline="") as handle:
                reader = csv.DictReader(handle)
                for row in reader:
                    for column in identity_columns:
                        if row.get(column):
                            row[column] = f"{source_id}:{row[column]}"
                    writer.writerow(row)


def build_dataset_manifest(
    settings: Settings,
    database: Database,
    dataset: dict,
    destination: Path,
) -> dict:
    lineage = dataset_lineage(database, dataset)
    if len(lineage) == 1:
        samples_path = dataset_file(settings.storage_root, dataset["id"], "samples")
        events_path = dataset_file(settings.storage_root, dataset["id"], "events")
    else:
        merged_root = destination.parent / "merged-dataset"
        samples_path = merged_root / "samples.csv"
        events_path = merged_root / "events.csv"
        merge_csv_files(
            [(dataset_file(settings.storage_root, item["id"], "samples"), item["id"]) for item in lineage],
            samples_path,
            ("session_id",),
        )
        merge_csv_files(
            [(dataset_file(settings.storage_root, item["id"], "events"), item["id"]) for item in lineage],
            events_path,
            ("session_id", "event_id"),
        )

    sample_rows = 0
    sample_sessions: set[str] = set()
    node_ids: set[int] = set()
    state_counts: Counter[str] = Counter()
    for chunk in pd.read_csv(samples_path, chunksize=200_000, low_memory=False):
        sample_rows += len(chunk)
        sample_sessions.update(chunk["session_id"].dropna().astype(str).unique().tolist())
        numeric_nodes = pd.to_numeric(chunk["node_id"], errors="coerce")
        node_ids.update(int(value) for value in numeric_nodes.dropna().unique().tolist())
        state_counts.update(chunk["state_label"].fillna("<missing>").astype(str).tolist())
    if node_ids != {1, 2, 3, 4}:
        raise ValueError(f"samples must contain node IDs 1,2,3,4; got {sorted(node_ids)}")

    events = pd.read_csv(events_path, low_memory=False)
    if events.empty:
        raise ValueError("events.csv must contain at least one event")
    event_sessions = set(events["session_id"].dropna().astype(str).unique().tolist())
    unknown_sessions = sorted(event_sessions - sample_sessions)
    if unknown_sessions:
        raise ValueError(f"events reference sessions missing from samples: {unknown_sessions}")

    manifest = {
        "schema_version": 1,
        "dataset_id": dataset["id"],
        "description": dataset["name"],
        "dataset_lineage": [item["id"] for item in lineage],
        "ingestion": {"kind": "processed_csv", "producer": "movetoplay-server-upload-v1"},
        "samples": {
            "path": str(samples_path.resolve()),
            "bytes": samples_path.stat().st_size,
            "sha256": sha256_file(samples_path),
            "rows": sample_rows,
            "session_count": len(sample_sessions),
            "node_ids": sorted(node_ids),
            "required_columns": sorted(SAMPLE_COLUMNS),
            "state_label_counts": counter_dict(state_counts),
        },
        "events": {
            "path": str(events_path.resolve()),
            "event_id_scope": "session" if len(lineage) > 1 else dataset["event_id_scope"],
            "bytes": events_path.stat().st_size,
            "sha256": sha256_file(events_path),
            "rows": int(len(events)),
            "session_count": len(event_sessions),
            "required_columns": sorted(EVENT_COLUMNS),
            "event_type_counts": counter_dict(
                Counter(events["event_type"].fillna("<missing>").astype(str).tolist())
            ),
        },
    }
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return manifest


def run_one(settings: Settings, database: Database) -> bool:
    job = database.claim_next_job()
    if job is None:
        return False
    job_root = settings.storage_root / "jobs" / job["id"]
    job_root.mkdir(parents=True, exist_ok=True)
    manifest_path = job_root / "dataset_manifest.json"
    log_path = job_root / "pipeline.log"
    run_dir = settings.storage_root / "artifacts" / "training-runs" / job["id"]
    try:
        database.update_job_progress(
            job["id"], "preparing", "正在合并官方数据与玩家所选数据", 5,
            settings.training_estimate_seconds,
        )
        dataset = database.get_dataset(job["dataset_id"])
        if dataset is None or dataset["status"] != "ready":
            raise RuntimeError("dataset is missing or no longer ready")
        build_dataset_manifest(settings, database, dataset, manifest_path)
        command = [
            sys.executable,
            str(PROJECT_ROOT / "tools" / "run_training_pipeline.py"),
            "--dataset-manifest",
            str(manifest_path),
            "--output-root",
            str(settings.storage_root / "artifacts" / "training-runs"),
            "--run-id",
            job["id"],
        ]
        if job["mode"] == "validate":
            command.append("--validate-only")
        environment = os.environ.copy()
        environment.setdefault("PYTHONHASHSEED", "0")
        with log_path.open("w", encoding="utf-8", newline="\n") as log:
            process = subprocess.Popen(
                command,
                cwd=PROJECT_ROOT,
                env=environment,
                stdout=log,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
            )
            current_model: str | None = None
            log.flush()
            with log_path.open("r", encoding="utf-8") as reader:
                while process.poll() is None:
                    line = reader.readline()
                    if not line:
                        time.sleep(0.25)
                        continue
                    parsed = _progress_from_line(line, current_model)
                    if parsed is not None:
                        stage, model_hint, percent = parsed
                        if model_hint is not None:
                            current_model = model_hint
                        remaining = int(settings.training_estimate_seconds * max(0.0, 1.0 - percent / 100.0))
                        database.update_job_progress(
                            job["id"], stage, _progress_detail(stage), percent, remaining
                        )
                for line in reader:
                    parsed = _progress_from_line(line, current_model)
                    if parsed is not None:
                        stage, model_hint, percent = parsed
                        if model_hint is not None:
                            current_model = model_hint
                        remaining = int(settings.training_estimate_seconds * max(0.0, 1.0 - percent / 100.0))
                        database.update_job_progress(
                            job["id"], stage, _progress_detail(stage), percent, remaining
                        )
            return_code = process.wait()
        if return_code != 0:
            error = f"pipeline exited with code {return_code}"
            run_manifest_path = run_dir / "run_manifest.json"
            if run_manifest_path.is_file():
                run_manifest = json.loads(run_manifest_path.read_text(encoding="utf-8"))
                error = run_manifest.get("error", {}).get("message") or error
            raise RuntimeError(error)
        final_status = "validated" if job["mode"] == "validate" else "passed"
        database.finish_job(job["id"], final_status, str(run_dir), None)
    except Exception as exc:
        with log_path.open("a", encoding="utf-8", newline="\n") as log:
            log.write("\n[worker:error]\n")
            log.write(traceback.format_exc())
        database.finish_job(job["id"], "failed", str(run_dir) if run_dir.exists() else None, str(exc))
    finally:
        if log_path.is_file() and run_dir.is_dir():
            shutil.copy2(log_path, run_dir / "worker.log")
        cleanup_old_jobs(settings, database)
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description="MoveToPlay training worker")
    parser.add_argument("--once", action="store_true", help="Process at most one queued job")
    args = parser.parse_args()
    os.umask(0o077)
    settings = Settings.from_env()
    settings.ensure_directories()
    database = Database(settings.database_path)
    database.initialize()
    database.recover_running_jobs()

    stopping = False

    def stop(_signum: int, _frame: Any) -> None:
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    while not stopping:
        processed = run_one(settings, database)
        if args.once:
            break
        if not processed:
            time.sleep(settings.worker_poll_seconds)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
