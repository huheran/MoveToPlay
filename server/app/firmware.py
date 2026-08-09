"""把训练生成的模型 C 数组编译为可直接烧录的 Dongle 固件包。"""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable
from zipfile import ZIP_DEFLATED, ZipFile

from .storage import sha256_file


REQUIRED_MODEL_FILES = (
    "rf_model_generated.c",
    "rf_model_generated.h",
    "rf_state_model_generated.c",
    "rf_state_model_generated.h",
)
PROJECT_FILES = (
    "CMakeLists.txt",
    "dependencies.lock",
    "partitions.csv",
    "partitions_16mb.csv",
    "sdkconfig.defaults",
    "sdkconfig.defaults.16mb",
)
BOARD_PROFILE_PATTERN = re.compile(
    r"^#define\s+M2P_BOARD_PROFILE\s+\d+\s*$", re.MULTILINE
)
NINJA_PROGRESS_PATTERN = re.compile(r"\[(\d+)\s*/\s*(\d+)\]")
ProgressCallback = Callable[[str, str, float], None]


def _copy_firmware_project(project_root: Path, workspace: Path, generated: Path) -> None:
    for relative in PROJECT_FILES:
        source = project_root / relative
        if not source.is_file():
            raise FileNotFoundError(f"firmware project is missing {relative}")
        shutil.copy2(source, workspace / relative)
    for directory in ("main",):
        source = project_root / directory
        if not source.is_dir():
            raise FileNotFoundError(f"firmware project is missing {directory}/")
        shutil.copytree(source, workspace / directory)
    # 本地开发环境可能已经缓存了托管组件；正式发布包只需要 dependencies.lock，
    # ESP-IDF 会按锁文件从官方组件仓库恢复完全相同的版本。
    managed = project_root / "managed_components"
    if managed.is_dir():
        shutil.copytree(managed, workspace / "managed_components")

    destination = workspace / "main" / "generated"
    destination.mkdir(parents=True, exist_ok=True)
    for name in REQUIRED_MODEL_FILES:
        source = generated / name
        if not source.is_file():
            raise FileNotFoundError(f"training artifacts are missing generated/{name}")
        shutil.copy2(source, destination / name)

    app_main = workspace / "main" / "app_main.c"
    contents = app_main.read_text(encoding="utf-8")
    replaced, count = BOARD_PROFILE_PATTERN.subn(
        "#define M2P_BOARD_PROFILE             1", contents, count=1
    )
    if count != 1:
        raise ValueError("cannot set M2P_BOARD_PROFILE to Dongle (1)")
    app_main.write_text(replaced, encoding="utf-8", newline="\n")


def _package_build(job_id: str, build_dir: Path, output_dir: Path) -> Path:
    flasher_path = build_dir / "flasher_args.json"
    flasher = json.loads(flasher_path.read_text(encoding="utf-8"))
    extra = flasher.get("extra_esptool_args", {})
    chip = str(extra.get("chip", ""))
    if chip != "esp32s3":
        raise ValueError(f"unexpected firmware chip: {chip or '<missing>'}")
    flash_files = flasher.get("flash_files")
    if not isinstance(flash_files, dict) or not flash_files:
        raise ValueError("flasher_args.json contains no flash_files")

    output_dir.mkdir(parents=True, exist_ok=True)
    files: list[dict[str, object]] = []
    used_names: set[str] = set()
    for offset, relative in sorted(flash_files.items(), key=lambda item: int(item[0], 0)):
        source = (build_dir / str(relative)).resolve()
        if not source.is_relative_to(build_dir.resolve()) or not source.is_file():
            raise FileNotFoundError(f"compiled firmware file is missing: {relative}")
        name = source.name
        if name in used_names:
            raise ValueError(f"duplicate firmware filename: {name}")
        used_names.add(name)
        target = output_dir / name
        shutil.copy2(source, target)
        files.append(
            {
                "name": name,
                "offset": str(offset),
                "bytes": target.stat().st_size,
                "sha256": sha256_file(target),
            }
        )

    manifest = {
        "schema_version": 2,
        "job_id": job_id,
        "created_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "board_profile": 1,
        "chip": chip,
        "before": str(extra.get("before", "default_reset")).replace("_", "-"),
        "after": str(extra.get("after", "hard_reset")).replace("_", "-"),
        "write_flash_args": [str(value).replace("_", "-") for value in flasher.get("write_flash_args", [])],
        "files": files,
    }
    manifest_path = output_dir / "firmware-manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    bundle = output_dir / "firmware-bundle.zip"
    with ZipFile(bundle, "w", compression=ZIP_DEFLATED, compresslevel=9) as archive:
        archive.write(manifest_path, manifest_path.name)
        for item in files:
            archive.write(output_dir / str(item["name"]), str(item["name"]))
    return bundle


def build_dongle_firmware(
    project_root: Path,
    run_dir: Path,
    job_id: str,
    progress: ProgressCallback | None = None,
) -> Path:
    """在临时工作区编译固件，只把可烧录文件保留为任务产物。"""
    generated = run_dir / "generated"
    firmware_dir = run_dir / "firmware"
    if firmware_dir.exists():
        shutil.rmtree(firmware_dir)
    firmware_dir.mkdir(parents=True)
    log_path = firmware_dir / "firmware-build.log"

    workspace_parent = run_dir.parents[2] / "jobs" / job_id
    workspace_parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="firmware-", dir=workspace_parent) as temporary:
        workspace = Path(temporary) / "project"
        workspace.mkdir()
        if progress:
            progress("firmware_preparing", "正在集成模型 C 数组并准备 Dongle 源码", 5)
        _copy_firmware_project(project_root, workspace, generated)
        build_dir = workspace / "build-dongle"
        command = [
            "idf.py",
            "-B",
            str(build_dir),
            "-D",
            f"SDKCONFIG={build_dir / 'sdkconfig'}",
            "-D",
            "SDKCONFIG_DEFAULTS=sdkconfig.defaults.16mb",
            "build",
        ]
        if progress:
            progress("firmware_building", "云端正在编译完整 Dongle 固件", 10)
        with log_path.open("w", encoding="utf-8", newline="\n") as log:
            process = subprocess.Popen(
                command,
                cwd=workspace,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
            )
            assert process.stdout is not None
            last_percent = 10.0
            for line in process.stdout:
                log.write(line)
                match = NINJA_PROGRESS_PATTERN.search(line)
                if not match or not progress:
                    continue
                completed = int(match.group(1))
                total = int(match.group(2))
                if total <= 0:
                    continue
                percent = 10.0 + min(1.0, completed / total) * 84.0
                if percent >= last_percent + 1.0:
                    last_percent = percent
                    progress(
                        "firmware_building",
                        f"云端正在编译完整 Dongle 固件（{completed}/{total}）",
                        percent,
                    )
            process.wait()
        if process.returncode != 0:
            raise RuntimeError(f"cloud firmware build failed with code {process.returncode}")
        if progress:
            progress("firmware_packaging", "正在校验并打包可烧录固件", 96)
        return _package_build(job_id, build_dir, firmware_dir)
