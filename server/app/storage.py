"""数据文件路径与完整性工具。"""

from __future__ import annotations

import csv
import hashlib
from pathlib import Path


SAMPLE_COLUMNS = {
    "pc_timestamp_ms",
    "board_timestamp_ms",
    "node_id",
    "ax",
    "ay",
    "az",
    "gx",
    "gy",
    "gz",
    "state_label",
    "session_id",
}
EVENT_COLUMNS = {
    "event_id",
    "event_group",
    "event_type",
    "pc_timestamp_ms",
    "state_label",
    "session_id",
}


def dataset_dir(storage_root: Path, dataset_id: str) -> Path:
    return storage_root / "datasets" / dataset_id


def dataset_file(storage_root: Path, dataset_id: str, kind: str) -> Path:
    return dataset_dir(storage_root, dataset_id) / f"{kind}.csv"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def validate_csv_header(path: Path, kind: str) -> list[str]:
    required = SAMPLE_COLUMNS if kind == "samples" else EVENT_COLUMNS
    try:
        with path.open("r", encoding="utf-8-sig", newline="") as handle:
            header = next(csv.reader(handle))
    except (OSError, StopIteration, UnicodeDecodeError, csv.Error) as exc:
        return [f"{kind}.csv cannot be read: {exc}"]
    missing = sorted(required - set(header))
    return [f"{kind}.csv missing required columns: {missing}"] if missing else []
