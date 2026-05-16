#!/usr/bin/env python3
"""Verify INT8 quantization accuracy vs float32 baseline."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
from sklearn.metrics import classification_report, accuracy_score

THIS_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = THIS_DIR.parent
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from tools.cnn1d_train import (IMU1DCNN, load_data, align_session,
                                build_windows, BASE_CHANNELS, NODE_IDS, WINDOW_SIZE, NUM_CHANNELS)
from tools.action_labels import MULTICLASS_LABEL_ORDER
from tools.export_cnn1d_int8 import fuse_conv_bn, quantize_weights_per_channel


def simulate_int8_inference(model, x_norm, act_ranges):
    """Simulate int8 quantized inference in Python (matches C implementation)."""
    model.eval()

    input_scale = act_ranges["input"] / 127.0
    conv1_out_scale = act_ranges["conv1_out"] / 127.0
    conv2_out_scale = act_ranges["conv2_out"] / 127.0

    w1, b1 = fuse_conv_bn(model.conv1, model.bn1)
    w2, b2 = fuse_conv_bn(model.conv2, model.bn2)
    w3, b3 = fuse_conv_bn(model.conv3, model.bn3)

    w1_int8, w1_scale = quantize_weights_per_channel(w1)
    w2_int8, w2_scale = quantize_weights_per_channel(w2)
    w3_int8, w3_scale = quantize_weights_per_channel(w3)

    results = []
    for sample in x_norm:
        x = sample  # (24, 25) numpy

        # Quantize input
        x_q = np.clip(np.round(x / input_scale), -127, 127).astype(np.int8)

        # Conv1: int8 x int8 -> int32 -> float -> relu -> quantize
        out1 = _conv1d_int8(x_q, w1_int8, b1, w1_scale, input_scale,
                            model.conv1.dilation[0], model.conv1.padding[0])
        out1 = np.maximum(out1, 0.0)  # ReLU
        out1_q = np.clip(np.round(out1 / conv1_out_scale), -127, 127).astype(np.int8)

        # Conv2
        out2 = _conv1d_int8(out1_q, w2_int8, b2, w2_scale, conv1_out_scale,
                            model.conv2.dilation[0], model.conv2.padding[0])
        out2 = np.maximum(out2, 0.0)
        out2_q = np.clip(np.round(out2 / conv2_out_scale), -127, 127).astype(np.int8)

        # Conv3
        out3 = _conv1d_int8(out2_q, w3_int8, b3, w3_scale, conv2_out_scale,
                            model.conv3.dilation[0], model.conv3.padding[0])
        out3 = np.maximum(out3, 0.0)

        # AvgPool (float)
        pooled = out3.mean(axis=1)  # (64,)

        # FC (float)
        fc_w = model.fc.weight.data.numpy()
        fc_b = model.fc.bias.data.numpy()
        logits = fc_w @ pooled + fc_b

        results.append(logits.argmax())

    return np.array(results)


def _conv1d_int8(x_q, w_int8, bias, w_scale, input_scale, dilation, padding):
    """Simulate int8 conv1d: matches the C implementation."""
    out_ch, in_ch, kernel = w_int8.shape
    time_len = x_q.shape[1]
    output = np.zeros((out_ch, time_len), dtype=np.float32)

    for oc in range(out_ch):
        for t in range(time_len):
            acc = np.int32(0)
            for ic in range(in_ch):
                for k in range(kernel):
                    ti = t + k * dilation - padding
                    if 0 <= ti < time_len:
                        acc += np.int32(w_int8[oc, ic, k]) * np.int32(x_q[ic, ti])
            output[oc, t] = float(acc) * w_scale[oc] * input_scale + bias[oc]

    return output


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--meta", required=True)
    parser.add_argument("--norm", required=True)
    parser.add_argument("--data", required=True)
    args = parser.parse_args()

    with open(args.meta) as f:
        meta = json.load(f)
    with open(args.norm) as f:
        norm = json.load(f)

    num_classes = len(meta["class_names"])
    class_names = meta["class_names"]
    model = IMU1DCNN(NUM_CHANNELS, num_classes, WINDOW_SIZE, dropout=0.0)
    model.load_state_dict(torch.load(args.model, map_location="cpu", weights_only=True))
    model.eval()

    norm_mean = np.array(norm["mean"], dtype=np.float32).reshape(1, -1, 1)
    norm_std = np.array(norm["std"], dtype=np.float32).reshape(1, -1, 1)

    # Load test data
    import pandas as pd
    df = load_data(args.data)
    sessions = []
    for _, session_df in df.groupby("session_id"):
        aligned = align_session(session_df, 40.0, 25.0)
        if len(aligned) > 0:
            sessions.append(aligned)
    sync_df = pd.concat(sessions, ignore_index=True)
    X, y, groups = build_windows(sync_df, WINDOW_SIZE, 10, 200)

    label_set = set(y)
    used_classes = [l for l in MULTICLASS_LABEL_ORDER if l in label_set]
    used_classes.extend(sorted(label_set - set(used_classes)))
    label_to_idx = {name: i for i, name in enumerate(used_classes)}
    y_idx = np.array([label_to_idx[l] for l in y])

    # Normalize
    X_norm = (X - norm_mean.reshape(1, -1, 1)) / norm_std.reshape(1, -1, 1)

    # Float32 baseline
    print("Running float32 inference...")
    with torch.no_grad():
        logits_f32 = model(torch.FloatTensor(X_norm)).numpy()
    preds_f32 = logits_f32.argmax(axis=1)
    acc_f32 = accuracy_score(y_idx, preds_f32)

    # Collect activation ranges for quantization
    print("Collecting activation ranges...")
    from tools.export_cnn1d_int8 import collect_activation_ranges
    act_ranges = collect_activation_ranges(model, norm["mean"], norm["std"], X[:200])

    # INT8 simulation (on subset for speed)
    n_test = min(500, len(X_norm))
    print(f"Running int8 simulation on {n_test} samples...")
    preds_int8 = simulate_int8_inference(model, X_norm[:n_test], act_ranges)
    acc_int8 = accuracy_score(y_idx[:n_test], preds_int8)
    acc_f32_subset = accuracy_score(y_idx[:n_test], preds_f32[:n_test])

    print(f"\n{'='*50}")
    print(f"  Float32 accuracy (full):   {acc_f32*100:.2f}%")
    print(f"  Float32 accuracy (subset): {acc_f32_subset*100:.2f}%")
    print(f"  INT8 accuracy (subset):    {acc_int8*100:.2f}%")
    print(f"  Accuracy drop:             {(acc_f32_subset - acc_int8)*100:.2f}%")
    print(f"{'='*50}")

    # Per-class comparison
    print(f"\nPer-class (subset of {n_test}):")
    print(f"  {'Class':<25s} {'F32':>5s} {'INT8':>5s} {'Drop':>6s}")
    for i, name in enumerate(used_classes):
        mask = y_idx[:n_test] == i
        if mask.sum() == 0:
            continue
        f32_acc = (preds_f32[:n_test][mask] == i).mean()
        i8_acc = (preds_int8[mask] == i).mean()
        print(f"  {name:<25s} {f32_acc*100:>5.1f} {i8_acc*100:>5.1f} {(f32_acc-i8_acc)*100:>+6.1f}")


if __name__ == "__main__":
    main()
