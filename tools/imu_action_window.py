#!/usr/bin/env python3
"""
MoveToPlay dual-node action recognition window.

This tool reads the dongle serial stream, applies the small calibration table
captured during manual validation, and shows simple action decisions in a
matplotlib window. It does not send keyboard events.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import time
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import matplotlib.pyplot as plt
import numpy as np
import serial

from imu_dual_node_phase5 import NonBlockingKeyReader, Packet, parse_packet


NODE_CHEST = 1
NODE_HAND = 2


@dataclass
class CalibrationProfile:
    tpose_unit: dict[int, np.ndarray]
    gyro_bias: dict[int, np.ndarray]


@dataclass
class ActionThresholds:
    left_dx: float = -0.25
    right_dx: float = 0.25
    neutral_dx: float = 0.15
    arm_raise_hand_angle_deg: float = 45.0
    arm_raise_chest_guard_deg: float = 12.0
    wave_hand_gyro_dps: float = 120.0
    wave_ratio: float = 3.0
    jump_low_g: float = 0.75
    jump_high_g: float = 1.35
    dynamic_window_s: float = 0.30
    jump_pair_window_s: float = 0.55
    event_hold_s: float = 0.28
    event_cooldown_s: float = 0.60


@dataclass
class NodeFeature:
    node_id: int
    name: str
    seq: int
    age_s: float
    acc_norm: float
    gyro_norm: float
    angle_deg: float
    delta: np.ndarray
    link_ok: bool


@dataclass
class ActionDecision:
    move: str = "NEUTRAL"
    keys: list[str] = field(default_factory=list)
    arm_raise: bool = False
    wave_active: bool = False
    jump_active: bool = False
    wave_triggered: bool = False
    jump_triggered: bool = False
    chest_dx: Optional[float] = None
    chest_angle: Optional[float] = None
    hand_angle: Optional[float] = None
    hand_gyro_max: Optional[float] = None
    chest_gyro_max: Optional[float] = None
    gyro_ratio: Optional[float] = None
    jump_acc_min: Optional[float] = None
    jump_acc_max: Optional[float] = None


class SampleHistory:
    def __init__(self, keep_s: float = 5.0) -> None:
        self.keep_s = keep_s
        self.samples: dict[int, deque[tuple[float, float, float]]] = {
            NODE_CHEST: deque(),
            NODE_HAND: deque(),
        }

    def add(self, node_id: int, now: float, acc_norm: float, gyro_norm: float) -> None:
        rows = self.samples.get(node_id)
        if rows is None:
            return
        rows.append((now, acc_norm, gyro_norm))
        cutoff = now - self.keep_s
        while rows and rows[0][0] < cutoff:
            rows.popleft()

    def window_values(self, node_id: int, now: float, window_s: float) -> tuple[list[float], list[float]]:
        rows = self.samples.get(node_id, ())
        cutoff = now - window_s
        acc = []
        gyro = []
        for t, acc_norm, gyro_norm in rows:
            if t >= cutoff:
                acc.append(acc_norm)
                gyro.append(gyro_norm)
        return acc, gyro


def vector_norm(v: np.ndarray) -> float:
    return float(np.linalg.norm(v))


def unit_or_zero(v: np.ndarray) -> np.ndarray:
    n = vector_norm(v)
    if n < 1e-9:
        return np.zeros(3, dtype=float)
    return v / n


def angle_between_deg(a: np.ndarray, b: np.ndarray) -> float:
    au = unit_or_zero(a)
    bu = unit_or_zero(b)
    dot = float(np.dot(au, bu))
    dot = max(-1.0, min(1.0, dot))
    return math.degrees(math.acos(dot))


def load_calibration_profile(path: Path) -> CalibrationProfile:
    with path.open("r", encoding="utf-8") as f:
        raw = json.load(f)

    tpose_raw = raw.get("tpose", {})
    bias_raw = raw.get("gyro_bias", {})
    tpose_unit = {}
    gyro_bias = {}

    for node_id in (NODE_CHEST, NODE_HAND):
        key = str(node_id)
        if key not in tpose_raw or "acc_unit" not in tpose_raw[key]:
            raise ValueError(f"missing tpose acc_unit for node {node_id}")
        tpose_unit[node_id] = unit_or_zero(np.array(tpose_raw[key]["acc_unit"], dtype=float))
        gyro_bias[node_id] = np.array(bias_raw.get(key, [0.0, 0.0, 0.0]), dtype=float)

    return CalibrationProfile(tpose_unit=tpose_unit, gyro_bias=gyro_bias)


def feature_from_packet(
    packet: Packet,
    profile: CalibrationProfile,
    now: float,
    link_timeout_s: float,
) -> NodeFeature:
    acc = np.array([packet.ax, packet.ay, packet.az], dtype=float)
    gyro = np.array([packet.gx, packet.gy, packet.gz], dtype=float)
    gyro = gyro - profile.gyro_bias.get(packet.node_id, np.zeros(3, dtype=float))

    acc_unit = unit_or_zero(acc)
    tpose_unit = profile.tpose_unit[packet.node_id]
    delta = acc_unit - tpose_unit
    angle_deg = angle_between_deg(acc_unit, tpose_unit)
    age_s = max(0.0, now - packet.arrival_time)
    name = "chest" if packet.node_id == NODE_CHEST else "hand"

    return NodeFeature(
        node_id=packet.node_id,
        name=name,
        seq=packet.seq,
        age_s=age_s,
        acc_norm=vector_norm(acc),
        gyro_norm=vector_norm(gyro),
        angle_deg=angle_deg,
        delta=delta,
        link_ok=age_s <= link_timeout_s,
    )


class ActionRecognizer:
    def __init__(self, thresholds: ActionThresholds) -> None:
        self.th = thresholds
        self.last_wave_t = -999.0
        self.last_jump_t = -999.0
        self.last_jump_low_t = -999.0

    def _event_state(self, name: str, condition: bool, now: float) -> tuple[bool, bool]:
        if name == "wave":
            last_t = self.last_wave_t
        elif name == "jump":
            last_t = self.last_jump_t
        else:
            raise ValueError(name)

        triggered = False
        if condition and (now - last_t) >= self.th.event_cooldown_s:
            triggered = True
            last_t = now
            if name == "wave":
                self.last_wave_t = now
            else:
                self.last_jump_t = now

        active = (now - last_t) <= self.th.event_hold_s
        return active, triggered

    def update(
        self,
        features: dict[int, NodeFeature],
        history: SampleHistory,
        now: float,
    ) -> ActionDecision:
        decision = ActionDecision()
        chest = features.get(NODE_CHEST)
        hand = features.get(NODE_HAND)

        if chest is not None and chest.link_ok:
            decision.chest_dx = float(chest.delta[0])
            decision.chest_angle = chest.angle_deg
            if chest.delta[0] < self.th.left_dx:
                decision.move = "LEFT"
                decision.keys.append("A")
            elif chest.delta[0] > self.th.right_dx:
                decision.move = "RIGHT"
                decision.keys.append("D")
            elif abs(chest.delta[0]) <= self.th.neutral_dx:
                decision.move = "NEUTRAL"
            else:
                decision.move = "CENTERING"

        if hand is not None and hand.link_ok:
            decision.hand_angle = hand.angle_deg
            if (
                hand.angle_deg < self.th.arm_raise_hand_angle_deg
                and (chest is None or chest.angle_deg < self.th.arm_raise_chest_guard_deg)
            ):
                decision.arm_raise = True
                decision.keys.append("W")

        chest_acc, chest_gyro = history.window_values(NODE_CHEST, now, self.th.dynamic_window_s)
        hand_acc, hand_gyro = history.window_values(NODE_HAND, now, self.th.dynamic_window_s)

        chest_gyro_max = max(chest_gyro) if chest_gyro else 0.0
        hand_gyro_max = max(hand_gyro) if hand_gyro else 0.0
        ratio = hand_gyro_max / max(chest_gyro_max, 1.0)
        decision.chest_gyro_max = chest_gyro_max
        decision.hand_gyro_max = hand_gyro_max
        decision.gyro_ratio = ratio

        wave_condition = hand_gyro_max > self.th.wave_hand_gyro_dps and ratio > self.th.wave_ratio
        decision.wave_active, decision.wave_triggered = self._event_state("wave", wave_condition, now)
        if decision.wave_active:
            decision.keys.append("E")

        jump_acc = chest_acc + hand_acc
        if jump_acc:
            decision.jump_acc_min = min(jump_acc)
            decision.jump_acc_max = max(jump_acc)
        else:
            decision.jump_acc_min = None
            decision.jump_acc_max = None

        current_acc = [
            f.acc_norm
            for f in (chest, hand)
            if f is not None and f.link_ok and math.isfinite(f.acc_norm)
        ]
        if current_acc and min(current_acc) < self.th.jump_low_g:
            self.last_jump_low_t = now
        jump_condition = (
            bool(current_acc)
            and max(current_acc) > self.th.jump_high_g
            and (now - self.last_jump_low_t) <= self.th.jump_pair_window_s
        )
        decision.jump_active, decision.jump_triggered = self._event_state("jump", jump_condition, now)
        if decision.jump_active:
            decision.keys.append("SPACE")

        return decision


class ActionWindow:
    def __init__(self, thresholds: ActionThresholds, plot_window_s: float) -> None:
        self.th = thresholds
        self.plot_window_s = plot_window_s
        self.ts: deque[float] = deque()
        self.chest_dx: deque[float] = deque()
        self.hand_angle: deque[float] = deque()
        self.hand_gyro: deque[float] = deque()
        self.chest_acc: deque[float] = deque()
        self.start_t = time.monotonic()
        self.last_draw = 0.0
        self.pending_key: Optional[str] = None

        plt.ion()
        self.fig, axes = plt.subplots(4, 1, figsize=(11, 8), sharex=True)
        self.fig.canvas.manager.set_window_title("MoveToPlay Action Window")
        self.ax_dx, self.ax_angle, self.ax_gyro, self.ax_acc = axes

        self.status_text = self.fig.text(0.02, 0.97, "", va="top", family="monospace", fontsize=10)
        self.line_dx, = self.ax_dx.plot([], [], label="chest_delta_x")
        self.line_hand_angle, = self.ax_angle.plot([], [], label="hand_angle")
        self.line_hand_gyro, = self.ax_gyro.plot([], [], label="hand_gyro_max")
        self.line_chest_acc, = self.ax_acc.plot([], [], label="chest_acc_norm")

        self.ax_dx.axhline(self.th.left_dx, color="tab:blue", linestyle="--", linewidth=1, label="left")
        self.ax_dx.axhline(self.th.right_dx, color="tab:orange", linestyle="--", linewidth=1, label="right")
        self.ax_dx.axhline(0.0, color="0.6", linewidth=1)
        self.ax_angle.axhline(self.th.arm_raise_hand_angle_deg, color="tab:green", linestyle="--", linewidth=1)
        self.ax_gyro.axhline(self.th.wave_hand_gyro_dps, color="tab:red", linestyle="--", linewidth=1)
        self.ax_acc.axhline(self.th.jump_low_g, color="tab:purple", linestyle="--", linewidth=1)
        self.ax_acc.axhline(self.th.jump_high_g, color="tab:brown", linestyle="--", linewidth=1)

        self.ax_dx.set_ylabel("chest dx")
        self.ax_angle.set_ylabel("hand deg")
        self.ax_gyro.set_ylabel("gyro dps")
        self.ax_acc.set_ylabel("acc g")
        self.ax_acc.set_xlabel("time (s)")

        for ax in axes:
            ax.grid(True)
            ax.legend(loc="upper left")

        self.fig.subplots_adjust(top=0.78, hspace=0.24)
        self.fig.canvas.mpl_connect("key_press_event", self._on_key_press)

    def _on_key_press(self, event) -> None:
        if event is None or event.key is None:
            return
        key = str(event.key).lower()
        if key == "q":
            self.pending_key = key

    def poll_key(self) -> Optional[str]:
        key = self.pending_key
        self.pending_key = None
        return key

    def append(self, now: float, decision: ActionDecision, features: dict[int, NodeFeature]) -> None:
        t = now - self.start_t
        self.ts.append(t)
        self.chest_dx.append(decision.chest_dx if decision.chest_dx is not None else math.nan)
        self.hand_angle.append(decision.hand_angle if decision.hand_angle is not None else math.nan)
        self.hand_gyro.append(decision.hand_gyro_max if decision.hand_gyro_max is not None else math.nan)
        chest = features.get(NODE_CHEST)
        self.chest_acc.append(chest.acc_norm if chest is not None else math.nan)

        cutoff = t - self.plot_window_s
        while self.ts and self.ts[0] < cutoff:
            self.ts.popleft()
            self.chest_dx.popleft()
            self.hand_angle.popleft()
            self.hand_gyro.popleft()
            self.chest_acc.popleft()

    def draw(self, now: float, decision: ActionDecision, features: dict[int, NodeFeature]) -> None:
        if now - self.last_draw < 0.05:
            return
        self.last_draw = now

        xs = np.array(self.ts, dtype=float)
        if xs.size:
            self.line_dx.set_data(xs, np.array(self.chest_dx, dtype=float))
            self.line_hand_angle.set_data(xs, np.array(self.hand_angle, dtype=float))
            self.line_hand_gyro.set_data(xs, np.array(self.hand_gyro, dtype=float))
            self.line_chest_acc.set_data(xs, np.array(self.chest_acc, dtype=float))
            right = max(self.plot_window_s, float(xs[-1]))
            left = max(0.0, right - self.plot_window_s)
            for ax in (self.ax_dx, self.ax_angle, self.ax_gyro, self.ax_acc):
                ax.set_xlim(left, right)

        self.ax_dx.set_ylim(-0.70, 0.70)
        self.ax_angle.set_ylim(0.0, 120.0)
        self.ax_gyro.set_ylim(0.0, max(260.0, (decision.hand_gyro_max or 0.0) + 30.0))
        self.ax_acc.set_ylim(0.0, max(4.5, (decision.jump_acc_max or 0.0) + 0.5))

        chest = features.get(NODE_CHEST)
        hand = features.get(NODE_HAND)
        chest_link = format_feature_link(chest)
        hand_link = format_feature_link(hand)
        keys = "+".join(dict.fromkeys(decision.keys)) if decision.keys else "-"
        status = (
            "Press q to quit. This window only displays decisions; it does not press keys.\n"
            f"decision: move={decision.move:<9} keys={keys:<12} "
            f"arm_raise={'ON' if decision.arm_raise else 'off'} "
            f"wave={'PULSE' if decision.wave_active else 'off'} "
            f"jump={'PULSE' if decision.jump_active else 'off'}\n"
            f"features: chest_dx={fmt(decision.chest_dx)} "
            f"chest_angle={fmt(decision.chest_angle, 'deg')} "
            f"hand_angle={fmt(decision.hand_angle, 'deg')} "
            f"hand_gyro_max={fmt(decision.hand_gyro_max, 'dps')} "
            f"ratio={fmt(decision.gyro_ratio)} "
            f"jump_acc={fmt(decision.jump_acc_min, 'g')}..{fmt(decision.jump_acc_max, 'g')}\n"
            f"links: {chest_link} | {hand_link}"
        )
        self.status_text.set_text(status)
        self.fig.canvas.draw()
        self.fig.canvas.flush_events()


def fmt(value: Optional[float], suffix: str = "") -> str:
    if value is None or not math.isfinite(value):
        return "n/a"
    if suffix:
        return f"{value:.2f}{suffix}"
    return f"{value:.2f}"


def format_feature_link(feature: Optional[NodeFeature]) -> str:
    if feature is None:
        return "offline"
    link = "ok" if feature.link_ok else "stale"
    return f"{feature.name}:seq={feature.seq} age={feature.age_s*1000.0:.0f}ms acc={feature.acc_norm:.2f}g link={link}"


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="MoveToPlay dual-node action output window")
    parser.add_argument("--port", required=True, help="Serial port, e.g. COM20")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--calib", type=Path, default=Path("build/action_calibration_table.json"))
    parser.add_argument("--link-timeout-ms", type=float, default=300.0)
    parser.add_argument("--plot-window-s", type=float, default=8.0)
    parser.add_argument("--left-dx", type=float, default=-0.25)
    parser.add_argument("--right-dx", type=float, default=0.25)
    parser.add_argument("--neutral-dx", type=float, default=0.15)
    parser.add_argument("--arm-raise-angle", type=float, default=45.0)
    parser.add_argument("--wave-gyro", type=float, default=120.0)
    parser.add_argument("--jump-low-g", type=float, default=0.75)
    parser.add_argument("--jump-high-g", type=float, default=1.35)
    parser.add_argument("--dynamic-window-s", type=float, default=0.30)
    parser.add_argument("--jump-pair-window-s", type=float, default=0.55)
    parser.add_argument("--event-hold-s", type=float, default=0.28)
    parser.add_argument("--event-cooldown-s", type=float, default=0.60)
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()
    thresholds = ActionThresholds(
        left_dx=args.left_dx,
        right_dx=args.right_dx,
        neutral_dx=args.neutral_dx,
        arm_raise_hand_angle_deg=args.arm_raise_angle,
        wave_hand_gyro_dps=args.wave_gyro,
        jump_low_g=args.jump_low_g,
        jump_high_g=args.jump_high_g,
        dynamic_window_s=args.dynamic_window_s,
        jump_pair_window_s=args.jump_pair_window_s,
        event_hold_s=args.event_hold_s,
        event_cooldown_s=args.event_cooldown_s,
    )
    link_timeout_s = args.link_timeout_ms / 1000.0

    try:
        profile = load_calibration_profile(args.calib)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"[error] failed to load calibration table: {args.calib}: {exc}")
        return 1

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.02)
    except serial.SerialException as exc:
        print(f"[error] open serial failed: {exc}")
        return 1

    print("[info] action window started")
    print(f"[info] serial={args.port} baud={args.baud}")
    print(f"[info] calibration={args.calib}")
    print("[info] mappings shown only: LEFT=A, RIGHT=D, ARM_RAISE=W, WAVE=E, JUMP=SPACE")
    print("[info] press q in terminal or figure window to quit")

    latest_packets: dict[int, Packet] = {}
    latest_features: dict[int, NodeFeature] = {}
    history = SampleHistory()
    recognizer = ActionRecognizer(thresholds)
    window = ActionWindow(thresholds, plot_window_s=args.plot_window_s)
    last_logic_t = 0.0
    last_print_t = 0.0
    decision = ActionDecision()

    with NonBlockingKeyReader() as key_reader:
        try:
            while True:
                now = time.monotonic()
                raw = ser.readline().decode("utf-8", errors="ignore").strip()
                if raw:
                    packet = parse_packet(raw, arrival_time=now)
                    if packet is not None and packet.node_id in (NODE_CHEST, NODE_HAND):
                        latest_packets[packet.node_id] = packet
                        feature = feature_from_packet(packet, profile, now, link_timeout_s)
                        latest_features[packet.node_id] = feature
                        history.add(packet.node_id, now, feature.acc_norm, feature.gyro_norm)

                for node_id, packet in latest_packets.items():
                    if node_id in latest_features:
                        latest_features[node_id].age_s = max(0.0, now - packet.arrival_time)
                        latest_features[node_id].link_ok = latest_features[node_id].age_s <= link_timeout_s

                if now - last_logic_t >= 0.04:
                    last_logic_t = now
                    decision = recognizer.update(latest_features, history, now)
                    window.append(now, decision, latest_features)

                if decision.wave_triggered:
                    print("\n[event] right_hand_wave -> E")
                    decision.wave_triggered = False
                if decision.jump_triggered:
                    print("\n[event] standing_jump -> SPACE")
                    decision.jump_triggered = False

                if now - last_print_t >= 0.25:
                    last_print_t = now
                    keys = "+".join(dict.fromkeys(decision.keys)) if decision.keys else "-"
                    print(
                        "\r"
                        f"move={decision.move:<9} keys={keys:<12} "
                        f"chest_dx={fmt(decision.chest_dx)} "
                        f"hand_angle={fmt(decision.hand_angle, 'deg')} "
                        f"wave_g={fmt(decision.hand_gyro_max, 'dps')} "
                        f"jump_acc={fmt(decision.jump_acc_min, 'g')}..{fmt(decision.jump_acc_max, 'g')}   ",
                        end="",
                        flush=True,
                    )

                window.draw(now, decision, latest_features)

                key = key_reader.poll()
                if key is None:
                    key = window.poll_key()
                if key is not None and key.lower() == "q":
                    break

                plt.pause(0.001)

        except KeyboardInterrupt:
            pass
        finally:
            print("\n[info] stopping...")
            ser.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
