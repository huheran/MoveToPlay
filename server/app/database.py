"""API 与训练 worker 共用的 SQLite 状态库。"""

from __future__ import annotations

import sqlite3
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterator


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


class Database:
    def __init__(self, path: Path):
        self.path = path

    @contextmanager
    def connect(self) -> Iterator[sqlite3.Connection]:
        connection = sqlite3.connect(self.path, timeout=30)
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA foreign_keys = ON")
        connection.execute("PRAGMA busy_timeout = 30000")
        try:
            yield connection
            connection.commit()
        finally:
            connection.close()

    def initialize(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self.connect() as connection:
            connection.execute("PRAGMA journal_mode = WAL")
            connection.executescript(
                """
                CREATE TABLE IF NOT EXISTS datasets (
                    id TEXT PRIMARY KEY,
                    name TEXT NOT NULL,
                    status TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    completed_at TEXT,
                    samples_filename TEXT NOT NULL,
                    samples_expected_bytes INTEGER NOT NULL,
                    samples_expected_sha256 TEXT NOT NULL,
                    samples_received_bytes INTEGER NOT NULL DEFAULT 0,
                    events_filename TEXT NOT NULL,
                    events_expected_bytes INTEGER NOT NULL,
                    events_expected_sha256 TEXT NOT NULL,
                    events_received_bytes INTEGER NOT NULL DEFAULT 0,
                    event_id_scope TEXT NOT NULL,
                    base_dataset_id TEXT REFERENCES datasets(id),
                    error TEXT
                );

                CREATE TABLE IF NOT EXISTS jobs (
                    id TEXT PRIMARY KEY,
                    dataset_id TEXT NOT NULL REFERENCES datasets(id),
                    mode TEXT NOT NULL,
                    status TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    started_at TEXT,
                    finished_at TEXT,
                    run_dir TEXT,
                    error TEXT,
                    approved_at TEXT,
                    approved_by TEXT,
                    progress_stage TEXT NOT NULL DEFAULT 'queued',
                    progress_detail TEXT,
                    progress_percent REAL NOT NULL DEFAULT 0,
                    estimated_remaining_seconds INTEGER,
                    progress_updated_at TEXT,
                    model_version TEXT,
                    is_active_model INTEGER NOT NULL DEFAULT 0,
                    oss_backup_status TEXT NOT NULL DEFAULT 'not_requested',
                    oss_object_key TEXT,
                    oss_backed_up_at TEXT,
                    oss_backup_error TEXT,
                    artifacts_cleaned_at TEXT,
                    firmware_status TEXT NOT NULL DEFAULT 'not_requested',
                    firmware_detail TEXT,
                    firmware_progress_percent REAL NOT NULL DEFAULT 0,
                    firmware_built_at TEXT,
                    firmware_error TEXT
                );

                CREATE INDEX IF NOT EXISTS jobs_status_created_idx
                    ON jobs(status, created_at);
                """
            )
            columns = {
                row["name"] for row in connection.execute("PRAGMA table_info(datasets)").fetchall()
            }
            if "event_id_scope" not in columns:
                connection.execute(
                    "ALTER TABLE datasets ADD COLUMN event_id_scope TEXT NOT NULL DEFAULT 'global'"
                )
            if "base_dataset_id" not in columns:
                connection.execute(
                    "ALTER TABLE datasets ADD COLUMN base_dataset_id TEXT REFERENCES datasets(id)"
                )
            job_columns = {
                row["name"] for row in connection.execute("PRAGMA table_info(jobs)").fetchall()
            }
            migrations = {
                "progress_stage": "TEXT NOT NULL DEFAULT 'queued'",
                "progress_detail": "TEXT",
                "progress_percent": "REAL NOT NULL DEFAULT 0",
                "estimated_remaining_seconds": "INTEGER",
                "progress_updated_at": "TEXT",
                "model_version": "TEXT",
                "is_active_model": "INTEGER NOT NULL DEFAULT 0",
                "oss_backup_status": "TEXT NOT NULL DEFAULT 'not_requested'",
                "oss_object_key": "TEXT",
                "oss_backed_up_at": "TEXT",
                "oss_backup_error": "TEXT",
                "artifacts_cleaned_at": "TEXT",
                "firmware_status": "TEXT NOT NULL DEFAULT 'not_requested'",
                "firmware_detail": "TEXT",
                "firmware_progress_percent": "REAL NOT NULL DEFAULT 0",
                "firmware_built_at": "TEXT",
                "firmware_error": "TEXT",
            }
            for name, definition in migrations.items():
                if name not in job_columns:
                    connection.execute(f"ALTER TABLE jobs ADD COLUMN {name} {definition}")
            # 旧版本任务在增加进度字段时会得到 queued/0 的默认值。把已经结束的
            # 历史记录归一化，避免模型版本库把旧模型误显示成仍在排队。
            connection.execute(
                """UPDATE jobs SET progress_stage = 'completed',
                       progress_detail = '训练与模型导出已完成', progress_percent = 100,
                       estimated_remaining_seconds = 0,
                       progress_updated_at = COALESCE(progress_updated_at, finished_at)
                   WHERE status IN ('passed', 'validated')
                     AND (progress_stage = 'queued' OR progress_percent < 100)"""
            )
            connection.execute(
                """UPDATE jobs SET progress_stage = 'failed',
                       progress_detail = COALESCE(progress_detail, '训练任务失败'),
                       estimated_remaining_seconds = 0,
                       progress_updated_at = COALESCE(progress_updated_at, finished_at)
                   WHERE status = 'failed' AND progress_stage = 'queued'"""
            )
            approved_rows = connection.execute(
                "SELECT * FROM jobs WHERE approved_at IS NOT NULL AND model_version IS NULL"
            ).fetchall()
            for row in approved_rows:
                connection.execute(
                    "UPDATE jobs SET model_version = ? WHERE id = ?",
                    (self._model_version(dict(row)), row["id"]),
                )
            active_count = connection.execute(
                "SELECT COUNT(*) FROM jobs WHERE is_active_model = 1"
            ).fetchone()[0]
            if active_count == 0:
                latest = connection.execute(
                    "SELECT id FROM jobs WHERE approved_at IS NOT NULL ORDER BY approved_at DESC LIMIT 1"
                ).fetchone()
                if latest is not None:
                    connection.execute("UPDATE jobs SET is_active_model = 1 WHERE id = ?", (latest["id"],))
        self.path.chmod(0o600)

    @staticmethod
    def _row(row: sqlite3.Row | None) -> dict[str, Any] | None:
        return dict(row) if row is not None else None

    def create_dataset(self, record: dict[str, Any]) -> dict[str, Any]:
        with self.connect() as connection:
            connection.execute(
                """
                INSERT INTO datasets (
                    id, name, status, created_at,
                    samples_filename, samples_expected_bytes, samples_expected_sha256,
                    events_filename, events_expected_bytes, events_expected_sha256,
                    event_id_scope, base_dataset_id
                ) VALUES (?, ?, 'uploading', ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    record["id"],
                    record["name"],
                    utc_now(),
                    record["samples_filename"],
                    record["samples_expected_bytes"],
                    record["samples_expected_sha256"],
                    record["events_filename"],
                    record["events_expected_bytes"],
                    record["events_expected_sha256"],
                    record["event_id_scope"],
                    record.get("base_dataset_id"),
                ),
            )
        result = self.get_dataset(record["id"])
        assert result is not None
        return result

    def get_dataset(self, dataset_id: str) -> dict[str, Any] | None:
        with self.connect() as connection:
            return self._row(connection.execute("SELECT * FROM datasets WHERE id = ?", (dataset_id,)).fetchone())

    def list_datasets(self) -> list[dict[str, Any]]:
        with self.connect() as connection:
            rows = connection.execute("SELECT * FROM datasets ORDER BY created_at DESC").fetchall()
        return [dict(row) for row in rows]

    def update_received(self, dataset_id: str, kind: str, old_bytes: int, new_bytes: int) -> bool:
        column = "samples_received_bytes" if kind == "samples" else "events_received_bytes"
        with self.connect() as connection:
            cursor = connection.execute(
                f"UPDATE datasets SET {column} = ? WHERE id = ? AND {column} = ? AND status = 'uploading'",
                (new_bytes, dataset_id, old_bytes),
            )
            return cursor.rowcount == 1

    def mark_dataset_ready(self, dataset_id: str) -> None:
        with self.connect() as connection:
            connection.execute(
                "UPDATE datasets SET status = 'ready', completed_at = ?, error = NULL WHERE id = ?",
                (utc_now(), dataset_id),
            )

    def reject_dataset(self, dataset_id: str, error: str) -> None:
        with self.connect() as connection:
            connection.execute(
                "UPDATE datasets SET status = 'rejected', completed_at = ?, error = ? WHERE id = ?",
                (utc_now(), error[:2000], dataset_id),
            )

    def create_job(self, job_id: str, dataset_id: str, mode: str) -> dict[str, Any]:
        with self.connect() as connection:
            connection.execute(
                "INSERT INTO jobs (id, dataset_id, mode, status, created_at) VALUES (?, ?, ?, 'queued', ?)",
                (job_id, dataset_id, mode, utc_now()),
            )
        result = self.get_job(job_id)
        assert result is not None
        return result

    def get_job(self, job_id: str) -> dict[str, Any] | None:
        with self.connect() as connection:
            return self._row(connection.execute("SELECT * FROM jobs WHERE id = ?", (job_id,)).fetchone())

    def list_jobs(self) -> list[dict[str, Any]]:
        with self.connect() as connection:
            rows = connection.execute("SELECT * FROM jobs ORDER BY created_at DESC").fetchall()
        return [dict(row) for row in rows]

    def claim_next_job(self) -> dict[str, Any] | None:
        with self.connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            row = connection.execute(
                "SELECT * FROM jobs WHERE status = 'queued' ORDER BY created_at, id LIMIT 1"
            ).fetchone()
            if row is None:
                return None
            connection.execute(
                """UPDATE jobs SET status = 'running', started_at = ?,
                   progress_stage = 'preparing', progress_detail = '正在准备训练数据',
                   progress_percent = 2, estimated_remaining_seconds = NULL, progress_updated_at = ?
                   WHERE id = ? AND status = 'queued'""",
                (utc_now(), utc_now(), row["id"]),
            )
            claimed = connection.execute("SELECT * FROM jobs WHERE id = ?", (row["id"],)).fetchone()
            return self._row(claimed)

    def recover_running_jobs(self) -> int:
        """单 worker 重启后，把中断的任务放回队列。"""
        with self.connect() as connection:
            cursor = connection.execute(
                """UPDATE jobs SET status = 'queued', started_at = NULL,
                   progress_stage = 'queued', progress_detail = '服务重启后已重新排队',
                   progress_percent = 0, estimated_remaining_seconds = NULL
                   WHERE status = 'running'"""
            )
            connection.execute(
                """UPDATE jobs SET firmware_status = 'queued',
                   firmware_detail = '服务重启后已重新排队', firmware_progress_percent = 0
                   WHERE firmware_status = 'building'"""
            )
            return cursor.rowcount

    def queue_firmware_build(self, job_id: str, *, force: bool = False) -> bool:
        with self.connect() as connection:
            row = connection.execute("SELECT * FROM jobs WHERE id = ?", (job_id,)).fetchone()
            if row is None or row["status"] != "passed" or row["mode"] != "train":
                return False
            if row["firmware_status"] in {"queued", "building"}:
                return True
            if row["firmware_status"] == "ready" and not force:
                return True
            connection.execute(
                """UPDATE jobs SET firmware_status = 'queued',
                   firmware_detail = '等待云端固件编译', firmware_progress_percent = 0,
                   firmware_error = NULL WHERE id = ?""",
                (job_id,),
            )
            return True

    def claim_next_firmware_job(self) -> dict[str, Any] | None:
        with self.connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            row = connection.execute(
                """SELECT * FROM jobs WHERE firmware_status = 'queued' AND status = 'passed'
                   ORDER BY COALESCE(approved_at, finished_at, created_at), id LIMIT 1"""
            ).fetchone()
            if row is None:
                return None
            connection.execute(
                """UPDATE jobs SET firmware_status = 'building',
                   firmware_detail = '正在准备云端固件编译', firmware_progress_percent = 2,
                   firmware_error = NULL WHERE id = ? AND firmware_status = 'queued'""",
                (row["id"],),
            )
            return self._row(connection.execute("SELECT * FROM jobs WHERE id = ?", (row["id"],)).fetchone())

    def update_firmware_progress(self, job_id: str, detail: str, percent: float) -> None:
        with self.connect() as connection:
            connection.execute(
                """UPDATE jobs SET firmware_detail = ?,
                   firmware_progress_percent = MAX(firmware_progress_percent, ?)
                   WHERE id = ? AND firmware_status = 'building'""",
                (detail[:500], max(0.0, min(99.0, percent)), job_id),
            )

    def finish_firmware_build(self, job_id: str, error: str | None = None) -> None:
        ready = error is None
        with self.connect() as connection:
            connection.execute(
                """UPDATE jobs SET firmware_status = ?, firmware_detail = ?,
                   firmware_progress_percent = ?, firmware_built_at = ?, firmware_error = ?
                   WHERE id = ?""",
                (
                    "ready" if ready else "failed",
                    "云端固件已编译、打包并通过完整性校验" if ready else "云端固件编译失败",
                    100 if ready else 0,
                    utc_now() if ready else None,
                    error[:4000] if error else None,
                    job_id,
                ),
            )

    def finish_job(self, job_id: str, status: str, run_dir: str | None, error: str | None) -> None:
        stage = "completed" if status in {"passed", "validated"} else "failed"
        percent = 100.0 if status in {"passed", "validated"} else 0.0
        with self.connect() as connection:
            connection.execute(
                """UPDATE jobs SET status = ?, finished_at = ?, run_dir = ?, error = ?,
                   progress_stage = ?, progress_detail = ?, progress_percent = ?,
                   estimated_remaining_seconds = 0, progress_updated_at = ? WHERE id = ?""",
                (
                    status,
                    utc_now(),
                    run_dir,
                    error[:4000] if error else None,
                    stage,
                    "训练与模型导出已完成" if percent == 100 else "训练任务失败",
                    percent,
                    utc_now(),
                    job_id,
                ),
            )

    def update_job_progress(
        self,
        job_id: str,
        stage: str,
        detail: str,
        percent: float,
        estimated_remaining_seconds: int | None,
    ) -> None:
        with self.connect() as connection:
            connection.execute(
                """UPDATE jobs SET progress_stage = ?, progress_detail = ?, progress_percent = ?,
                   estimated_remaining_seconds = ?, progress_updated_at = ?
                   WHERE id = ? AND status = 'running'""",
                (
                    stage[:80],
                    detail[:500],
                    max(0.0, min(99.0, percent)),
                    estimated_remaining_seconds,
                    utc_now(),
                    job_id,
                ),
            )

    def approve_job(self, job_id: str, approved_by: str) -> bool:
        with self.connect() as connection:
            row = connection.execute("SELECT * FROM jobs WHERE id = ?", (job_id,)).fetchone()
            if row is None or row["status"] != "passed":
                return False
            version = row["model_version"] or self._model_version(dict(row))
            connection.execute("UPDATE jobs SET is_active_model = 0 WHERE is_active_model = 1")
            cursor = connection.execute(
                """
                UPDATE jobs SET approved_at = COALESCE(approved_at, ?),
                    approved_by = COALESCE(approved_by, ?), model_version = ?, is_active_model = 1,
                    oss_backup_status = CASE
                        WHEN oss_backup_status = 'completed' THEN oss_backup_status ELSE 'pending' END
                WHERE id = ? AND status = 'passed'
                """,
                (utc_now(), approved_by[:100], version, job_id),
            )
            return cursor.rowcount == 1

    @staticmethod
    def _model_version(row: dict[str, Any]) -> str:
        try:
            value = datetime.fromisoformat(str(row.get("finished_at") or row.get("created_at")))
            compact = value.strftime("%Y%m%d-%H%M%S")
        except (TypeError, ValueError):
            compact = "unknown"
        return f"M-{compact}-{str(row['id'])[:8]}"

    def list_models(self) -> list[dict[str, Any]]:
        with self.connect() as connection:
            rows = connection.execute(
                "SELECT * FROM jobs WHERE approved_at IS NOT NULL ORDER BY approved_at DESC"
            ).fetchall()
        return [dict(row) for row in rows]

    def activate_model(self, job_id: str) -> bool:
        with self.connect() as connection:
            row = connection.execute(
                "SELECT id FROM jobs WHERE id = ? AND approved_at IS NOT NULL AND status = 'passed'",
                (job_id,),
            ).fetchone()
            if row is None:
                return False
            connection.execute("UPDATE jobs SET is_active_model = 0 WHERE is_active_model = 1")
            connection.execute("UPDATE jobs SET is_active_model = 1 WHERE id = ?", (job_id,))
            return True

    def update_oss_backup(
        self,
        job_id: str,
        status: str,
        object_key: str | None = None,
        error: str | None = None,
    ) -> None:
        with self.connect() as connection:
            connection.execute(
                """UPDATE jobs SET oss_backup_status = ?, oss_object_key = COALESCE(?, oss_object_key),
                   oss_backed_up_at = CASE WHEN ? = 'completed' THEN ? ELSE oss_backed_up_at END,
                   oss_backup_error = ? WHERE id = ?""",
                (status, object_key, status, utc_now(), error[:2000] if error else None, job_id),
            )

    def mark_artifacts_cleaned(self, job_id: str) -> None:
        with self.connect() as connection:
            connection.execute(
                "UPDATE jobs SET artifacts_cleaned_at = ? WHERE id = ?",
                (utc_now(), job_id),
            )
