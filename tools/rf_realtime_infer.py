#!/usr/bin/env python3
"""
Real-time IMU action inference using a trained Random Forest model bundle.

Input serial format:
    timestamp_ms,node_id,ax,ay,az,gx,gy,gz
"""

from __future__ import annotations

import argparse
import sys
import time
from collections import deque
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import Optional

import joblib
import numpy as np
import pandas as pd
import serial

THIS_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = THIS_DIR.parent
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from tools.rf_baseline import (
    BASE_CHANNELS,
    RF_NODE_IDS,
    RF_NODE_NAMES,
    add_norm_features,
    extract_window_features,
)


@dataclass
class SerialPacket:
    pc_timestamp_ms: float
    board_timestamp_ms: float
    node_id: int
    ax: float
    ay: float
    az: float
    gx: float
    gy: float
    gz: float


class ParseStatus(str, Enum):
    OK = "ok"
    SKIP = "skip"
    MALFORMED = "malformed"


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Real-time IMU action inference with Random Forest")
    parser.add_argument("--port", required=True, help="Serial port, e.g. COM12 or /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate")
    parser.add_argument("--model", default="output/rf_model.joblib", help="Path to saved model bundle")
    parser.add_argument("--serial-timeout", type=float, default=0.05, help="Serial read timeout in seconds")
    parser.add_argument("--predict-interval-ms", type=float, default=120.0, help="Minimum interval between predictions")
    parser.add_argument("--min-confidence", type=float, default=0.45, help="Confidence threshold below which output becomes uncertain")
    parser.add_argument("--history-factor", type=float, default=4.0, help="Keep roughly window_size * factor frames in rolling history")
    return parser


def parse_line(line: str, pc_timestamp_ms: float) -> tuple[ParseStatus, Optional[SerialPacket]]:
    text = line.strip()
    if not text:
        return ParseStatus.SKIP, None

    if text.startswith(("#", "[", "I (", "W (", "E (", "D (", "V (")):
        return ParseStatus.SKIP, None

    try:
        parts = [part.strip() for part in text.split(",")]
        if len(parts) < 8:
            return ParseStatus.MALFORMED, None
        packet = SerialPacket(
            pc_timestamp_ms=pc_timestamp_ms,
            board_timestamp_ms=float(parts[0]),
            node_id=int(parts[1]),
            ax=float(parts[2]),
            ay=float(parts[3]),
            az=float(parts[4]),
            gx=float(parts[5]),
            gy=float(parts[6]),
            gz=float(parts[7]),
        )
        return ParseStatus.OK, packet
    except ValueError:
        return ParseStatus.MALFORMED, None


def packets_to_synced_frame_df(
    packets: list[SerialPacket],
    node_ids: list[int],
    align_ms: float,
    tolerance_ms: float,
) -> pd.DataFrame:
    if not packets:
        return pd.DataFrame()

    df = pd.DataFrame([packet.__dict__ for packet in packets]).sort_values("pc_timestamp_ms").reset_index(drop=True)
    start_ms = float(df["pc_timestamp_ms"].min())
    end_ms = float(df["pc_timestamp_ms"].max())
    if end_ms < start_ms:
        return pd.DataFrame()

    grid = pd.DataFrame({"sync_time_ms": np.arange(start_ms, end_ms + align_ms, align_ms, dtype=float)})

    for node_id in node_ids:
        node_df = (
            df[df["node_id"] == node_id]
            .groupby("pc_timestamp_ms", as_index=False)[["board_timestamp_ms"] + BASE_CHANNELS]
            .mean()
            .rename(columns={"pc_timestamp_ms": "event_time_ms"})
            .sort_values("event_time_ms")
        )
        if node_df.empty:
            continue

        merged = pd.merge_asof(
            grid[["sync_time_ms"]],
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
    for col in essential_cols:
        if col not in grid.columns:
            grid[col] = np.nan
    grid = grid.dropna(subset=essential_cols).reset_index(drop=True)
    if grid.empty:
        return grid
    grid = add_norm_features(grid, node_ids=node_ids)
    return grid


def main() -> int:
    args = build_arg_parser().parse_args()

    bundle = joblib.load(args.model)
    model = bundle["model"]
    feature_names: list[str] = bundle["feature_names"]
    class_names: list[str] = bundle["class_names"]
    node_ids: list[int] = bundle["node_ids"]
    if node_ids != RF_NODE_IDS:
        raise ValueError(
            f"model node_ids={node_ids} does not match required RF node_ids={RF_NODE_IDS}"
        )
    config = bundle["config"]

    window_size = int(config["window_size"])
    align_ms = float(config["align_ms"])
    tolerance_ms = float(config["align_tolerance_ms"])
    max_packets = max(int(window_size * max(2.0, args.history_factor) * max(1, len(node_ids))), 200)

    print(f"[info] loading model: {args.model}")
    print(
        f"[info] classes={class_names} node_ids={node_ids} "
        f"names={[RF_NODE_NAMES[node_id] for node_id in node_ids]}"
    )
    print(f"[info] window_size={window_size} align_ms={align_ms} tolerance_ms={tolerance_ms}")

    try:
        ser = serial.Serial(args.port, args.baud, timeout=args.serial_timeout)
    except serial.SerialException as exc:
        print(f"[error] failed to open serial port: {exc}")
        return 1

    packets: deque[SerialPacket] = deque(maxlen=max_packets)
    last_predict_ms = 0.0
    valid_lines = 0
    bad_lines = 0
    skipped_lines = 0

    print("[info] real-time inference started, press Ctrl+C to stop")
    try:
        while True:
            raw_line = ser.readline().decode("utf-8", errors="ignore").strip()
            now_ms = time.time() * 1000.0
            if not raw_line:
                continue

            status, packet = parse_line(raw_line, now_ms)
            if status is ParseStatus.SKIP:
                skipped_lines += 1
                continue

            if status is ParseStatus.MALFORMED or packet is None:
                bad_lines += 1
                if bad_lines % 20 == 0:
                    print(f"[warn] ignored malformed lines: {bad_lines}")
                continue

            valid_lines += 1
            packets.append(packet)

            if (now_ms - last_predict_ms) < args.predict_interval_ms:
                continue

            synced_df = packets_to_synced_frame_df(list(packets), node_ids=node_ids, align_ms=align_ms, tolerance_ms=tolerance_ms)
            if len(synced_df) < window_size:
                continue

            window_df = synced_df.iloc[-window_size:].copy()
            features = extract_window_features(window_df, node_ids=node_ids)
            X = pd.DataFrame([features])
            X = X.reindex(columns=feature_names, fill_value=0.0)

            proba = model.predict_proba(X)[0]
            pred_idx = int(np.argmax(proba))
            pred_label = model.classes_[pred_idx]
            confidence = float(proba[pred_idx])
            display_label = pred_label if confidence >= args.min_confidence else "uncertain"

            class_prob_text = " ".join(
                f"{label}={prob:.2f}" for label, prob in sorted(zip(model.classes_, proba), key=lambda x: x[0])
            )
            print(
                f"\rpred={display_label:<10s} raw={pred_label:<10s} conf={confidence:.2f} "
                f"frames={len(synced_df):<4d} valid={valid_lines:<6d} bad={bad_lines:<4d} skip={skipped_lines:<4d} "
                f"{class_prob_text}   ",
                end="",
                flush=True,
            )
            last_predict_ms = now_ms

    except KeyboardInterrupt:
        print("\n[info] stopping inference")
    finally:
        ser.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
