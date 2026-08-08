"""MoveToPlay API 请求模型。"""

from __future__ import annotations

from typing import Literal

from pydantic import BaseModel, Field, field_validator


class UploadFileSpec(BaseModel):
    filename: str = Field(min_length=1, max_length=200)
    bytes: int = Field(gt=0)
    sha256: str = Field(pattern=r"^[0-9a-fA-F]{64}$")

    @field_validator("filename")
    @classmethod
    def filename_must_be_plain(cls, value: str) -> str:
        if "/" in value or "\\" in value or value in {".", ".."}:
            raise ValueError("filename must not contain a path")
        return value


class DatasetCreate(BaseModel):
    name: str = Field(min_length=1, max_length=100)
    samples: UploadFileSpec
    events: UploadFileSpec
    event_id_scope: Literal["global", "session"] = "global"
    base_dataset_id: str | None = Field(default=None, pattern=r"^[0-9a-f]{32}$")


class JobCreate(BaseModel):
    dataset_id: str = Field(pattern=r"^[0-9a-f]{32}$")
    mode: Literal["validate", "train"] = "train"


class ApprovalCreate(BaseModel):
    approved_by: str = Field(min_length=1, max_length=100)
