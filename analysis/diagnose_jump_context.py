#!/usr/bin/env python3
"""Diagnose: is jump-after-walk / jump-with-raised-hands failure a data or algorithm problem?

Simulates the full firmware pipeline on real recorded jump events:
  event RF (window 25) -> confidence gate (0.60) -> 3-frame confirm -> edge trigger
grouped by what the person was doing right before the jump (idle / walk / run).
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

from tools.rf_baseline import RF_NODE_IDS, extract_window_features
from tools.train_event_rf import align_session_samples

CONF_GATE = 0.60          # DONGLE_RF_MIN_CONFIDENCE
CONFIRM_FRAMES = 3        # DONGLE_CONFIRM_FRAMES

df = pd.read_csv(PROJECT_ROOT / "data/processed/event_samples_combined_slash_full.csv", low_memory=False)
df["pc_timestamp_ms"] = df["pc_timestamp_ms"].astype(float)
df["board_timestamp_ms"] = pd.to_numeric(df["board_timestamp_ms"], errors="coerce").astype(float)
ev = pd.read_csv(PROJECT_ROOT / "data/processed/event_events_combined_slash_full.csv")

eb = joblib.load(PROJECT_ROOT / "model/event_rf/rf_model.joblib")
e_clf, e_feats, e_cls = eb["model"], eb["feature_names"], eb["class_names"]

jump_sessions = sorted(ev[ev["event_type"] == "jump"]["session_id"].unique())
sessions = {}
for sid in jump_sessions:
    sessions[sid] = align_session_samples(df[df["session_id"] == sid], 40.0, 25.0, RF_NODE_IDS)

jumps = ev[ev["event_type"] == "jump"]
results = []
for _, e in jumps.iterrows():
    sid, t0 = e["session_id"], float(e["pc_timestamp_ms"])
    if sid not in sessions:
        continue
    g = sessions[sid]
    seg = g[(g["sync_time_ms"] >= t0 - 2500) & (g["sync_time_ms"] <= t0 + 1500)].reset_index(drop=True)
    if len(seg) < 60:
        continue
    pre = seg[(seg["sync_time_ms"] >= t0 - 1500) & (seg["sync_time_ms"] <= t0 - 300)]["state_label"]
    pre_ctx = pre.mode().iloc[0] if len(pre) else "unknown"

    max_consec = 0
    consec = 0
    best_jump_conf = 0.0
    preds = []
    for end in range(24, len(seg)):
        t = seg["sync_time_ms"].iloc[end]
        if t < t0 - 200 or t > t0 + 900:
            continue
        w25 = seg.iloc[end - 24 : end + 1]
        f25 = pd.DataFrame([extract_window_features(w25, RF_NODE_IDS)]).reindex(columns=e_feats)
        p = e_clf.predict_proba(f25)[0]
        k = int(np.argmax(p))
        pj = float(p[e_cls.index("jump")])
        best_jump_conf = max(best_jump_conf, pj)
        preds.append((e_cls[k], round(float(p[k]), 2)))
        hit = e_cls[k] == "jump" and p[k] >= CONF_GATE
        consec = consec + 1 if hit else 0
        max_consec = max(max_consec, consec)
    fired = max_consec >= CONFIRM_FRAMES
    results.append({
        "sid": sid, "pre": pre_ctx, "fired": fired,
        "max_consec": max_consec, "best_jump_conf": round(best_jump_conf, 2),
        "preds": preds,
    })

R = pd.DataFrame([{k: r[k] for k in ("sid", "pre", "fired", "max_consec", "best_jump_conf")} for r in results])
print("=== jump fire rate by pre-context (full pipeline sim) ===")
print(R.groupby("pre").agg(n=("fired", "size"), fired=("fired", "sum"),
                           fire_rate=("fired", "mean"),
                           median_best_conf=("best_jump_conf", "median")).round(2))
print()
print("=== per-session breakdown ===")
print(R.groupby(["sid", "pre"]).agg(n=("fired", "size"), fired=("fired", "sum")).to_string())
print()

from collections import Counter
for pre_ctx in ["idle", "walk", "run"]:
    fails = [r for r in results if r["pre"] == pre_ctx and not r["fired"]]
    c = Counter()
    for r in fails:
        c.update(x[0] for x in r["preds"])
    confs = [r["best_jump_conf"] for r in fails]
    print(f"pre={pre_ctx} failed={len(fails)} "
          f"best_jump_conf_of_failures={sorted(confs)[:8]} "
          f"what model said instead={dict(c.most_common(5))}")
