#!/usr/bin/env python3
"""
Random Forest baseline for multi-node IMU action recognition.

Pipeline:
1. Read raw per-frame CSV exported by the data collector
2. Align multiple nodes onto a shared PC-time grid
3. Split each session into contiguous labeled chunks
4. Build sliding windows inside each chunk
5. Extract hand-crafted statistics for each node and node-pair
6. Train / evaluate a Random Forest classifier without row-level leakage

Default task:
    multiclass action recognition aligned with the collector labels

The script is intentionally written for clarity and easy modification.
"""

from __future__ import annotations

import argparse
import itertools
import json
import sys
from pathlib import Path
from typing import Iterable, Optional

import joblib
import numpy as np
import pandas as pd
from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import (
    accuracy_score,
    classification_report,
    confusion_matrix,
    precision_recall_fscore_support,
)
from sklearn.model_selection import GroupShuffleSplit

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
NORM_CHANNELS = ["acc_norm", "gyro_norm"]
ALL_CHANNELS = BASE_CHANNELS + NORM_CHANNELS
RF_NODE_IDS = [1, 2, 3, 4]
RF_NODE_NAMES = {
    1: "chest",
    2: "right_hand",
    3: "left_hand",
    4: "leg",
}


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Random Forest baseline for IMU HAR")
    parser.add_argument("--input", required=True, help="Input CSV path")
    parser.add_argument(
        "--task",
        choices=["binary_jump", "three_class", "multiclass"],
        default="multiclass",
        help="Classification task type; default uses the full collector label set",
    )
    parser.add_argument(
        "--window-size",
        type=int,
        default=25,
        help="Sliding window size in synchronized frames",
    )
    parser.add_argument(
        "--step-size",
        type=int,
        default=10,
        help="Sliding window step in synchronized frames",
    )
    parser.add_argument(
        "--align-ms",
        type=float,
        default=40.0,
        help="Shared synchronization grid step in milliseconds",
    )
    parser.add_argument(
        "--align-tolerance-ms",
        type=float,
        default=25.0,
        help="Nearest-neighbor alignment tolerance in milliseconds",
    )
    parser.add_argument(
        "--group-chunk-frames",
        type=int,
        default=200,
        help="Split each contiguous label segment into smaller chunks for train/test grouping",
    )
    parser.add_argument(
        "--test-size",
        type=float,
        default=0.25,
        help="Group-wise test split ratio",
    )
    parser.add_argument(
        "--n-estimators",
        type=int,
        default=400,
        help="Random Forest tree count",
    )
    parser.add_argument(
        "--max-depth",
        type=int,
        default=None,
        help="Optional Random Forest max_depth",
    )
    parser.add_argument(
        "--random-state",
        type=int,
        default=42,
        help="Random seed",
    )
    parser.add_argument(
        "--top-k-features",
        type=int,
        default=25,
        help="How many feature importances to print",
    )
    parser.add_argument(
        "--output-dir",
        default="output",
        help="Directory for reports and optional plots",
    )
    return parser


def load_data(csv_path: str) -> pd.DataFrame:
    required_cols = [
        "pc_timestamp_ms",
        "board_timestamp_ms",
        "node_id",
        "ax",
        "ay",
        "az",
        "gx",
        "gy",
        "gz",
        "label",
        "session_id",
    ]
    df = pd.read_csv(csv_path)
    missing = [col for col in required_cols if col not in df.columns]
    if missing:
        raise ValueError(f"missing required columns: {missing}")

    numeric_cols = [
        "pc_timestamp_ms",
        "board_timestamp_ms",
        "node_id",
        "ax",
        "ay",
        "az",
        "gx",
        "gy",
        "gz",
    ]
    for col in numeric_cols:
        df[col] = pd.to_numeric(df[col], errors="coerce")

    df = df.dropna(subset=["pc_timestamp_ms", "node_id", "label"])
    df["session_id"] = df["session_id"].astype(str)
    df["label"] = df["label"].astype(str)
    df["pc_timestamp_ms"] = df["pc_timestamp_ms"].astype(float)
    df["board_timestamp_ms"] = df["board_timestamp_ms"].astype(float)
    df["node_id"] = df["node_id"].astype(int)

    unknown_node_ids = sorted(set(df["node_id"].unique().tolist()) - set(RF_NODE_IDS))
    if unknown_node_ids:
        raise ValueError(
            f"unsupported node_id values: {unknown_node_ids}; expected only {RF_NODE_IDS}"
        )

    missing_node_ids = [node_id for node_id in RF_NODE_IDS if node_id not in set(df["node_id"].unique().tolist())]
    if missing_node_ids:
        missing_names = [RF_NODE_NAMES[node_id] for node_id in missing_node_ids]
        raise ValueError(
            f"missing required node_id values: {missing_node_ids} ({missing_names})"
        )

    df = df.sort_values(["session_id", "pc_timestamp_ms", "node_id"]).reset_index(drop=True)
    return df


def choose_task_label(raw_label: str, task: str) -> str:
    if task == "binary_jump":
        return "jump" if raw_label == "jump" else "non_jump"
    if task == "three_class":
        if raw_label == "jump":
            return "jump"
        if raw_label == "attack":
            return "attack"
        return "else"
    return raw_label


def task_class_order(task: str, labels: Iterable[str]) -> list[str]:
    if task == "binary_jump":
        return ["jump", "non_jump"]
    if task == "three_class":
        return ["jump", "attack", "else"]
    preferred = MULTICLASS_LABEL_ORDER
    label_set = set(labels)
    ordered = [label for label in preferred if label in label_set]
    ordered.extend(sorted(label_set - set(ordered)))
    return ordered


def _mode_or_first(values: pd.Series) -> str:
    modes = values.mode()
    if not modes.empty:
        return str(modes.iloc[0])
    return str(values.iloc[0])


def align_session(
    session_df: pd.DataFrame,
    align_ms: float,
    tolerance_ms: float,
    node_ids: list[int],
) -> pd.DataFrame:
    session_df = session_df.sort_values("pc_timestamp_ms").copy()
    start_ms = float(session_df["pc_timestamp_ms"].min())
    end_ms = float(session_df["pc_timestamp_ms"].max())
    grid_times = np.arange(start_ms, end_ms + align_ms, align_ms)
    grid = pd.DataFrame({"sync_time_ms": grid_times.astype(float)})

    label_df = (
        session_df.groupby("pc_timestamp_ms", as_index=False)["label"]
        .agg(_mode_or_first)
        .rename(columns={"pc_timestamp_ms": "event_time_ms", "label": "label"})
        .sort_values("event_time_ms")
    )
    label_df["event_time_ms"] = label_df["event_time_ms"].astype(float)
    grid = pd.merge_asof(
        grid.sort_values("sync_time_ms"),
        label_df,
        left_on="sync_time_ms",
        right_on="event_time_ms",
        direction="nearest",
        tolerance=tolerance_ms,
    )

    for node_id in node_ids:
        node_df = (
            session_df[session_df["node_id"] == node_id]
            .groupby("pc_timestamp_ms", as_index=False)[["board_timestamp_ms"] + BASE_CHANNELS]
            .mean()
            .rename(columns={"pc_timestamp_ms": "event_time_ms"})
            .sort_values("event_time_ms")
        )
        node_df["event_time_ms"] = node_df["event_time_ms"].astype(float)
        merged = pd.merge_asof(
            grid[["sync_time_ms"]].sort_values("sync_time_ms"),
            node_df,
            left_on="sync_time_ms",
            right_on="event_time_ms",
            direction="nearest",
            tolerance=tolerance_ms,
        )
        rename_map = {
            "board_timestamp_ms": f"n{node_id}_board_timestamp_ms",
            "ax": f"n{node_id}_ax",
            "ay": f"n{node_id}_ay",
            "az": f"n{node_id}_az",
            "gx": f"n{node_id}_gx",
            "gy": f"n{node_id}_gy",
            "gz": f"n{node_id}_gz",
        }
        merged = merged.rename(columns=rename_map)
        grid = grid.merge(
            merged.drop(columns=["event_time_ms"], errors="ignore"),
            on="sync_time_ms",
            how="left",
        )

    essential_cols = [f"n{node_id}_ax" for node_id in node_ids]
    grid = grid.dropna(subset=["label"] + essential_cols).reset_index(drop=True)
    grid["session_id"] = str(session_df["session_id"].iloc[0])
    return grid


def add_norm_features(sync_df: pd.DataFrame, node_ids: list[int]) -> pd.DataFrame:
    sync_df = sync_df.copy()
    for node_id in node_ids:
        prefix = f"n{node_id}"
        acc = sync_df[[f"{prefix}_ax", f"{prefix}_ay", f"{prefix}_az"]].to_numpy(dtype=float)
        gyro = sync_df[[f"{prefix}_gx", f"{prefix}_gy", f"{prefix}_gz"]].to_numpy(dtype=float)
        sync_df[f"{prefix}_acc_norm"] = np.linalg.norm(acc, axis=1)
        sync_df[f"{prefix}_gyro_norm"] = np.linalg.norm(gyro, axis=1)
    return sync_df


def add_group_columns(sync_df: pd.DataFrame, align_ms: float, group_chunk_frames: int) -> pd.DataFrame:
    sync_df = sync_df.sort_values("sync_time_ms").reset_index(drop=True).copy()
    time_diff = sync_df["sync_time_ms"].diff().fillna(align_ms)
    label_changed = sync_df["label"] != sync_df["label"].shift(1)
    large_gap = time_diff > (align_ms * 2.5)
    base_segment = (label_changed | large_gap).cumsum().astype(int)
    sync_df["base_segment_id"] = base_segment

    split_groups: list[str] = []
    for _, seg_df in sync_df.groupby("base_segment_id", sort=False):
        session_id = str(seg_df["session_id"].iloc[0])
        segment_id = int(seg_df["base_segment_id"].iloc[0])
        chunk_index = np.arange(len(seg_df)) // group_chunk_frames
        split_groups.extend(
            f"{session_id}_seg{segment_id:03d}_chunk{idx:03d}" for idx in chunk_index
        )
    sync_df["split_group"] = split_groups
    return sync_df


def compute_channel_features(x: np.ndarray, prefix: str) -> dict[str, float]:
    x = np.asarray(x, dtype=float)
    feats = {
        f"{prefix}_mean": float(np.mean(x)),
        f"{prefix}_std": float(np.std(x)),
        f"{prefix}_max": float(np.max(x)),
        f"{prefix}_min": float(np.min(x)),
        f"{prefix}_ptp": float(np.ptp(x)),
        f"{prefix}_rms": float(np.sqrt(np.mean(np.square(x)))),
        f"{prefix}_energy": float(np.mean(np.square(x))),
    }

    if len(x) > 1:
        dx = np.diff(x)
        feats[f"{prefix}_jerk_mean_abs"] = float(np.mean(np.abs(dx)))
        feats[f"{prefix}_jerk_std"] = float(np.std(dx))
        feats[f"{prefix}_jerk_rms"] = float(np.sqrt(np.mean(np.square(dx))))
    else:
        feats[f"{prefix}_jerk_mean_abs"] = 0.0
        feats[f"{prefix}_jerk_std"] = 0.0
        feats[f"{prefix}_jerk_rms"] = 0.0
    return feats


def extract_window_features(window_df: pd.DataFrame, node_ids: list[int]) -> dict[str, float]:
    feats: dict[str, float] = {}

    for node_id in node_ids:
        prefix = f"n{node_id}"
        for channel in ALL_CHANNELS:
            values = window_df[f"{prefix}_{channel}"].to_numpy(dtype=float)
            feats.update(compute_channel_features(values, f"{prefix}_{channel}"))

    for node_a, node_b in itertools.combinations(node_ids, 2):
        prefix_a = f"n{node_a}"
        prefix_b = f"n{node_b}"
        for channel in ALL_CHANNELS:
            diff_values = (
                window_df[f"{prefix_a}_{channel}"].to_numpy(dtype=float)
                - window_df[f"{prefix_b}_{channel}"].to_numpy(dtype=float)
            )
            feats.update(
                compute_channel_features(diff_values, f"pair_{node_a}_{node_b}_{channel}_diff")
            )

        for channel in ["acc_norm", "gyro_norm"]:
            x = window_df[f"{prefix_a}_{channel}"].to_numpy(dtype=float)
            y = window_df[f"{prefix_b}_{channel}"].to_numpy(dtype=float)
            if np.std(x) < 1e-9 or np.std(y) < 1e-9:
                corr = 0.0
            else:
                corr = float(np.corrcoef(x, y)[0, 1])
            feats[f"pair_{node_a}_{node_b}_{channel}_corr"] = corr

    return feats


def window_label(labels: pd.Series) -> str:
    modes = labels.mode()
    if not modes.empty:
        return str(modes.iloc[0])
    return str(labels.iloc[len(labels) // 2])


def build_windows(
    sync_df: pd.DataFrame,
    node_ids: list[int],
    task: str,
    window_size: int,
    step_size: int,
) -> tuple[pd.DataFrame, pd.Series, pd.Series]:
    feature_rows: list[dict[str, float]] = []
    labels: list[str] = []
    groups: list[str] = []

    for split_group, group_df in sync_df.groupby("split_group", sort=False):
        group_df = group_df.reset_index(drop=True)
        if len(group_df) < window_size:
            continue

        for start in range(0, len(group_df) - window_size + 1, step_size):
            window = group_df.iloc[start : start + window_size]
            raw_label = window_label(window["label"])
            feature_rows.append(extract_window_features(window, node_ids))
            labels.append(choose_task_label(raw_label, task))
            groups.append(split_group)

    if not feature_rows:
        raise RuntimeError("no valid windows were generated; reduce window_size or inspect input data")

    X = pd.DataFrame(feature_rows)
    y = pd.Series(labels, name="label")
    group_series = pd.Series(groups, name="group")
    return X, y, group_series


def prepare_dataset(
    df: pd.DataFrame,
    align_ms: float,
    tolerance_ms: float,
    group_chunk_frames: int,
    window_size: int,
    step_size: int,
    task: str,
) -> tuple[pd.DataFrame, pd.Series, pd.Series, list[int]]:
    node_ids = RF_NODE_IDS.copy()
    synced_sessions: list[pd.DataFrame] = []

    for session_id, session_df in df.groupby("session_id", sort=False):
        synced = align_session(session_df, align_ms=align_ms, tolerance_ms=tolerance_ms, node_ids=node_ids)
        synced = add_norm_features(synced, node_ids=node_ids)
        synced = add_group_columns(synced, align_ms=align_ms, group_chunk_frames=group_chunk_frames)
        synced_sessions.append(synced)
        print(
            f"[info] session={session_id} raw_rows={len(session_df)} "
            f"synced_rows={len(synced)} groups={synced['split_group'].nunique()}"
        )

    synced_all = pd.concat(synced_sessions, ignore_index=True)
    X, y, groups = build_windows(
        synced_all,
        node_ids=node_ids,
        task=task,
        window_size=window_size,
        step_size=step_size,
    )
    return X, y, groups, node_ids


def make_group_split(
    X: pd.DataFrame,
    y: pd.Series,
    groups: pd.Series,
    test_size: float,
    random_state: int,
) -> tuple[np.ndarray, np.ndarray]:
    all_classes = set(y.unique().tolist())
    fallback: Optional[tuple[np.ndarray, np.ndarray]] = None

    for offset in range(50):
        splitter = GroupShuffleSplit(
            n_splits=1,
            test_size=test_size,
            random_state=random_state + offset,
        )
        train_idx, test_idx = next(splitter.split(X, y, groups))
        if fallback is None:
            fallback = (train_idx, test_idx)
        if set(y.iloc[train_idx].unique()) == all_classes and set(y.iloc[test_idx].unique()) == all_classes:
            return train_idx, test_idx

    print("[warn] could not find a split containing every class in both train and test; using fallback split")
    if fallback is None:
        raise RuntimeError("failed to create train/test split")
    return fallback


def save_confusion_matrix(
    cm: np.ndarray,
    class_names: list[str],
    output_path: Path,
) -> None:
    if not HAS_PLOT:
        print("[warn] matplotlib/seaborn unavailable, skipped confusion matrix plot")
        return

    output_path.parent.mkdir(parents=True, exist_ok=True)
    plt.figure(figsize=(7, 5))
    sns.heatmap(cm, annot=True, fmt="d", cmap="Blues", xticklabels=class_names, yticklabels=class_names)
    plt.xlabel("Predicted")
    plt.ylabel("True")
    plt.title("Random Forest Confusion Matrix")
    plt.tight_layout()
    plt.savefig(output_path, dpi=180)
    plt.close()


def save_model_bundle(
    clf: RandomForestClassifier,
    feature_names: list[str],
    class_names: list[str],
    node_ids: list[int],
    args: argparse.Namespace,
    output_dir: Path,
) -> None:
    bundle = {
        "model": clf,
        "feature_names": feature_names,
        "class_names": class_names,
        "node_ids": node_ids,
        "config": {
            "task": args.task,
            "window_size": args.window_size,
            "step_size": args.step_size,
            "align_ms": args.align_ms,
            "align_tolerance_ms": args.align_tolerance_ms,
            "group_chunk_frames": args.group_chunk_frames,
        },
    }
    model_path = output_dir / "rf_model.joblib"
    joblib.dump(bundle, model_path)
    print(f"[info] model bundle saved to: {model_path}")

    meta_path = output_dir / "rf_model_meta.json"
    meta = {
        "class_names": class_names,
        "node_ids": node_ids,
        "feature_count": len(feature_names),
        "feature_names": feature_names,
        "config": bundle["config"],
    }
    meta_path.write_text(json.dumps(meta, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"[info] model metadata saved to: {meta_path}")


def train_and_evaluate(
    X: pd.DataFrame,
    y: pd.Series,
    groups: pd.Series,
    node_ids: list[int],
    args: argparse.Namespace,
) -> None:
    train_idx, test_idx = make_group_split(
        X=X,
        y=y,
        groups=groups,
        test_size=args.test_size,
        random_state=args.random_state,
    )

    X_train = X.iloc[train_idx]
    X_test = X.iloc[test_idx]
    y_train = y.iloc[train_idx]
    y_test = y.iloc[test_idx]
    train_groups = groups.iloc[train_idx].nunique()
    test_groups = groups.iloc[test_idx].nunique()

    print(
        f"[info] windows train={len(X_train)} test={len(X_test)} "
        f"train_groups={train_groups} test_groups={test_groups}"
    )
    print(f"[info] train label counts:\n{y_train.value_counts().to_string()}")
    print(f"[info] test label counts:\n{y_test.value_counts().to_string()}")

    clf = RandomForestClassifier(
        n_estimators=args.n_estimators,
        max_depth=args.max_depth,
        class_weight="balanced",
        random_state=args.random_state,
        n_jobs=-1,
    )
    clf.fit(X_train, y_train)

    y_pred = clf.predict(X_test)
    class_names = task_class_order(args.task, y.unique())

    accuracy = accuracy_score(y_test, y_pred)
    precision, recall, f1, _ = precision_recall_fscore_support(
        y_test,
        y_pred,
        labels=class_names,
        average="weighted",
        zero_division=0,
    )
    cm = confusion_matrix(y_test, y_pred, labels=class_names)

    print("\n=== Metrics ===")
    print(f"accuracy  : {accuracy:.4f}")
    print(f"precision : {precision:.4f}")
    print(f"recall    : {recall:.4f}")
    print(f"f1        : {f1:.4f}")

    print("\n=== Classification Report ===")
    print(classification_report(y_test, y_pred, labels=class_names, zero_division=0))

    print("\n=== Confusion Matrix ===")
    print(pd.DataFrame(cm, index=class_names, columns=class_names).to_string())

    importance_df = (
        pd.DataFrame(
            {
                "feature": X.columns,
                "importance": clf.feature_importances_,
            }
        )
        .sort_values("importance", ascending=False)
        .reset_index(drop=True)
    )

    print(f"\n=== Top {args.top_k_features} Features ===")
    print(importance_df.head(args.top_k_features).to_string(index=False))

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    importance_path = output_dir / "feature_importance.csv"
    importance_df.to_csv(importance_path, index=False)
    print(f"\n[info] feature importance saved to: {importance_path}")

    cm_plot_path = output_dir / "confusion_matrix.png"
    save_confusion_matrix(cm, class_names, cm_plot_path)
    if cm_plot_path.exists():
        print(f"[info] confusion matrix plot saved to: {cm_plot_path}")

    save_model_bundle(
        clf=clf,
        feature_names=X.columns.tolist(),
        class_names=class_names,
        node_ids=node_ids,
        args=args,
        output_dir=output_dir,
    )


def main() -> int:
    args = build_arg_parser().parse_args()
    node_ids = RF_NODE_IDS.copy()

    if args.window_size <= 1:
        raise ValueError("window-size must be greater than 1")
    if args.step_size <= 0:
        raise ValueError("step-size must be greater than 0")
    if args.group_chunk_frames < args.window_size:
        raise ValueError("group-chunk-frames must be >= window-size")

    df = load_data(args.input)
    print(
        f"[info] loaded rows={len(df)} sessions={df['session_id'].nunique()} "
        f"nodes={node_ids} names={[RF_NODE_NAMES[node_id] for node_id in node_ids]}"
    )
    print(f"[info] raw label counts:\n{df['label'].value_counts().to_string()}")

    X, y, groups, node_ids = prepare_dataset(
        df=df,
        align_ms=args.align_ms,
        tolerance_ms=args.align_tolerance_ms,
        group_chunk_frames=args.group_chunk_frames,
        window_size=args.window_size,
        step_size=args.step_size,
        task=args.task,
    )
    print(
        f"[info] dataset ready windows={len(X)} features={X.shape[1]} "
        f"classes={sorted(y.unique().tolist())} node_ids={node_ids}"
    )
    train_and_evaluate(X=X, y=y, groups=groups, node_ids=node_ids, args=args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
