from __future__ import annotations

import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace

from app.config import Settings
from app.database import Database
from app.maintenance import cleanup_old_jobs
from app.oss_backup import backup_approved_model
from app.worker import _progress_from_line


def make_passed_job(tmp_path: Path, job_id: str = "a" * 32) -> tuple[Settings, Database, Path]:
    settings = Settings(storage_root=tmp_path, api_token="test")
    settings.ensure_directories()
    database = Database(settings.database_path)
    database.initialize()
    dataset = database.create_dataset(
        {
            "id": "d" * 32,
            "name": "model library",
            "samples_filename": "samples.csv",
            "samples_expected_bytes": 1,
            "samples_expected_sha256": "0" * 64,
            "events_filename": "events.csv",
            "events_expected_bytes": 1,
            "events_expected_sha256": "0" * 64,
            "event_id_scope": "global",
        }
    )
    database.create_job(job_id, dataset["id"], "train")
    database.claim_next_job()
    run_dir = settings.storage_root / "artifacts" / "training-runs" / job_id
    run_dir.mkdir(parents=True)
    (run_dir / "run_manifest.json").write_text("{}", encoding="utf-8")
    database.finish_job(job_id, "passed", str(run_dir), None)
    return settings, database, run_dir


def test_approved_model_is_versioned_and_uploaded(monkeypatch, tmp_path: Path) -> None:
    settings, database, run_dir = make_passed_job(tmp_path)
    database.approve_job("a" * 32, "tester")
    uploaded: dict[str, object] = {}

    class FakeClient:
        def __init__(self, configuration):
            uploaded["configuration"] = configuration

        def put_object_from_file(self, request, path):
            uploaded["request"] = request
            uploaded["path"] = path
            assert Path(path).is_file()

    fake_oss = SimpleNamespace(
        config=SimpleNamespace(load_default=lambda: SimpleNamespace()),
        credentials=SimpleNamespace(EnvironmentVariableCredentialsProvider=lambda: object()),
        Client=FakeClient,
        PutObjectRequest=lambda **values: values,
    )
    monkeypatch.setitem(sys.modules, "alibabacloud_oss_v2", fake_oss)
    configured = Settings(
        storage_root=tmp_path,
        api_token="test",
        oss_region="cn-hangzhou",
        oss_bucket="private-test-bucket",
    )
    backup_approved_model(configured, database.path, "a" * 32, str(run_dir))
    row = database.get_job("a" * 32)
    assert row is not None and row["oss_backup_status"] == "completed"
    assert row["oss_object_key"].endswith("/model-bundle.tar.gz")
    assert uploaded["request"]["bucket"] == "private-test-bucket"


def test_cleanup_never_deletes_approved_model(tmp_path: Path) -> None:
    settings, database, run_dir = make_passed_job(tmp_path)
    database.approve_job("a" * 32, "tester")
    with sqlite3.connect(database.path) as connection:
        connection.execute(
            "UPDATE jobs SET finished_at = '2020-01-01T00:00:00+00:00' WHERE id = ?",
            ("a" * 32,),
        )
    assert cleanup_old_jobs(settings, database) == []
    assert run_dir.is_dir()


def test_worker_progress_tracks_both_models() -> None:
    assert _progress_from_line("[pipeline] training model=state_rf_15_full", None) == (
        "training_state", "state", 22
    )
    assert _progress_from_line("[info] windows total=100", "event") == (
        "training_event", "event", 70
    )
    assert _progress_from_line("[pipeline] model=event_rf quality=PASS", "event") == (
        "quality_gate", "event", 90
    )
