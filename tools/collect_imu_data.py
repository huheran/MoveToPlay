#!/usr/bin/env python3
"""
Command-line IMU data collector with real-time keyboard labeling.

Default input line format:
    board_timestamp_ms,node_id,ax,ay,az,gx,gy,gz

Example:
    123456,0,0.12,-0.03,9.81,0.01,0.02,-0.15

This script is designed for raw data collection. It stores each parsed IMU frame
with the current state label in CSV for later feature engineering, ML baselines,
and deep learning workflows.
"""

from __future__ import annotations

import argparse
import csv
import queue
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import serial
from pynput import keyboard

THIS_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = THIS_DIR.parent
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from tools.action_labels import DEFAULT_LABEL_MAP


CSV_COLUMNS = [
    "pc_timestamp_ms",
    "board_timestamp_ms",
    "node_id",
    "ax",
    "ay",
    "az",
    "gx",
    "gy",
    "gz",
    "label",
    "session_id",
]


@dataclass
class ImuSample:
    pc_timestamp_ms: int
    board_timestamp_ms: Optional[float]
    node_id: int
    ax: float
    ay: float
    az: float
    gx: float
    gy: float
    gz: float
    raw_line: str


class ImuLineParser:
    """Parse one-line IMU frames into structured samples."""

    def __init__(self, delimiter: str = ",") -> None:
        self.delimiter = delimiter

    def parse(self, line: str, pc_timestamp_ms: int) -> Optional[ImuSample]:
        text = line.strip()
        if not text:
            return None

        parts = [part.strip() for part in text.split(self.delimiter)]
        if len(parts) < 8:
            raise ValueError(f"expected 8 fields, got {len(parts)}")

        board_timestamp_ms = self._parse_optional_float(parts[0])
        node_id = int(parts[1])
        ax = float(parts[2])
        ay = float(parts[3])
        az = float(parts[4])
        gx = float(parts[5])
        gy = float(parts[6])
        gz = float(parts[7])

        return ImuSample(
            pc_timestamp_ms=pc_timestamp_ms,
            board_timestamp_ms=board_timestamp_ms,
            node_id=node_id,
            ax=ax,
            ay=ay,
            az=az,
            gx=gx,
            gy=gy,
            gz=gz,
            raw_line=text,
        )

    @staticmethod
    def _parse_optional_float(value: str) -> Optional[float]:
        if value == "" or value.lower() in {"none", "null", "nan"}:
            return None
        return float(value)


class CsvSessionWriter:
    """Manage CSV output and optional session rotation."""

    def __init__(self, base_output_path: Path) -> None:
        self.base_output_path = base_output_path
        self.session_index = 0
        self.session_id = ""
        self.current_path: Optional[Path] = None
        self._file = None
        self._writer: Optional[csv.DictWriter] = None
        self.rotate_session()

    def rotate_session(self) -> Path:
        self.close()
        self.session_index += 1
        self.session_id = f"session_{self.session_index:03d}"
        output_path = self._build_output_path(self.session_index)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        self._file = output_path.open("w", newline="", encoding="utf-8")
        self._writer = csv.DictWriter(self._file, fieldnames=CSV_COLUMNS)
        self._writer.writeheader()
        self._file.flush()
        self.current_path = output_path
        return output_path

    def write_sample(self, sample: ImuSample, label: str) -> None:
        if self._writer is None or self._file is None:
            raise RuntimeError("CSV writer is not initialized")

        self._writer.writerow(
            {
                "pc_timestamp_ms": sample.pc_timestamp_ms,
                "board_timestamp_ms": "" if sample.board_timestamp_ms is None else sample.board_timestamp_ms,
                "node_id": sample.node_id,
                "ax": sample.ax,
                "ay": sample.ay,
                "az": sample.az,
                "gx": sample.gx,
                "gy": sample.gy,
                "gz": sample.gz,
                "label": label,
                "session_id": self.session_id,
            }
        )
        self._file.flush()

    def close(self) -> None:
        if self._file is not None:
            self._file.flush()
            self._file.close()
            self._file = None
            self._writer = None

    def _build_output_path(self, session_index: int) -> Path:
        stem = self.base_output_path.stem
        suffix = self.base_output_path.suffix or ".csv"
        if session_index == 1:
            filename = f"{stem}{suffix}"
        else:
            filename = f"{stem}_{session_index:03d}{suffix}"
        return self.base_output_path.with_name(filename)


class KeyboardController:
    """Capture user hotkeys on a background thread and expose them as events."""

    def __init__(self) -> None:
        self._events: queue.Queue[str] = queue.Queue()
        self._listener = keyboard.Listener(on_press=self._on_press)

    def start(self) -> None:
        self._listener.start()

    def stop(self) -> None:
        self._listener.stop()

    def poll_events(self) -> list[str]:
        events: list[str] = []
        while True:
            try:
                events.append(self._events.get_nowait())
            except queue.Empty:
                return events

    def _on_press(self, key) -> None:
        try:
            char = key.char
        except AttributeError:
            char = None

        if char is None:
            return
        self._events.put(char.lower())


class SerialReader:
    """Thin wrapper over pyserial for line-based reads."""

    def __init__(self, port: str, baud: int, timeout_s: float) -> None:
        self.port = port
        self.baud = baud
        self.timeout_s = timeout_s
        self._ser: Optional[serial.Serial] = None

    def open(self) -> None:
        self._ser = serial.Serial(self.port, self.baud, timeout=self.timeout_s)

    def close(self) -> None:
        if self._ser is not None:
            self._ser.close()
            self._ser = None

    def readline(self) -> str:
        if self._ser is None:
            raise RuntimeError("serial port is not open")
        raw = self._ser.readline()
        if not raw:
            return ""
        return raw.decode("utf-8", errors="replace").strip()

    @property
    def is_open(self) -> bool:
        return bool(self._ser and self._ser.is_open)


class CollectorApp:
    """Main data collection loop and operator interaction."""

    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.label_map = DEFAULT_LABEL_MAP.copy()
        self.current_label = args.default_label
        self.collecting = args.autostart
        self.total_samples = 0
        self.parse_error_count = 0
        self.last_status_time = 0.0
        self.running = True

        self.parser = ImuLineParser(delimiter=args.delimiter)
        self.writer = CsvSessionWriter(Path(args.output))
        self.keyboard = KeyboardController()
        self.serial_reader = SerialReader(args.port, args.baud, args.serial_timeout)

    def run(self) -> int:
        self._print_banner()

        try:
            self.serial_reader.open()
        except serial.SerialException as exc:
            print(f"[error] failed to open serial port {self.args.port}: {exc}")
            self.writer.close()
            return 1

        self.keyboard.start()
        print(f"[info] serial opened: port={self.args.port} baud={self.args.baud}")
        print(f"[info] writing CSV to: {self.writer.current_path}")
        self._print_help()

        try:
            while self.running:
                self._handle_keyboard_events()
                self._read_and_store_one_line()
                self._print_periodic_status()
        except KeyboardInterrupt:
            print("\n[info] Ctrl+C received, stopping collector")
        except serial.SerialException as exc:
            print(f"\n[error] serial read failed: {exc}")
            return 1
        finally:
            self.keyboard.stop()
            self.serial_reader.close()
            self.writer.close()
            self._print_final_summary()

        return 0

    def _handle_keyboard_events(self) -> None:
        for key in self.keyboard.poll_events():
            if key in self.label_map:
                self.current_label = self.label_map[key]
                print(f"\n[label] current label -> {self.current_label}")
            elif key == "s":
                self.collecting = not self.collecting
                state = "running" if self.collecting else "paused"
                print(f"\n[state] collection {state}")
            elif key == "c":
                new_path = self.writer.rotate_session()
                print(f"\n[session] switched to {self.writer.session_id}: {new_path}")
            elif key == "p":
                print()
                self._print_status(full=True)
            elif key == "q":
                print("\n[info] safe exit requested by user")
                self.running = False

    def _read_and_store_one_line(self) -> None:
        line = self.serial_reader.readline()
        if not line:
            return

        pc_timestamp_ms = int(time.time() * 1000)
        try:
            sample = self.parser.parse(line, pc_timestamp_ms)
        except ValueError as exc:
            self.parse_error_count += 1
            print(f"\n[warn] parse failed: {exc}; raw={line}")
            return

        if sample is None:
            return

        if self.collecting:
            self.writer.write_sample(sample, self.current_label)
            self.total_samples += 1

    def _print_periodic_status(self) -> None:
        now = time.monotonic()
        if now - self.last_status_time < self.args.status_interval:
            return
        self.last_status_time = now
        self._print_status(full=False)

    def _print_status(self, full: bool) -> None:
        serial_state = "open" if self.serial_reader.is_open else "closed"
        collecting_text = "ON" if self.collecting else "OFF"
        session_path = self.writer.current_path if self.writer.current_path is not None else "n/a"
        message = (
            f"[status] serial={serial_state} "
            f"collecting={collecting_text} "
            f"label={self.current_label} "
            f"samples={self.total_samples} "
            f"parse_errors={self.parse_error_count} "
            f"session={self.writer.session_id}"
        )
        if full:
            print(message)
            print(f"[status] output={session_path}")
        else:
            print("\r" + message + "   ", end="", flush=True)

    def _print_banner(self) -> None:
        print("IMU Data Collector")
        print("Raw serial acquisition with keyboard labeling")

    def _print_help(self) -> None:
        key_hints = {
            "1": "ESC",
            "2": "mouse_left",
            "3": "W",
            "4": "Shift+W",
            "5": "SPACE",
            "6": "Q",
            "7": "hold mouse_left -> release",
        }
        for key, label in DEFAULT_LABEL_MAP.items():
            hint = key_hints.get(key)
            if hint:
                print(f"[keys] {key}={label} ({hint})")
            else:
                print(f"[keys] {key}={label}")
        print("[keys] s=start/pause  c=new session  p=print status  q=save and quit")

    def _print_final_summary(self) -> None:
        print(f"[summary] samples saved={self.total_samples}")
        print(f"[summary] parse errors={self.parse_error_count}")
        if self.writer.current_path is not None:
            print(f"[summary] last output file={self.writer.current_path}")


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Collect IMU data from serial and label it in real time")
    parser.add_argument("--port", required=True, help="Serial port, e.g. COM6 or /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate")
    parser.add_argument("--output", required=True, help="Output CSV path, e.g. data/session_001.csv")
    parser.add_argument("--delimiter", default=",", help="Input line delimiter, default is ','")
    parser.add_argument("--default-label", default="idle", help="Initial state label")
    parser.add_argument("--serial-timeout", type=float, default=0.05, help="Serial read timeout in seconds")
    parser.add_argument("--status-interval", type=float, default=0.5, help="Status refresh interval in seconds")
    parser.add_argument("--autostart", action="store_true", help="Start collecting immediately")
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()
    app = CollectorApp(args)
    return app.run()


if __name__ == "__main__":
    sys.exit(main())
