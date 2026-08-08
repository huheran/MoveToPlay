"""服务运行配置。"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Settings:
    storage_root: Path
    api_token: str | None
    official_dataset_id: str | None = None
    max_file_bytes: int = 256 * 1024 * 1024
    max_chunk_bytes: int = 8 * 1024 * 1024
    worker_poll_seconds: float = 2.0

    @classmethod
    def from_env(cls) -> "Settings":
        token = os.getenv("MOVETOPLAY_API_TOKEN", "").strip() or None
        official_dataset_id = os.getenv("MOVETOPLAY_OFFICIAL_DATASET_ID", "").strip() or None
        return cls(
            storage_root=Path(os.getenv("MOVETOPLAY_STORAGE_ROOT", "/srv/movetoplay")),
            api_token=token,
            official_dataset_id=official_dataset_id,
            max_file_bytes=int(os.getenv("MOVETOPLAY_MAX_FILE_BYTES", str(256 * 1024 * 1024))),
            max_chunk_bytes=int(os.getenv("MOVETOPLAY_MAX_CHUNK_BYTES", str(8 * 1024 * 1024))),
            worker_poll_seconds=float(os.getenv("MOVETOPLAY_WORKER_POLL_SECONDS", "2")),
        )

    @property
    def database_path(self) -> Path:
        return self.storage_root / "state" / "movetoplay.sqlite3"

    def ensure_directories(self) -> None:
        self.storage_root.mkdir(parents=True, exist_ok=True)
        self.storage_root.chmod(0o700)
        for name in ("state", "datasets", "jobs", "artifacts"):
            path = self.storage_root / name
            path.mkdir(parents=True, exist_ok=True, mode=0o700)
            path.chmod(0o700)
