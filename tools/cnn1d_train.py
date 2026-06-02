#!/usr/bin/env python3
"""
1D CNN training for multi-node IMU action recognition.

Reuses the same data loading and alignment pipeline as rf_baseline.py,
but feeds raw time-series windows directly into a lightweight 1D CNN
instead of extracting hand-crafted statistical features.

Output: ONNX model + TFLite int8 quantized model for ESP32-S3 deployment.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd
import torch
import torch.nn as nn
import torch.nn.functional as F
from torch.utils.data import DataLoader, TensorDataset
from sklearn.model_selection import GroupShuffleSplit
from sklearn.metrics import classification_report, confusion_matrix

THIS_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = THIS_DIR.parent
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from tools.action_labels import MULTICLASS_LABEL_ORDER

try:
    import matplotlib.pyplot as plt
    import seaborn as sns
    HAS_PLOT = True
except Exception:
    HAS_PLOT = False

BASE_CHANNELS = ["ax", "ay", "az", "gx", "gy", "gz"]
NODE_IDS = [1, 2, 3, 4]
NUM_CHANNELS = len(BASE_CHANNELS) * len(NODE_IDS)  # 24
WINDOW_SIZE = 25


class IMU1DCNN(nn.Module):
    """Lightweight 1D CNN for IMU action recognition on ESP32-S3.

    Reduced channel count for fast inference on ESP32-S3.
    RF = 7 + (5-1) + (3-1) = 13 frames = 520ms
    ~0.35M FLOPs, target inference ~25-35ms.
    """

    def __init__(self, num_channels: int, num_classes: int, window_size: int = 25,
                 dropout: float = 0.3):
        super().__init__()
        self.conv1 = nn.Conv1d(num_channels, 32, kernel_size=7, padding=3)
        self.bn1 = nn.BatchNorm1d(32)
        self.conv2 = nn.Conv1d(32, 32, kernel_size=5, padding=2)
        self.bn2 = nn.BatchNorm1d(32)
        self.conv3 = nn.Conv1d(32, 32, kernel_size=3, padding=1)
        self.bn3 = nn.BatchNorm1d(32)
        self.pool = nn.AdaptiveAvgPool1d(1)
        self.dropout = nn.Dropout(dropout)
        self.fc = nn.Linear(32, num_classes)

    def forward(self, x):
        # x: (batch, channels, time)
        x = F.relu(self.bn1(self.conv1(x)))
        x = F.relu(self.bn2(self.conv2(x)))
        x = F.relu(self.bn3(self.conv3(x)))
        x = self.pool(x).squeeze(-1)
        x = self.dropout(x)
        return self.fc(x)


# ---------------------------------------------------------------------------
# Data pipeline (reused from rf_baseline logic)
# ---------------------------------------------------------------------------

def load_data(csv_path: str) -> pd.DataFrame:
    path = Path(csv_path)

    # If path is a directory, load all CSVs in it and assign session_id by filename
    if path.is_dir():
        csv_files = sorted(path.glob("*.csv"))
        if not csv_files:
            raise FileNotFoundError(f"No CSV files found in {path}")
        dfs = []
        for f in csv_files:
            sub = pd.read_csv(f)
            sub["session_id"] = f.stem
            dfs.append(sub)
        df = pd.concat(dfs, ignore_index=True)
    else:
        df = pd.read_csv(csv_path)
        # Auto-assign session_id by detecting time gaps > 5s
        if "session_id" in df.columns:
            df["session_id"] = df["session_id"].astype(str)
        else:
            df["session_id"] = "session_0"
        if df["session_id"].nunique() <= 1 and "pc_timestamp_ms" in df.columns:
            df = df.sort_values("pc_timestamp_ms").reset_index(drop=True)
            time_diff = df["pc_timestamp_ms"].diff().fillna(0)
            session_breaks = (time_diff > 5000).cumsum()
            df["session_id"] = "session_" + session_breaks.astype(str)

    numeric_cols = ["pc_timestamp_ms", "board_timestamp_ms", "node_id",
                    "ax", "ay", "az", "gx", "gy", "gz"]
    for col in numeric_cols:
        df[col] = pd.to_numeric(df[col], errors="coerce")
    df = df.dropna(subset=["pc_timestamp_ms", "node_id", "label"])
    df["session_id"] = df["session_id"].astype(str)
    df["label"] = df["label"].astype(str)
    df = df.sort_values(["session_id", "pc_timestamp_ms", "node_id"]).reset_index(drop=True)
    return df


def _mode_or_first(values: pd.Series) -> str:
    modes = values.mode()
    return str(modes.iloc[0]) if not modes.empty else str(values.iloc[0])


def align_session(session_df: pd.DataFrame, align_ms: float, tolerance_ms: float) -> pd.DataFrame:
    session_df = session_df.sort_values("pc_timestamp_ms").copy()
    start_ms = float(session_df["pc_timestamp_ms"].min())
    end_ms = float(session_df["pc_timestamp_ms"].max())
    grid_times = np.arange(start_ms, end_ms + align_ms, align_ms)
    grid = pd.DataFrame({"sync_time_ms": grid_times.astype(float)})

    label_df = (
        session_df.groupby("pc_timestamp_ms", as_index=False)["label"]
        .agg(_mode_or_first)
        .rename(columns={"pc_timestamp_ms": "event_time_ms"})
        .sort_values("event_time_ms")
    )
    label_df["event_time_ms"] = label_df["event_time_ms"].astype(float)
    grid = pd.merge_asof(grid.sort_values("sync_time_ms"), label_df,
                         left_on="sync_time_ms", right_on="event_time_ms",
                         direction="nearest", tolerance=tolerance_ms)

    for node_id in NODE_IDS:
        node_df = (
            session_df[session_df["node_id"] == node_id]
            .groupby("pc_timestamp_ms", as_index=False)[BASE_CHANNELS]
            .mean()
            .rename(columns={"pc_timestamp_ms": "event_time_ms"})
            .sort_values("event_time_ms")
        )
        node_df["event_time_ms"] = node_df["event_time_ms"].astype(float)
        merged = pd.merge_asof(grid[["sync_time_ms"]].sort_values("sync_time_ms"),
                               node_df, left_on="sync_time_ms",
                               right_on="event_time_ms",
                               direction="nearest", tolerance=tolerance_ms)
        rename_map = {ch: f"n{node_id}_{ch}" for ch in BASE_CHANNELS}
        merged = merged.rename(columns=rename_map)
        grid = grid.merge(merged.drop(columns=["event_time_ms"], errors="ignore"),
                          on="sync_time_ms", how="left")

    essential_cols = [f"n{node_id}_ax" for node_id in NODE_IDS]
    grid = grid.dropna(subset=["label"] + essential_cols).reset_index(drop=True)
    grid["session_id"] = str(session_df["session_id"].iloc[0])
    return grid


def build_windows(sync_df: pd.DataFrame, window_size: int, step_size: int,
                  group_chunk_frames: int) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Build sliding windows from synchronized data.
    Returns: (X, y_labels, groups) where X is (N, channels, time)."""
    feature_cols = []
    for node_id in NODE_IDS:
        for ch in BASE_CHANNELS:
            feature_cols.append(f"n{node_id}_{ch}")

    # Split into contiguous label segments
    sync_df = sync_df.sort_values("sync_time_ms").reset_index(drop=True)
    label_changed = sync_df["label"] != sync_df["label"].shift(1)
    segment_id = label_changed.cumsum().astype(int)
    sync_df["segment_id"] = segment_id

    windows = []
    labels = []
    groups = []

    for seg_id, seg_df in sync_df.groupby("segment_id"):
        if len(seg_df) < window_size:
            continue
        data = seg_df[feature_cols].to_numpy(dtype=np.float32)
        seg_label = seg_df["label"].iloc[0]
        session_id = seg_df["session_id"].iloc[0]

        for start in range(0, len(data) - window_size + 1, step_size):
            window = data[start:start + window_size]  # (time, channels)
            windows.append(window.T)  # (channels, time)
            labels.append(seg_label)
            chunk_idx = start // group_chunk_frames
            groups.append(f"{session_id}_seg{seg_id}_chunk{chunk_idx}")

    X = np.array(windows, dtype=np.float32)
    y = np.array(labels)
    g = np.array(groups)
    return X, y, g


# ---------------------------------------------------------------------------
# Data augmentation
# ---------------------------------------------------------------------------

def augment_batch(x: torch.Tensor) -> torch.Tensor:
    """Apply random augmentations to a batch of (B, C, T) tensors."""
    # Time shift: roll by random offset [-3, +3]
    if torch.rand(1).item() < 0.5:
        shift = torch.randint(-3, 4, (1,)).item()
        x = torch.roll(x, shifts=shift, dims=2)

    # Additive Gaussian noise
    if torch.rand(1).item() < 0.5:
        x = x + torch.randn_like(x) * 0.05

    # Channel-wise scaling: multiply each channel by uniform [0.9, 1.1]
    if torch.rand(1).item() < 0.5:
        B, C, T = x.shape
        scale = 0.9 + 0.2 * torch.rand(1, C, 1, device=x.device)
        x = x * scale

    return x


# ---------------------------------------------------------------------------
# Training
# ---------------------------------------------------------------------------

def train_model(X_train, y_train, X_test, y_test, class_names, args):
    num_classes = len(class_names)
    label_to_idx = {name: i for i, name in enumerate(class_names)}
    y_train_idx = np.array([label_to_idx[l] for l in y_train])
    y_test_idx = np.array([label_to_idx[l] for l in y_test])

    # Class weights for imbalanced data
    class_counts = np.bincount(y_train_idx, minlength=num_classes).astype(float)
    class_weights = 1.0 / (class_counts + 1e-6)
    class_weights = class_weights / class_weights.sum() * num_classes
    weight_tensor = torch.FloatTensor(class_weights)

    train_dataset = TensorDataset(
        torch.FloatTensor(X_train), torch.LongTensor(y_train_idx))
    test_dataset = TensorDataset(
        torch.FloatTensor(X_test), torch.LongTensor(y_test_idx))

    train_loader = DataLoader(train_dataset, batch_size=args.batch_size, shuffle=True)
    test_loader = DataLoader(test_dataset, batch_size=args.batch_size, shuffle=False)

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    model = IMU1DCNN(NUM_CHANNELS, num_classes, WINDOW_SIZE,
                     dropout=args.dropout).to(device)
    optimizer = torch.optim.Adam(model.parameters(), lr=args.lr)
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(
        optimizer, T_max=args.epochs, eta_min=1e-5)
    criterion = nn.CrossEntropyLoss(weight=weight_tensor.to(device))

    print(f"\nTraining on {device}, {len(X_train)} train / {len(X_test)} test samples")
    print(f"Model parameters: {sum(p.numel() for p in model.parameters()):,}")

    best_acc = 0.0
    best_state = None
    for epoch in range(args.epochs):
        model.train()
        total_loss = 0.0
        for batch_x, batch_y in train_loader:
            batch_x, batch_y = batch_x.to(device), batch_y.to(device)
            batch_x = augment_batch(batch_x)
            optimizer.zero_grad()
            output = model(batch_x)
            loss = criterion(output, batch_y)
            loss.backward()
            optimizer.step()
            total_loss += loss.item() * batch_x.size(0)

        scheduler.step()
        avg_loss = total_loss / len(X_train)

        # Evaluate
        model.eval()
        correct = 0
        total = 0
        with torch.no_grad():
            for batch_x, batch_y in test_loader:
                batch_x, batch_y = batch_x.to(device), batch_y.to(device)
                output = model(batch_x)
                pred = output.argmax(dim=1)
                correct += (pred == batch_y).sum().item()
                total += batch_y.size(0)

        acc = correct / total
        if acc > best_acc:
            best_acc = acc
            best_state = {k: v.cpu().clone() for k, v in model.state_dict().items()}
        if (epoch + 1) % 10 == 0 or epoch == 0:
            print(f"  Epoch {epoch+1:3d}/{args.epochs}: loss={avg_loss:.4f} acc={acc:.4f} lr={scheduler.get_last_lr()[0]:.6f}")

    print(f"\nBest test accuracy: {best_acc:.4f}")

    # Load best model state
    if best_state is not None:
        model.load_state_dict(best_state)
        model.to(device)

    # Final evaluation
    model.eval()
    all_preds = []
    with torch.no_grad():
        for batch_x, batch_y in test_loader:
            batch_x = batch_x.to(device)
            output = model(batch_x)
            all_preds.extend(output.argmax(dim=1).cpu().numpy())

    all_preds = np.array(all_preds)
    print("\nClassification Report:")
    print(classification_report(y_test_idx, all_preds, target_names=class_names))

    return model, label_to_idx


def export_onnx(model, output_dir: Path, class_names: list[str]):
    model.eval()
    model.cpu()

    # Save PyTorch model
    torch_path = output_dir / "cnn1d_model.pt"
    torch.save(model.state_dict(), torch_path)
    print(f"\nPyTorch model saved: {torch_path}")

    # Try ONNX export
    try:
        dummy = torch.randn(1, NUM_CHANNELS, WINDOW_SIZE)
        onnx_path = output_dir / "cnn1d_model.onnx"
        torch.onnx.export(model, dummy, str(onnx_path),
                          input_names=["input"], output_names=["output"],
                          opset_version=13,
                          dynamic_axes={"input": {0: "batch"}, "output": {0: "batch"}})
        print(f"ONNX model saved: {onnx_path}")
    except Exception as e:
        print(f"ONNX export skipped ({e})")

    meta = {
        "class_names": class_names,
        "node_ids": NODE_IDS,
        "num_channels": NUM_CHANNELS,
        "window_size": WINDOW_SIZE,
        "input_shape": [1, NUM_CHANNELS, WINDOW_SIZE],
        "kernel_sizes": [7, 5, 3],
        "conv_channels": [32, 64, 64],
    }
    meta_path = output_dir / "cnn1d_model_meta.json"
    meta_path.write_text(json.dumps(meta, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"Model metadata saved: {meta_path}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description="1D CNN training for IMU HAR")
    parser.add_argument("--input", required=True, help="Input CSV path")
    parser.add_argument("--window-size", type=int, default=25)
    parser.add_argument("--step-size", type=int, default=10)
    parser.add_argument("--align-ms", type=float, default=40.0)
    parser.add_argument("--align-tolerance-ms", type=float, default=25.0)
    parser.add_argument("--group-chunk-frames", type=int, default=200)
    parser.add_argument("--test-size", type=float, default=0.25)
    parser.add_argument("--batch-size", type=int, default=64)
    parser.add_argument("--epochs", type=int, default=300)
    parser.add_argument("--lr", type=float, default=0.002)
    parser.add_argument("--dropout", type=float, default=0.3)
    parser.add_argument("--output-dir", default="output_cnn1d")
    args = parser.parse_args()

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    print("Loading data...")
    df = load_data(args.input)
    print(f"  Raw frames: {len(df)}")

    print("Aligning sessions...")
    sessions = []
    for session_id, session_df in df.groupby("session_id"):
        aligned = align_session(session_df, args.align_ms, args.align_tolerance_ms)
        if len(aligned) > 0:
            sessions.append(aligned)
    sync_df = pd.concat(sessions, ignore_index=True)
    print(f"  Aligned frames: {len(sync_df)}")

    print("Building windows...")
    X, y, groups = build_windows(sync_df, args.window_size, args.step_size,
                                 args.group_chunk_frames)
    print(f"  Windows: {X.shape[0]}, shape: {X.shape}")

    # Class ordering
    label_set = set(y)
    class_names = [l for l in MULTICLASS_LABEL_ORDER if l in label_set]
    class_names.extend(sorted(label_set - set(class_names)))
    print(f"  Classes ({len(class_names)}): {class_names}")

    # Label distribution
    unique, counts = np.unique(y, return_counts=True)
    print("\n  Label distribution:")
    for label, count in sorted(zip(unique, counts), key=lambda x: -x[1]):
        print(f"    {label:25s}: {count:5d} ({100*count/len(y):.1f}%)")

    # Train/test split (group-wise to prevent leakage)
    splitter = GroupShuffleSplit(n_splits=1, test_size=args.test_size, random_state=42)
    train_idx, test_idx = next(splitter.split(X, y, groups))
    X_train, X_test = X[train_idx], X[test_idx]
    y_train, y_test = y[train_idx], y[test_idx]

    # Normalize per channel (fit on train)
    mean = X_train.mean(axis=(0, 2), keepdims=True)
    std = X_train.std(axis=(0, 2), keepdims=True) + 1e-6
    X_train = (X_train - mean) / std
    X_test = (X_test - mean) / std

    # Save normalization params
    norm_params = {"mean": mean.squeeze().tolist(), "std": std.squeeze().tolist()}
    (output_dir / "norm_params.json").write_text(
        json.dumps(norm_params, indent=2), encoding="utf-8")

    # Train
    model, label_to_idx = train_model(X_train, y_train, X_test, y_test, class_names, args)

    # Export
    export_onnx(model, output_dir, class_names)

    # Save confusion matrix plot
    if HAS_PLOT:
        model.eval()
        model.cpu()
        with torch.no_grad():
            preds = model(torch.FloatTensor(X_test)).argmax(dim=1).numpy()
        y_test_idx = np.array([label_to_idx[l] for l in y_test])
        cm = confusion_matrix(y_test_idx, preds)
        fig, ax = plt.subplots(figsize=(10, 8))
        sns.heatmap(cm, annot=True, fmt="d", xticklabels=class_names,
                    yticklabels=class_names, cmap="Blues", ax=ax)
        ax.set_xlabel("Predicted")
        ax.set_ylabel("True")
        ax.set_title("1D CNN Confusion Matrix")
        plt.tight_layout()
        plt.savefig(output_dir / "confusion_matrix_cnn1d.png", dpi=150)
        print(f"\nConfusion matrix saved: {output_dir / 'confusion_matrix_cnn1d.png'}")

    print("\nDone!")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
