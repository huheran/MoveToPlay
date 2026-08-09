from __future__ import annotations

import json
from pathlib import Path
from zipfile import ZipFile

from app.database import Database
from app.firmware import _package_build


def test_firmware_package_contains_verified_flash_set(tmp_path: Path) -> None:
    build = tmp_path / "build"
    (build / "bootloader").mkdir(parents=True)
    (build / "partition_table").mkdir()
    files = {
        "bootloader/bootloader.bin": b"boot",
        "partition_table/partition-table.bin": b"parts",
        "esp_idf_template.bin": b"application",
    }
    for name, contents in files.items():
        (build / name).write_bytes(contents)
    (build / "flasher_args.json").write_text(
        json.dumps(
            {
                "write_flash_args": ["--flash_mode", "dio", "--flash_size", "16MB"],
                "flash_files": {
                    "0x0": "bootloader/bootloader.bin",
                    "0x8000": "partition_table/partition-table.bin",
                    "0x10000": "esp_idf_template.bin",
                },
                "extra_esptool_args": {
                    "chip": "esp32s3",
                    "before": "default_reset",
                    "after": "hard_reset",
                },
            }
        ),
        encoding="utf-8",
    )
    output = tmp_path / "artifacts" / "firmware"
    bundle = _package_build("a" * 32, build, output)
    manifest = json.loads((output / "firmware-manifest.json").read_text(encoding="utf-8"))
    assert manifest["schema_version"] == 2
    assert manifest["chip"] == "esp32s3"
    assert [item["offset"] for item in manifest["files"]] == ["0x0", "0x8000", "0x10000"]
    assert all(len(item["sha256"]) == 64 for item in manifest["files"])
    with ZipFile(bundle) as archive:
        assert set(archive.namelist()) == {
            "firmware-manifest.json",
            "bootloader.bin",
            "partition-table.bin",
            "esp_idf_template.bin",
        }


def test_firmware_queue_can_be_rebuilt(tmp_path: Path) -> None:
    database = Database(tmp_path / "state.sqlite3")
    database.initialize()
    database.create_dataset(
        {
            "id": "d" * 32,
            "name": "firmware",
            "samples_filename": "samples.csv",
            "samples_expected_bytes": 1,
            "samples_expected_sha256": "0" * 64,
            "events_filename": "events.csv",
            "events_expected_bytes": 1,
            "events_expected_sha256": "0" * 64,
            "event_id_scope": "global",
        }
    )
    database.create_job("a" * 32, "d" * 32, "train")
    database.claim_next_job()
    database.finish_job("a" * 32, "passed", str(tmp_path / "run"), None)
    assert database.queue_firmware_build("a" * 32)
    claimed = database.claim_next_firmware_job()
    assert claimed is not None and claimed["firmware_status"] == "building"
    database.finish_firmware_build("a" * 32)
    assert database.get_job("a" * 32)["firmware_status"] == "ready"
    assert database.queue_firmware_build("a" * 32, force=True)
    assert database.get_job("a" * 32)["firmware_status"] == "queued"
