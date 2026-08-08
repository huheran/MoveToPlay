FROM python:3.13.9-slim-bookworm

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PYTHONUTF8=1 \
    PIP_DISABLE_PIP_VERSION_CHECK=1

WORKDIR /workspace

COPY training/requirements.txt /tmp/movetoplay-training-requirements.txt
RUN python -m pip install --no-cache-dir -r /tmp/movetoplay-training-requirements.txt

COPY . /workspace

ENTRYPOINT ["python", "tools/run_training_pipeline.py"]
CMD ["--dataset-manifest", "training/datasets/movetoplay-latest-v2.json"]
