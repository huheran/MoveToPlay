#!/usr/bin/env bash
# 模型重训与导出命令备份（记录当前部署模型的完整超参数）
# 数据: data/processed/event_samples_combined_slash_full.csv (含 wms6b_session_001)
set -euo pipefail
cd "$(dirname "$0")/.."

SAMPLES=data/processed/event_samples_combined_slash_full.csv
EVENTS=data/processed/event_events_combined_slash_full.csv

# ── 状态模型 (walk/run/idle/move_noise/right_hand_slash, 窗口15帧=600ms) ──
# 关键点:
#   --synth-onset-per-label 400   合成 idle→walk/run 起步窗口(数据里无真实过渡)
#   --hard-negative-*             jump/kick 事件后 0-600ms 窗口标为 move_noise,
#                                 防止起跳/踢腿瞬间误触发 run/walk
python tools/train_event_rf.py \
  --samples "$SAMPLES" --events "$EVENTS" \
  --output-dir model/state_rf_15_full \
  --window-size 15 \
  --include-event-group __state_only__ \
  --include-state-label idle --include-state-label walk \
  --include-state-label run --include-state-label move_noise \
  --include-state-label right_hand_slash \
  --max-state-windows-per-label 2500 \
  --state-onset-label walk --state-onset-label run \
  --state-onset-tail-frames 6 \
  --synth-onset-per-label 400 \
  --hard-negative-event-type jump --hard-negative-event-type kick \
  --hard-negative-label move_noise \
  --hard-negative-start-ms 0 --hard-negative-end-ms 600 \
  --negative-ratio 0 \
  --n-estimators 50 --max-depth 9

# ── 事件模型 (15类, 窗口25帧=1s) ──
python tools/train_event_rf.py \
  --samples "$SAMPLES" --events "$EVENTS" \
  --output-dir model/event_rf \
  --window-size 25 \
  --positive-end-ms 500 \
  --include-state-label idle --include-state-label walk \
  --include-state-label run --include-state-label move_noise \
  --include-state-label right_hand_slash \
  --max-state-windows-per-label 700 \
  --negative-ratio 0 \
  --n-estimators 180 --max-depth 12

# ── 导出固件 C 数组 ──
python tools/export_rf_model_c.py \
  --model model/state_rf_15_full/rf_model.joblib \
  --output-dir main/generated \
  --file-prefix rf_state_model_generated --symbol-prefix rf_state_model

python tools/export_rf_model_c.py \
  --model model/event_rf/rf_model.joblib \
  --output-dir main/generated \
  --file-prefix rf_model_generated --symbol-prefix rf_model
