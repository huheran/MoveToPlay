from pathlib import Path

from fastapi.testclient import TestClient

from app.config import Settings
from app.main import create_app


def make_client(tmp_path: Path) -> TestClient:
    return TestClient(create_app(Settings(storage_root=tmp_path, api_token="test-token")))


def test_public_health_endpoints(tmp_path: Path) -> None:
    with make_client(tmp_path) as client:
        assert client.get("/").json() == {"status": "MoveToPlay server running"}
        assert client.get("/health").json() == {"status": "ok"}


def test_protected_endpoint_requires_bearer_token(tmp_path: Path) -> None:
    with make_client(tmp_path) as client:
        assert client.get("/api/v1/datasets").status_code == 401
        assert client.get(
            "/api/v1/datasets", headers={"Authorization": "Bearer wrong"}
        ).status_code == 401
        assert client.get(
            "/api/v1/datasets", headers={"Authorization": "Bearer test-token"}
        ).status_code == 200


def test_system_config_requires_ready_official_dataset(tmp_path: Path) -> None:
    headers = {"Authorization": "Bearer test-token"}
    settings = Settings(
        storage_root=tmp_path,
        api_token="test-token",
        official_dataset_id="a" * 32,
    )
    with TestClient(create_app(settings)) as client:
        response = client.get("/api/v1/system-config", headers=headers)
        assert response.status_code == 503


def test_system_config_allows_unconfigured_official_dataset(tmp_path: Path) -> None:
    with make_client(tmp_path) as client:
        response = client.get(
            "/api/v1/system-config",
            headers={"Authorization": "Bearer test-token"},
        )
        assert response.status_code == 200
        assert response.json() == {"official_dataset_id": None}
