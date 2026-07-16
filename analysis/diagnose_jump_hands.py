#!/usr/bin/env python3
"""Check hand-motion diversity during recorded jump events.

If every recorded jump has hands swinging (run-style arm pump), then a jump
performed with hands raised/static is out-of-distribution -> data problem.
Also checks how much the event model's jump decision depends on hand features.
"""
import sys
import warnings
from pathlib import Path

warnings.filterwarnings("ignore")
PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT))

import joblib
import numpy as np
import pandas as pd

df = pd.read_csv(PROJECT_ROOT / "data/processed/event_samples_combined_slash_full.csv", low_memory=False)
ev = pd.read_csv(PROJECT_ROOT / "data/processed/event_events_combined_slash_full.csv")
jumps = ev[ev["event_type"] == "jump"]

# For each jump event, measure hand (n2/n3) and leg (n4) gyro energy in [t0, t0+600ms]
rows = []
for _, e in jumps.iterrows():
    sid, t0 = e["session_id"], float(e["pc_timestamp_ms"])
    s = df[df["session_id"] == sid]
    r = {"sid": sid}
    ok = True
    for nid, part in [(1, "chest"), (2, "rhand"), (3, "lhand"), (4, "leg")]:
        nd = s[(s["node_id"] == nid) & (s["pc_timestamp_ms"] >= t0) & (s["pc_timestamp_ms"] <= t0 + 600)]
        if len(nd) < 5:
            ok = False
            break
        gn = np.sqrt(nd["gx"] ** 2 + nd["gy"] ** 2 + nd["gz"] ** 2)
        an = np.sqrt(nd["ax"] ** 2 + nd["ay"] ** 2 + nd["az"] ** 2)
        r[f"{part}_gyro"] = gn.mean()
        r[f"{part}_acc"] = an.mean()
    if ok:
        rows.append(r)

J = pd.DataFrame(rows)
print(f"=== hand/leg gyro energy during {len(J)} recorded jumps (dps mean over 600ms) ===")
print(J[["chest_gyro", "rhand_gyro", "lhand_gyro", "leg_gyro"]].describe().round(1).loc[["min", "25%", "50%", "75%", "max"]])
print()
# how many jumps had "quiet hands" (hands moving < 40% of leg energy)?
quiet = ((J["rhand_gyro"] + J["lhand_gyro"]) / 2 < 0.4 * J["leg_gyro"]).sum()
print(f"jumps with quiet hands (avg hand gyro < 40% of leg gyro): {quiet}/{len(J)} = {quiet/len(J):.0%}")
hand_leg_ratio = ((J["rhand_gyro"] + J["lhand_gyro"]) / 2 / J["leg_gyro"])
print(f"hand/leg energy ratio: min={hand_leg_ratio.min():.2f} median={hand_leg_ratio.median():.2f} max={hand_leg_ratio.max():.2f}")
print()

# Feature importance: how much does the event model rely on hand nodes for jump?
eb = joblib.load(PROJECT_ROOT / "model/event_rf/rf_model.joblib")
clf, feats = eb["model"], eb["feature_names"]
imp = pd.Series(clf.feature_importances_, index=feats)


def node_group(name: str) -> str:
    if name.startswith("n1_"):
        return "chest"
    if name.startswith("n2_"):
        return "right_hand"
    if name.startswith("n3_"):
        return "left_hand"
    if name.startswith("n4_"):
        return "leg"
    if name.startswith("pair_"):
        parts = name.split("_")
        return f"pair_{parts[1]}_{parts[2]}"
    return "other"


g = imp.groupby(node_group).sum().sort_values(ascending=False)
print("=== event model total feature importance by node (all classes) ===")
print(g.round(3).to_string())
print()
print("note: pair_x_y features couple two nodes; pairs involving 2/3 (hands) mean")
print("hand motion directly shapes every prediction, including jump.")
