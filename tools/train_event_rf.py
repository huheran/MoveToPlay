#!/usr/bin/env python3
"""
Train a firmware-compatible Random Forest event classifier.

This script uses samples.csv + events.csv, creates causal rolling windows that
match main/rf_infer.c, and saves model/rf_model.joblib for export_rf_model_c.py.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

THIS_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = THIS_DIR.parent
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

import joblib
import numpy as np
import pandas as pd
from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import accuracy_score, classification_report, confusion_matrix

from tools.export_rf_model_c import build_expected_feature_names
from tools.rf_baseline import (
    BASE_CHANNELS,
    RF_NODE_IDS,
    add_norm_features,
    extract_window_features,
    make_group_split,
)


EVENT_TYPE_TO_GROUP = {
    "right_hand_slash": "attack_event",
    "hands_shoot": "attack_event",
    "kick": "attack_event",
    "jump": "jump_event",
    "hands_press_down": "skill_event",
    "hands_cross_forehead": "skill_event",
    "ultraman_beam": "skill_event",
    "right_hand_raise": "pause_event",
    "left_hand_raise": "pause_event",
    "turn_left": "turn_event",
    "turn_right": "turn_event",
}


def parse_filter_values(values: list[str]) -> set[str]:
    parsed: set[str] = set()
    for value in values:
        for item in value.split(","):
            text = item.strip()
            if text:
                parsed.add(text)
    return parsed


def normalize_event_group(value: str) -> str:
    text = str(value).strip()
    if text in {"attack", "jump", "pause", "skill", "turn"}:
        return f"{text}_event"
    return text if text else "none"


def load_samples(path: Path) -> pd.DataFrame:
    df = pd.read_csv(path)
    required = {"pc_timestamp_ms", "board_timestamp_ms", "node_id", *BASE_CHANNELS, "session_id"}
    missing = sorted(required - set(df.columns))
    if missing:
        raise ValueError(f"samples CSV missing columns: {missing}")

    for col in ["pc_timestamp_ms", "board_timestamp_ms", "node_id", *BASE_CHANNELS]:
        df[col] = pd.to_numeric(df[col], errors="coerce")
    df = df.dropna(subset=["pc_timestamp_ms", "node_id", *BASE_CHANNELS]).copy()
    df["pc_timestamp_ms"] = df["pc_timestamp_ms"].astype(float)
    df["board_timestamp_ms"] = df["board_timestamp_ms"].astype(float)
    df["node_id"] = df["node_id"].astype(int)
    df["session_id"] = df["session_id"].astype(str)
    unknown_nodes = sorted(set(df["node_id"].unique()) - set(RF_NODE_IDS))
    if unknown_nodes:
        raise ValueError(f"unsupported node_id values: {unknown_nodes}; expected only {RF_NODE_IDS}")
    return df.sort_values(["session_id", "pc_timestamp_ms", "node_id"]).reset_index(drop=True)


def load_events(path: Path) -> pd.DataFrame:
    df = pd.read_csv(path)
    required = {"event_id", "event_type", "pc_timestamp_ms", "session_id"}
    missing = sorted(required - set(df.columns))
    if missing:
        raise ValueError(f"events CSV missing columns: {missing}")

    df["pc_timestamp_ms"] = pd.to_numeric(df["pc_timestamp_ms"], errors="coerce")
    df = df.dropna(subset=["pc_timestamp_ms"]).copy()
    df["pc_timestamp_ms"] = df["pc_timestamp_ms"].astype(float)
    df["event_id"] = df["event_id"].astype(str)
    df["event_type"] = df["event_type"].astype(str)
    df["session_id"] = df["session_id"].astype(str)
    if "event_group" not in df.columns:
        df["event_group"] = df["event_type"].map(EVENT_TYPE_TO_GROUP).fillna("none")
    df["event_group"] = df["event_group"].map(normalize_event_group)
    return df.sort_values(["session_id", "pc_timestamp_ms"]).reset_index(drop=True)


def filter_positive_events(events: pd.DataFrame, args: argparse.Namespace) -> pd.DataFrame:
    include_groups = {normalize_event_group(v) for v in parse_filter_values(args.include_event_group)}
    exclude_groups = {normalize_event_group(v) for v in parse_filter_values(args.exclude_event_group)}
    include_types = parse_filter_values(args.include_event_type)
    exclude_types = parse_filter_values(args.exclude_event_type)
    include_ids = parse_filter_values(args.include_event_id)
    exclude_ids = parse_filter_values(args.exclude_event_id)

    filtered = events.copy()
    if include_groups:
        filtered = filtered[filtered["event_group"].isin(include_groups)]
    if exclude_groups:
        filtered = filtered[~filtered["event_group"].isin(exclude_groups)]
    if include_types:
        filtered = filtered[filtered["event_type"].isin(include_types)]
    if exclude_types:
        filtered = filtered[~filtered["event_type"].isin(exclude_types)]
    if include_ids:
        filtered = filtered[filtered["event_id"].isin(include_ids)]
    if exclude_ids:
        filtered = filtered[~filtered["event_id"].isin(exclude_ids)]
    return filtered.reset_index(drop=True)


def align_session_samples(
    session_df: pd.DataFrame,
    align_ms: float,
    tolerance_ms: float,
    node_ids: list[int],
) -> pd.DataFrame:
    session_df = session_df.sort_values("pc_timestamp_ms").copy()
    start_ms = float(session_df["pc_timestamp_ms"].min())
    end_ms = float(session_df["pc_timestamp_ms"].max())
    grid = pd.DataFrame({"sync_time_ms": np.arange(start_ms, end_ms + align_ms, align_ms, dtype=float)})

    if "state_label" in session_df.columns:
        state_df = (
            session_df.groupby("pc_timestamp_ms", as_index=False)["state_label"]
            .agg(lambda values: str(values.mode().iloc[0]) if not values.mode().empty else str(values.iloc[0]))
            .rename(columns={"pc_timestamp_ms": "event_time_ms"})
            .sort_values("event_time_ms")
        )
        grid = pd.merge_asof(
            grid.sort_values("sync_time_ms"),
            state_df,
            left_on="sync_time_ms",
            right_on="event_time_ms",
            direction="nearest",
            tolerance=tolerance_ms,
        ).drop(columns=["event_time_ms"], errors="ignore")

    for node_id in node_ids:
        node_df = (
            session_df[session_df["node_id"] == node_id]
            .groupby("pc_timestamp_ms", as_index=False)[["board_timestamp_ms"] + BASE_CHANNELS]
            .mean()
            .rename(columns={"pc_timestamp_ms": "event_time_ms"})
            .sort_values("event_time_ms")
        )
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
        grid = grid.merge(merged.drop(columns=["event_time_ms"], errors="ignore"), on="sync_time_ms", how="left")

    essential_cols = [f"n{node_id}_ax" for node_id in node_ids]
    grid = grid.dropna(subset=essential_cols).reset_index(drop=True)
    if "state_label" not in grid.columns:
        grid["state_label"] = "unknown"
    grid["state_label"] = grid["state_label"].fillna("unknown").astype(str)
    grid["session_id"] = str(session_df["session_id"].iloc[0])
    return add_norm_features(grid, node_ids=node_ids)


def add_segment_ids(sync_df: pd.DataFrame, align_ms: float) -> pd.DataFrame:
    out = sync_df.sort_values(["session_id", "sync_time_ms"]).reset_index(drop=True).copy()
    segment_ids: list[str] = []
    for session_id, session_df in out.groupby("session_id", sort=False):
        diffs = session_df["sync_time_ms"].diff().fillna(align_ms)
        segment = (diffs > (align_ms * 2.5)).cumsum().astype(int)
        segment_ids.extend(f"{session_id}_seg{seg:04d}" for seg in segment)
    out["segment_id"] = segment_ids
    return out


def prepare_synced_samples(samples: pd.DataFrame, args: argparse.Namespace) -> pd.DataFrame:
    synced_sessions: list[pd.DataFrame] = []
    for session_id, session_df in samples.groupby("session_id", sort=False):
        synced = align_session_samples(
            session_df,
            align_ms=args.align_ms,
            tolerance_ms=args.align_tolerance_ms,
            node_ids=RF_NODE_IDS,
        )
        print(f"[info] session={session_id} raw_rows={len(session_df)} synced_rows={len(synced)}")
        synced_sessions.append(synced)
    return add_segment_ids(pd.concat(synced_sessions, ignore_index=True), align_ms=args.align_ms)


def window_is_contiguous(window_df: pd.DataFrame, align_ms: float) -> bool:
    diffs = window_df["sync_time_ms"].diff().dropna()
    return bool((diffs <= (align_ms * 2.5)).all())


def build_event_lookup(events: pd.DataFrame) -> dict[str, list[dict[str, object]]]:
    lookup: dict[str, list[dict[str, object]]] = {}
    for _, event in events.iterrows():
        lookup.setdefault(str(event["session_id"]), []).append(event.to_dict())
    return lookup


def is_near_any_event(session_id: str, time_ms: float, event_lookup: dict[str, list[dict[str, object]]], exclude_ms: int) -> bool:
    for event in event_lookup.get(session_id, []):
        if abs(float(event["pc_timestamp_ms"]) - time_ms) <= exclude_ms:
            return True
    return False


def build_training_windows(
    synced: pd.DataFrame,
    positive_events: pd.DataFrame,
    all_events: pd.DataFrame,
    args: argparse.Namespace,
) -> tuple[pd.DataFrame, pd.Series, pd.Series, pd.DataFrame]:
    expected_features = build_expected_feature_names(RF_NODE_IDS)
    feature_rows: list[dict[str, float]] = []
    labels: list[str] = []
    groups: list[str] = []
    manifest_rows: list[dict[str, object]] = []

    for _, event in positive_events.iterrows():
        session_id = str(event["session_id"])
        event_time = float(event["pc_timestamp_ms"])
        event_type = str(event["event_type"])
        session_df = synced[synced["session_id"] == session_id].reset_index(drop=True)
        candidate_end = session_df[
            (session_df["sync_time_ms"] >= event_time + args.positive_start_ms)
            & (session_df["sync_time_ms"] <= event_time + args.positive_end_ms)
        ].index.to_list()
        candidate_end = candidate_end[:: max(1, args.positive_stride_frames)]

        for end_idx in candidate_end:
            start_idx = end_idx - args.window_size + 1
            if start_idx < 0:
                continue
            window = session_df.iloc[start_idx : end_idx + 1]
            if len(window) != args.window_size or not window_is_contiguous(window, args.align_ms):
                continue
            feature_rows.append(extract_window_features(window, RF_NODE_IDS))
            labels.append(event_type)
            groups.append(str(event["event_id"]))
            manifest_rows.append(
                {
                    "source": "event",
                    "event_id": str(event["event_id"]),
                    "label": event_type,
                    "session_id": session_id,
                    "window_start_ms": float(window["sync_time_ms"].iloc[0]),
                    "window_end_ms": float(window["sync_time_ms"].iloc[-1]),
                }
            )

    positive_count = len(feature_rows)

    event_lookup = build_event_lookup(all_events)
    include_state_labels = parse_filter_values(args.include_state_label)
    if positive_count == 0 and not include_state_labels:
        raise RuntimeError("no positive windows generated; inspect event timing and window parameters")
    if include_state_labels:
        state_candidates: list[tuple[str, int, pd.DataFrame, str]] = []
        for segment_id, segment_df in synced.groupby("segment_id", sort=False):
            segment_df = segment_df.reset_index(drop=True)
            if len(segment_df) < args.window_size:
                continue
            for end_idx in range(args.window_size - 1, len(segment_df), args.state_stride_frames):
                window = segment_df.iloc[end_idx - args.window_size + 1 : end_idx + 1]
                session_id = str(window["session_id"].iloc[0])
                end_time = float(window["sync_time_ms"].iloc[-1])
                if is_near_any_event(session_id, end_time, event_lookup, args.state_exclude_ms):
                    continue
                state_counts = window["state_label"].value_counts(normalize=True)
                if state_counts.empty:
                    continue
                state_label = str(state_counts.index[0])
                if state_label not in include_state_labels:
                    continue
                if float(state_counts.iloc[0]) < args.state_min_purity:
                    continue
                state_candidates.append((segment_id, end_idx, window, state_label))

        if args.max_state_windows_per_label > 0:
            rng = np.random.default_rng(args.random_state)
            selected_state_candidates: list[tuple[str, int, pd.DataFrame, str]] = []
            for state_label in sorted(include_state_labels):
                rows = [candidate for candidate in state_candidates if candidate[3] == state_label]
                if len(rows) > args.max_state_windows_per_label:
                    indices = rng.choice(len(rows), size=args.max_state_windows_per_label, replace=False)
                    rows = [rows[int(index)] for index in indices]
                selected_state_candidates.extend(rows)
        else:
            selected_state_candidates = state_candidates

        for segment_id, end_idx, window, state_label in selected_state_candidates:
            feature_rows.append(extract_window_features(window, RF_NODE_IDS))
            labels.append(state_label)
            groups.append(f"state_{segment_id}_{end_idx // max(1, args.group_chunk_frames):04d}")
            manifest_rows.append(
                {
                    "source": "state",
                    "event_id": "",
                    "label": state_label,
                    "session_id": str(window["session_id"].iloc[0]),
                    "window_start_ms": float(window["sync_time_ms"].iloc[0]),
                    "window_end_ms": float(window["sync_time_ms"].iloc[-1]),
                }
            )

    negative_target = int(round(positive_count * args.negative_ratio))
    negative_candidates: list[tuple[str, int, pd.DataFrame]] = []
    if negative_target > 0:
        for segment_id, segment_df in synced.groupby("segment_id", sort=False):
            segment_df = segment_df.reset_index(drop=True)
            if len(segment_df) < args.window_size:
                continue
            for end_idx in range(args.window_size - 1, len(segment_df), args.negative_stride_frames):
                window = segment_df.iloc[end_idx - args.window_size + 1 : end_idx + 1]
                session_id = str(window["session_id"].iloc[0])
                end_time = float(window["sync_time_ms"].iloc[-1])
                if is_near_any_event(session_id, end_time, event_lookup, args.negative_exclude_ms):
                    continue
                negative_candidates.append((segment_id, end_idx, window))

        if len(negative_candidates) < negative_target:
            print(f"[warn] negative candidates={len(negative_candidates)} below target={negative_target}; using all candidates")
            selected_negative = negative_candidates
        else:
            rng = np.random.default_rng(args.random_state)
            indices = rng.choice(len(negative_candidates), size=negative_target, replace=False)
            selected_negative = [negative_candidates[int(index)] for index in indices]
    else:
        selected_negative = []

    for neg_idx, (segment_id, end_idx, window) in enumerate(selected_negative, start=1):
        feature_rows.append(extract_window_features(window, RF_NODE_IDS))
        labels.append(args.negative_label)
        groups.append(f"neg_{segment_id}_{end_idx // max(1, args.group_chunk_frames):04d}")
        manifest_rows.append(
            {
                "source": "negative",
                "event_id": "",
                "label": args.negative_label,
                "session_id": str(window["session_id"].iloc[0]),
                "window_start_ms": float(window["sync_time_ms"].iloc[0]),
                "window_end_ms": float(window["sync_time_ms"].iloc[-1]),
            }
        )

    if not feature_rows:
        raise RuntimeError("no training windows generated; inspect filters and window parameters")

    X = pd.DataFrame(feature_rows).reindex(columns=expected_features)
    if X.isna().any().any():
        missing = X.columns[X.isna().any()].tolist()
        raise RuntimeError(f"generated feature matrix contains NaN values, first missing columns: {missing[:10]}")
    y = pd.Series(labels, name="label")
    group_series = pd.Series(groups, name="group")
    manifest = pd.DataFrame(manifest_rows)
    return X, y, group_series, manifest


def train_and_save(
    X: pd.DataFrame,
    y: pd.Series,
    groups: pd.Series,
    manifest: pd.DataFrame,
    args: argparse.Namespace,
) -> None:
    train_idx, test_idx = make_group_split(X, y, groups, test_size=args.test_size, random_state=args.random_state)
    X_train = X.iloc[train_idx]
    X_test = X.iloc[test_idx]
    y_train = y.iloc[train_idx]
    y_test = y.iloc[test_idx]

    print(f"[info] windows total={len(X)} train={len(X_train)} test={len(X_test)}")
    print(f"[info] label counts:\n{y.value_counts().to_string()}")
    print(f"[info] train label counts:\n{y_train.value_counts().to_string()}")
    print(f"[info] test label counts:\n{y_test.value_counts().to_string()}")

    clf = RandomForestClassifier(
        n_estimators=args.n_estimators,
        max_depth=args.max_depth,
        min_samples_leaf=args.min_samples_leaf,
        class_weight="balanced",
        random_state=args.random_state,
        n_jobs=-1,
    )
    clf.fit(X_train, y_train)

    y_pred = clf.predict(X_test)
    class_names = sorted(y.unique().tolist())
    cm = confusion_matrix(y_test, y_pred, labels=class_names)
    accuracy = accuracy_score(y_test, y_pred)
    report = classification_report(y_test, y_pred, labels=class_names, zero_division=0, output_dict=True)

    print("\n=== Metrics ===")
    print(f"accuracy: {accuracy:.4f}")
    print("\n=== Classification Report ===")
    print(classification_report(y_test, y_pred, labels=class_names, zero_division=0))
    print("\n=== Confusion Matrix ===")
    print(pd.DataFrame(cm, index=class_names, columns=class_names).to_string())

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    bundle = {
        "model": clf,
        "feature_names": list(X.columns),
        "class_names": class_names,
        "node_ids": RF_NODE_IDS,
        "config": {
            "task": "event_rf",
            "window_size": args.window_size,
            "align_ms": args.align_ms,
            "align_tolerance_ms": args.align_tolerance_ms,
            "positive_start_ms": args.positive_start_ms,
            "positive_end_ms": args.positive_end_ms,
            "include_state_label": args.include_state_label,
            "state_exclude_ms": args.state_exclude_ms,
            "negative_exclude_ms": args.negative_exclude_ms,
        },
    }
    model_path = output_dir / "rf_model.joblib"
    joblib.dump(bundle, model_path)
    print(f"[info] model saved: {model_path}")

    pd.DataFrame(cm, index=class_names, columns=class_names).to_csv(output_dir / "confusion_matrix.csv")
    manifest.to_csv(output_dir / "window_manifest.csv", index=False)
    importance_df = (
        pd.DataFrame({"feature": X.columns, "importance": clf.feature_importances_})
        .sort_values("importance", ascending=False)
        .reset_index(drop=True)
    )
    importance_df.to_csv(output_dir / "feature_importance.csv", index=False)

    summary = {
        "accuracy": accuracy,
        "class_names": class_names,
        "label_counts": y.value_counts().to_dict(),
        "train_count": int(len(X_train)),
        "test_count": int(len(X_test)),
        "feature_count": int(X.shape[1]),
        "model_path": str(model_path),
        "report": report,
    }
    (output_dir / "training_summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"[info] reports saved under: {output_dir}")


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Train firmware-compatible RF from IMU event labels")
    parser.add_argument("--samples", required=True)
    parser.add_argument("--events", required=True)
    parser.add_argument("--output-dir", default="model")

    parser.add_argument("--include-event-group", action="append", default=[])
    parser.add_argument("--exclude-event-group", action="append", default=[])
    parser.add_argument("--include-event-type", action="append", default=[])
    parser.add_argument("--exclude-event-type", action="append", default=[])
    parser.add_argument("--include-event-id", action="append", default=[])
    parser.add_argument("--exclude-event-id", action="append", default=[])

    parser.add_argument("--window-size", type=int, default=25)
    parser.add_argument("--align-ms", type=float, default=40.0)
    parser.add_argument("--align-tolerance-ms", type=float, default=25.0)
    parser.add_argument("--positive-start-ms", type=int, default=0)
    parser.add_argument("--positive-end-ms", type=int, default=500)
    parser.add_argument("--positive-stride-frames", type=int, default=2)
    parser.add_argument("--include-state-label", action="append", default=[])
    parser.add_argument("--state-stride-frames", type=int, default=5)
    parser.add_argument("--state-exclude-ms", type=int, default=3000)
    parser.add_argument("--state-min-purity", type=float, default=0.95)
    parser.add_argument("--max-state-windows-per-label", type=int, default=0)
    parser.add_argument("--negative-exclude-ms", type=int, default=3000)
    parser.add_argument("--negative-ratio", type=float, default=1.0)
    parser.add_argument("--negative-stride-frames", type=int, default=5)
    parser.add_argument("--negative-label", default="else")
    parser.add_argument("--group-chunk-frames", type=int, default=200)

    parser.add_argument("--test-size", type=float, default=0.25)
    parser.add_argument("--n-estimators", type=int, default=120)
    parser.add_argument("--max-depth", type=int, default=12)
    parser.add_argument("--min-samples-leaf", type=int, default=2)
    parser.add_argument("--random-state", type=int, default=42)
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()
    samples = load_samples(Path(args.samples))
    all_events = load_events(Path(args.events))
    positive_events = filter_positive_events(all_events, args)
    print(f"[info] source events={len(all_events)} selected positive events={len(positive_events)}")

    synced = prepare_synced_samples(samples, args)
    X, y, groups, manifest = build_training_windows(synced, positive_events, all_events, args)
    train_and_save(X, y, groups, manifest, args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
