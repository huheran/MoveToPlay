#!/usr/bin/env python3
"""Export the trained scikit-learn RandomForest model to compact C arrays."""

from __future__ import annotations

import argparse
import itertools
import json
import warnings
from pathlib import Path

import joblib
import numpy as np

BASE_CHANNELS = ["ax", "ay", "az", "gx", "gy", "gz"]
NORM_CHANNELS = ["acc_norm", "gyro_norm"]
ALL_CHANNELS = BASE_CHANNELS + NORM_CHANNELS
RF_NODE_IDS = [1, 2, 3, 4]


def compute_channel_features(x: list[float], prefix: str) -> dict[str, float]:
    values = np.asarray(x, dtype=float)
    feats = {
        f"{prefix}_mean": float(np.mean(values)),
        f"{prefix}_std": float(np.std(values)),
        f"{prefix}_max": float(np.max(values)),
        f"{prefix}_min": float(np.min(values)),
        f"{prefix}_ptp": float(np.ptp(values)),
        f"{prefix}_rms": float(np.sqrt(np.mean(np.square(values)))),
        f"{prefix}_energy": float(np.mean(np.square(values))),
    }

    if len(values) > 1:
        dx = np.diff(values)
        feats[f"{prefix}_jerk_mean_abs"] = float(np.mean(np.abs(dx)))
        feats[f"{prefix}_jerk_std"] = float(np.std(dx))
        feats[f"{prefix}_jerk_rms"] = float(np.sqrt(np.mean(np.square(dx))))
    else:
        feats[f"{prefix}_jerk_mean_abs"] = 0.0
        feats[f"{prefix}_jerk_std"] = 0.0
        feats[f"{prefix}_jerk_rms"] = 0.0
    return feats


def build_expected_feature_names(node_ids: list[int]) -> list[str]:
    names: list[str] = []
    sample = [0.0, 1.0, 2.0]

    for node_id in node_ids:
        prefix = f"n{node_id}"
        for channel in ALL_CHANNELS:
            names.extend(compute_channel_features(sample, f"{prefix}_{channel}").keys())

    for node_a, node_b in itertools.combinations(node_ids, 2):
        for channel in ALL_CHANNELS:
            names.extend(compute_channel_features(sample, f"pair_{node_a}_{node_b}_{channel}_diff").keys())
        for channel in ["acc_norm", "gyro_norm"]:
            names.append(f"pair_{node_a}_{node_b}_{channel}_corr")

    return names


def c_array_u16(name: str, values: list[int], per_line: int = 16) -> str:
    lines = [f"const uint16_t {name}[] = {{"]
    for i in range(0, len(values), per_line):
        chunk = ", ".join(str(v) for v in values[i : i + per_line])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines)


def c_array_i16(name: str, values: list[int], per_line: int = 16) -> str:
    lines = [f"const int16_t {name}[] = {{"]
    for i in range(0, len(values), per_line):
        chunk = ", ".join(str(v) for v in values[i : i + per_line])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines)


def c_array_u8(name: str, values: list[int], per_line: int = 24) -> str:
    lines = [f"const uint8_t {name}[] = {{"]
    for i in range(0, len(values), per_line):
        chunk = ", ".join(str(v) for v in values[i : i + per_line])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines)


def c_array_float(name: str, values: list[float], per_line: int = 6) -> str:
    def float_literal(value: float) -> str:
        text = f"{value:.9g}"
        if "e" not in text.lower() and "." not in text:
            text += ".0"
        return f"{text}f"

    lines = [f"const float {name}[] = {{"]
    for i in range(0, len(values), per_line):
        chunk = ", ".join(float_literal(v) for v in values[i : i + per_line])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines)


def c_string_array(name: str, values: list[str]) -> str:
    escaped = [v.replace("\\", "\\\\").replace('"', '\\"') for v in values]
    lines = [f"const char *const {name}[] = {{"]
    lines.extend(f'    "{v}",' for v in escaped)
    lines.append("};")
    return "\n".join(lines)


def write_text_lf(path: Path, text: str) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def export_model(bundle_path: Path, output_dir: Path, file_prefix: str, symbol_prefix: str) -> None:
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        bundle = joblib.load(bundle_path)

    model = bundle["model"]
    feature_names: list[str] = list(bundle["feature_names"])
    class_names: list[str] = list(model.classes_)
    node_ids: list[int] = list(bundle["node_ids"])

    if node_ids != RF_NODE_IDS:
        raise ValueError(f"model node_ids={node_ids} does not match expected {RF_NODE_IDS}")

    expected_feature_names = build_expected_feature_names(node_ids)
    if feature_names != expected_feature_names:
        raise ValueError("feature order does not match firmware feature generator")

    tree_offsets: list[int] = [0]
    children_left: list[int] = []
    children_right: list[int] = []
    features: list[int] = []
    thresholds: list[float] = []
    leaf_classes: list[int] = []

    for estimator in model.estimators_:
        tree = estimator.tree_
        node_count = int(tree.node_count)
        for node_index in range(node_count):
            left = int(tree.children_left[node_index])
            right = int(tree.children_right[node_index])
            feat = int(tree.feature[node_index])
            threshold = float(tree.threshold[node_index])
            values = tree.value[node_index][0]
            leaf_class = int(values.argmax())

            children_left.append(left)
            children_right.append(right)
            features.append(feat)
            thresholds.append(threshold)
            leaf_classes.append(leaf_class)
        tree_offsets.append(len(children_left))

    output_dir.mkdir(parents=True, exist_ok=True)

    macro_prefix = symbol_prefix.upper()
    header_name = f"{file_prefix}.h"
    source_name = f"{file_prefix}.c"
    header_path = output_dir / header_name
    source_path = output_dir / source_name

    write_text_lf(
        header_path,
        "\n".join(
            [
                "#pragma once",
                "",
                "#include <stdint.h>",
                "",
                f"#define {macro_prefix}_TREE_COUNT {len(model.estimators_)}",
                f"#define {macro_prefix}_NODE_COUNT {len(children_left)}",
                f"#define {macro_prefix}_FEATURE_COUNT {len(feature_names)}",
                f"#define {macro_prefix}_CLASS_COUNT {len(class_names)}",
                "",
                f"extern const uint16_t {symbol_prefix}_tree_offsets[{macro_prefix}_TREE_COUNT + 1];",
                f"extern const int16_t {symbol_prefix}_children_left[{macro_prefix}_NODE_COUNT];",
                f"extern const int16_t {symbol_prefix}_children_right[{macro_prefix}_NODE_COUNT];",
                f"extern const int16_t {symbol_prefix}_features[{macro_prefix}_NODE_COUNT];",
                f"extern const float {symbol_prefix}_thresholds[{macro_prefix}_NODE_COUNT];",
                f"extern const uint8_t {symbol_prefix}_leaf_classes[{macro_prefix}_NODE_COUNT];",
                f"extern const char *const {symbol_prefix}_class_names[{macro_prefix}_CLASS_COUNT];",
                "",
            ]
        )
    )

    write_text_lf(
        source_path,
        "\n\n".join(
            [
                f'#include "{header_name}"',
                "",
                c_array_u16(f"{symbol_prefix}_tree_offsets", tree_offsets),
                c_array_i16(f"{symbol_prefix}_children_left", children_left),
                c_array_i16(f"{symbol_prefix}_children_right", children_right),
                c_array_i16(f"{symbol_prefix}_features", features),
                c_array_float(f"{symbol_prefix}_thresholds", thresholds),
                c_array_u8(f"{symbol_prefix}_leaf_classes", leaf_classes),
                c_string_array(f"{symbol_prefix}_class_names", class_names),
                "",
            ]
        )
    )

    summary = {
        "tree_count": len(model.estimators_),
        "node_count": len(children_left),
        "feature_count": len(feature_names),
        "class_count": len(class_names),
        "class_names": class_names,
        "node_ids": node_ids,
    }
    write_text_lf(
        output_dir / f"{file_prefix}_summary.json",
        json.dumps(summary, ensure_ascii=False, indent=2),
    )
    print(json.dumps(summary, ensure_ascii=False, indent=2))


def main() -> int:
    parser = argparse.ArgumentParser(description="Export RF model bundle to firmware C arrays")
    parser.add_argument("--model", default="model/rf_model.joblib")
    parser.add_argument("--output-dir", default="main/generated")
    parser.add_argument("--file-prefix", default="rf_model_generated")
    parser.add_argument("--symbol-prefix", default="rf_model")
    args = parser.parse_args()

    export_model(Path(args.model), Path(args.output_dir), args.file_prefix, args.symbol_prefix)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
