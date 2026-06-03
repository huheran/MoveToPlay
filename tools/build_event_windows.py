#!/usr/bin/env python3
"""
Build event-centered IMU window features from samples.csv and events.csv.

The output is one row per training window, suitable for Random Forest, XGBoost,
SVM, or other tabular baselines.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

np = None
pd = None


BASE_CHANNELS = ["ax", "ay", "az", "gx", "gy", "gz"]
NORM_CHANNELS = ["acc_norm", "gyro_norm"]
FEATURE_CHANNELS = BASE_CHANNELS + NORM_CHANNELS
STATS = ["mean", "std", "max", "min", "ptp", "rms", "energy"]

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


def require_data_deps() -> None:
    global np, pd
    if np is not None and pd is not None:
        return
    try:
        import numpy as numpy_module
        import pandas as pandas_module
    except ImportError as exc:
        raise RuntimeError("missing dependency: install numpy and pandas") from exc
    np = numpy_module
    pd = pandas_module


@dataclass(frozen=True)
class WindowSpec:
    pre_ms: int
    post_ms: int


@dataclass(frozen=True)
class EventWindow:
    window_id: str
    label: str
    event_id: str
    event_group: str
    event_type: str
    start_time_ms: int
    end_time_ms: int
    original_event_time_ms: int
    refined_event_time_ms: int
    session_id: str


def normalize_event_group(value: str) -> str:
    text = str(value).strip()
    if text in {"", "nan", "None"}:
        return "none"
    if text in {"attack", "jump", "pause", "skill", "turn"}:
        return f"{text}_event"
    return text


def add_norm_columns(df: pd.DataFrame) -> pd.DataFrame:
    out = df.copy()
    out["acc_norm"] = np.sqrt(out["ax"] ** 2 + out["ay"] ** 2 + out["az"] ** 2)
    out["gyro_norm"] = np.sqrt(out["gx"] ** 2 + out["gy"] ** 2 + out["gz"] ** 2)
    return out


def load_samples(path: Path) -> pd.DataFrame:
    df = pd.read_csv(path)
    required = {"pc_timestamp_ms", "node_id", *BASE_CHANNELS, "session_id"}
    missing = sorted(required - set(df.columns))
    if missing:
        raise ValueError(f"samples CSV missing columns: {missing}")

    numeric_cols = ["pc_timestamp_ms", "board_timestamp_ms", "node_id", *BASE_CHANNELS]
    for col in numeric_cols:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce")
    df = df.dropna(subset=["pc_timestamp_ms", "node_id", *BASE_CHANNELS]).copy()
    df["pc_timestamp_ms"] = df["pc_timestamp_ms"].astype(np.int64)
    df["node_id"] = df["node_id"].astype(int)
    df["session_id"] = df["session_id"].astype(str)
    return add_norm_columns(df).sort_values(["session_id", "pc_timestamp_ms", "node_id"]).reset_index(drop=True)


def load_events(path: Path) -> pd.DataFrame:
    df = pd.read_csv(path)
    required = {"event_id", "event_type", "pc_timestamp_ms", "session_id"}
    missing = sorted(required - set(df.columns))
    if missing:
        raise ValueError(f"events CSV missing columns: {missing}")

    df["pc_timestamp_ms"] = pd.to_numeric(df["pc_timestamp_ms"], errors="coerce")
    df = df.dropna(subset=["pc_timestamp_ms"]).copy()
    df["pc_timestamp_ms"] = df["pc_timestamp_ms"].astype(np.int64)
    df["event_id"] = df["event_id"].astype(str)
    df["event_type"] = df["event_type"].astype(str)
    df["session_id"] = df["session_id"].astype(str)

    if "event_group" not in df.columns:
        df["event_group"] = df["event_type"].map(EVENT_TYPE_TO_GROUP).fillna("none")
    df["event_group"] = df["event_group"].map(normalize_event_group)
    return df.sort_values(["session_id", "pc_timestamp_ms"]).reset_index(drop=True)


def build_window_specs(args: argparse.Namespace) -> dict[str, WindowSpec]:
    return {
        "attack_event": WindowSpec(args.attack_pre_ms, args.attack_post_ms),
        "jump_event": WindowSpec(args.jump_pre_ms, args.jump_post_ms),
        "pause_event": WindowSpec(args.pause_pre_ms, args.pause_post_ms),
        "skill_event": WindowSpec(args.skill_pre_ms, args.skill_post_ms),
        "turn_event": WindowSpec(args.turn_pre_ms, args.turn_post_ms),
    }


def parse_filter_values(values: list[str]) -> set[str]:
    parsed: set[str] = set()
    for value in values:
        for item in value.split(","):
            text = item.strip()
            if text:
                parsed.add(text)
    return parsed


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


def refine_event_time(
    samples: pd.DataFrame,
    event_group: str,
    event_time_ms: int,
    radius_ms: int,
) -> int:
    if event_group == "pause_event":
        return event_time_ms

    start_ms = event_time_ms - radius_ms
    end_ms = event_time_ms + radius_ms
    nearby = samples[(samples["pc_timestamp_ms"] >= start_ms) & (samples["pc_timestamp_ms"] <= end_ms)]
    if nearby.empty:
        return event_time_ms

    if event_group in {"attack_event", "skill_event", "turn_event"}:
        score = nearby["gyro_norm"]
    elif event_group == "jump_event":
        score = np.maximum(nearby["acc_norm"].to_numpy(), nearby["gyro_norm"].to_numpy())
        score = pd.Series(score, index=nearby.index)
    else:
        return event_time_ms

    best_idx = int(score.idxmax())
    return int(nearby.loc[best_idx, "pc_timestamp_ms"])


def stat_values(values: np.ndarray) -> dict[str, float]:
    if values.size == 0:
        return {name: np.nan for name in STATS}
    return {
        "mean": float(np.mean(values)),
        "std": float(np.std(values, ddof=0)),
        "max": float(np.max(values)),
        "min": float(np.min(values)),
        "ptp": float(np.ptp(values)),
        "rms": float(np.sqrt(np.mean(values ** 2))),
        "energy": float(np.sum(values ** 2)),
    }


def extract_features(window_df: pd.DataFrame) -> dict[str, float]:
    features: dict[str, float] = {
        "sample_count": float(len(window_df)),
        "node_count": float(window_df["node_id"].nunique()) if not window_df.empty else 0.0,
    }
    node_ids = sorted(int(node_id) for node_id in window_df["node_id"].dropna().unique())

    for node_id in node_ids:
        node_df = window_df[window_df["node_id"] == node_id]
        features[f"n{node_id}_sample_count"] = float(len(node_df))
        for channel in FEATURE_CHANNELS:
            values = node_df[channel].dropna().to_numpy(dtype=float)
            for stat_name, stat_value in stat_values(values).items():
                features[f"n{node_id}_{channel}_{stat_name}"] = stat_value

    for left, right in pairwise(node_ids):
        for channel in ["acc_norm", "gyro_norm"]:
            for stat_name in ["mean", "max"]:
                left_key = f"n{left}_{channel}_{stat_name}"
                right_key = f"n{right}_{channel}_{stat_name}"
                if left_key in features and right_key in features:
                    features[f"n{left}_minus_n{right}_{channel}_{stat_name}"] = (
                        features[left_key] - features[right_key]
                    )

    return features


def pairwise(items: list[int]) -> Iterable[tuple[int, int]]:
    for i, left in enumerate(items):
        for right in items[i + 1 :]:
            yield left, right


def slice_window(samples: pd.DataFrame, session_id: str, start_ms: int, end_ms: int) -> pd.DataFrame:
    return samples[
        (samples["session_id"] == session_id)
        & (samples["pc_timestamp_ms"] >= start_ms)
        & (samples["pc_timestamp_ms"] <= end_ms)
    ]


def choose_label(event_group: str, event_type: str, label_source: str) -> str:
    if label_source == "event_type":
        return event_type
    return event_group


def build_positive_windows(
    samples: pd.DataFrame,
    events: pd.DataFrame,
    specs: dict[str, WindowSpec],
    args: argparse.Namespace,
) -> list[EventWindow]:
    windows: list[EventWindow] = []
    for index, event in events.iterrows():
        event_group = normalize_event_group(event["event_group"])
        if event_group == "none":
            continue
        if event_group not in specs:
            print(f"[warn] skip event_id={event['event_id']}: unsupported event_group={event_group}")
            continue

        session_id = str(event["session_id"])
        session_samples = samples[samples["session_id"] == session_id]
        original_time = int(event["pc_timestamp_ms"])
        refined_time = original_time
        if args.refine_event_center:
            refined_time = refine_event_time(session_samples, event_group, original_time, args.refine_radius_ms)

        spec = specs[event_group]
        start_ms = refined_time - spec.pre_ms
        end_ms = refined_time + spec.post_ms
        event_type = str(event["event_type"])
        label = choose_label(event_group, event_type, args.label_source)
        windows.append(
            EventWindow(
                window_id=f"event_{index + 1:05d}",
                label=label,
                event_id=str(event["event_id"]),
                event_group=event_group,
                event_type=event_type,
                start_time_ms=start_ms,
                end_time_ms=end_ms,
                original_event_time_ms=original_time,
                refined_event_time_ms=refined_time,
                session_id=session_id,
            )
        )
    return windows


def make_exclusion_intervals(events: pd.DataFrame, exclude_ms: int) -> dict[str, list[tuple[int, int]]]:
    intervals: dict[str, list[tuple[int, int]]] = {}
    for _, event in events.iterrows():
        session_id = str(event["session_id"])
        event_time = int(event["pc_timestamp_ms"])
        intervals.setdefault(session_id, []).append((event_time - exclude_ms, event_time + exclude_ms))
    for session_id, session_intervals in intervals.items():
        intervals[session_id] = merge_intervals(session_intervals)
    return intervals


def merge_intervals(intervals: list[tuple[int, int]]) -> list[tuple[int, int]]:
    if not intervals:
        return []
    sorted_intervals = sorted(intervals)
    merged = [sorted_intervals[0]]
    for start, end in sorted_intervals[1:]:
        last_start, last_end = merged[-1]
        if start <= last_end:
            merged[-1] = (last_start, max(last_end, end))
        else:
            merged.append((start, end))
    return merged


def overlaps_any(start_ms: int, end_ms: int, intervals: list[tuple[int, int]]) -> bool:
    for blocked_start, blocked_end in intervals:
        if start_ms <= blocked_end and end_ms >= blocked_start:
            return True
    return False


def majority_state_label(window_df: pd.DataFrame, default_label: str) -> str:
    if "state_label" not in window_df.columns or window_df.empty:
        return default_label
    counts = window_df["state_label"].dropna().astype(str).value_counts()
    if counts.empty:
        return default_label
    return str(counts.index[0])


def build_negative_windows(
    samples: pd.DataFrame,
    events: pd.DataFrame,
    positive_count: int,
    args: argparse.Namespace,
) -> list[EventWindow]:
    target_count = int(round(positive_count * args.negative_ratio))
    if target_count <= 0:
        return []

    rng = np.random.default_rng(args.random_seed)
    exclusions = make_exclusion_intervals(events, args.negative_exclude_ms)
    session_ranges = (
        samples.groupby("session_id")["pc_timestamp_ms"]
        .agg(["min", "max"])
        .reset_index()
        .to_dict("records")
    )
    if not session_ranges:
        return []

    windows: list[EventWindow] = []
    attempts = 0
    max_attempts = max(args.negative_max_attempts, target_count * 50)
    while len(windows) < target_count and attempts < max_attempts:
        attempts += 1
        session_info = session_ranges[int(rng.integers(0, len(session_ranges)))]
        session_id = str(session_info["session_id"])
        min_ts = int(session_info["min"])
        max_ts = int(session_info["max"])
        if max_ts - min_ts < args.negative_window_ms:
            continue

        start_ms = int(rng.integers(min_ts, max_ts - args.negative_window_ms + 1))
        end_ms = start_ms + args.negative_window_ms
        if overlaps_any(start_ms, end_ms, exclusions.get(session_id, [])):
            continue

        window_df = slice_window(samples, session_id, start_ms, end_ms)
        if len(window_df) < args.min_samples_per_window:
            continue

        label = "else"
        if args.negative_label_mode == "state":
            label = majority_state_label(window_df, default_label="else")

        window_index = len(windows) + 1
        windows.append(
            EventWindow(
                window_id=f"negative_{window_index:05d}",
                label=label,
                event_id="",
                event_group="none",
                event_type="none",
                start_time_ms=start_ms,
                end_time_ms=end_ms,
                original_event_time_ms=-1,
                refined_event_time_ms=-1,
                session_id=session_id,
            )
        )

    if len(windows) < target_count:
        print(f"[warn] generated {len(windows)} negative windows, target was {target_count}")
    return windows


def materialize_windows(samples: pd.DataFrame, windows: list[EventWindow], args: argparse.Namespace) -> pd.DataFrame:
    rows: list[dict[str, object]] = []
    for window in windows:
        window_df = slice_window(samples, window.session_id, window.start_time_ms, window.end_time_ms)
        if len(window_df) < args.min_samples_per_window:
            print(f"[warn] skip {window.window_id}: only {len(window_df)} samples")
            continue

        row: dict[str, object] = {
            "window_id": window.window_id,
            "label": window.label,
            "event_id": window.event_id,
            "event_group": window.event_group,
            "event_type": window.event_type,
            "start_time_ms": window.start_time_ms,
            "end_time_ms": window.end_time_ms,
            "original_event_time_ms": window.original_event_time_ms,
            "refined_event_time_ms": window.refined_event_time_ms,
            "session_id": window.session_id,
        }
        row.update(extract_features(window_df))
        rows.append(row)

    return pd.DataFrame(rows)


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Build event-centered IMU window feature CSV")
    parser.add_argument("--samples", required=True, help="Input samples CSV")
    parser.add_argument("--events", required=True, help="Input events CSV")
    parser.add_argument("--output", required=True, help="Output window feature CSV")

    parser.add_argument("--attack-pre-ms", type=int, default=300)
    parser.add_argument("--attack-post-ms", type=int, default=700)
    parser.add_argument("--jump-pre-ms", type=int, default=500)
    parser.add_argument("--jump-post-ms", type=int, default=1000)
    parser.add_argument("--pause-pre-ms", type=int, default=700)
    parser.add_argument("--pause-post-ms", type=int, default=1200)
    parser.add_argument("--skill-pre-ms", type=int, default=500)
    parser.add_argument("--skill-post-ms", type=int, default=1200)
    parser.add_argument("--turn-pre-ms", type=int, default=500)
    parser.add_argument("--turn-post-ms", type=int, default=1000)

    parser.add_argument("--negative-ratio", type=float, default=1.0, help="Negative windows per positive window")
    parser.add_argument("--negative-window-ms", type=int, default=1000)
    parser.add_argument("--negative-exclude-ms", type=int, default=1500)
    parser.add_argument("--negative-max-attempts", type=int, default=10000)
    parser.add_argument("--negative-label-mode", choices=["else", "state"], default="else")
    parser.add_argument("--random-seed", type=int, default=42)

    parser.add_argument("--label-source", choices=["event_group", "event_type"], default="event_group")
    parser.add_argument(
        "--include-event-group",
        action="append",
        default=[],
        help="Only build positive windows for these event groups. Accepts comma-separated values and can repeat.",
    )
    parser.add_argument(
        "--exclude-event-group",
        action="append",
        default=[],
        help="Skip positive windows for these event groups. Accepts comma-separated values and can repeat.",
    )
    parser.add_argument(
        "--include-event-type",
        action="append",
        default=[],
        help="Only build positive windows for these event types. Accepts comma-separated values and can repeat.",
    )
    parser.add_argument(
        "--exclude-event-type",
        action="append",
        default=[],
        help="Skip positive windows for these event types. Accepts comma-separated values and can repeat.",
    )
    parser.add_argument(
        "--include-event-id",
        action="append",
        default=[],
        help="Only build positive windows for these event IDs. Accepts comma-separated values and can repeat.",
    )
    parser.add_argument(
        "--exclude-event-id",
        action="append",
        default=[],
        help="Skip positive windows for these event IDs. They are still excluded from negative windows.",
    )
    parser.add_argument("--min-samples-per-window", type=int, default=1)
    parser.add_argument("--refine-event-center", action="store_true")
    parser.add_argument("--refine-radius-ms", type=int, default=300)
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()
    try:
        require_data_deps()
    except RuntimeError as exc:
        print(f"[error] {exc}")
        print("[hint] install dependencies: python -m pip install numpy pandas")
        return 1
    samples = load_samples(Path(args.samples))
    events = load_events(Path(args.events))
    positive_events = filter_positive_events(events, args)
    specs = build_window_specs(args)

    positive_windows = build_positive_windows(samples, positive_events, specs, args)
    negative_windows = build_negative_windows(samples, events, len(positive_windows), args)
    all_windows = positive_windows + negative_windows
    output_df = materialize_windows(samples, all_windows, args)

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_df.to_csv(output_path, index=False)
    print(f"[info] source events={len(events)}")
    print(f"[info] selected positive events={len(positive_events)}")
    print(f"[info] positive windows={len(positive_windows)}")
    print(f"[info] negative windows={len(negative_windows)}")
    print(f"[info] written rows={len(output_df)}")
    print(f"[info] output={output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
