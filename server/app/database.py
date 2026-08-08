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
                    approved_by TEXT
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
                    events_filename, events_expected_bytes, events_expected_sha256, event_id_scope
                ) VALUES (?, ?, 'uploading', ?, ?, ?, ?, ?, ?, ?, ?)
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
                "UPDATE jobs SET status = 'running', started_at = ? WHERE id = ? AND status = 'queued'",
                (utc_now(), row["id"]),
            )
            claimed = connection.execute("SELECT * FROM jobs WHERE id = ?", (row["id"],)).fetchone()
            return self._row(claimed)

    def recover_running_jobs(self) -> int:
        """单 worker 重启后，把中断的任务放回队列。"""
        with self.connect() as connection:
            cursor = connection.execute(
                "UPDATE jobs SET status = 'queued', started_at = NULL WHERE status = 'running'"
            )
            return cursor.rowcount

    def finish_job(self, job_id: str, status: str, run_dir: str | None, error: str | None) -> None:
        with self.connect() as connection:
            connection.execute(
                "UPDATE jobs SET status = ?, finished_at = ?, run_dir = ?, error = ? WHERE id = ?",
                (status, utc_now(), run_dir, error[:4000] if error else None, job_id),
            )

    def approve_job(self, job_id: str, approved_by: str) -> bool:
        with self.connect() as connection:
            cursor = connection.execute(
                """
                UPDATE jobs SET approved_at = ?, approved_by = ?
                WHERE id = ? AND status = 'passed' AND approved_at IS NULL
                """,
                (utc_now(), approved_by[:100], job_id),
            )
            return cursor.rowcount == 1
