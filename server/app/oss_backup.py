"""已批准模型的阿里云 OSS 归档。未配置 OSS 时安全跳过。"""

from __future__ import annotations

import tarfile
import uuid
from pathlib import Path

from .config import Settings
from .database import Database


def backup_approved_model(settings: Settings, database_path: Path, job_id: str, run_dir: str) -> None:
    database = Database(database_path)
    if not settings.oss_configured:
        database.update_oss_backup(job_id, "not_configured", error="OSS Bucket 尚未配置")
        return

    source = Path(run_dir)
    if not source.is_dir():
        database.update_oss_backup(job_id, "failed", error="模型产物目录不存在")
        return

    database.update_oss_backup(job_id, "uploading")
    temporary = settings.storage_root / "backups" / f".{job_id}.{uuid.uuid4().hex}.tar.gz"
    key = f"{settings.oss_prefix}/{job_id}/model-bundle.tar.gz"
    try:
        import alibabacloud_oss_v2 as oss

        with tarfile.open(temporary, "w:gz") as archive:
            archive.add(source, arcname=job_id, recursive=True)

        configuration = oss.config.load_default()
        configuration.credentials_provider = oss.credentials.EnvironmentVariableCredentialsProvider()
        configuration.region = settings.oss_region
        if settings.oss_endpoint:
            configuration.endpoint = settings.oss_endpoint
        client = oss.Client(configuration)
        client.put_object_from_file(
            oss.PutObjectRequest(bucket=settings.oss_bucket, key=key),
            str(temporary),
        )
        database.update_oss_backup(job_id, "completed", object_key=key)
    except Exception as exc:  # OSS failure must never revoke an approved local model.
        database.update_oss_backup(job_id, "failed", object_key=key, error=str(exc))
    finally:
        temporary.unlink(missing_ok=True)


def remove_backup_staging(settings: Settings) -> None:
    """清除异常退出遗留的临时压缩包，不触碰已经上传的 OSS 对象。"""
    root = settings.storage_root / "backups"
    if not root.is_dir():
        return
    for path in root.glob(".*.tar.gz"):
        if path.is_file():
            path.unlink(missing_ok=True)
