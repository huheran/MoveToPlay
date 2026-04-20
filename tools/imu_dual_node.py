#!/usr/bin/env python3
"""
MoveToPlay Phase-5 minimal closed-loop verifier:
- chest(node=1) + hand(node=2)
- independent 6-axis fusion per node (Mahony, no mag)
- gyro bias calibration while both trackers are kept still
- T-pose calibration (windowed quaternion average)
- relative hand-to-chest pose output
- terminal status + matplotlib relative Euler curves

Notes:
- Internal rotation math uses quaternions only.
- Euler angles are display-only.
- Without magnetometer, yaw drifts over time.
"""

from __future__ import annotations

import argparse
import math
import os
import sys
import time
from collections import deque
from dataclasses import dataclass, field
from enum import Enum
from typing import Optional

import matplotlib.pyplot as plt
import numpy as np
import serial

MIN_FUSION_DT_S = 0.001
MAX_FUSION_DT_S = 0.100
ACC_FULL_TRUST_DEV_G = 0.10
ACC_ZERO_TRUST_DEV_G = 0.45


def accel_correction_weight(acc_norm: float) -> float:
    if not math.isfinite(acc_norm) or acc_norm <= 1e-6:
        return 0.0

    deviation = abs(acc_norm - 1.0)
    if deviation <= ACC_FULL_TRUST_DEV_G:
        return 1.0
    if deviation >= ACC_ZERO_TRUST_DEV_G:
        return 0.0

    span = ACC_ZERO_TRUST_DEV_G - ACC_FULL_TRUST_DEV_G
    return (ACC_ZERO_TRUST_DEV_G - deviation) / span


def quat_normalize(q: np.ndarray) -> np.ndarray:
    n = float(np.linalg.norm(q))
    if n < 1e-12:
        return np.array([1.0, 0.0, 0.0, 0.0], dtype=float)
    return q / n


def quat_multiply(q: np.ndarray, r: np.ndarray) -> np.ndarray:
    w0, x0, y0, z0 = q
    w1, x1, y1, z1 = r
    return np.array(
        [
            w0 * w1 - x0 * x1 - y0 * y1 - z0 * z1,
            w0 * x1 + x0 * w1 + y0 * z1 - z0 * y1,
            w0 * y1 - x0 * z1 + y0 * w1 + z0 * x1,
            w0 * z1 + x0 * y1 - y0 * x1 + z0 * w1,
        ],
        dtype=float,
    )


def quat_inverse(q: np.ndarray) -> np.ndarray:
    qn = quat_normalize(q)
    w, x, y, z = qn
    return np.array([w, -x, -y, -z], dtype=float)


def quat_to_euler_deg(q: np.ndarray) -> tuple[float, float, float]:
    w, x, y, z = quat_normalize(q)

    sinr_cosp = 2.0 * (w * x + y * z)
    cosr_cosp = 1.0 - 2.0 * (x * x + y * y)
    roll = math.degrees(math.atan2(sinr_cosp, cosr_cosp))

    sinp = 2.0 * (w * y - z * x)
    sinp = max(-1.0, min(1.0, sinp))
    pitch = math.degrees(math.asin(sinp))

    siny_cosp = 2.0 * (w * z + x * y)
    cosy_cosp = 1.0 - 2.0 * (y * y + z * z)
    yaw = math.degrees(math.atan2(siny_cosp, cosy_cosp))

    return roll, pitch, yaw


def quat_average_hemisphere(samples: list[np.ndarray]) -> Optional[np.ndarray]:
    if not samples:
        return None
    ref = quat_normalize(samples[0])
    acc = np.zeros(4, dtype=float)
    for raw_q in samples:
        q = quat_normalize(raw_q)
        if float(np.dot(q, ref)) < 0.0:
            q = -q
        acc += q
    return quat_normalize(acc)


class Mahony6DoF:
    def __init__(self, kp: float = 1.2, ki: float = 0.05) -> None:
        self.kp = kp
        self.ki = ki
        self.q = np.array([1.0, 0.0, 0.0, 0.0], dtype=float)
        self.integral = np.zeros(3, dtype=float)
        self.last_accel_weight = 0.0

    def reset(self) -> None:
        self.q = np.array([1.0, 0.0, 0.0, 0.0], dtype=float)
        self.integral = np.zeros(3, dtype=float)
        self.last_accel_weight = 0.0

    def update(self, gyro_dps: np.ndarray, accel_g: np.ndarray, dt: float) -> np.ndarray:
        if dt <= 0.0:
            return self.q.copy()

        acc_norm = float(np.linalg.norm(accel_g))
        acc_weight = accel_correction_weight(acc_norm)
        self.last_accel_weight = acc_weight
        gyro = np.radians(gyro_dps.astype(float))

        if acc_weight > 0.0:
            a = accel_g / acc_norm
            q0, q1, q2, q3 = self.q
            v = np.array(
                [
                    2.0 * (q1 * q3 - q0 * q2),
                    2.0 * (q0 * q1 + q2 * q3),
                    q0 * q0 - q1 * q1 - q2 * q2 + q3 * q3,
                ],
                dtype=float,
            )
            err = np.cross(v, a)
            weighted_err = err * acc_weight
            self.integral += self.ki * weighted_err * dt
            gyro = gyro + self.kp * weighted_err + self.integral

        q_dot = 0.5 * quat_multiply(self.q, np.array([0.0, gyro[0], gyro[1], gyro[2]]))
        self.q = quat_normalize(self.q + q_dot * dt)
        return self.q.copy()


@dataclass
class Packet:
    node_id: int
    seq: int
    ax: float
    ay: float
    az: float
    gx: float
    gy: float
    gz: float
    arrival_time: float
    board_timestamp_ms: Optional[float] = None
    timestamp_us: Optional[int] = None


def parse_packet(line: str, arrival_time: float) -> Optional[Packet]:
    if not line:
        return None
    try:
        parts = [part.strip() for part in line.strip().split(",")]
        if len(parts) != 8:
            return None

        board_timestamp_ms = float(parts[0]) if parts[0] else None
        return Packet(
            node_id=int(parts[1]),
            seq=int(round(board_timestamp_ms)) if board_timestamp_ms is not None else 0,
            ax=float(parts[2]),
            ay=float(parts[3]),
            az=float(parts[4]),
            gx=float(parts[5]),
            gy=float(parts[6]),
            gz=float(parts[7]),
            arrival_time=arrival_time,
            board_timestamp_ms=board_timestamp_ms,
            timestamp_us=int(round(board_timestamp_ms * 1000.0)) if board_timestamp_ms is not None else None,
        )
    except ValueError:
        return None


class CalibrationState(Enum):
    UNCALIBRATED = "UNCALIBRATED"
    CALIBRATING = "CALIBRATING"
    CALIBRATED = "CALIBRATED"


@dataclass
class SensorState:
    node_id: int
    name: str
    fusion: Mahony6DoF
    gyro_bias: np.ndarray = field(default_factory=lambda: np.zeros(3, dtype=float))
    q_live: np.ndarray = field(default_factory=lambda: np.array([1.0, 0.0, 0.0, 0.0], dtype=float))
    q_tpose: Optional[np.ndarray] = None
    q_corr: np.ndarray = field(default_factory=lambda: np.array([1.0, 0.0, 0.0, 0.0], dtype=float))
    last_seq: Optional[int] = None
    last_update_time: Optional[float] = None
    last_timestamp_us: Optional[int] = None
    last_dt_s: float = 0.0
    last_dt_source: str = "default"
    link_ok: bool = False
    accel_norm: float = 1.0
    accel_weight: float = 0.0
    bad_accel_count: int = 0
    seq_backwards_count: int = 0
    last_warned_bad_accel_count: int = 0
    last_warned_seq_backwards_count: int = 0
    latest_raw: Optional[Packet] = None

    def reset_attitude(self) -> None:
        self.fusion.reset()
        self.q_live = np.array([1.0, 0.0, 0.0, 0.0], dtype=float)
        self.q_tpose = None
        self.q_corr = np.array([1.0, 0.0, 0.0, 0.0], dtype=float)
        self.last_timestamp_us = None
        self.last_dt_s = 0.0
        self.last_dt_source = "default"

    def _compute_dt(
        self,
        packet: Packet,
        default_dt: float,
        use_arrival_dt: bool,
    ) -> float:
        dt = default_dt
        source = "default"

        if packet.timestamp_us is not None:
            timestamp_us = packet.timestamp_us
            if self.last_timestamp_us is not None:
                dt_sensor = (timestamp_us - self.last_timestamp_us) * 1e-6
                if MIN_FUSION_DT_S <= dt_sensor <= MAX_FUSION_DT_S:
                    dt = dt_sensor
                    source = "board_ts"
            self.last_timestamp_us = timestamp_us

        if source == "default" and use_arrival_dt and self.last_update_time is not None:
            dt_arrive = packet.arrival_time - self.last_update_time
            if MIN_FUSION_DT_S <= dt_arrive <= MAX_FUSION_DT_S:
                dt = dt_arrive
                source = "pc_arrival"

        dt = max(MIN_FUSION_DT_S, min(MAX_FUSION_DT_S, dt))
        self.last_dt_s = dt
        self.last_dt_source = source
        return dt

    def update_from_packet(self, packet: Packet, default_dt: float, use_arrival_dt: bool) -> bool:
        if self.last_seq is not None and packet.seq > 0 and packet.seq <= self.last_seq:
            self.seq_backwards_count += 1
            return False

        dt = self._compute_dt(packet, default_dt, use_arrival_dt)

        accel = np.array([packet.ax, packet.ay, packet.az], dtype=float)
        gyro = np.array([packet.gx, packet.gy, packet.gz], dtype=float) - self.gyro_bias
        self.accel_norm = float(np.linalg.norm(accel))
        if self.accel_norm < 0.5 or self.accel_norm > 2.0:
            self.bad_accel_count += 1

        self.q_live = self.fusion.update(gyro_dps=gyro, accel_g=accel, dt=dt)
        self.accel_weight = self.fusion.last_accel_weight
        if self.q_tpose is not None:
            self.q_corr = quat_normalize(quat_multiply(quat_inverse(self.q_tpose), self.q_live))
        else:
            self.q_corr = self.q_live.copy()

        self.last_seq = packet.seq
        self.last_update_time = packet.arrival_time
        self.latest_raw = packet
        self.link_ok = True
        return True


class GyroBiasCalibrationManager:
    def __init__(self, window_s: float = 3.0, min_samples: int = 20) -> None:
        self.window_s = window_s
        self.min_samples = min_samples
        self.active = False
        self.start_time: Optional[float] = None
        self.samples = {"chest": [], "hand": []}
        self.last_sampled_seq = {"chest": None, "hand": None}
        self.last_error: Optional[str] = None
        self.last_result: dict[str, np.ndarray] = {}

    def start(self, now: float) -> None:
        self.active = True
        self.start_time = now
        self.samples = {"chest": [], "hand": []}
        self.last_sampled_seq = {"chest": None, "hand": None}
        self.last_error = None
        self.last_result = {}

    def add_sample(self, sensor: SensorState, packet: Packet) -> None:
        if not self.active:
            return
        packet_key = packet.seq if packet.seq > 0 else int(round(packet.arrival_time * 1000.0))
        if self.last_sampled_seq[sensor.name] == packet_key:
            return
        self.samples[sensor.name].append(np.array([packet.gx, packet.gy, packet.gz], dtype=float))
        self.last_sampled_seq[sensor.name] = packet_key

    def should_finish(self, now: float) -> bool:
        if not self.active or self.start_time is None:
            return False
        return (now - self.start_time) >= self.window_s

    def finish(self, sensors: dict[int, SensorState]) -> bool:
        if (
            len(self.samples["chest"]) < self.min_samples
            or len(self.samples["hand"]) < self.min_samples
        ):
            self.active = False
            self.last_error = (
                "Not enough still samples: "
                f"chest={len(self.samples['chest'])}, hand={len(self.samples['hand'])}, "
                f"min={self.min_samples}"
            )
            return False

        result = {}
        for sensor in sensors.values():
            bias = np.mean(np.array(self.samples[sensor.name], dtype=float), axis=0)
            sensor.gyro_bias = bias
            sensor.reset_attitude()
            result[sensor.name] = bias

        self.active = False
        self.last_result = result
        self.last_error = None
        return True


class CalibrationManager:
    def __init__(self, window_s: float = 2.5, min_samples: int = 30) -> None:
        self.window_s = window_s
        self.min_samples = min_samples
        self.state = CalibrationState.UNCALIBRATED
        self.start_time: Optional[float] = None
        self.samples = {"chest": [], "hand": []}
        self.last_sampled_seq = {"chest": None, "hand": None}
        self.last_error: Optional[str] = None

    def start(self, now: float) -> None:
        self.state = CalibrationState.CALIBRATING
        self.start_time = now
        self.samples = {"chest": [], "hand": []}
        self.last_sampled_seq = {"chest": None, "hand": None}
        self.last_error = None

    def add_sample(self, sensor: SensorState) -> None:
        if self.state != CalibrationState.CALIBRATING:
            return
        if sensor.last_seq is None:
            return
        if self.last_sampled_seq[sensor.name] == sensor.last_seq:
            return
        self.samples[sensor.name].append(sensor.q_live.copy())
        self.last_sampled_seq[sensor.name] = sensor.last_seq

    def should_finish(self, now: float) -> bool:
        if self.state != CalibrationState.CALIBRATING or self.start_time is None:
            return False
        return (now - self.start_time) >= self.window_s

    def finish(self, chest: SensorState, hand: SensorState) -> bool:
        chest_avg = quat_average_hemisphere(self.samples["chest"])
        hand_avg = quat_average_hemisphere(self.samples["hand"])

        if (
            chest_avg is None
            or hand_avg is None
            or len(self.samples["chest"]) < self.min_samples
            or len(self.samples["hand"]) < self.min_samples
        ):
            self.state = CalibrationState.UNCALIBRATED
            self.last_error = (
                "Not enough samples: "
                f"chest={len(self.samples['chest'])}, hand={len(self.samples['hand'])}, "
                f"min={self.min_samples}"
            )
            return False

        chest.q_tpose = quat_normalize(chest_avg)
        hand.q_tpose = quat_normalize(hand_avg)
        chest.q_corr = quat_normalize(quat_multiply(quat_inverse(chest.q_tpose), chest.q_live))
        hand.q_corr = quat_normalize(quat_multiply(quat_inverse(hand.q_tpose), hand.q_live))
        self.state = CalibrationState.CALIBRATED
        self.last_error = None
        return True


class RelativePoseComputer:
    def __init__(self, max_pair_dt_s: float = 0.04) -> None:
        self.max_pair_dt_s = max_pair_dt_s

    def compute(
        self,
        chest: SensorState,
        hand: SensorState,
        calib_state: CalibrationState,
        link_timeout_s: float,
        now: float,
    ) -> tuple[Optional[np.ndarray], Optional[tuple[float, float, float]], str]:
        if calib_state != CalibrationState.CALIBRATED:
            return None, None, "not_calibrated"
        if chest.last_update_time is None or hand.last_update_time is None:
            return None, None, "no_data"
        if (now - chest.last_update_time) > link_timeout_s or (now - hand.last_update_time) > link_timeout_s:
            return None, None, "link_stale"

        if chest.last_dongle_time_s is not None and hand.last_dongle_time_s is not None:
            pair_dt = abs(chest.last_dongle_time_s - hand.last_dongle_time_s)
        else:
            pair_dt = abs(chest.last_update_time - hand.last_update_time)
        if pair_dt > self.max_pair_dt_s:
            return None, None, f"unsynced({pair_dt*1000.0:.1f}ms)"

        q_rel = quat_normalize(quat_multiply(quat_inverse(chest.q_corr), hand.q_corr))
        rel_euler = quat_to_euler_deg(q_rel)
        return q_rel, rel_euler, "ok"


class RelativePlotter:
    def __init__(self, window_s: float, dt_hint: float) -> None:
        self.maxlen = max(200, int(window_s / max(0.001, dt_hint)))
        self.ts = deque(maxlen=self.maxlen)
        self.roll = deque(maxlen=self.maxlen)
        self.pitch = deque(maxlen=self.maxlen)
        self.yaw = deque(maxlen=self.maxlen)
        self.last_draw = 0.0

        plt.ion()
        self.fig, self.ax = plt.subplots(figsize=(10, 5))
        self.l_roll, = self.ax.plot([], [], label="rel_roll")
        self.l_pitch, = self.ax.plot([], [], label="rel_pitch")
        self.l_yaw, = self.ax.plot([], [], label="rel_yaw")
        self.status_text = self.ax.text(0.02, 0.95, "", transform=self.ax.transAxes, va="top")
        self.ax.set_title("Relative Euler (Hand vs Chest)")
        self.ax.set_xlabel("time (s)")
        self.ax.set_ylabel("deg")
        self.ax.grid(True)
        self.ax.legend(loc="upper left")
        self.pending_key: Optional[str] = None
        self.fig.canvas.mpl_connect("key_press_event", self._on_key_press)

    def _on_key_press(self, event) -> None:
        if event is None or event.key is None:
            return
        key = str(event.key).lower()
        if key in ("c", "q"):
            self.pending_key = key

    def poll_key(self) -> Optional[str]:
        if self.pending_key is None:
            return None
        key = self.pending_key
        self.pending_key = None
        return key

    def update(self, t: float, euler_deg: tuple[float, float, float], status: str) -> None:
        r, p, y = euler_deg
        self.ts.append(t)
        self.roll.append(r)
        self.pitch.append(p)
        self.yaw.append(y)

        now = time.time()
        if now - self.last_draw < 0.05:
            return
        self.last_draw = now

        xs = np.array(self.ts, dtype=float)
        if xs.size == 0:
            return
        xs = xs - xs[0]
        r_arr = np.array(self.roll, dtype=float)
        p_arr = np.array(self.pitch, dtype=float)
        y_arr = np.array(self.yaw, dtype=float)
        self.l_roll.set_data(xs, r_arr)
        self.l_pitch.set_data(xs, p_arr)
        self.l_yaw.set_data(xs, y_arr)

        right = max(10.0, float(xs[-1]))
        left = max(0.0, right - 10.0)
        self.ax.set_xlim(left, right)

        y_all = np.concatenate([r_arr, p_arr, y_arr])
        ymin = float(np.min(y_all)) - 5.0
        ymax = float(np.max(y_all)) + 5.0
        if (ymax - ymin) < 20.0:
            c = 0.5 * (ymax + ymin)
            ymin, ymax = c - 10.0, c + 10.0
        self.ax.set_ylim(ymin, ymax)

        self.status_text.set_text(status)
        self.fig.canvas.draw()
        self.fig.canvas.flush_events()


class NonBlockingKeyReader:
    def __init__(self) -> None:
        self._is_windows = os.name == "nt"
        self._enabled = False
        self._fd: Optional[int] = None
        self._old_term = None

    def __enter__(self) -> "NonBlockingKeyReader":
        if not self._is_windows and sys.stdin.isatty():
            import termios
            import tty

            self._fd = sys.stdin.fileno()
            self._old_term = termios.tcgetattr(self._fd)
            tty.setcbreak(self._fd)
            self._enabled = True
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        if self._enabled and self._fd is not None and self._old_term is not None:
            import termios

            termios.tcsetattr(self._fd, termios.TCSADRAIN, self._old_term)

    def poll(self) -> Optional[str]:
        if self._is_windows:
            import msvcrt

            if msvcrt.kbhit():
                return msvcrt.getwch()
            return None

        if not self._enabled:
            return None

        import select

        ready, _, _ = select.select([sys.stdin], [], [], 0.0)
        if ready:
            return sys.stdin.read(1)
        return None


class DualNodeSession:
    def __init__(
        self,
        mahony_kp: float,
        mahony_ki: float,
        default_dt: float,
        use_arrival_dt: bool,
        link_timeout_s: float,
        pair_threshold_s: float,
        calib_window_s: float,
        gyro_bias_window_s: float,
        gyro_bias_min_samples: int,
    ) -> None:
        self.sensors = {
            1: SensorState(node_id=1, name="chest", fusion=Mahony6DoF(kp=mahony_kp, ki=mahony_ki)),
            2: SensorState(node_id=2, name="hand", fusion=Mahony6DoF(kp=mahony_kp, ki=mahony_ki)),
        }
        self.default_dt = default_dt
        self.use_arrival_dt = use_arrival_dt
        self.link_timeout_s = link_timeout_s
        self.gyro_bias_calib = GyroBiasCalibrationManager(
            window_s=gyro_bias_window_s,
            min_samples=gyro_bias_min_samples,
        )
        self.calib = CalibrationManager(window_s=calib_window_s)
        self.rel = RelativePoseComputer(max_pair_dt_s=pair_threshold_s)
        self.rel_last_status = "not_calibrated"

    def update_from_packet(self, packet: Packet) -> None:
        sensor = self.sensors.get(packet.node_id)
        if sensor is None:
            return
        updated = sensor.update_from_packet(packet, default_dt=self.default_dt, use_arrival_dt=self.use_arrival_dt)
        if updated and self.gyro_bias_calib.active:
            self.gyro_bias_calib.add_sample(sensor, packet)
        if updated and self.calib.state == CalibrationState.CALIBRATING:
            self.calib.add_sample(sensor)

    def start_gyro_bias_calibration(self, now: float) -> None:
        self.gyro_bias_calib.start(now)

    def finish_gyro_bias_calibration(self) -> bool:
        return self.gyro_bias_calib.finish(self.sensors)

    def start_calibration(self, now: float) -> None:
        self.calib.start(now)

    def tick(self, now: float) -> None:
        for sensor in self.sensors.values():
            if sensor.last_update_time is None:
                sensor.link_ok = False
            else:
                sensor.link_ok = (now - sensor.last_update_time) <= self.link_timeout_s

        if self.calib.should_finish(now):
            self.calib.finish(self.sensors[1], self.sensors[2])

    def compute_relative_pose(
        self, now: float
    ) -> tuple[Optional[np.ndarray], Optional[tuple[float, float, float]], str]:
        q_rel, e_rel, status = self.rel.compute(
            chest=self.sensors[1],
            hand=self.sensors[2],
            calib_state=self.calib.state,
            link_timeout_s=self.link_timeout_s,
            now=now,
        )
        self.rel_last_status = status
        return q_rel, e_rel, status


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="MoveToPlay Phase-5 dual-node minimal verifier")
    p.add_argument("--port", required=True, help="Serial port, e.g. COM7 or /dev/ttyUSB0")
    p.add_argument("--baud", type=int, default=115200, help="Baud rate")
    p.add_argument("--dt", type=float, default=0.01, help="Fallback dt in seconds")
    p.add_argument("--use-arrival-dt", action="store_true", help="Use host arrival delta if no timestamp_us")
    p.add_argument("--mahony-kp", type=float, default=1.2)
    p.add_argument("--mahony-ki", type=float, default=0.05)
    p.add_argument("--link-timeout-ms", type=float, default=300.0)
    p.add_argument("--pair-threshold-ms", type=float, default=40.0)
    p.add_argument("--calib-window-s", type=float, default=2.5)
    p.add_argument("--gyro-bias-window-s", type=float, default=3.0)
    p.add_argument("--gyro-bias-min-samples", type=int, default=20)
    p.add_argument("--auto-gyro-bias", action="store_true", help="Start gyro-bias calibration after opening serial")
    p.add_argument("--exit-after-gyro-bias", action="store_true", help="Exit after auto gyro-bias calibration finishes")
    p.add_argument("--plot-window-s", type=float, default=10.0)
    return p


def format_sensor_brief(sensor: SensorState) -> str:
    if sensor.last_seq is None:
        return f"{sensor.name}: offline"
    roll, pitch, yaw = quat_to_euler_deg(sensor.q_live)
    link_label = "ok" if sensor.link_ok else "stale"
    return (
        f"{sensor.name}:seq={sensor.last_seq:<7d} "
        f"euler=({roll:>6.1f},{pitch:>6.1f},{yaw:>6.1f}) "
        f"acc={sensor.accel_norm:>4.2f}g "
        f"aw={sensor.accel_weight:.2f} "
        f"dt={sensor.last_dt_s*1000.0:>5.1f}ms/{sensor.last_dt_source} "
        f"link={link_label}"
    )


def main() -> int:
    args = build_arg_parser().parse_args()

    link_timeout_s = args.link_timeout_ms / 1000.0
    pair_threshold_s = args.pair_threshold_ms / 1000.0

    session = DualNodeSession(
        mahony_kp=args.mahony_kp,
        mahony_ki=args.mahony_ki,
        default_dt=args.dt,
        use_arrival_dt=args.use_arrival_dt,
        link_timeout_s=link_timeout_s,
        pair_threshold_s=pair_threshold_s,
        calib_window_s=args.calib_window_s,
        gyro_bias_window_s=args.gyro_bias_window_s,
        gyro_bias_min_samples=args.gyro_bias_min_samples,
    )
    plotter = RelativePlotter(window_s=args.plot_window_s, dt_hint=args.dt)

    print("[info] opening serial...")
    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.02)
    except serial.SerialException as exc:
        print(f"[error] open serial failed: {exc}")
        return 1

    print("[info] phase-5 minimal closed-loop started")
    print("[info] press 'b' for gyro-bias calibration, 'c' for T-pose calibration, 'q' to quit")
    print("[info] key input works in terminal and in the matplotlib figure window")
    print("[info] roll/pitch are relatively trustworthy; yaw drifts without magnetometer")

    start_t = time.monotonic()
    if args.auto_gyro_bias:
        session.start_gyro_bias_calibration(start_t)
        print("[gyro-bias] started: keep both trackers still for "
              f"{session.gyro_bias_calib.window_s:.1f}s")

    last_print_t = 0.0
    last_warn_t = 0.0

    with NonBlockingKeyReader() as key_reader:
        try:
            while True:
                now = time.monotonic()
                prev_calib_state = session.calib.state
                prev_gyro_bias_active = session.gyro_bias_calib.active
                raw = ser.readline().decode("utf-8", errors="ignore").strip()
                if raw:
                    pkt = parse_packet(raw, arrival_time=now)
                    if pkt is not None:
                        session.update_from_packet(pkt)

                session.tick(now)

                key = key_reader.poll()
                if key is None:
                    key = plotter.poll_key()
                if key is not None:
                    key = key.lower()
                    if key == "q":
                        break
                    if key == "b":
                        session.start_gyro_bias_calibration(now)
                        print("\n[gyro-bias] started: keep both trackers still for "
                              f"{session.gyro_bias_calib.window_s:.1f}s")
                    if key == "c":
                        session.start_calibration(now)
                        print("\n[calib] started: hold T-pose for "
                              f"{session.calib.window_s:.1f}s (collecting chest+hand)")

                if session.gyro_bias_calib.should_finish(now):
                    if session.finish_gyro_bias_calibration():
                        chest_bias = session.gyro_bias_calib.last_result["chest"]
                        hand_bias = session.gyro_bias_calib.last_result["hand"]
                        print(
                            "\n[gyro-bias] success: "
                            f"chest=({chest_bias[0]:.4f},{chest_bias[1]:.4f},{chest_bias[2]:.4f}) dps, "
                            f"hand=({hand_bias[0]:.4f},{hand_bias[1]:.4f},{hand_bias[2]:.4f}) dps"
                        )
                        print("[gyro-bias] attitude reset; press 'c' for T-pose calibration next")
                        if args.exit_after_gyro_bias:
                            break
                    elif session.gyro_bias_calib.last_error:
                        print(f"\n[gyro-bias] failed: {session.gyro_bias_calib.last_error}")
                        if args.exit_after_gyro_bias:
                            break
                elif prev_gyro_bias_active and not session.gyro_bias_calib.active and session.gyro_bias_calib.last_error:
                    print(f"\n[gyro-bias] failed: {session.gyro_bias_calib.last_error}")

                if prev_calib_state == CalibrationState.CALIBRATING and session.calib.state != CalibrationState.CALIBRATING:
                    if session.calib.state == CalibrationState.CALIBRATED:
                        print(
                            "\n[calib] success: "
                            f"chest_samples={len(session.calib.samples['chest'])}, "
                            f"hand_samples={len(session.calib.samples['hand'])}"
                        )
                    elif session.calib.last_error:
                        print(f"\n[calib] failed: {session.calib.last_error}")

                q_rel, rel_euler, rel_status = session.compute_relative_pose(now)
                chest = session.sensors[1]
                hand = session.sensors[2]

                if now - last_warn_t > 1.0:
                    last_warn_t = now
                    for sensor in (chest, hand):
                        if (
                            sensor.seq_backwards_count >= 5
                            and sensor.seq_backwards_count % 5 == 0
                            and sensor.seq_backwards_count != sensor.last_warned_seq_backwards_count
                        ):
                            sensor.last_warned_seq_backwards_count = sensor.seq_backwards_count
                            print(f"\n[warn] {sensor.name} seq rollback count={sensor.seq_backwards_count}")
                        if (
                            sensor.bad_accel_count >= 20
                            and sensor.bad_accel_count % 20 == 0
                            and sensor.bad_accel_count != sensor.last_warned_bad_accel_count
                        ):
                            sensor.last_warned_bad_accel_count = sensor.bad_accel_count
                            print(
                                f"\n[warn] {sensor.name} accel norm abnormal count={sensor.bad_accel_count}, "
                                "fusion falls back to gyro dominance"
                            )

                if now - last_print_t > 0.1:
                    last_print_t = now
                    state = "GYRO_BIAS" if session.gyro_bias_calib.active else session.calib.state.value
                    rel_text = "rel: n/a"
                    if rel_euler is not None:
                        rr, rp, ry = rel_euler
                        rel_text = f"rel=({rr:>6.1f},{rp:>6.1f},{ry:>6.1f})"
                    else:
                        rel_text = f"rel: {rel_status}"
                    line = (
                        f"\r[{state}] "
                        f"{format_sensor_brief(chest)} | "
                        f"{format_sensor_brief(hand)} | "
                        f"{rel_text}   "
                    )
                    print(line, end="", flush=True)

                if rel_euler is not None:
                    plotter.update(now - start_t, rel_euler, f"state={session.calib.state.value}, rel={rel_status}")

                plt.pause(0.001)

        except KeyboardInterrupt:
            pass
        finally:
            print("\n[info] stopping...")
            ser.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
