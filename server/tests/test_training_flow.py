from __future__ import annotations

import hashlib
import uuid
from pathlib import Path

from fastapi.testclient import TestClient

from app.config import Settings
from app.database import Database
from app.main import create_app
from app.worker import run_one


TOKEN_HEADER = {"Authorization": "Bearer test-token"}
SAMPLES = b"""pc_timestamp_ms,board_timestamp_ms,node_id,ax,ay,az,gx,gy,gz,state_label,session_id\n1000,10,1,0.1,0.2,0.3,1,2,3,idle,test_session\n1001,11,2,0.1,0.2,0.3,1,2,3,idle,test_session\n1002,12,3,0.1,0.2,0.3,1,2,3,idle,test_session\n1003,13,4,0.1,0.2,0.3,1,2,3,idle,test_session\n"""
EVENTS = b"""event_id,event_group,event_type,pc_timestamp_ms,state_label,session_id\nevent-0001,jump_event,jump,1002,idle,test_session\n"""


def sha256(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def create_and_upload_dataset(client: TestClient) -> str:
    response = client.post(
        "/api/v1/datasets",
        headers=TOKEN_HEADER,
        json={
            "name": "API integration dataset",
            "samples": {"filename": "samples.csv", "bytes": len(SAMPLES), "sha256": sha256(SAMPLES)},
            "events": {"filename": "events.csv", "bytes": len(EVENTS), "sha256": sha256(EVENTS)},
        },
    )
    assert response.status_code == 201, response.text
    dataset_id = response.json()["id"]

    for kind, content in (("samples", SAMPLES), ("events", EVENTS)):
        offset = 0
        for start in range(0, len(content), 47):
            chunk = content[start : start + 47]
            upload = client.put(
                f"/api/v1/datasets/{dataset_id}/files/{kind}",
                headers={**TOKEN_HEADER, "X-Upload-Offset": str(offset)},
                content=chunk,
            )
            assert upload.status_code == 200, upload.text
            offset += len(chunk)

    complete = client.post(f"/api/v1/datasets/{dataset_id}/complete", headers=TOKEN_HEADER)
    assert complete.status_code == 200, complete.text
    assert complete.json()["status"] == "ready"
    return dataset_id


def test_resumable_upload_rejects_wrong_offset(tmp_path: Path) -> None:
    settings = Settings(storage_root=tmp_path, api_token="test-token", max_chunk_bytes=1024)
    with TestClient(create_app(settings)) as client:
        created = client.post(
            "/api/v1/datasets",
            headers=TOKEN_HEADER,
            json={
                "name": "offset test",
                "samples": {"filename": "samples.csv", "bytes": len(SAMPLES), "sha256": sha256(SAMPLES)},
                "events": {"filename": "events.csv", "bytes": len(EVENTS), "sha256": sha256(EVENTS)},
            },
        ).json()
        response = client.put(
            f"/api/v1/datasets/{created['id']}/files/samples",
            headers={**TOKEN_HEADER, "X-Upload-Offset": "5"},
            content=b"abc",
        )
        assert response.status_code == 409
        assert response.json()["detail"] == {"expected_offset": 0}


def test_upload_validate_and_download_manifest(tmp_path: Path) -> None:
    settings = Settings(storage_root=tmp_path, api_token="test-token", max_chunk_bytes=1024)
    application = create_app(settings)
    with TestClient(application) as client:
        dataset_id = create_and_upload_dataset(client)
        queued = client.post(
            "/api/v1/jobs",
            headers=TOKEN_HEADER,
            json={"dataset_id": dataset_id, "mode": "validate"},
        )
        assert queued.status_code == 201, queued.text
        job_id = queued.json()["id"]

    database = Database(settings.database_path)
    assert run_one(settings, database) is True
    finished = database.get_job(job_id)
    assert finished is not None
    assert finished["status"] == "validated"

    with TestClient(application) as client:
        artifacts = client.get(f"/api/v1/jobs/{job_id}/artifacts", headers=TOKEN_HEADER)
        assert artifacts.status_code == 200
        paths = {item["path"] for item in artifacts.json()}
        assert {"dataset_validation.json", "run_manifest.json"}.issubset(paths)
        downloaded = client.get(
            f"/api/v1/jobs/{job_id}/artifacts/run_manifest.json", headers=TOKEN_HEADER
        )
        assert downloaded.status_code == 200
        assert downloaded.json()["status"] == "validated"

        cannot_approve = client.post(
            f"/api/v1/jobs/{job_id}/approve",
            headers=TOKEN_HEADER,
            json={"approved_by": "test operator"},
        )
        assert cannot_approve.status_code == 409

    train_job_id = uuid.uuid4().hex
    database.create_job(train_job_id, dataset_id, "train")
    claimed = database.claim_next_job()
    assert claimed is not None and claimed["id"] == train_job_id
    database.finish_job(train_job_id, "passed", finished["run_dir"], None)
    with TestClient(application) as client:
        approved = client.post(
            f"/api/v1/jobs/{train_job_id}/approve",
            headers=TOKEN_HEADER,
            json={"approved_by": "test operator"},
        )
        assert approved.status_code == 200
        assert approved.json()["approved_by"] == "test operator"
    assert (Path(finished["run_dir"]) / "approval.json").is_file()
