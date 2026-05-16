#!/usr/bin/env python3
"""Export trained 1D CNN model to C arrays for ESP32-S3 firmware deployment.

Fuses BatchNorm into Conv layers and exports all weights as float arrays.
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
    """Fuse Conv1d + BatchNorm1d into a single Conv1d with adjusted weights/bias."""
    w = conv.weight.data  # (out_ch, in_ch, kernel)
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


def c_array_float(name: str, values: np.ndarray, per_line: int = 8) -> str:
    flat = values.flatten()
    lines = [f"const float {name}[{len(flat)}] = {{"]
    for i in range(0, len(flat), per_line):
        chunk = ", ".join(f"{v:.8g}f" for v in flat[i:i + per_line])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines)


def export_model(model_pt_path: Path, meta_path: Path, norm_path: Path,
                 output_dir: Path) -> None:
    with open(meta_path, "r") as f:
        meta = json.load(f)
    with open(norm_path, "r") as f:
        norm = json.load(f)

    num_classes = len(meta["class_names"])
    model = IMU1DCNN(NUM_CHANNELS, num_classes, WINDOW_SIZE)
    model.load_state_dict(torch.load(model_pt_path, map_location="cpu", weights_only=True))
    model.eval()

    # Fuse BN into conv layers
    w1, b1 = fuse_conv_bn(model.conv1, model.bn1)
    w2, b2 = fuse_conv_bn(model.conv2, model.bn2)
    w3, b3 = fuse_conv_bn(model.conv3, model.bn3)

    # FC layer
    fc_w = model.fc.weight.data.numpy()  # (num_classes, 64)
    fc_b = model.fc.bias.data.numpy()    # (num_classes,)

    # Normalization params
    norm_mean = np.array(norm["mean"], dtype=np.float32)
    norm_inv_std = (1.0 / np.array(norm["std"], dtype=np.float32)).astype(np.float32)

    output_dir.mkdir(parents=True, exist_ok=True)

    # Generate header
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
        f"#define CNN1D_NUM_CHANNELS {NUM_CHANNELS}",
        f"#define CNN1D_WINDOW_SIZE {WINDOW_SIZE}",
        f"#define CNN1D_NUM_CLASSES {num_classes}",
        f"#define CNN1D_CONV1_OUT 32",
        f"#define CNN1D_CONV2_OUT 64",
        f"#define CNN1D_CONV3_OUT 64",
        f"#define CNN1D_CONV1_KERNEL {conv1_kernel}",
        f"#define CNN1D_CONV2_KERNEL {conv2_kernel}",
        f"#define CNN1D_CONV3_KERNEL {conv3_kernel}",
        f"#define CNN1D_CONV1_DILATION {conv1_dilation}",
        f"#define CNN1D_CONV2_DILATION {conv2_dilation}",
        f"#define CNN1D_CONV3_DILATION {conv3_dilation}",
        f"#define CNN1D_CONV1_PADDING {conv1_padding}",
        f"#define CNN1D_CONV2_PADDING {conv2_padding}",
        f"#define CNN1D_CONV3_PADDING {conv3_padding}",
        "",
        "extern const float cnn1d_norm_mean[CNN1D_NUM_CHANNELS];",
        "extern const float cnn1d_norm_inv_std[CNN1D_NUM_CHANNELS];",
        "extern const float cnn1d_conv1_w[CNN1D_CONV1_OUT * CNN1D_NUM_CHANNELS * CNN1D_CONV1_KERNEL];",
        "extern const float cnn1d_conv1_b[CNN1D_CONV1_OUT];",
        "extern const float cnn1d_conv2_w[CNN1D_CONV2_OUT * CNN1D_CONV1_OUT * CNN1D_CONV2_KERNEL];",
        "extern const float cnn1d_conv2_b[CNN1D_CONV2_OUT];",
        "extern const float cnn1d_conv3_w[CNN1D_CONV3_OUT * CNN1D_CONV2_OUT * CNN1D_CONV3_KERNEL];",
        "extern const float cnn1d_conv3_b[CNN1D_CONV3_OUT];",
        "extern const float cnn1d_fc_w[CNN1D_NUM_CLASSES * CNN1D_CONV3_OUT];",
        "extern const float cnn1d_fc_b[CNN1D_NUM_CLASSES];",
        f"extern const char *const cnn1d_class_names[CNN1D_NUM_CLASSES];",
        "",
    ])
    (output_dir / "cnn1d_model_generated.h").write_text(header, encoding="utf-8")

    # Generate source
    parts = [
        '#include "cnn1d_model_generated.h"',
        "",
        c_array_float("cnn1d_norm_mean", norm_mean),
        c_array_float("cnn1d_norm_inv_std", norm_inv_std),
        c_array_float("cnn1d_conv1_w", w1),
        c_array_float("cnn1d_conv1_b", b1),
        c_array_float("cnn1d_conv2_w", w2),
        c_array_float("cnn1d_conv2_b", b2),
        c_array_float("cnn1d_conv3_w", w3),
        c_array_float("cnn1d_conv3_b", b3),
        c_array_float("cnn1d_fc_w", fc_w),
        c_array_float("cnn1d_fc_b", fc_b),
    ]

    # Class names
    class_lines = [f"const char *const cnn1d_class_names[CNN1D_NUM_CLASSES] = {{"]
    for name in meta["class_names"]:
        class_lines.append(f'    "{name}",')
    class_lines.append("};")
    parts.append("\n".join(class_lines))
    parts.append("")

    (output_dir / "cnn1d_model_generated.c").write_text(
        "\n\n".join(parts), encoding="utf-8")

    # Summary
    total_params = (w1.size + b1.size + w2.size + b2.size +
                    w3.size + b3.size + fc_w.size + fc_b.size +
                    norm_mean.size + norm_inv_std.size)
    summary = {
        "total_params": int(total_params),
        "total_bytes_float32": int(total_params * 4),
        "class_names": meta["class_names"],
        "conv1_shape": list(w1.shape),
        "conv2_shape": list(w2.shape),
        "conv3_shape": list(w3.shape),
        "fc_shape": list(fc_w.shape),
        "kernel_sizes": [conv1_kernel, conv2_kernel, conv3_kernel],
    }
    print(json.dumps(summary, indent=2))


def main() -> int:
    parser = argparse.ArgumentParser(description="Export 1D CNN to C arrays")
    parser.add_argument("--model", required=True, help="Path to .pt file")
    parser.add_argument("--meta", required=True, help="Path to model meta JSON")
    parser.add_argument("--norm", required=True, help="Path to norm_params.json")
    parser.add_argument("--output-dir", default="main/generated")
    args = parser.parse_args()

    export_model(Path(args.model), Path(args.meta), Path(args.norm), Path(args.output_dir))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
