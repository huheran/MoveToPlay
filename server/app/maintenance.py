"""MoveToPlay 旧训练任务的本地清理策略。"""

from __future__ import annotations

import argparse
import shutil
from datetime import datetime, timezone
from pathlib import Path

from .config import Settings
from .database import Database


def _age_days(value: str | None) -> float:
    if not value:
        return 0.0
    moment = datetime.fromisoformat(value)
    if moment.tzinfo is None:
        moment = moment.replace(tzinfo=timezone.utc)
    return (datetime.now(timezone.utc) - moment).total_seconds() / 86400.0


def _safe_remove_tree(path: Path, allowed_root: Path, dry_run: bool) -> bool:
    resolved = path.resolve()
    root = allowed_root.resolve()
    if not resolved.is_relative_to(root) or resolved == root or not resolved.exists():
        return False
    if not dry_run:
        shutil.rmtree(resolved)
    return True


def cleanup_old_jobs(settings: Settings, database: Database, *, dry_run: bool = False) -> list[str]:
    """保留所有已批准模型；按状态清理可重建的旧任务产物。"""
    removed: list[str] = []
    artifacts_root = settings.storage_root / "artifacts" / "training-runs"
    jobs_root = settings.storage_root / "jobs"
    for job in database.list_jobs():
        if job.get("approved_at") or job.get("status") in {"queued", "running"}:
            continue
        status = str(job.get("status"))
        retention = {
            "failed": settings.cleanup_failed_days,
            "validated": settings.cleanup_validated_days,
            "passed": settings.cleanup_unapproved_days,
        }.get(status)
        if retention is None or _age_days(job.get("finished_at")) < retention:
            continue

        changed = False
        run_dir = job.get("run_dir")
        if run_dir:
            changed |= _safe_remove_tree(Path(run_dir), artifacts_root, dry_run)
        changed |= _safe_remove_tree(jobs_root / str(job["id"]), jobs_root, dry_run)
        if changed:
            removed.append(str(job["id"]))
            if not dry_run:
                database.mark_artifacts_cleaned(str(job["id"]))
    return removed


def main() -> int:
    parser = argparse.ArgumentParser(description="清理 MoveToPlay 旧训练任务")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    settings = Settings.from_env()
    settings.ensure_directories()
    database = Database(settings.database_path)
    database.initialize()
    for job_id in cleanup_old_jobs(settings, database, dry_run=args.dry_run):
        print(job_id)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
