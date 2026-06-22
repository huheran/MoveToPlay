#!/usr/bin/env python3
"""
Serial IMU collector with state labels and event markers.

Input line format:
    board_timestamp_ms,node_id,ax,ay,az,gx,gy,gz

Output:
    samples.csv:
        pc_timestamp_ms,board_timestamp_ms,node_id,ax,ay,az,gx,gy,gz,
        state_label,event_group,event_type,event_id,session_id

    samples_events.csv:
        event_id,event_group,event_type,pc_timestamp_ms,state_label,session_id
"""

from __future__ import annotations

import argparse
import csv
import queue
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


STATE_KEY_MAP = {
    "1": "idle",
    "2": "walk",
    "3": "run",
    "4": "move_noise",
    "5": "right_hand_slash",
    "a": "right_hand_slash",
}

EVENT_KEY_MAP = {
    "h": ("attack_event", "hands_shoot"),
    "k": ("attack_event", "kick"),
    "j": ("jump_event", "jump"),
    "d": ("skill_event", "hands_press_down"),
    "f": ("skill_event", "hands_cross_forehead"),
    "u": ("skill_event", "ultraman_beam"),
    "p": ("pause_event", "right_hand_raise"),
    "o": ("pause_event", "left_hand_raise"),
    "z": ("turn_event", "turn_left"),
    "x": ("turn_event", "turn_right"),
}

SAMPLE_COLUMNS = [
    "pc_timestamp_ms",
    "board_timestamp_ms",
    "node_id",
    "ax",
    "ay",
    "az",
    "gx",
    "gy",
    "gz",
    "state_label",
    "event_group",
    "event_type",
    "event_id",
    "session_id",
]

EVENT_COLUMNS = [
    "event_id",
    "event_group",
    "event_type",
    "pc_timestamp_ms",
    "state_label",
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


@dataclass
class EventRecord:
    event_id: str
    event_group: str
    event_type: str
    pc_timestamp_ms: int
    state_label: str
    session_id: str
    assigned: bool = False


@dataclass
class BufferedSample:
    sample: ImuSample
    state_label: str
    event_group: str = "none"
    event_type: str = "none"
    event_id: str = ""


class ImuLineParser:
    def __init__(self, delimiter: str = ",") -> None:
        self.delimiter = delimiter

    def parse(self, line: str, pc_timestamp_ms: int) -> Optional[ImuSample]:
        text = line.strip()
        if not text:
            return None

        parts = [part.strip() for part in text.split(self.delimiter)]
        if len(parts) < 8:
            raise ValueError(f"expected 8 fields, got {len(parts)}")

        return ImuSample(
            pc_timestamp_ms=pc_timestamp_ms,
            board_timestamp_ms=self._parse_optional_float(parts[0]),
            node_id=int(parts[1]),
            ax=float(parts[2]),
            ay=float(parts[3]),
            az=float(parts[4]),
            gx=float(parts[5]),
            gy=float(parts[6]),
            gz=float(parts[7]),
        )

    @staticmethod
    def _parse_optional_float(value: str) -> Optional[float]:
        if value == "" or value.lower() in {"none", "null", "nan"}:
            return None
        return float(value)


class KeyboardController:
    def __init__(self) -> None:
        try:
            from pynput import keyboard as pynput_keyboard
        except ImportError as exc:
            raise RuntimeError("missing dependency: install pynput") from exc

        self._events: queue.Queue[str] = queue.Queue()
        self._listener = pynput_keyboard.Listener(on_press=self._on_press)

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
        if char:
            self._events.put(char.lower())


class SerialReader:
    def __init__(self, port: str, baud: int, timeout_s: float) -> None:
        self.port = port
        self.baud = baud
        self.timeout_s = timeout_s
        self._ser: Optional[serial.Serial] = None
        self._serial_exception_type: tuple[type[BaseException], ...] = ()

    def open(self) -> None:
        try:
            import serial
        except ImportError as exc:
            raise RuntimeError("missing dependency: install pyserial") from exc

        self._serial_exception_type = (serial.SerialException,)
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

    def is_serial_exception(self, exc: BaseException) -> bool:
        return bool(self._serial_exception_type and isinstance(exc, self._serial_exception_type))


class CsvSessionWriter:
    def __init__(self, base_sample_path: Path) -> None:
        self.base_sample_path = base_sample_path
        self.session_index = 0
        self.session_id = ""
        self.sample_path: Optional[Path] = None
        self.event_path: Optional[Path] = None
        self._sample_file = None
        self._event_file = None
        self._sample_writer: Optional[csv.DictWriter] = None
        self._event_writer: Optional[csv.DictWriter] = None
        self.rotate_session()

    def rotate_session(self) -> tuple[Path, Path]:
        self.close()
        self.session_index += 1
        self.session_id = f"session_{self.session_index:03d}"
        self.sample_path = self._build_sample_path(self.session_index)
        self.event_path = self._build_event_path(self.sample_path)
        self.sample_path.parent.mkdir(parents=True, exist_ok=True)

        self._sample_file = self.sample_path.open("w", newline="", encoding="utf-8")
        self._event_file = self.event_path.open("w", newline="", encoding="utf-8")
        self._sample_writer = csv.DictWriter(self._sample_file, fieldnames=SAMPLE_COLUMNS)
        self._event_writer = csv.DictWriter(self._event_file, fieldnames=EVENT_COLUMNS)
        self._sample_writer.writeheader()
        self._event_writer.writeheader()
        self.flush()
        return self.sample_path, self.event_path

    def write_sample(self, row: BufferedSample) -> None:
        if self._sample_writer is None:
            raise RuntimeError("sample writer is not initialized")
        sample = row.sample
        self._sample_writer.writerow(
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
                "state_label": row.state_label,
                "event_group": row.event_group,
                "event_type": row.event_type,
                "event_id": row.event_id,
                "session_id": self.session_id,
            }
        )

    def write_event(self, event: EventRecord) -> None:
        if self._event_writer is None:
            raise RuntimeError("event writer is not initialized")
        self._event_writer.writerow(
            {
                "event_id": event.event_id,
                "event_group": event.event_group,
                "event_type": event.event_type,
                "pc_timestamp_ms": event.pc_timestamp_ms,
                "state_label": event.state_label,
                "session_id": event.session_id,
            }
        )

    def flush(self) -> None:
        if self._sample_file is not None:
            self._sample_file.flush()
        if self._event_file is not None:
            self._event_file.flush()

    def close(self) -> None:
        for handle in (self._sample_file, self._event_file):
            if handle is not None:
                handle.flush()
                handle.close()
        self._sample_file = None
        self._event_file = None
        self._sample_writer = None
        self._event_writer = None

    def _build_sample_path(self, session_index: int) -> Path:
        stem = self.base_sample_path.stem
        suffix = self.base_sample_path.suffix or ".csv"
        if session_index == 1:
            filename = f"{stem}{suffix}"
        else:
            filename = f"{stem}_{session_index:03d}{suffix}"
        return self.base_sample_path.with_name(filename)

    @staticmethod
    def _build_event_path(sample_path: Path) -> Path:
        return sample_path.with_name(f"{sample_path.stem}_events{sample_path.suffix}")


class CollectorApp:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.current_state = args.default_state
        self.collecting = args.autostart
        self.running = True
        self.total_samples = 0
        self.total_events = 0
        self.parse_error_count = 0
        self.last_status_time = 0.0
        self.event_counter = 0

        self.keyboard = KeyboardController()
        self.parser = ImuLineParser(args.delimiter)
        self.writer = CsvSessionWriter(Path(args.output))
        self.serial_reader = SerialReader(args.port, args.baud, args.serial_timeout)
        self.buffer: list[BufferedSample] = []
        self.pending_events: list[EventRecord] = []

    def run(self) -> int:
        self._print_banner()
        try:
            self.serial_reader.open()
        except RuntimeError as exc:
            print(f"[error] failed to open serial port {self.args.port}: {exc}")
            self.writer.close()
            return 1
        except Exception as exc:
            if self.serial_reader.is_serial_exception(exc):
                print(f"[error] failed to open serial port {self.args.port}: {exc}")
                self.writer.close()
                return 1
            raise

        self.keyboard.start()
        print(f"[info] serial opened: port={self.args.port} baud={self.args.baud}")
        print(f"[info] samples: {self.writer.sample_path}")
        print(f"[info] events:  {self.writer.event_path}")
        self._print_help()

        try:
            while self.running:
                self._handle_keyboard_events()
                self._read_and_buffer_one_line()
                self._flush_old_samples()
                self._print_periodic_status()
        except KeyboardInterrupt:
            print("\n[info] Ctrl+C received, stopping collector")
        except Exception as exc:
            if self.serial_reader.is_serial_exception(exc):
                print(f"\n[error] serial read failed: {exc}")
                return 1
            raise
        finally:
            self._assign_ready_events(force=True)
            self._flush_all_samples()
            self.keyboard.stop()
            self.serial_reader.close()
            self.writer.close()
            self._print_final_summary()
        return 0

    def _handle_keyboard_events(self) -> None:
        for key in self.keyboard.poll_events():
            if key in STATE_KEY_MAP:
                self.current_state = STATE_KEY_MAP[key]
                print(f"\n[state] state_label -> {self.current_state}")
            elif key in EVENT_KEY_MAP:
                self._record_event(*EVENT_KEY_MAP[key])
            elif key == "s":
                self.collecting = not self.collecting
                text = "running" if self.collecting else "paused"
                print(f"\n[state] collection {text}")
            elif key == "c":
                self._rotate_session()
            elif key == "i":
                print()
                self._print_status(full=True)
            elif key == "q":
                print("\n[info] safe exit requested by user")
                self.running = False

    def _record_event(self, event_group: str, event_type: str) -> None:
        if not self.collecting:
            print(f"\n[warn] ignored {event_type}: collection is paused")
            return

        self.event_counter += 1
        event_id = f"{self._slug(event_type)}_{self.event_counter:04d}"
        now_ms = int(time.time() * 1000)
        event = EventRecord(
            event_id=event_id,
            event_group=event_group,
            event_type=event_type,
            pc_timestamp_ms=now_ms,
            state_label=self.current_state,
            session_id=self.writer.session_id,
        )
        self.pending_events.append(event)
        self.writer.write_event(event)
        self.writer.flush()
        self.total_events += 1
        print(f"\n[event] {event_id} group={event_group} type={event_type} state={self.current_state}")

    def _read_and_buffer_one_line(self) -> None:
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

        if sample is None or not self.collecting:
            return

        self.buffer.append(BufferedSample(sample=sample, state_label=self.current_state))
        self._assign_ready_events(force=False)

    def _assign_ready_events(self, force: bool) -> None:
        if not self.buffer or not self.pending_events:
            return

        latest_sample_ms = self.buffer[-1].sample.pc_timestamp_ms
        for event in self.pending_events:
            if event.assigned:
                continue
            if not force and latest_sample_ms < event.pc_timestamp_ms + self.args.event_marker_lookahead_ms:
                continue

            best_index = min(
                range(len(self.buffer)),
                key=lambda idx: abs(self.buffer[idx].sample.pc_timestamp_ms - event.pc_timestamp_ms),
            )
            best_row = self.buffer[best_index]
            distance_ms = abs(best_row.sample.pc_timestamp_ms - event.pc_timestamp_ms)
            if distance_ms <= self.args.event_marker_tolerance_ms:
                best_row.event_group = event.event_group
                best_row.event_type = event.event_type
                best_row.event_id = event.event_id
            else:
                print(
                    f"\n[warn] event {event.event_id} has no nearby sample "
                    f"(nearest distance={distance_ms}ms)"
                )
            event.assigned = True

        self.pending_events = [event for event in self.pending_events if not event.assigned]

    def _flush_old_samples(self) -> None:
        if not self.buffer:
            return

        now_ms = int(time.time() * 1000)
        flush_before_ms = now_ms - self.args.write_delay_ms
        flush_count = 0
        while flush_count < len(self.buffer) and self.buffer[flush_count].sample.pc_timestamp_ms <= flush_before_ms:
            flush_count += 1

        if flush_count <= 0 and len(self.buffer) <= self.args.max_buffer_samples:
            return
        if flush_count <= 0:
            flush_count = max(1, len(self.buffer) // 2)

        for row in self.buffer[:flush_count]:
            self.writer.write_sample(row)
            self.total_samples += 1
        self.writer.flush()
        self.buffer = self.buffer[flush_count:]

    def _flush_all_samples(self) -> None:
        for row in self.buffer:
            self.writer.write_sample(row)
            self.total_samples += 1
        self.writer.flush()
        self.buffer.clear()

    def _rotate_session(self) -> None:
        self._assign_ready_events(force=True)
        self._flush_all_samples()
        sample_path, event_path = self.writer.rotate_session()
        self.pending_events.clear()
        print(f"\n[session] switched to {self.writer.session_id}")
        print(f"[session] samples: {sample_path}")
        print(f"[session] events:  {event_path}")

    def _print_periodic_status(self) -> None:
        now = time.monotonic()
        if now - self.last_status_time < self.args.status_interval:
            return
        self.last_status_time = now
        self._print_status(full=False)

    def _print_status(self, full: bool) -> None:
        serial_state = "open" if self.serial_reader.is_open else "closed"
        collecting_text = "ON" if self.collecting else "OFF"
        message = (
            f"[status] serial={serial_state} collecting={collecting_text} "
            f"state={self.current_state} samples={self.total_samples} "
            f"events={self.total_events} parse_errors={self.parse_error_count} "
            f"session={self.writer.session_id}"
        )
        if full:
            print(message)
            print(f"[status] samples={self.writer.sample_path}")
            print(f"[status] events={self.writer.event_path}")
        else:
            print("\r" + message + "   ", end="", flush=True)

    def _print_banner(self) -> None:
        print("IMU Event Collector")
        print("Raw serial acquisition with state labels and event markers")

    def _print_help(self) -> None:
        print("[state keys]")
        for key, label in STATE_KEY_MAP.items():
            print(f"  {key}: {label}")
        print("[event keys]")
        for key, (event_group, event_type) in EVENT_KEY_MAP.items():
            print(f"  {key}: {event_group}/{event_type}")
        print("[control keys]")
        print("  s: start/pause")
        print("  c: new session")
        print("  i: print status")
        print("  q: save and quit")

    def _print_final_summary(self) -> None:
        print(f"[summary] samples saved={self.total_samples}")
        print(f"[summary] events saved={self.total_events}")
        print(f"[summary] parse errors={self.parse_error_count}")
        if self.writer.sample_path is not None:
            print(f"[summary] last samples file={self.writer.sample_path}")
        if self.writer.event_path is not None:
            print(f"[summary] last events file={self.writer.event_path}")

    @staticmethod
    def _slug(value: str) -> str:
        return re.sub(r"[^a-zA-Z0-9]+", "_", value).strip("_").lower()


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Collect serial IMU data with state labels and event markers")
    parser.add_argument("--port", required=True, help="Serial port, e.g. COM6 or /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate")
    parser.add_argument("--output", required=True, help="Output samples CSV path, e.g. data/session_001.csv")
    parser.add_argument("--delimiter", default=",", help="Input line delimiter")
    parser.add_argument("--default-state", default="idle", choices=sorted(set(STATE_KEY_MAP.values())))
    parser.add_argument("--serial-timeout", type=float, default=0.05, help="Serial read timeout in seconds")
    parser.add_argument("--status-interval", type=float, default=0.5, help="Status refresh interval in seconds")
    parser.add_argument("--autostart", action="store_true", help="Start collecting immediately")
    parser.add_argument("--write-delay-ms", type=int, default=250, help="Delay sample writes so events can mark nearby rows")
    parser.add_argument("--event-marker-lookahead-ms", type=int, default=80, help="Wait this long before assigning marker row")
    parser.add_argument("--event-marker-tolerance-ms", type=int, default=250, help="Max distance from keypress to marked sample")
    parser.add_argument("--max-buffer-samples", type=int, default=2000, help="Safety limit for buffered samples")
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()
    try:
        app = CollectorApp(args)
    except RuntimeError as exc:
        print(f"[error] {exc}")
        print("[hint] install dependencies: python -m pip install pyserial pynput")
        return 1
    return app.run()


if __name__ == "__main__":
    sys.exit(main())
