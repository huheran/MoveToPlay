"""MoveToPlay 云端 API 入口。"""

from __future__ import annotations

import asyncio
import json
import os
import secrets
import shutil
import uuid
from collections import defaultdict
from contextlib import asynccontextmanager
from pathlib import Path
from typing import AsyncIterator
from datetime import datetime, timezone

from fastapi import BackgroundTasks, Depends, FastAPI, Header, HTTPException, Request, status
from fastapi.responses import FileResponse

from .config import Settings
from .database import Database
from .oss_backup import backup_approved_model, remove_backup_staging
from .schemas import ApprovalCreate, DatasetCreate, FirmwareBuildCreate, JobCreate
from .storage import dataset_dir, dataset_file, sha256_file, validate_csv_header


def dataset_response(row: dict) -> dict:
    return {
        "id": row["id"],
        "name": row["name"],
        "status": row["status"],
        "created_at": row["created_at"],
        "completed_at": row["completed_at"],
        "event_id_scope": row["event_id_scope"],
        "base_dataset_id": row.get("base_dataset_id"),
        "files": {
            kind: {
                "filename": row[f"{kind}_filename"],
                "expected_bytes": row[f"{kind}_expected_bytes"],
                "received_bytes": row[f"{kind}_received_bytes"],
                "sha256": row[f"{kind}_expected_sha256"],
            }
            for kind in ("samples", "events")
        },
        "error": row["error"],
    }


def job_response(row: dict) -> dict:
    response = {
        key: row[key]
        for key in (
            "id",
            "dataset_id",
            "mode",
            "status",
            "created_at",
            "started_at",
            "finished_at",
            "error",
            "approved_at",
            "approved_by",
            "progress_stage",
            "progress_detail",
            "progress_percent",
            "estimated_remaining_seconds",
            "model_version",
            "is_active_model",
            "oss_backup_status",
            "oss_object_key",
            "oss_backed_up_at",
            "oss_backup_error",
            "artifacts_cleaned_at",
            "firmware_status",
            "firmware_detail",
            "firmware_progress_percent",
            "firmware_built_at",
            "firmware_error",
        )
    }
    elapsed = 0
    if row.get("started_at"):
        try:
            started = datetime.fromisoformat(row["started_at"])
            end = datetime.fromisoformat(row["finished_at"]) if row.get("finished_at") else datetime.now(timezone.utc)
            elapsed = max(0, int((end - started).total_seconds()))
        except (TypeError, ValueError):
            elapsed = 0
    response["elapsed_seconds"] = elapsed
    response["is_active_model"] = bool(row.get("is_active_model"))
    if row.get("status") == "running" and row.get("estimated_remaining_seconds") is not None:
        since_update = 0
        try:
            updated = datetime.fromisoformat(row["progress_updated_at"])
            since_update = max(0, int((datetime.now(timezone.utc) - updated).total_seconds()))
        except (TypeError, ValueError):
            pass
        response["estimated_remaining_seconds"] = max(
            0, int(row["estimated_remaining_seconds"]) - since_update
        )
    return response


async def require_api_token(request: Request) -> None:
    expected = request.app.state.settings.api_token
    if not expected:
        raise HTTPException(status_code=503, detail="API token is not configured")
    authorization = request.headers.get("Authorization", "")
    scheme, _, supplied = authorization.partition(" ")
    if scheme.lower() != "bearer" or not supplied or not secrets.compare_digest(supplied, expected):
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="invalid bearer token",
            headers={"WWW-Authenticate": "Bearer"},
        )


def create_app(settings: Settings | None = None) -> FastAPI:
    active_settings = settings or Settings.from_env()

    @asynccontextmanager
    async def lifespan(application: FastAPI) -> AsyncIterator[None]:
        os.umask(0o077)
        active_settings.ensure_directories()
        database = Database(active_settings.database_path)
        database.initialize()
        remove_backup_staging(active_settings)
        application.state.database = database
        application.state.settings = active_settings
        application.state.upload_locks = defaultdict(asyncio.Lock)
        application.state.backup_tasks = set()
        for model in database.list_models():
            if model.get("run_dir") and model.get("oss_backup_status") != "completed":
                task = asyncio.create_task(asyncio.to_thread(
                    backup_approved_model,
                    active_settings,
                    active_settings.database_path,
                    str(model["id"]),
                    str(model["run_dir"]),
                ))
                application.state.backup_tasks.add(task)
                task.add_done_callback(application.state.backup_tasks.discard)
        yield

    application = FastAPI(
        title="MoveToPlay Server",
        version="0.2.0",
        docs_url="/docs",
        redoc_url=None,
        lifespan=lifespan,
    )

    @application.get("/")
    def root() -> dict[str, str]:
        return {"status": "MoveToPlay server running"}

    @application.get("/health")
    def health() -> dict[str, str]:
        return {"status": "ok"}

    protected = [Depends(require_api_token)]

    @application.get("/api/v1/system-config", dependencies=protected)
    def system_config(request: Request) -> dict[str, object]:
        official_dataset_id = request.app.state.settings.official_dataset_id
        if official_dataset_id is not None:
            official = request.app.state.database.get_dataset(official_dataset_id)
            if official is None or official["status"] != "ready":
                raise HTTPException(
                    status_code=503,
                    detail="official dataset is missing or not ready",
                )
        return {
            "official_dataset_id": official_dataset_id,
            "oss_backup_configured": active_settings.oss_configured,
            "cleanup_policy": {
                "approved_models": "永久保留本地副本并备份 OSS",
                "failed_days": active_settings.cleanup_failed_days,
                "validated_days": active_settings.cleanup_validated_days,
                "unapproved_passed_days": active_settings.cleanup_unapproved_days,
            },
        }

    @application.post("/api/v1/datasets", status_code=201, dependencies=protected)
    def create_dataset(payload: DatasetCreate, request: Request) -> dict:
        maximum = request.app.state.settings.max_file_bytes
        for kind, spec in (("samples", payload.samples), ("events", payload.events)):
            if spec.bytes > maximum:
                raise HTTPException(status_code=413, detail=f"{kind} exceeds maximum file size")
        if payload.base_dataset_id is not None:
            base = request.app.state.database.get_dataset(payload.base_dataset_id)
            if base is None:
                raise HTTPException(status_code=404, detail="base dataset not found")
            if base["status"] != "ready":
                raise HTTPException(status_code=409, detail="base dataset is not ready")
        dataset_id = uuid.uuid4().hex
        directory = dataset_dir(request.app.state.settings.storage_root, dataset_id)
        directory.mkdir(parents=True, exist_ok=False)
        row = request.app.state.database.create_dataset(
            {
                "id": dataset_id,
                "name": payload.name,
                "samples_filename": payload.samples.filename,
                "samples_expected_bytes": payload.samples.bytes,
                "samples_expected_sha256": payload.samples.sha256.upper(),
                "events_filename": payload.events.filename,
                "events_expected_bytes": payload.events.bytes,
                "events_expected_sha256": payload.events.sha256.upper(),
                "event_id_scope": payload.event_id_scope,
                "base_dataset_id": payload.base_dataset_id,
            }
        )
        return dataset_response(row)

    @application.get("/api/v1/datasets", dependencies=protected)
    def list_datasets(request: Request) -> list[dict]:
        return [dataset_response(row) for row in request.app.state.database.list_datasets()]

    @application.get("/api/v1/datasets/{dataset_id}", dependencies=protected)
    def get_dataset(dataset_id: str, request: Request) -> dict:
        row = request.app.state.database.get_dataset(dataset_id)
        if row is None:
            raise HTTPException(status_code=404, detail="dataset not found")
        return dataset_response(row)

    @application.put("/api/v1/datasets/{dataset_id}/files/{kind}", dependencies=protected)
    async def upload_chunk(
        dataset_id: str,
        kind: str,
        request: Request,
        x_upload_offset: int = Header(alias="X-Upload-Offset", ge=0),
    ) -> dict:
        if kind not in {"samples", "events"}:
            raise HTTPException(status_code=404, detail="unknown dataset file kind")
        database: Database = request.app.state.database
        row = database.get_dataset(dataset_id)
        if row is None:
            raise HTTPException(status_code=404, detail="dataset not found")
        if row["status"] != "uploading":
            raise HTTPException(status_code=409, detail="dataset no longer accepts uploads")
        current = int(row[f"{kind}_received_bytes"])
        if x_upload_offset != current:
            raise HTTPException(status_code=409, detail={"expected_offset": current})

        directory = dataset_dir(request.app.state.settings.storage_root, dataset_id)
        temporary = directory / f".{kind}.{uuid.uuid4().hex}.chunk"
        total = 0
        try:
            with temporary.open("wb") as handle:
                async for chunk in request.stream():
                    total += len(chunk)
                    if total > request.app.state.settings.max_chunk_bytes:
                        raise HTTPException(status_code=413, detail="upload chunk is too large")
                    handle.write(chunk)
            if total == 0:
                raise HTTPException(status_code=400, detail="empty upload chunk")

            lock = request.app.state.upload_locks[dataset_id]
            async with lock:
                row = database.get_dataset(dataset_id)
                assert row is not None
                current = int(row[f"{kind}_received_bytes"])
                expected = int(row[f"{kind}_expected_bytes"])
                if current != x_upload_offset:
                    raise HTTPException(status_code=409, detail={"expected_offset": current})
                if current + total > expected:
                    raise HTTPException(status_code=413, detail="chunk exceeds declared file size")
                target = dataset_file(request.app.state.settings.storage_root, dataset_id, kind)
                actual_size = target.stat().st_size if target.exists() else 0
                if actual_size != current:
                    raise HTTPException(status_code=500, detail="stored upload offset is inconsistent")
                with target.open("ab") as destination, temporary.open("rb") as source:
                    shutil.copyfileobj(source, destination)
                    destination.flush()
                new_size = current + total
                if not database.update_received(dataset_id, kind, current, new_size):
                    raise HTTPException(status_code=409, detail="upload state changed concurrently")
        finally:
            temporary.unlink(missing_ok=True)
        updated = database.get_dataset(dataset_id)
        assert updated is not None
        return dataset_response(updated)

    @application.post("/api/v1/datasets/{dataset_id}/complete", dependencies=protected)
    async def complete_dataset(dataset_id: str, request: Request) -> dict:
        database: Database = request.app.state.database
        lock = request.app.state.upload_locks[dataset_id]
        async with lock:
            row = database.get_dataset(dataset_id)
            if row is None:
                raise HTTPException(status_code=404, detail="dataset not found")
            if row["status"] == "ready":
                return dataset_response(row)
            if row["status"] != "uploading":
                raise HTTPException(status_code=409, detail="dataset cannot be completed")
            incomplete = [
                kind
                for kind in ("samples", "events")
                if row[f"{kind}_received_bytes"] != row[f"{kind}_expected_bytes"]
            ]
            if incomplete:
                raise HTTPException(status_code=409, detail={"incomplete_files": incomplete})

            errors: list[str] = []
            for kind in ("samples", "events"):
                path = dataset_file(request.app.state.settings.storage_root, dataset_id, kind)
                actual_hash = await asyncio.to_thread(sha256_file, path)
                if actual_hash != row[f"{kind}_expected_sha256"]:
                    errors.append(f"{kind}.csv SHA-256 mismatch")
                errors.extend(await asyncio.to_thread(validate_csv_header, path, kind))
            if errors:
                message = "; ".join(errors)
                database.reject_dataset(dataset_id, message)
                raise HTTPException(status_code=422, detail=errors)
            database.mark_dataset_ready(dataset_id)
        ready = database.get_dataset(dataset_id)
        assert ready is not None
        return dataset_response(ready)

    @application.post("/api/v1/jobs", status_code=201, dependencies=protected)
    def create_job(payload: JobCreate, request: Request) -> dict:
        dataset = request.app.state.database.get_dataset(payload.dataset_id)
        if dataset is None:
            raise HTTPException(status_code=404, detail="dataset not found")
        if dataset["status"] != "ready":
            raise HTTPException(status_code=409, detail="dataset is not ready")
        row = request.app.state.database.create_job(uuid.uuid4().hex, payload.dataset_id, payload.mode)
        return job_response(row)

    @application.get("/api/v1/jobs", dependencies=protected)
    def list_jobs(request: Request) -> list[dict]:
        return [job_response(row) for row in request.app.state.database.list_jobs()]

    @application.get("/api/v1/jobs/{job_id}", dependencies=protected)
    def get_job(job_id: str, request: Request) -> dict:
        row = request.app.state.database.get_job(job_id)
        if row is None:
            raise HTTPException(status_code=404, detail="job not found")
        return job_response(row)

    @application.post("/api/v1/jobs/{job_id}/approve", dependencies=protected)
    def approve_job(
        job_id: str,
        payload: ApprovalCreate,
        request: Request,
        background_tasks: BackgroundTasks,
    ) -> dict:
        row = request.app.state.database.get_job(job_id)
        if row is None:
            raise HTTPException(status_code=404, detail="job not found")
        if row["mode"] != "train" or row["status"] != "passed":
            raise HTTPException(status_code=409, detail="only a passed training job can be approved")
        if row["approved_at"] is None:
            request.app.state.database.approve_job(job_id, payload.approved_by)
        approved = request.app.state.database.get_job(job_id)
        assert approved is not None
        if approved["run_dir"]:
            run_dir = Path(approved["run_dir"])
            if run_dir.is_dir():
                run_manifest = run_dir / "run_manifest.json"
                approval = {
                    "schema_version": 1,
                    "job_id": approved["id"],
                    "dataset_id": approved["dataset_id"],
                    "approved_at": approved["approved_at"],
                    "approved_by": approved["approved_by"],
                    "run_manifest_sha256": sha256_file(run_manifest) if run_manifest.is_file() else None,
                }
                temporary = run_dir / f".approval.{uuid.uuid4().hex}.tmp"
                temporary.write_text(
                    json.dumps(approval, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
                )
                temporary.replace(run_dir / "approval.json")
                background_tasks.add_task(
                    backup_approved_model,
                    request.app.state.settings,
                    request.app.state.settings.database_path,
                    job_id,
                    str(run_dir),
                )
        return job_response(approved)

    @application.post("/api/v1/jobs/{job_id}/firmware", dependencies=protected)
    def build_firmware(job_id: str, payload: FirmwareBuildCreate, request: Request) -> dict:
        row = request.app.state.database.get_job(job_id)
        if row is None:
            raise HTTPException(status_code=404, detail="job not found")
        if not request.app.state.database.queue_firmware_build(job_id, force=payload.force):
            raise HTTPException(
                status_code=409, detail="only a passed training job can build firmware"
            )
        queued = request.app.state.database.get_job(job_id)
        assert queued is not None
        return job_response(queued)

    @application.get("/api/v1/models", dependencies=protected)
    def list_models(request: Request) -> list[dict]:
        return [job_response(row) for row in request.app.state.database.list_models()]

    @application.post("/api/v1/models/{job_id}/activate", dependencies=protected)
    def activate_model(job_id: str, request: Request) -> dict:
        if not request.app.state.database.activate_model(job_id):
            raise HTTPException(status_code=409, detail="only an approved passed model can be activated")
        row = request.app.state.database.get_job(job_id)
        assert row is not None
        return job_response(row)

    @application.get("/api/v1/jobs/{job_id}/artifacts", dependencies=protected)
    def list_artifacts(job_id: str, request: Request) -> list[dict]:
        row = request.app.state.database.get_job(job_id)
        if row is None:
            raise HTTPException(status_code=404, detail="job not found")
        if not row["run_dir"]:
            return []
        base = Path(row["run_dir"])
        if not base.is_dir():
            return []
        manifest_hashes: dict[str, str] = {}
        manifest_path = base / "run_manifest.json"
        if manifest_path.is_file():
            try:
                manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
                manifest_hashes = {item["path"]: item["sha256"] for item in manifest.get("artifacts", [])}
            except (OSError, json.JSONDecodeError, KeyError, TypeError):
                pass
        return [
            {
                "path": path.relative_to(base).as_posix(),
                "bytes": path.stat().st_size,
                "sha256": manifest_hashes.get(path.relative_to(base).as_posix()) or sha256_file(path),
            }
            for path in sorted(base.rglob("*"))
            if path.is_file()
        ]

    @application.get("/api/v1/jobs/{job_id}/artifacts/{artifact_path:path}", dependencies=protected)
    def download_artifact(job_id: str, artifact_path: str, request: Request) -> FileResponse:
        row = request.app.state.database.get_job(job_id)
        if row is None or not row["run_dir"]:
            raise HTTPException(status_code=404, detail="job artifacts not found")
        base = Path(row["run_dir"]).resolve()
        target = (base / artifact_path).resolve()
        if not target.is_relative_to(base) or not target.is_file():
            raise HTTPException(status_code=404, detail="artifact not found")
        return FileResponse(target, filename=target.name)

    return application


app = create_app()
