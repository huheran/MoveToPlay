"""MoveToPlay 云端 API 入口。"""

from fastapi import FastAPI


app = FastAPI(
    title="MoveToPlay Server",
    version="0.1.0",
    docs_url="/docs",
    redoc_url=None,
)


@app.get("/")
def root() -> dict[str, str]:
    """返回最小可用性标志。"""
    return {"status": "MoveToPlay server running"}


@app.get("/health")
def health() -> dict[str, str]:
    """供容器编排器探测进程是否正常响应。"""
    return {"status": "ok"}
