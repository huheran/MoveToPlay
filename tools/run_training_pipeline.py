#!/usr/bin/env python3
"""Run the reproducible MoveToPlay processed-CSV -> RF -> C pipeline.

The runner deliberately writes every result beneath a unique run directory.
It never overwrites the checked-in firmware arrays or the recovered model
bundles.  The same entrypoint is intended for local, Docker, and server jobs.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.metadata
import json
import os
import platform
import shlex
import subprocess
import sys
import traceback
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import pandas as pd


PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DATASET_MANIFEST = PROJECT_ROOT / "training" / "datasets" / "movetoplay-latest-v2.json"
DEFAULT_PIPELINE_CONFIG = PROJECT_ROOT / "training" / "pipeline.json"
DEFAULT_OUTPUT_ROOT = PROJECT_ROOT / "output" / "training-runs"
DEFAULT_REFERENCE_GENERATED = PROJECT_ROOT / "main" / "generated"
BASE_CHANNELS = ["ax", "ay", "az", "gx", "gy", "gz"]


class PipelineError(RuntimeError):
    """A user-actionable pipeline failure."""


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise PipelineError(f"JSON file does not exist: {path}") from exc
    except json.JSONDecodeError as exc:
        raise PipelineError(f"invalid JSON in {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise PipelineError(f"expected a JSON object in {path}")
    return value


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def resolve_project_path(value: str | Path) -> Path:
    path = Path(value)
    if not path.is_absolute():
        path = PROJECT_ROOT / path
    return path.resolve()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def normalized_text_sha256(path: Path) -> str:
    text = path.read_text(encoding="utf-8").replace("\r\n", "\n").replace("\r", "\n")
    return hashlib.sha256(text.encode("utf-8")).hexdigest().upper()


def counter_to_dict(counter: Counter[str]) -> dict[str, int]:
    return {key: int(counter[key]) for key in sorted(counter)}


def compare_expected(
    errors: list[str],
    label: str,
    actual: Any,
    expected: Any,
) -> None:
    if actual != expected:
        errors.append(f"{label}: expected {expected!r}, got {actual!r}")


def validate_samples(path: Path, spec: dict[str, Any]) -> tuple[dict[str, Any], list[str]]:
    errors: list[str] = []
    if not path.is_file():
        return {"path": str(path), "exists": False}, [f"samples file does not exist: {path}"]

    actual_size = path.stat().st_size
    actual_hash = sha256_file(path)
    compare_expected(errors, "samples bytes", actual_size, int(spec["bytes"]))
    compare_expected(errors, "samples sha256", actual_hash, str(spec["sha256"]).upper())

    header = list(pd.read_csv(path, nrows=0).columns)
    required_columns = [str(value) for value in spec["required_columns"]]
    missing_columns = sorted(set(required_columns) - set(header))
    if missing_columns:
        errors.append(f"samples missing required columns: {missing_columns}")
        return {
            "path": str(path),
            "exists": True,
            "bytes": actual_size,
            "sha256": actual_hash,
            "columns": header,
        }, errors

    rows = 0
    sessions: set[str] = set()
    node_ids: set[int] = set()
    state_counts: Counter[str] = Counter()
    invalid_required_numeric: Counter[str] = Counter()
    numeric_required = ["pc_timestamp_ms", "node_id", *BASE_CHANNELS]

    for chunk in pd.read_csv(path, chunksize=200_000, low_memory=False):
        rows += len(chunk)
        sessions.update(chunk["session_id"].dropna().astype(str).unique().tolist())
        numeric_nodes = pd.to_numeric(chunk["node_id"], errors="coerce")
        node_ids.update(int(value) for value in numeric_nodes.dropna().unique().tolist())
        state_counts.update(chunk["state_label"].fillna("<missing>").astype(str).tolist())
        for column in numeric_required:
            invalid_required_numeric[column] += int(pd.to_numeric(chunk[column], errors="coerce").isna().sum())

    state_count_dict = counter_to_dict(state_counts)
    invalid_dict = {key: int(value) for key, value in invalid_required_numeric.items() if value}
    compare_expected(errors, "samples rows", rows, int(spec["rows"]))
    compare_expected(errors, "samples session_count", len(sessions), int(spec["session_count"]))
    compare_expected(errors, "samples node_ids", sorted(node_ids), sorted(int(v) for v in spec["node_ids"]))
    compare_expected(errors, "samples state_label_counts", state_count_dict, spec["state_label_counts"])
    if invalid_dict:
        errors.append(f"samples contain invalid required numeric values: {invalid_dict}")

    return {
        "path": str(path),
        "exists": True,
        "bytes": actual_size,
        "sha256": actual_hash,
        "columns": header,
        "rows": rows,
        "session_count": len(sessions),
        "sessions": sorted(sessions),
        "node_ids": sorted(node_ids),
        "state_label_counts": state_count_dict,
        "invalid_required_numeric": invalid_dict,
    }, errors


def validate_events(path: Path, spec: dict[str, Any]) -> tuple[dict[str, Any], list[str]]:
    errors: list[str] = []
    if not path.is_file():
        return {"path": str(path), "exists": False}, [f"events file does not exist: {path}"]

    actual_size = path.stat().st_size
    actual_hash = sha256_file(path)
    compare_expected(errors, "events bytes", actual_size, int(spec["bytes"]))
    compare_expected(errors, "events sha256", actual_hash, str(spec["sha256"]).upper())

    frame = pd.read_csv(path, low_memory=False)
    header = list(frame.columns)
    required_columns = [str(value) for value in spec["required_columns"]]
    missing_columns = sorted(set(required_columns) - set(header))
    if missing_columns:
        errors.append(f"events missing required columns: {missing_columns}")
        return {
            "path": str(path),
            "exists": True,
            "bytes": actual_size,
            "sha256": actual_hash,
            "columns": header,
        }, errors

    event_counts = counter_to_dict(Counter(frame["event_type"].fillna("<missing>").astype(str).tolist()))
    sessions = sorted(frame["session_id"].dropna().astype(str).unique().tolist())
    invalid_event_times = int(pd.to_numeric(frame["pc_timestamp_ms"], errors="coerce").isna().sum())
    duplicate_event_ids = int(frame["event_id"].astype(str).duplicated().sum())
    duplicate_session_event_ids = int(frame[["session_id", "event_id"]].astype(str).duplicated().sum())

    compare_expected(errors, "events rows", len(frame), int(spec["rows"]))
    compare_expected(errors, "events session_count", len(sessions), int(spec["session_count"]))
    compare_expected(errors, "events event_type_counts", event_counts, spec["event_type_counts"])
    if invalid_event_times:
        errors.append(f"events contain {invalid_event_times} invalid pc_timestamp_ms values")
    event_id_scope = str(spec.get("event_id_scope", "global"))
    if event_id_scope not in {"global", "session"}:
        errors.append(f"unsupported events event_id_scope: {event_id_scope!r}")
    if event_id_scope == "global" and duplicate_event_ids:
        errors.append(f"events contain {duplicate_event_ids} duplicate global event_id values")
    if duplicate_session_event_ids:
        errors.append(
            "events contain "
            f"{duplicate_session_event_ids} duplicate (session_id, event_id) values"
        )

    return {
        "path": str(path),
        "exists": True,
        "bytes": actual_size,
        "sha256": actual_hash,
        "columns": header,
        "rows": int(len(frame)),
        "session_count": len(sessions),
        "sessions": sessions,
        "event_type_counts": event_counts,
        "invalid_event_times": invalid_event_times,
        "event_id_scope": event_id_scope,
        "duplicate_event_ids": duplicate_event_ids,
        "duplicate_session_event_ids": duplicate_session_event_ids,
    }, errors


def validate_dataset(manifest: dict[str, Any]) -> dict[str, Any]:
    if int(manifest.get("schema_version", 0)) != 1:
        raise PipelineError(f"unsupported dataset manifest schema_version: {manifest.get('schema_version')}")
    samples_spec = manifest.get("samples")
    events_spec = manifest.get("events")
    if not isinstance(samples_spec, dict) or not isinstance(events_spec, dict):
        raise PipelineError("dataset manifest must contain samples and events objects")

    samples_path = resolve_project_path(str(samples_spec["path"]))
    events_path = resolve_project_path(str(events_spec["path"]))
    samples_report, sample_errors = validate_samples(samples_path, samples_spec)
    events_report, event_errors = validate_events(events_path, events_spec)
    errors = sample_errors + event_errors
    return {
        "ok": not errors,
        "dataset_id": manifest.get("dataset_id"),
        "validated_at": utc_now(),
        "samples": samples_report,
        "events": events_report,
        "errors": errors,
    }


def command_text(command: list[str]) -> str:
    if os.name == "nt":
        return subprocess.list2cmdline(command)
    return shlex.join(command)


def run_command(command: list[str], log_path: Path) -> None:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    rendered = command_text(command)
    print(f"\n$ {rendered}", flush=True)
    env = os.environ.copy()
    env.setdefault("PYTHONUTF8", "1")
    env.setdefault("PYTHONHASHSEED", "0")
    env["PYTHONUNBUFFERED"] = "1"
    with log_path.open("w", encoding="utf-8", newline="\n") as log:
        log.write(f"$ {rendered}\n\n")
        process = subprocess.Popen(
            command,
            cwd=PROJECT_ROOT,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
        assert process.stdout is not None
        for line in process.stdout:
            print(line, end="", flush=True)
            log.write(line)
        return_code = process.wait()
    if return_code != 0:
        raise PipelineError(f"command failed with exit code {return_code}: {rendered}")


def safe_git_value(*args: str) -> str | None:
    try:
        completed = subprocess.run(
            ["git", *args],
            cwd=PROJECT_ROOT,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            encoding="utf-8",
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    return completed.stdout.strip()


def source_control_metadata() -> dict[str, Any]:
    status = safe_git_value("status", "--porcelain")
    packaged_commit = os.getenv("MOVETOPLAY_SOURCE_COMMIT")
    return {
        "commit": safe_git_value("rev-parse", "HEAD") or packaged_commit,
        "branch": safe_git_value("branch", "--show-current") or ("packaged-release" if packaged_commit else None),
        "dirty": bool(status) if status is not None else (False if packaged_commit else None),
        "status": status.splitlines() if status else [],
    }


def environment_metadata() -> dict[str, Any]:
    packages: dict[str, str] = {}
    for name in ["joblib", "numpy", "pandas", "scikit-learn", "scipy", "threadpoolctl"]:
        try:
            packages[name] = importlib.metadata.version(name)
        except importlib.metadata.PackageNotFoundError:
            packages[name] = "missing"
    return {
        "python": sys.version.replace("\n", " "),
        "executable": sys.executable,
        "platform": platform.platform(),
        "packages": packages,
    }


def quality_gate(
    model_name: str,
    training_summary: dict[str, Any],
    export_summary: dict[str, Any],
    quality: dict[str, Any],
) -> dict[str, Any]:
    checks: list[dict[str, Any]] = []

    def add_check(name: str, ok: bool, actual: Any, expected: Any) -> None:
        checks.append({"name": name, "ok": bool(ok), "actual": actual, "expected": expected})

    accuracy = float(training_summary["accuracy"])
    macro_f1 = float(training_summary["report"]["macro avg"]["f1-score"])
    class_names = list(training_summary["class_names"])
    required_classes = list(quality["required_classes"])
    add_check("accuracy", accuracy >= float(quality["min_accuracy"]), accuracy, f">={quality['min_accuracy']}")
    add_check("macro_f1", macro_f1 >= float(quality["min_macro_f1"]), macro_f1, f">={quality['min_macro_f1']}")
    add_check("class_names", class_names == required_classes, class_names, required_classes)
    add_check(
        "feature_count",
        int(training_summary["feature_count"]) == int(quality["feature_count"]),
        int(training_summary["feature_count"]),
        int(quality["feature_count"]),
    )
    add_check(
        "export_feature_count",
        int(export_summary["feature_count"]) == int(quality["feature_count"]),
        int(export_summary["feature_count"]),
        int(quality["feature_count"]),
    )
    add_check(
        "tree_count",
        int(export_summary["tree_count"]) == int(quality["tree_count"]),
        int(export_summary["tree_count"]),
        int(quality["tree_count"]),
    )
    add_check(
        "export_class_names",
        list(export_summary["class_names"]) == required_classes,
        list(export_summary["class_names"]),
        required_classes,
    )
    min_recall = float(quality["min_class_recall"])
    for class_name in required_classes:
        recall = float(training_summary["report"].get(class_name, {}).get("recall", -1.0))
        add_check(f"recall:{class_name}", recall >= min_recall, recall, f">={min_recall}")

    return {
        "model": model_name,
        "ok": all(check["ok"] for check in checks),
        "checks": checks,
    }


def compare_generated_to_reference(
    generated_dir: Path,
    reference_dir: Path,
    file_prefix: str,
) -> dict[str, Any]:
    files: list[dict[str, Any]] = []
    for suffix in [".c", ".h", "_summary.json"]:
        name = f"{file_prefix}{suffix}"
        generated_path = generated_dir / name
        reference_path = reference_dir / name
        exists = generated_path.is_file() and reference_path.is_file()
        normalized_match = exists and normalized_text_sha256(generated_path) == normalized_text_sha256(reference_path)
        files.append(
            {
                "name": name,
                "reference_exists": reference_path.is_file(),
                "generated_exists": generated_path.is_file(),
                "normalized_text_match": normalized_match,
            }
        )
    return {"ok": all(item["normalized_text_match"] for item in files), "files": files}


def artifact_hashes(run_dir: Path) -> list[dict[str, Any]]:
    artifacts: list[dict[str, Any]] = []
    for base_name in ["models", "generated", "quality"]:
        base = run_dir / base_name
        if not base.exists():
            continue
        for path in sorted(item for item in base.rglob("*") if item.is_file()):
            artifacts.append(
                {
                    "path": path.relative_to(run_dir).as_posix(),
                    "bytes": path.stat().st_size,
                    "sha256": sha256_file(path),
                }
            )
    return artifacts


def make_run_id() -> str:
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    commit = safe_git_value("rev-parse", "--short=8", "HEAD")
    if not commit:
        commit = (os.getenv("MOVETOPLAY_SOURCE_COMMIT") or "nogit")[:8]
    return f"{timestamp}-{commit}"


def run_pipeline(args: argparse.Namespace) -> Path:
    dataset_manifest_path = resolve_project_path(args.dataset_manifest)
    pipeline_config_path = resolve_project_path(args.pipeline_config)
    output_root = resolve_project_path(args.output_root)
    reference_dir = resolve_project_path(args.reference_generated)
    dataset_manifest = load_json(dataset_manifest_path)
    pipeline_config = load_json(pipeline_config_path)
    if int(pipeline_config.get("schema_version", 0)) != 1:
        raise PipelineError(f"unsupported pipeline schema_version: {pipeline_config.get('schema_version')}")

    run_id = args.run_id or make_run_id()
    run_dir = output_root / run_id
    if run_dir.exists():
        raise PipelineError(f"run directory already exists: {run_dir}")
    run_dir.mkdir(parents=True)

    run_manifest: dict[str, Any] = {
        "schema_version": 1,
        "run_id": run_id,
        "status": "running",
        "started_at": utc_now(),
        "finished_at": None,
        "dataset_manifest": str(dataset_manifest_path),
        "dataset_manifest_sha256": sha256_file(dataset_manifest_path),
        "pipeline_config": str(pipeline_config_path),
        "pipeline_config_sha256": sha256_file(pipeline_config_path),
        "source_control": source_control_metadata(),
        "environment": environment_metadata(),
        "models": [],
        "artifacts": [],
        "error": None,
    }
    write_json(run_dir / "run_manifest.json", run_manifest)

    try:
        print(f"[pipeline] run_id={run_id}")
        print(f"[pipeline] output={run_dir}")
        print(f"[pipeline] validating dataset={dataset_manifest.get('dataset_id')}")
        validation = validate_dataset(dataset_manifest)
        write_json(run_dir / "dataset_validation.json", validation)
        if not validation["ok"]:
            raise PipelineError("dataset validation failed:\n- " + "\n- ".join(validation["errors"]))
        print(
            "[pipeline] dataset valid: "
            f"samples={validation['samples']['rows']} events={validation['events']['rows']}"
        )

        if args.validate_only:
            run_manifest["status"] = "validated"
            run_manifest["dataset_validation"] = validation
            return run_dir

        samples_path = Path(validation["samples"]["path"])
        events_path = Path(validation["events"]["path"])
        generated_dir = run_dir / "generated"
        generated_dir.mkdir()
        quality_dir = run_dir / "quality"
        quality_dir.mkdir()

        for model_config in pipeline_config["models"]:
            name = str(model_config["name"])
            print(f"\n[pipeline] training model={name}")
            model_output = run_dir / "models" / name
            train_command = [
                sys.executable,
                str(PROJECT_ROOT / "tools" / "train_event_rf.py"),
                "--samples",
                str(samples_path),
                "--events",
                str(events_path),
                "--output-dir",
                str(model_output),
                *[str(value) for value in model_config["training_args"]],
            ]
            run_command(train_command, run_dir / "logs" / f"train_{name}.log")

            export_config = model_config["export"]
            export_command = [
                sys.executable,
                str(PROJECT_ROOT / "tools" / "export_rf_model_c.py"),
                "--model",
                str(model_output / "rf_model.joblib"),
                "--output-dir",
                str(generated_dir),
                "--file-prefix",
                str(export_config["file_prefix"]),
                "--symbol-prefix",
                str(export_config["symbol_prefix"]),
            ]
            run_command(export_command, run_dir / "logs" / f"export_{name}.log")

            training_summary = load_json(model_output / "training_summary.json")
            export_summary = load_json(generated_dir / f"{export_config['file_prefix']}_summary.json")
            gate = quality_gate(name, training_summary, export_summary, model_config["quality"])
            reference = compare_generated_to_reference(
                generated_dir,
                reference_dir,
                str(export_config["file_prefix"]),
            )
            model_result = {
                "name": name,
                "training_summary": training_summary,
                "export_summary": export_summary,
                "quality_gate": gate,
                "reference_generated_comparison": reference,
            }
            write_json(quality_dir / f"{name}.json", model_result)
            run_manifest["models"].append(model_result)
            if not gate["ok"]:
                failed = [check["name"] for check in gate["checks"] if not check["ok"]]
                raise PipelineError(f"quality gate failed for {name}: {failed}")
            if args.require_reference_match and not reference["ok"]:
                raise PipelineError(f"generated C does not match reference for {name}")
            print(
                f"[pipeline] model={name} quality=PASS "
                f"accuracy={training_summary['accuracy']:.6f} "
                f"reference_match={reference['ok']}"
            )

        run_manifest["status"] = "passed"
        run_manifest["artifacts"] = artifact_hashes(run_dir)
        return run_dir
    except Exception as exc:
        run_manifest["status"] = "failed"
        run_manifest["error"] = {
            "type": type(exc).__name__,
            "message": str(exc),
            "traceback": traceback.format_exc(),
        }
        raise
    finally:
        run_manifest["finished_at"] = utc_now()
        write_json(run_dir / "run_manifest.json", run_manifest)


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run the MoveToPlay dual-RF training pipeline")
    parser.add_argument("--dataset-manifest", default=str(DEFAULT_DATASET_MANIFEST))
    parser.add_argument("--pipeline-config", default=str(DEFAULT_PIPELINE_CONFIG))
    parser.add_argument("--output-root", default=str(DEFAULT_OUTPUT_ROOT))
    parser.add_argument("--reference-generated", default=str(DEFAULT_REFERENCE_GENERATED))
    parser.add_argument("--run-id", help="Explicit immutable run directory name")
    parser.add_argument("--validate-only", action="store_true", help="Validate the dataset without training")
    parser.add_argument(
        "--require-reference-match",
        action="store_true",
        help="Fail unless newly trained C arrays match the currently deployed arrays",
    )
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()
    try:
        run_dir = run_pipeline(args)
    except PipelineError as exc:
        print(f"[pipeline:error] {exc}", file=sys.stderr)
        return 2
    except Exception as exc:
        print(f"[pipeline:error] unexpected failure: {exc}", file=sys.stderr)
        traceback.print_exc()
        return 1
    print(f"[pipeline] complete: {run_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
