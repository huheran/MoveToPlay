#!/usr/bin/env python3
"""Analyze threshold_test.csv to determine if threshold-based detection is feasible."""
import pandas as pd
import numpy as np
import os
os.environ["KMP_DUPLICATE_LIB_OK"] = "TRUE"

df = pd.read_csv("D:/git_fork_sure_mine/MoveToPlay/data/threshold_test.csv")

# Focus on leg node (node_id=4)
leg = df[df["node_id"] == 4].copy()
leg["acc_norm"] = np.sqrt(leg["ax"]**2 + leg["ay"]**2 + leg["az"]**2)
leg["gyro_norm"] = np.sqrt(leg["gx"]**2 + leg["gy"]**2 + leg["gz"]**2)

label_map = {"both_hands_raise": "kick"}

print("=" * 70)
print("LEG NODE (node_id=4) - Accelerometer Norm (g)")
print("=" * 70)
header = f"{'Label':<12} {'Mean':>7} {'Std':>7} {'Max':>7} {'Min':>7} {'P95':>7} {'P05':>7}"
print(header)
for label in ["idle", "walk", "run", "jump", "both_hands_raise"]:
    subset = leg[leg["label"] == label]["acc_norm"]
    if len(subset) == 0:
        continue
    name = label_map.get(label, label)
    print(f"{name:<12} {subset.mean():>7.3f} {subset.std():>7.3f} {subset.max():>7.3f} {subset.min():>7.3f} {subset.quantile(0.95):>7.3f} {subset.quantile(0.05):>7.3f}")

print()
print("=" * 70)
print("LEG NODE (node_id=4) - Gyroscope Norm (dps)")
print("=" * 70)
print(header)
for label in ["idle", "walk", "run", "jump", "both_hands_raise"]:
    subset = leg[leg["label"] == label]["gyro_norm"]
    if len(subset) == 0:
        continue
    name = label_map.get(label, label)
    print(f"{name:<12} {subset.mean():>7.2f} {subset.std():>7.2f} {subset.max():>7.2f} {subset.min():>7.2f} {subset.quantile(0.95):>7.2f} {subset.quantile(0.05):>7.2f}")

print()
print("=" * 70)
print("LEG NODE (node_id=4) - Per-axis acceleration stats")
print("=" * 70)
for axis in ["ax", "ay", "az"]:
    print(f"\n  --- {axis} ---")
    print(f"  {'Label':<12} {'Mean':>7} {'Std':>7} {'Max':>7} {'Min':>7}")
    for label in ["idle", "walk", "run", "jump", "both_hands_raise"]:
        subset = leg[leg["label"] == label][axis]
        if len(subset) == 0:
            continue
        name = label_map.get(label, label)
        print(f"  {name:<12} {subset.mean():>7.3f} {subset.std():>7.3f} {subset.max():>7.3f} {subset.min():>7.3f}")

# Also check waist node (node_id=1)
print()
print("=" * 70)
print("WAIST NODE (node_id=1) - Accelerometer Norm (g)")
print("=" * 70)
waist = df[df["node_id"] == 1].copy()
waist["acc_norm"] = np.sqrt(waist["ax"]**2 + waist["ay"]**2 + waist["az"]**2)
print(f"{'Label':<12} {'Mean':>7} {'Std':>7} {'Max':>7} {'Min':>7} {'P95':>7} {'P05':>7}")
for label in ["idle", "walk", "run", "jump", "both_hands_raise"]:
    subset = waist[waist["label"] == label]["acc_norm"]
    if len(subset) == 0:
        continue
    name = label_map.get(label, label)
    print(f"{name:<12} {subset.mean():>7.3f} {subset.std():>7.3f} {subset.max():>7.3f} {subset.min():>7.3f} {subset.quantile(0.95):>7.3f} {subset.quantile(0.05):>7.3f}")

# Check hand nodes during kick (should be relatively still)
print()
print("=" * 70)
print("RIGHT HAND (node_id=2) during each action - Acc Norm")
print("=" * 70)
rhand = df[df["node_id"] == 2].copy()
rhand["acc_norm"] = np.sqrt(rhand["ax"]**2 + rhand["ay"]**2 + rhand["az"]**2)
print(f"{'Label':<12} {'Mean':>7} {'Std':>7} {'Max':>7}")
for label in ["idle", "walk", "run", "jump", "both_hands_raise"]:
    subset = rhand[rhand["label"] == label]["acc_norm"]
    if len(subset) == 0:
        continue
    name = label_map.get(label, label)
    print(f"{name:<12} {subset.mean():>7.3f} {subset.std():>7.3f} {subset.max():>7.3f}")
