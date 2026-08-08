# MoveToPlay model training pipeline

This directory defines the reproducible processed-CSV -> dual Random Forest ->
firmware C-array pipeline. The runner is shared by local development, Docker,
and the future remote training service.

The pipeline never writes to `model/` or `main/generated/`. Each execution gets
an immutable directory under `output/training-runs/<run-id>/` containing model
bundles, reports, logs, generated C files, quality checks, and SHA-256 hashes.

## Canonical input

`datasets/movetoplay-deployed-v1.json` identifies the exact snapshot used by
the currently deployed arrays. `datasets/movetoplay-latest-v2.json` adds the
later zhq10/zhq11 sessions and is the default for new experiments. Each file
records paths, sizes, row counts, label distributions, and SHA-256. The data
itself stays outside Git and must be mounted or downloaded before a job starts.

Validation checks:

- exact file hashes, byte sizes, and row counts;
- required CSV columns;
- four-node coverage;
- session and label/event distributions;
- required numeric values;
- event IDs unique within their declared scope (historical v1 uses
  `session_id + event_id`; future uploads should use globally unique IDs).

## Local Windows run

Docker is preferred but is not required for the first experiment. The helper
creates `.venv-training`, installs the pinned environment, and starts the same
Python entrypoint used by the container.

Validate data only:

```powershell
powershell -ExecutionPolicy Bypass -File training/run-local.ps1 -ValidateOnly -Reinstall
```

Run both training jobs and export C arrays:

```powershell
powershell -ExecutionPolicy Bypass -File training/run-local.ps1
```

Reproduce the dataset snapshot used by the deployed firmware and require its C
arrays to match:

```powershell
powershell -ExecutionPolicy Bypass -File training/run-local.ps1 `
  -DatasetManifest training/datasets/movetoplay-deployed-v1.json `
  -RequireReferenceMatch
```

The historical model bundles were produced by scikit-learn 1.7.2, so that
version is pinned in `requirements.txt`.

## Docker run

Build the image from the repository root:

```powershell
docker build -f training/Dockerfile -t movetoplay-training:1 .
```

Run with the dataset read-only and the output directory writable:

```powershell
docker run --rm `
  --mount "type=bind,src=$PWD/data,dst=/workspace/data,readonly" `
  --mount "type=bind,src=$PWD/output,dst=/workspace/output" `
  movetoplay-training:1
```

The Docker build context excludes `data/`, recovered models, build products,
and temporary friend folders. Training data is never baked into the image.

## Run layout

```text
output/training-runs/<run-id>/
|-- dataset_validation.json
|-- run_manifest.json
|-- logs/
|-- models/
|   |-- state_rf_15_full/
|   `-- event_rf/
|-- generated/
`-- quality/
```

`run_manifest.json` records the Git commit and dirty state, Python/package
versions, dataset/config hashes, metrics, quality results, and every artifact
hash. A run passes only when both models satisfy the gates in `pipeline.json`.

Matching the currently deployed C arrays is reported but is not mandatory by
default. Use `--require-reference-match` only when intentionally verifying an
exact reproduction of the deployed model.

## Future Companion/server integration

The desktop app should not invoke training code directly. In Data Collect mode
it should produce one immutable upload bundle per collection session:

```text
session metadata + samples CSV + events CSV + per-file SHA-256
```

The server should authenticate the user, validate and store that bundle, assign
a globally unique session ID, and then create a new dataset manifest. Once a
manifest points at the server-produced aggregate processed CSV files, the
server invokes this same runner without changing the model-training code.

Recommended separation:

1. Companion/Dongle collection and upload.
2. Server-side session validation and immutable storage.
3. Dataset-version construction.
4. This training/export pipeline.
5. Manual promotion of a passing model/firmware candidate.

The historical manual merge is therefore not part of this pipeline. Dataset v1
is frozen; newly uploaded sessions should create dataset v2 or later through a
new, explicit dataset builder.
