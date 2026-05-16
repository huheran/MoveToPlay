#!/usr/bin/env python3
"""Export trained 1D CNN model to INT8 quantized C arrays for ESP32-S3.

Workflow:
1. Load float32 model (with BN fused into Conv)
2. Run calibration data through the model to collect activation ranges
3. Quantize weights per-channel (symmetric, int8)
4. Export: int8 weight arrays + float32 scale/bias arrays
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn

import sys
THIS_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = THIS_DIR.parent
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

NUM_CHANNELS = 24
WINDOW_SIZE = 25


class IMU1DCNN(nn.Module):
    def __init__(self, num_channels: int, num_classes: int, window_size: int = 25):
        super().__init__()
        self.conv1 = nn.Conv1d(num_channels, 32, kernel_size=5, padding=2, dilation=1)
        self.bn1 = nn.BatchNorm1d(32)
        self.conv2 = nn.Conv1d(32, 64, kernel_size=5, padding=4, dilation=2)
        self.bn2 = nn.BatchNorm1d(64)
        self.conv3 = nn.Conv1d(64, 64, kernel_size=3, padding=4, dilation=4)
        self.bn3 = nn.BatchNorm1d(64)
        self.pool = nn.AdaptiveAvgPool1d(1)
        self.dropout = nn.Dropout(0.3)
        self.fc = nn.Linear(64, num_classes)

    def forward(self, x):
        x = torch.relu(self.bn1(self.conv1(x)))
        x = torch.relu(self.bn2(self.conv2(x)))
        x = torch.relu(self.bn3(self.conv3(x)))
        x = self.pool(x).squeeze(-1)
        return self.fc(x)


def fuse_conv_bn(conv: nn.Conv1d, bn: nn.BatchNorm1d):
    w = conv.weight.data
    b = conv.bias.data if conv.bias is not None else torch.zeros(conv.out_channels)
    gamma = bn.weight.data
    beta = bn.bias.data
    mean = bn.running_mean
    var = bn.running_var
    eps = bn.eps
    inv_std = gamma / torch.sqrt(var + eps)
    w_fused = w * inv_std.view(-1, 1, 1)
    b_fused = (b - mean) * inv_std + beta
    return w_fused.numpy(), b_fused.numpy()


def quantize_weights_per_channel(w: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Per-channel symmetric quantization of weights.
    w shape: (out_ch, in_ch, kernel)
    Returns: (w_int8, w_scale) where w_scale has shape (out_ch,)
    """
    out_ch = w.shape[0]
    w_flat = w.reshape(out_ch, -1)
    w_max = np.max(np.abs(w_flat), axis=1)
    w_max = np.maximum(w_max, 1e-8)
    w_scale = w_max / 127.0
    w_int8 = np.round(w / w_scale.reshape(-1, 1, 1)).astype(np.int8)
    return w_int8, w_scale.astype(np.float32)


def collect_activation_ranges(model, norm_mean, norm_std, calib_data):
    """Run calibration data and collect max abs activation per layer."""
    model.eval()
    ranges = {"input": 0.0, "conv1_out": 0.0, "conv2_out": 0.0, "conv3_out": 0.0}

    mean_t = torch.FloatTensor(norm_mean).reshape(1, -1, 1)
    std_t = torch.FloatTensor(norm_std).reshape(1, -1, 1)

    with torch.no_grad():
        for i in range(0, len(calib_data), 32):
            batch = torch.FloatTensor(calib_data[i:i+32])
            x = (batch - mean_t) / std_t
            ranges["input"] = max(ranges["input"], x.abs().max().item())

            x = torch.relu(model.bn1(model.conv1(x)))
            ranges["conv1_out"] = max(ranges["conv1_out"], x.abs().max().item())

            x = torch.relu(model.bn2(model.conv2(x)))
            ranges["conv2_out"] = max(ranges["conv2_out"], x.abs().max().item())

            x = torch.relu(model.bn3(model.conv3(x)))
            ranges["conv3_out"] = max(ranges["conv3_out"], x.abs().max().item())

    return ranges


def c_array_int8(name: str, values: np.ndarray, per_line: int = 16) -> str:
    flat = values.flatten()
    lines = [f"const int8_t {name}[{len(flat)}] = {{"]
    for i in range(0, len(flat), per_line):
        chunk = ", ".join(str(v) for v in flat[i:i + per_line])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines)


def c_array_float(name: str, values: np.ndarray, per_line: int = 8) -> str:
    flat = values.flatten()
    lines = [f"const float {name}[{len(flat)}] = {{"]
    for i in range(0, len(flat), per_line):
        chunk = ", ".join(f"{v:.8g}f" for v in flat[i:i + per_line])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines)


# --- PLACEHOLDER_EXPORT ---


def export_int8_model(model_pt_path: Path, meta_path: Path, norm_path: Path,
                      calib_data_path: Path, output_dir: Path, num_calib: int = 200):
    with open(meta_path) as f:
        meta = json.load(f)
    with open(norm_path) as f:
        norm = json.load(f)

    num_classes = len(meta["class_names"])
    model = IMU1DCNN(NUM_CHANNELS, num_classes, WINDOW_SIZE)
    model.load_state_dict(torch.load(model_pt_path, map_location="cpu", weights_only=True))
    model.eval()

    norm_mean = np.array(norm["mean"], dtype=np.float32)
    norm_std = np.array(norm["std"], dtype=np.float32)

    # Load calibration data
    import pandas as pd
    from tools.cnn1d_train import load_data, align_session, build_windows, BASE_CHANNELS, NODE_IDS
    from tools.action_labels import MULTICLASS_LABEL_ORDER

    df = load_data(str(calib_data_path))
    sessions = []
    for _, session_df in df.groupby("session_id"):
        aligned = align_session(session_df, 40.0, 25.0)
        if len(aligned) > 0:
            sessions.append(aligned)
    sync_df = pd.concat(sessions, ignore_index=True)
    X, _, _ = build_windows(sync_df, WINDOW_SIZE, 10, 200)

    np.random.seed(42)
    indices = np.random.choice(len(X), min(num_calib, len(X)), replace=False)
    calib_X = X[indices]
    print(f"Calibration samples: {len(calib_X)}")

    # Collect activation ranges
    act_ranges = collect_activation_ranges(model, norm_mean, norm_std, calib_X)
    print(f"Activation ranges: {act_ranges}")

    # Compute activation scales
    input_scale = float(act_ranges["input"]) / 127.0
    conv1_out_scale = float(act_ranges["conv1_out"]) / 127.0
    conv2_out_scale = float(act_ranges["conv2_out"]) / 127.0
    conv3_out_scale = float(act_ranges["conv3_out"]) / 127.0

    # Fuse BN and quantize weights
    w1, b1 = fuse_conv_bn(model.conv1, model.bn1)
    w2, b2 = fuse_conv_bn(model.conv2, model.bn2)
    w3, b3 = fuse_conv_bn(model.conv3, model.bn3)

    w1_int8, w1_scale = quantize_weights_per_channel(w1)
    w2_int8, w2_scale = quantize_weights_per_channel(w2)
    w3_int8, w3_scale = quantize_weights_per_channel(w3)

    # FC weights stay float32 (tiny layer)
    fc_w = model.fc.weight.data.numpy()
    fc_b = model.fc.bias.data.numpy()

    norm_inv_std = (1.0 / norm_std).astype(np.float32)

    output_dir.mkdir(parents=True, exist_ok=True)

    # --- PLACEHOLDER_HEADER ---

    conv1_kernel = model.conv1.kernel_size[0]
    conv2_kernel = model.conv2.kernel_size[0]
    conv3_kernel = model.conv3.kernel_size[0]
    conv1_dilation = model.conv1.dilation[0]
    conv2_dilation = model.conv2.dilation[0]
    conv3_dilation = model.conv3.dilation[0]
    conv1_padding = model.conv1.padding[0]
    conv2_padding = model.conv2.padding[0]
    conv3_padding = model.conv3.padding[0]

    header = "\n".join([
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"#define CNN1D_INT8_NUM_CHANNELS {NUM_CHANNELS}",
        f"#define CNN1D_INT8_WINDOW_SIZE {WINDOW_SIZE}",
        f"#define CNN1D_INT8_NUM_CLASSES {num_classes}",
        "#define CNN1D_INT8_CONV1_OUT 32",
        "#define CNN1D_INT8_CONV2_OUT 64",
        "#define CNN1D_INT8_CONV3_OUT 64",
        f"#define CNN1D_INT8_CONV1_KERNEL {conv1_kernel}",
        f"#define CNN1D_INT8_CONV2_KERNEL {conv2_kernel}",
        f"#define CNN1D_INT8_CONV3_KERNEL {conv3_kernel}",
        f"#define CNN1D_INT8_CONV1_DILATION {conv1_dilation}",
        f"#define CNN1D_INT8_CONV2_DILATION {conv2_dilation}",
        f"#define CNN1D_INT8_CONV3_DILATION {conv3_dilation}",
        f"#define CNN1D_INT8_CONV1_PADDING {conv1_padding}",
        f"#define CNN1D_INT8_CONV2_PADDING {conv2_padding}",
        f"#define CNN1D_INT8_CONV3_PADDING {conv3_padding}",
        "",
        "/* Normalization */",
        "extern const float cnn1d_int8_norm_mean[CNN1D_INT8_NUM_CHANNELS];",
        "extern const float cnn1d_int8_norm_inv_std[CNN1D_INT8_NUM_CHANNELS];",
        "",
        "/* Activation scales (per-tensor, from calibration) */",
        "extern const float cnn1d_int8_input_scale;",
        "extern const float cnn1d_int8_conv1_out_scale;",
        "extern const float cnn1d_int8_conv2_out_scale;",
        "extern const float cnn1d_int8_conv3_out_scale;",
        "",
        "/* Conv1 weights: int8 per-channel quantized */",
        "extern const int8_t cnn1d_int8_conv1_w[CNN1D_INT8_CONV1_OUT * CNN1D_INT8_NUM_CHANNELS * CNN1D_INT8_CONV1_KERNEL];",
        "extern const float cnn1d_int8_conv1_w_scale[CNN1D_INT8_CONV1_OUT];",
        "extern const float cnn1d_int8_conv1_bias[CNN1D_INT8_CONV1_OUT];",
        "",
        "/* Conv2 weights */",
        "extern const int8_t cnn1d_int8_conv2_w[CNN1D_INT8_CONV2_OUT * CNN1D_INT8_CONV1_OUT * CNN1D_INT8_CONV2_KERNEL];",
        "extern const float cnn1d_int8_conv2_w_scale[CNN1D_INT8_CONV2_OUT];",
        "extern const float cnn1d_int8_conv2_bias[CNN1D_INT8_CONV2_OUT];",
        "",
        "/* Conv3 weights */",
        "extern const int8_t cnn1d_int8_conv3_w[CNN1D_INT8_CONV3_OUT * CNN1D_INT8_CONV2_OUT * CNN1D_INT8_CONV3_KERNEL];",
        "extern const float cnn1d_int8_conv3_w_scale[CNN1D_INT8_CONV3_OUT];",
        "extern const float cnn1d_int8_conv3_bias[CNN1D_INT8_CONV3_OUT];",
        "",
        "/* FC layer (float32) */",
        "extern const float cnn1d_int8_fc_w[CNN1D_INT8_NUM_CLASSES * CNN1D_INT8_CONV3_OUT];",
        "extern const float cnn1d_int8_fc_b[CNN1D_INT8_NUM_CLASSES];",
        "",
        f"extern const char *const cnn1d_int8_class_names[CNN1D_INT8_NUM_CLASSES];",
        "",
    ])
    (output_dir / "cnn1d_model_int8_generated.h").write_text(header, encoding="utf-8")

    # Generate source file
    parts = [
        '#include "cnn1d_model_int8_generated.h"',
        "",
        c_array_float("cnn1d_int8_norm_mean", norm_mean),
        c_array_float("cnn1d_int8_norm_inv_std", norm_inv_std),
        "",
        f"const float cnn1d_int8_input_scale = {input_scale:.10g}f;",
        f"const float cnn1d_int8_conv1_out_scale = {conv1_out_scale:.10g}f;",
        f"const float cnn1d_int8_conv2_out_scale = {conv2_out_scale:.10g}f;",
        f"const float cnn1d_int8_conv3_out_scale = {conv3_out_scale:.10g}f;",
        "",
        c_array_int8("cnn1d_int8_conv1_w", w1_int8),
        c_array_float("cnn1d_int8_conv1_w_scale", w1_scale),
        c_array_float("cnn1d_int8_conv1_bias", b1),
        "",
        c_array_int8("cnn1d_int8_conv2_w", w2_int8),
        c_array_float("cnn1d_int8_conv2_w_scale", w2_scale),
        c_array_float("cnn1d_int8_conv2_bias", b2),
        "",
        c_array_int8("cnn1d_int8_conv3_w", w3_int8),
        c_array_float("cnn1d_int8_conv3_w_scale", w3_scale),
        c_array_float("cnn1d_int8_conv3_bias", b3),
        "",
        c_array_float("cnn1d_int8_fc_w", fc_w),
        c_array_float("cnn1d_int8_fc_b", fc_b),
    ]

    class_lines = [f"const char *const cnn1d_int8_class_names[{num_classes}] = {{"]
    for name in meta["class_names"]:
        class_lines.append(f'    "{name}",')
    class_lines.append("};")
    parts.append("\n".join(class_lines))
    parts.append("")

    (output_dir / "cnn1d_model_int8_generated.c").write_text(
        "\n\n".join(parts), encoding="utf-8")

    # Print summary
    total_int8 = w1_int8.size + w2_int8.size + w3_int8.size
    total_float = (w1_scale.size + w2_scale.size + w3_scale.size +
                   b1.size + b2.size + b3.size + fc_w.size + fc_b.size +
                   norm_mean.size + norm_inv_std.size + 4)
    print(f"\n=== INT8 Quantization Summary ===")
    print(f"  Int8 weights: {total_int8:,} bytes")
    print(f"  Float params: {total_float:,} * 4 = {total_float*4:,} bytes")
    print(f"  Total model size: {total_int8 + total_float*4:,} bytes "
          f"({(total_int8 + total_float*4)/1024:.1f} KB)")
    print(f"  vs float32: {(total_int8 + total_float*4) / (27421*4) * 100:.0f}% of original")
    print(f"\n  Activation scales:")
    print(f"    input:     {input_scale:.6f}")
    print(f"    conv1_out: {conv1_out_scale:.6f}")
    print(f"    conv2_out: {conv2_out_scale:.6f}")
    print(f"    conv3_out: {conv3_out_scale:.6f}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Export 1D CNN to INT8 C arrays")
    parser.add_argument("--model", required=True, help="Path to .pt file")
    parser.add_argument("--meta", required=True, help="Path to model meta JSON")
    parser.add_argument("--norm", required=True, help="Path to norm_params.json")
    parser.add_argument("--calib-data", required=True, help="Path to calibration CSV")
    parser.add_argument("--num-calib", type=int, default=200)
    parser.add_argument("--output-dir", default="main/generated")
    args = parser.parse_args()

    export_int8_model(Path(args.model), Path(args.meta), Path(args.norm),
                      Path(args.calib_data), Path(args.output_dir), args.num_calib)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
