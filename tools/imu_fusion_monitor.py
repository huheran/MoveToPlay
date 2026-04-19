#!/usr/bin/env python3
"""
Minimal PC-side IMU fusion monitor for MoveToPlay.

Features:
- Read tracker IMU data from serial output forwarded by dongle
- Robustly parse IMU lines and skip unrelated logs
- 3-second gyro bias calibration while device is kept still
- 6-axis Mahony attitude fusion (acc + gyro, no magnetometer)
- Real-time terminal output and matplotlib Euler plot
- Basic motion flag
- CSV logging of raw, corrected, and fused data

Notes:
- roll / pitch are generally more trustworthy
- yaw will drift over time because there is no magnetometer
"""

from __future__ import annotations

import argparse
import csv
import math
import re
import sys
import time
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import matplotlib.pyplot as plt
import numpy as np
import serial


IMU_LINE_RE = re.compile(
    r"seq=(?P<seq>\d+).*?"
    r"ax=(?P<ax>[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?),"
    r"ay=(?P<ay>[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?),"
    r"az=(?P<az>[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?),"
    r"gx=(?P<gx>[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?),"
    r"gy=(?P<gy>[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?),"
    r"gz=(?P<gz>[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?)"
)
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


@dataclass
class ImuSample:
    seq: int
    ax: float
    ay: float
    az: float
    gx: float
    gy: float
    gz: float
    dt: float
    monotonic_time: float


class Mahony6DoF:
    """Compact Mahony AHRS for accel + gyro only."""

    def __init__(self, kp: float = 1.2, ki: float = 0.05) -> None:
        self.kp = kp
        self.ki = ki
        self.q = np.array([1.0, 0.0, 0.0, 0.0], dtype=float)
        self.integral = np.zeros(3, dtype=float)
        self.last_accel_weight = 0.0

    def update(self, gyro_dps: np.ndarray, accel_g: np.ndarray, dt: float) -> np.ndarray:
        if dt <= 0.0:
            return self.q.copy()

        acc_norm = np.linalg.norm(accel_g)
        acc_weight = accel_correction_weight(float(acc_norm))
        self.last_accel_weight = acc_weight
        gyro = np.radians(gyro_dps.astype(float))

        if acc_weight > 0.0:
            a = accel_g / acc_norm
            q0, q1, q2, q3 = self.q

            # Estimated gravity direction from current quaternion.
            v = np.array(
                [
                    2.0 * (q1 * q3 - q0 * q2),
                    2.0 * (q0 * q1 + q2 * q3),
                    q0 * q0 - q1 * q1 - q2 * q2 + q3 * q3,
                ],
                dtype=float,
            )

            error = np.cross(v, a)
            weighted_error = error * acc_weight
            self.integral += self.ki * weighted_error * dt
            gyro = gyro + self.kp * weighted_error + self.integral

        q_dot = 0.5 * quat_multiply(self.q, np.array([0.0, gyro[0], gyro[1], gyro[2]]))
        self.q = self.q + q_dot * dt
        self.q = self.q / np.linalg.norm(self.q)
        return self.q.copy()


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


def quat_to_euler_deg(q: np.ndarray) -> tuple[float, float, float]:
    w, x, y, z = q

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


def parse_imu_line(line: str, dt: float, monotonic_time: float) -> Optional[ImuSample]:
    match = IMU_LINE_RE.search(line)
    if not match:
        return None

    try:
        return ImuSample(
            seq=int(match.group("seq")),
            ax=float(match.group("ax")),
            ay=float(match.group("ay")),
            az=float(match.group("az")),
            gx=float(match.group("gx")),
            gy=float(match.group("gy")),
            gz=float(match.group("gz")),
            dt=dt,
            monotonic_time=monotonic_time,
        )
    except ValueError:
        return None


class Plotter:
    def __init__(self, window_seconds: float, nominal_dt: float) -> None:
        self.maxlen = max(100, int(window_seconds / nominal_dt))
        self.ts = deque(maxlen=self.maxlen)
        self.roll = deque(maxlen=self.maxlen)
        self.pitch = deque(maxlen=self.maxlen)
        self.yaw = deque(maxlen=self.maxlen)

        plt.ion()
        self.fig, self.ax = plt.subplots(figsize=(10, 5))
        self.line_roll, = self.ax.plot([], [], label="roll")
        self.line_pitch, = self.ax.plot([], [], label="pitch")
        self.line_yaw, = self.ax.plot([], [], label="yaw")
        self.ax.set_title("MoveToPlay Euler Angles")
        self.ax.set_xlabel("time (s)")
        self.ax.set_ylabel("deg")
        self.ax.grid(True)
        self.ax.legend(loc="upper left")
        self.status_text = self.ax.text(0.02, 0.95, "", transform=self.ax.transAxes, va="top")
        self.last_draw = 0.0

    def update(self, t: float, roll: float, pitch: float, yaw: float, status: str) -> None:
        self.ts.append(t)
        self.roll.append(roll)
        self.pitch.append(pitch)
        self.yaw.append(yaw)

        now = time.time()
        if now - self.last_draw < 0.05:
            return
        self.last_draw = now

        xs = np.array(self.ts, dtype=float)
        if xs.size == 0:
            return

        xs = xs - xs[0]
        self.line_roll.set_data(xs, np.array(self.roll))
        self.line_pitch.set_data(xs, np.array(self.pitch))
        self.line_yaw.set_data(xs, np.array(self.yaw))
        self.ax.set_xlim(max(0.0, xs[-1] - (xs[-1] if xs[-1] < 10.0 else 10.0)), max(10.0, xs[-1]))

        y_all = np.concatenate([np.array(self.roll), np.array(self.pitch), np.array(self.yaw)])
        y_min = float(np.min(y_all)) - 5.0
        y_max = float(np.max(y_all)) + 5.0
        if y_max - y_min < 20.0:
            center = 0.5 * (y_max + y_min)
            y_min = center - 10.0
            y_max = center + 10.0
        self.ax.set_ylim(y_min, y_max)
        self.status_text.set_text(status)
        self.fig.canvas.draw()
        self.fig.canvas.flush_events()


def open_csv_writer(output_dir: Path) -> tuple[csv.writer, object]:
    output_dir.mkdir(parents=True, exist_ok=True)
    path = output_dir / f"imu_fusion_log_{time.strftime('%Y%m%d_%H%M%S')}.csv"
    csv_file = path.open("w", newline="", encoding="utf-8")
    writer = csv.writer(csv_file)
    writer.writerow(
        [
            "pc_time_s",
            "seq",
            "dt",
            "ax",
            "ay",
            "az",
            "gx",
            "gy",
            "gz",
            "gx_corr",
            "gy_corr",
            "gz_corr",
            "accel_norm",
            "qw",
            "qx",
            "qy",
            "qz",
            "roll",
            "pitch",
            "yaw",
            "motion_flag",
            "status",
        ]
    )
    print(f"[info] CSV logging -> {path}")
    return writer, csv_file


def calibrate_gyro_bias(ser: serial.Serial, seconds: float, nominal_dt: float) -> np.ndarray:
    print(f"[calib] Keep device still for {seconds:.1f} seconds...")
    samples = []
    start = time.monotonic()
    last_sample_time = start
    skipped = 0

    while time.monotonic() - start < seconds:
        line = ser.readline().decode("utf-8", errors="ignore").strip()
        now = time.monotonic()
        sample = parse_imu_line(line, nominal_dt, now)
        if sample is None:
            skipped += 1
            continue
        samples.append([sample.gx, sample.gy, sample.gz])
        last_sample_time = now

        remaining = max(0.0, seconds - (now - start))
        if len(samples) % 20 == 0:
            print(f"[calib] collected={len(samples)} remaining={remaining:.1f}s")

    if not samples:
        raise RuntimeError("No valid IMU samples received during calibration.")

    bias = np.mean(np.array(samples, dtype=float), axis=0)
    print(
        "[calib] gyro bias (dps): "
        f"gx={bias[0]:.4f}, gy={bias[1]:.4f}, gz={bias[2]:.4f} "
        f"(valid_samples={len(samples)}, skipped_lines={skipped})"
    )
    return bias


def estimate_motion_flag(
    gyro_corr_dps: np.ndarray,
    current_euler_deg: np.ndarray,
    prev_euler_deg: Optional[np.ndarray],
    dt: float,
    gyro_threshold_dps: float,
    euler_rate_threshold_dps: float,
) -> bool:
    gyro_motion = float(np.max(np.abs(gyro_corr_dps))) > gyro_threshold_dps

    euler_motion = False
    if prev_euler_deg is not None and dt > 0.0:
        euler_rate = np.abs((current_euler_deg - prev_euler_deg) / dt)
        euler_motion = float(np.max(euler_rate)) > euler_rate_threshold_dps

    return gyro_motion or euler_motion


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="MoveToPlay IMU fusion monitor")
    parser.add_argument("--port", required=True, help="Serial port, e.g. COM7 or /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate")
    parser.add_argument("--dt", type=float, default=0.01, help="Fallback sample dt in seconds")
    parser.add_argument("--calib-seconds", type=float, default=3.0, help="Gyro calibration duration")
    parser.add_argument("--gyro-threshold", type=float, default=20.0, help="Motion threshold in dps")
    parser.add_argument("--euler-rate-threshold", type=float, default=80.0, help="Motion threshold in deg/s")
    parser.add_argument("--mahony-kp", type=float, default=1.2, help="Mahony proportional gain")
    parser.add_argument("--mahony-ki", type=float, default=0.05, help="Mahony integral gain")
    parser.add_argument("--plot-window", type=float, default=10.0, help="Euler plot window in seconds")
    parser.add_argument("--out-dir", default="logs", help="CSV output directory")
    parser.add_argument("--use-arrival-dt", action="store_true", help="Use PC arrival time delta instead of fixed dt")
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()

    print("[info] opening serial...")
    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.2)
    except serial.SerialException as exc:
        print(f"[error] failed to open serial port: {exc}")
        return 1

    writer, csv_file = open_csv_writer(Path(args.out_dir))
    plotter = Plotter(window_seconds=args.plot_window, nominal_dt=args.dt)
    fusion = Mahony6DoF(kp=args.mahony_kp, ki=args.mahony_ki)

    try:
        gyro_bias = calibrate_gyro_bias(ser, args.calib_seconds, args.dt)

        print("[info] fusion started")
        print("[info] roll/pitch are relatively trustworthy; yaw will drift without magnetometer")

        prev_pc_time: Optional[float] = None
        prev_euler: Optional[np.ndarray] = None
        processed = 0
        bad_accel_count = 0
        start_time = time.monotonic()

        while True:
            raw_line = ser.readline().decode("utf-8", errors="ignore").strip()
            pc_time = time.monotonic()

            if not raw_line:
                plt.pause(0.001)
                continue

            dt = args.dt
            if args.use_arrival_dt and prev_pc_time is not None:
                dt = max(0.001, min(0.05, pc_time - prev_pc_time))
            prev_pc_time = pc_time

            sample = parse_imu_line(raw_line, dt, pc_time)
            if sample is None:
                continue

            accel = np.array([sample.ax, sample.ay, sample.az], dtype=float)
            gyro = np.array([sample.gx, sample.gy, sample.gz], dtype=float)
            gyro_corr = gyro - gyro_bias
            accel_norm = float(np.linalg.norm(accel))

            status = "stable"
            if accel_norm < 0.5 or accel_norm > 2.0:
                bad_accel_count += 1
                if bad_accel_count % 20 == 0:
                    print(f"[warn] accel norm abnormal: {accel_norm:.3f} g, fusion will trust gyro more")
                status = "bad_accel"

            q = fusion.update(gyro_corr, accel, dt)
            roll, pitch, yaw = quat_to_euler_deg(q)
            euler = np.array([roll, pitch, yaw], dtype=float)

            moving = estimate_motion_flag(
                gyro_corr_dps=gyro_corr,
                current_euler_deg=euler,
                prev_euler_deg=prev_euler,
                dt=dt,
                gyro_threshold_dps=args.gyro_threshold,
                euler_rate_threshold_dps=args.euler_rate_threshold,
            )
            prev_euler = euler

            motion_label = "moving" if moving else "stable"
            if status == "bad_accel":
                status = f"{status}|{motion_label}"
            else:
                status = motion_label

            writer.writerow(
                [
                    f"{pc_time:.6f}",
                    sample.seq,
                    f"{dt:.4f}",
                    f"{sample.ax:.6f}",
                    f"{sample.ay:.6f}",
                    f"{sample.az:.6f}",
                    f"{sample.gx:.6f}",
                    f"{sample.gy:.6f}",
                    f"{sample.gz:.6f}",
                    f"{gyro_corr[0]:.6f}",
                    f"{gyro_corr[1]:.6f}",
                    f"{gyro_corr[2]:.6f}",
                    f"{accel_norm:.6f}",
                    f"{q[0]:.6f}",
                    f"{q[1]:.6f}",
                    f"{q[2]:.6f}",
                    f"{q[3]:.6f}",
                    f"{roll:.6f}",
                    f"{pitch:.6f}",
                    f"{yaw:.6f}",
                    int(moving),
                    status,
                ]
            )
            csv_file.flush()

            processed += 1
            if processed % 5 == 0:
                print(
                    f"\rseq={sample.seq:<8d} "
                    f"roll={roll:>7.2f} pitch={pitch:>7.2f} yaw={yaw:>7.2f} "
                    f"motion={motion_label:<6s} "
                    f"acc_norm={accel_norm:>5.2f}g "
                    f"aw={fusion.last_accel_weight:.2f}",
                    end="",
                    flush=True,
                )

            plotter.update(pc_time - start_time, roll, pitch, yaw, f"status: {status}")
            plt.pause(0.001)

    except KeyboardInterrupt:
        print("\n[info] stopping...")
    except Exception as exc:
        print(f"\n[error] {exc}")
        return 1
    finally:
        csv_file.close()
        ser.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
