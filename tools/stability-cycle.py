#!/usr/bin/env python3
"""Repeat GoldenEye boot/menu/Dam/shutdown cycles in isolated state roots."""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace
from typing import Any


ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import metal_profile_parser as profile  # noqa: E402
import render_regression as rendering  # noqa: E402


FATAL_PATTERNS = (
    re.compile(r"libc\+\+abi: terminating", re.I),
    re.compile(r"segmentation fault", re.I),
    re.compile(r"EXC_BAD_ACCESS", re.I),
    re.compile(r"pure virtual method called", re.I),
    re.compile(r"uncaught exception", re.I),
    re.compile(r"fatal error", re.I),
    re.compile(r"direct guest output copy did not complete", re.I),
    re.compile(r"GPU tiled resolve failed", re.I),
    re.compile(r"shared-memory upload failed", re.I),
    re.compile(r"\[vpad\] FAILED", re.I),
    # Match only emitted stall records, not aggregate text such as
    # "GEWATCHDOG STALL count=0".
    re.compile(r"\bGEWATCHDOG STALL(?=:\s+ring\b|\s+rpi=0x[0-9a-f]+\b)", re.I),
    re.compile(r"\bGENOPRESENT STALL(?=:\s+ring\b|\s+rpi=0x[0-9a-f]+\b)", re.I),
    re.compile(
        r"\[GE-PLAYER-STUCK-v1\]\s+"
        r"(?:suspected live-render movement stall:|pipeline ring=|sample=\d+/\d+\s)",
        re.I,
    ),
    # Snapshot overflow/underflow diagnostics are distinct from an ABI repair.
    re.compile(
        r"\[GE-GUARD-AUDIO-CALLBACK-v1\]\s+repaired callback ABI "
        r"hit=[1-9]\d*\b",
        re.I,
    ),
    # A zero-valued ledger snapshot is expected diagnostic context. Fail only
    # on nonzero/incomplete ownership state or a concrete mismatch record.
    re.compile(
        r"\bCSLEDGER\b[^\r\n]*(?:"
        r"\bincomplete=(?:true|[1-9]\d*)\b|"
        r"\bleave_mismatches=[1-9]\d*\b|"
        r"\b(?:matching[ _]+leave[ _]+mismatch|"
        r"latest[ _]+(?:owner-thread|owner)[ _]+leave[ _]+mismatch)"
        r"\s+cs=0x[0-9a-f]+\b"
        r")",
        re.I,
    ),
)
ACTIVE_PROCESS: subprocess.Popen[bytes] | None = None
LOCAL_MULTIPLAYER_READY = re.compile(
    r"\[ge\] local multiplayer ready level=(\d+) players=(\d+) "
    r"network=0 stable_polls=(\d+)"
)
VPAD_ACK = re.compile(r"\[vpad\] ACK seq=(\d+)")
VPAD_REJECT = re.compile(r"\[vpad\] REJECT reason=.*? command=\S+\s+(\d+)(?:\s|$)")
GE_TEST_MENU_STATE = re.compile(
    r"\[ge-test\] menu state=(\d+) name=([a-z0-9-]+) joined=(\d+)"
)


def positive_integer(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return parsed


def finite_float(value: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed):
        raise argparse.ArgumentTypeError("must be a finite number")
    return parsed


def timestamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def safe_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def binary_contains(path: Path, marker: bytes) -> bool:
    overlap = max(len(marker) - 1, 0)
    previous = b""
    try:
        with path.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                combined = previous + chunk
                if marker in combined:
                    return True
                previous = combined[-overlap:] if overlap else b""
    except OSError:
        return False
    return False


def combined_cycle_text(raw_log_path: Path) -> str:
    parts = [safe_text(raw_log_path)]
    runtime_logs = raw_log_path.parent / "user-data" / "Logs"
    if runtime_logs.is_dir():
        for candidate in sorted(runtime_logs.glob("*.log")):
            parts.append(safe_text(candidate))
    return "\n".join(parts)


def detect_fatal_logs(cycle_root: Path) -> list[str]:
    matches: list[str] = []
    candidates = [cycle_root / "raw.log"]
    candidates.extend((cycle_root / "user-data" / "Logs").glob("*.log"))
    for candidate in candidates:
        text = safe_text(candidate)
        for pattern in FATAL_PATTERNS:
            match = pattern.search(text)
            if match:
                matches.append(f"{candidate.name}: {match.group(0)}")
    return sorted(set(matches))


def collect_crash_reports(cycle_root: Path, started_epoch: float) -> list[str]:
    source = Path.home() / "Library/Logs/DiagnosticReports"
    if not source.is_dir():
        return []
    destination = cycle_root / "crash-reports"
    copied: list[str] = []
    for candidate in source.iterdir():
        if not candidate.is_file() or candidate.suffix.lower() not in (
            ".ips",
            ".crash",
        ):
            continue
        if not re.match(r"(?:GoldenEye|ge)[-_ ]", candidate.name, re.I):
            continue
        try:
            if candidate.stat().st_mtime < started_epoch - 2:
                continue
            destination.mkdir(parents=True, exist_ok=True)
            target = destination / candidate.name
            shutil.copy2(candidate, target)
            copied.append(str(target))
        except OSError:
            continue
    return copied


def terminate_process(
    process: subprocess.Popen[bytes], method: str, timeout: float
) -> dict[str, Any]:
    try:
        process_group = os.getpgid(process.pid)
        if process_group != process.pid:
            process_group = None
    except ProcessLookupError:
        process_group = process.pid

    def group_alive() -> bool:
        if process_group is None:
            return False
        try:
            os.killpg(process_group, 0)
            return True
        except ProcessLookupError:
            return False
        except PermissionError:
            return True

    def send_signal_to_test(signal_number: int) -> None:
        if process_group is not None:
            try:
                os.killpg(process_group, signal_number)
                return
            except ProcessLookupError:
                return
        if process.poll() is None:
            process.send_signal(signal_number)

    result: dict[str, Any] = {
        "requested_method": method,
        "native_request_succeeded": False,
        "forced_signal": None,
        "timed_out": False,
        "process_group": process_group,
        "descendant_cleanup_required": False,
    }
    if process.poll() is not None:
        result["process_exited_before_request"] = True
        result["exit_code"] = process.returncode
    else:
        if method == "native":
            try:
                rendering.macos_app_control(ROOT, "terminate", process.pid)
                result["native_request_succeeded"] = True
            except (rendering.ImageError, OSError, subprocess.TimeoutExpired) as error:
                result["native_request_error"] = str(error)
                send_signal_to_test(signal.SIGTERM)
                result["forced_signal"] = "SIGTERM (native request fallback)"
        else:
            send_signal_to_test(signal.SIGTERM)
            result["forced_signal"] = "SIGTERM (requested harness mode)"

        deadline = time.monotonic() + timeout
        while process.poll() is None and time.monotonic() < deadline:
            time.sleep(0.1)

        if process.poll() is None:
            result["timed_out"] = True
            send_signal_to_test(signal.SIGTERM)
            result["forced_signal"] = "SIGTERM"
            fallback_deadline = time.monotonic() + min(5.0, timeout)
            while process.poll() is None and time.monotonic() < fallback_deadline:
                time.sleep(0.1)
        if process.poll() is None:
            send_signal_to_test(signal.SIGKILL)
            result["forced_signal"] = "SIGKILL"
            process.wait(timeout=5)
        else:
            process.wait()
        result["exit_code"] = process.returncode

    # start_new_session=True makes the child its process-group leader. Clean up
    # descendants even if a launcher exits before the game or a helper survives
    # the normal AppKit quit request.
    if group_alive():
        result["descendant_cleanup_required"] = True
        send_signal_to_test(signal.SIGTERM)
        descendant_deadline = time.monotonic() + min(5.0, timeout)
        while group_alive() and time.monotonic() < descendant_deadline:
            time.sleep(0.1)
        if group_alive():
            send_signal_to_test(signal.SIGKILL)
            result["descendant_force_killed"] = True
    return result


def readiness(
    mode: str,
    log_path: Path,
    elapsed: float,
    menu_settle_seconds: float,
    warmup: int,
    observe: int,
    players: int,
    expected_virtual_gamepad_ack: int = 0,
) -> tuple[bool, dict[str, Any]]:
    windows, violations, counts = profile.parse_log(log_path)
    log_text = combined_cycle_text(log_path)
    details: dict[str, Any] = {
        "profile_windows": len(windows),
        "dam_windows": sum(window["dam_candidate"] for window in windows),
        "profile_failures": violations + profile.wait_reg_mem_violations(windows),
        "failure_counts": counts,
    }
    if details["profile_failures"]:
        return False, details
    if mode == "local-multiplayer":
        details["virtual_gamepads_ready"] = f"[vpad] READY pads={players}" in log_text
        acknowledgements = {
            int(match.group(1)) for match in VPAD_ACK.finditer(log_text)
        }
        details["last_virtual_gamepad_ack"] = (
            max(acknowledgements) if acknowledgements else 0
        )
        matches = list(LOCAL_MULTIPLAYER_READY.finditer(log_text))
        matching = [
            match
            for match in matches
            if int(match.group(2)) == players and int(match.group(3)) >= 120
        ]
        if matching:
            latest = matching[-1]
            details["local_multiplayer"] = {
                "level": int(latest.group(1)),
                "players": int(latest.group(2)),
                "stable_polls": int(latest.group(3)),
            }
        details["required_virtual_gamepad_ack"] = expected_virtual_gamepad_ack
        missing_acknowledgements = [
            sequence
            for sequence in range(1, expected_virtual_gamepad_ack + 1)
            if sequence not in acknowledgements
        ]
        details["missing_virtual_gamepad_acks"] = missing_acknowledgements
        return (
            bool(matching)
            and details["virtual_gamepads_ready"]
            and not missing_acknowledgements
            and bool(windows),
            details,
        )
    if mode == "menu":
        injected = "GOLDENEYE_AUTO_START=menu injecting Start" in log_text
        details["auto_start_seen"] = injected
        return injected and elapsed >= menu_settle_seconds and bool(windows), details
    selected = profile.select_windows(windows, warmup, observe)
    details["selected_windows"] = len(selected)
    if len(selected) == observe:
        details["aggregate"] = profile.aggregate(selected)
        return True, details
    return False, details


def compare_capture(
    reference: Path, actual: Path, cycle_root: Path, args: argparse.Namespace
) -> dict[str, Any]:
    report, difference = rendering.compare_images(
        rendering.read_png(reference),
        rendering.read_png(actual),
        pixel_threshold=args.render_pixel_threshold,
        max_changed_ratio=args.render_max_changed_ratio,
        max_mae=args.render_max_mae,
        max_coarse_mae=args.render_max_coarse_mae,
        min_luma_stddev=args.render_min_luma_stddev,
        rectangles=args.render_ignore,
    )
    report["reference"] = {
        "path": str(reference),
        "sha256": rendering.file_sha256(reference),
    }
    report["actual"] = {"path": str(actual), "sha256": rendering.file_sha256(actual)}
    difference_path = cycle_root / "frame-diff.png"
    rendering.write_png(difference_path, difference)
    report["difference_image"] = str(difference_path)
    (cycle_root / "render-comparison.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return report


def build_environment(
    args: argparse.Namespace,
    cycle_root: Path,
    *,
    create_directories: bool = True,
    virtual_gamepad_fd: int | None = None,
) -> dict[str, str]:
    user_data = cycle_root / "user-data"
    cache = cycle_root / "cache"
    home = cycle_root / "home"
    temporary = cycle_root / "tmp"
    if create_directories:
        for directory in (user_data, cache, home, temporary):
            directory.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    for variable in (
        "REX_INPUT_TEST_HARNESS",
        "REX_TEST_VIRTUAL_GAMEPADS",
        "REX_TEST_VIRTUAL_GAMEPAD_FD",
        "GOLDENEYE_TEST_MENU_TRACE",
    ):
        environment.pop(variable, None)
    environment.update(
        {
            "HOME": str(home),
            "TMPDIR": str(temporary),
            "REX_GPU": "metal",
            "REX_INPUT_BACKEND": "none",
            "REX_MNK_MODE": "false",
            "REX_USER_DATA_ROOT": str(user_data),
            "REX_CACHE_PATH": str(cache),
            "REX_WINDOW_WIDTH": "1280",
            "REX_WINDOW_HEIGHT": "720",
            "REX_VIDEO_MODE_WIDTH": "1280",
            "REX_VIDEO_MODE_HEIGHT": "720",
            "REX_GPU_VSYNC": "false",
            "REX_MAX_FPS": "60",
            "REX_FULLSCREEN": "false",
            "REX_METAL_SHOW_FPS": "false",
            "GOLDENEYE_AUTO_START": "menu",
            "GOLDENEYE_METAL_PROFILE": "1",
            "GOLDENEYE_LAUNCHER_BYPASS_UI": "1",
        }
    )
    if args.mode == "local-multiplayer":
        environment["REX_INPUT_BACKEND"] = "sdl"
        environment.pop("GOLDENEYE_AUTO_START", None)
        environment.pop("GOLDENEYE_AUTO_MISSION", None)
        environment["REX_CONTROLLER_LAYOUT"] = "modern"
        environment["REX_CONTROLLER_BUTTON_MAP"] = ""
        environment["GOLDENEYE_TEST_MENU_TRACE"] = "1"
        if virtual_gamepad_fd is not None:
            environment["REX_INPUT_TEST_HARNESS"] = "1"
            environment["REX_TEST_VIRTUAL_GAMEPADS"] = str(args.players)
            environment["REX_TEST_VIRTUAL_GAMEPAD_FD"] = str(virtual_gamepad_fd)
    elif args.mode == "dam":
        environment["GOLDENEYE_AUTO_MISSION"] = "dam"
    else:
        environment.pop("GOLDENEYE_AUTO_MISSION", None)
    previous_libraries = environment.get("DYLD_LIBRARY_PATH")
    environment["DYLD_LIBRARY_PATH"] = str(args.runtime_dir) + (
        f":{previous_libraries}" if previous_libraries else ""
    )
    return environment


class LocalMultiplayerInputDriver:
    SOAK_HEARTBEAT_INTERVAL_SECONDS = 1.0

    def __init__(self, players: int, command_fd: int):
        self.players = players
        self.command_fd = command_fd
        self.ready_elapsed: float | None = None
        self.phase = "waiting-for-harness"
        self.sequence = 0
        self.last_sequence = 0
        self.last_ack = 0
        self.acknowledged_sequences: set[int] = set()
        self.next_action_at = 0.0
        self.settled_at = 0.0
        self.observed_state: int | None = None
        self.observed_menu: str | None = None
        self.observed_joined = 0
        self.completed = False
        self.sent: list[dict[str, Any]] = []
        self.error: str | None = None
        self.soak_active = False
        self.soak_heartbeat_batches: list[list[int]] = []
        self.next_soak_heartbeat_at = 0.0

    def _pulse_button(self, player: int, button: str, hold_ms: int = 250) -> str:
        self.sequence += 1
        return f"PULSE_BUTTON {self.sequence} {player} {button} {hold_ms}\n"

    def _pulse_axis(
        self, player: int, axis: str, value: int, hold_ms: int = 350
    ) -> str:
        self.sequence += 1
        return f"PULSE_AXIS {self.sequence} {player} {axis} {value} {hold_ms}\n"

    def _reset(self, player: int) -> str:
        self.sequence += 1
        return f"RESET {self.sequence} {player}\n"

    def _send(
        self,
        commands: list[str],
        elapsed: float,
        reason: str,
        settle_seconds: float,
    ) -> bool:
        payload = "".join(commands).encode("ascii")
        try:
            written = os.write(self.command_fd, payload)
            if written != len(payload):
                raise OSError(f"short virtual-gamepad write: {written}/{len(payload)}")
        except OSError as error:
            self.error = str(error)
            return False
        self.last_sequence = self.sequence
        self.settled_at = elapsed + settle_seconds
        self.sent.append(
            {
                "phase": self.phase,
                "reason": reason,
                "sent_seconds": elapsed,
                "commands": [command.rstrip() for command in commands],
            }
        )
        return True

    def _command_finished(self, elapsed: float) -> bool:
        return elapsed >= self.settled_at and (
            self.last_sequence == 0
            or all(
                sequence in self.acknowledged_sequences
                for sequence in range(1, self.last_sequence + 1)
            )
        )

    def _send_soak_heartbeat(self, elapsed: float) -> None:
        first_sequence = self.sequence + 1
        commands = [self._reset(player) for player in range(1, self.players + 1)]
        if self._send(
            commands,
            elapsed,
            "verify post-ready virtual-gamepad input continuity",
            0.0,
        ):
            self.soak_heartbeat_batches.append(
                list(range(first_sequence, self.sequence + 1))
            )
            self.next_soak_heartbeat_at = (
                elapsed + self.SOAK_HEARTBEAT_INTERVAL_SECONDS
            )

    def begin_post_ready_soak(
        self, elapsed: float, *, heartbeat_enabled: bool
    ) -> None:
        # Readiness is emitted only after the title has consumed the final
        # navigation pulse and observed a stable local match.
        self.completed = True
        self.phase = "post-ready-soak"
        self.soak_active = heartbeat_enabled
        if heartbeat_enabled:
            self._send_soak_heartbeat(elapsed)

    def _advance_soak_heartbeat(self, elapsed: float) -> None:
        if self.soak_heartbeat_batches:
            pending = [
                sequence
                for sequence in self.soak_heartbeat_batches[-1]
                if sequence not in self.acknowledged_sequences
            ]
            if pending:
                return
        if elapsed >= self.next_soak_heartbeat_at:
            self._send_soak_heartbeat(elapsed)

    def finish_post_ready_soak(self, log_path: Path, elapsed: float) -> None:
        if not self.soak_active or self.error:
            return
        self.advance(log_path, elapsed, allow_soak_heartbeat=False)
        if self.error:
            return
        if not self.soak_heartbeat_batches:
            self.error = "post-ready virtual-gamepad heartbeat was not sent"
            return
        missing = [
            sequence
            for batch in self.soak_heartbeat_batches
            for sequence in batch
            if sequence not in self.acknowledged_sequences
        ]
        if missing:
            missing_text = ", ".join(str(sequence) for sequence in missing)
            self.error = (
                "post-ready virtual-gamepad heartbeat was not acknowledged: "
                f"{missing_text}"
            )

    def advance(
        self, log_path: Path, elapsed: float, *, allow_soak_heartbeat: bool = True
    ) -> None:
        if self.error or self.command_fd < 0:
            return
        log_text = combined_cycle_text(log_path)
        if self.ready_elapsed is None:
            if f"[vpad] READY pads={self.players}" not in log_text:
                return
            self.ready_elapsed = elapsed
            self.phase = "boot"
            self.next_action_at = elapsed

        self.acknowledged_sequences = {
            int(match.group(1)) for match in VPAD_ACK.finditer(log_text)
        }
        self.last_ack = max(self.acknowledged_sequences, default=0)
        rejections = list(VPAD_REJECT.finditer(log_text))
        if rejections:
            rejected_sequence = int(rejections[-1].group(1))
            self.error = f"virtual-gamepad command {rejected_sequence} was rejected"
            return
        menu_matches = list(GE_TEST_MENU_STATE.finditer(log_text))
        if menu_matches:
            latest = menu_matches[-1]
            self.observed_state = int(latest.group(1))
            self.observed_menu = latest.group(2)
            self.observed_joined = int(latest.group(3))

        if self.soak_active:
            if allow_soak_heartbeat:
                self._advance_soak_heartbeat(elapsed)
            return

        if self.phase == "boot":
            if self.observed_state == 7:
                self.phase = "dossier-move"
                self.next_action_at = max(elapsed + 0.75, self.settled_at)
            elif (
                self.observed_state == 5
                and elapsed >= self.next_action_at
                and self._command_finished(elapsed)
            ):
                hold_ms = 250
                command = self._pulse_button(1, "START", hold_ms)
                if self._send(
                    [command],
                    elapsed,
                    "enter Dossier from input-ready title screen",
                    0.75,
                ):
                    self.next_action_at = elapsed + 2.0
            return

        if self.phase == "dossier-move":
            if (
                self.observed_state == 7
                and elapsed >= self.next_action_at
                and self._command_finished(elapsed)
            ):
                command = self._pulse_axis(1, "LY", 32767, 80)
                if self._send(
                    [command],
                    elapsed,
                    "select Multiplayer in Dossier",
                    0.6,
                ):
                    self.phase = "dossier-confirm"
            return

        if self.phase == "dossier-confirm":
            if self.observed_state == 7 and self._command_finished(elapsed):
                command = self._pulse_button(1, "SOUTH")
                if self._send(
                    [command],
                    elapsed,
                    "open Multiplayer Modes",
                    0.75,
                ):
                    self.phase = "waiting-for-multiplayer-modes"
            return

        if self.phase == "waiting-for-multiplayer-modes":
            if self.observed_state == 27:
                self.phase = "multiplayer-modes-move"
                self.next_action_at = elapsed + 0.75
            return

        if self.phase == "multiplayer-modes-move":
            if (
                self.observed_state == 27
                and elapsed >= self.next_action_at
                and self._command_finished(elapsed)
            ):
                command = self._pulse_axis(1, "LY", -32768, 80)
                if self._send(
                    [command],
                    elapsed,
                    "select Local in Multiplayer Modes",
                    0.6,
                ):
                    self.phase = "multiplayer-modes-confirm"
            return

        if self.phase == "multiplayer-modes-confirm":
            if self.observed_state == 27 and self._command_finished(elapsed):
                command = self._pulse_button(1, "SOUTH")
                if self._send(
                    [command],
                    elapsed,
                    "choose Local multiplayer",
                    0.75,
                ):
                    self.phase = "waiting-for-create-local-game"
            return

        if self.phase == "waiting-for-create-local-game":
            if self.observed_state == 15 and self.observed_joined >= 1:
                self.phase = "joining-guests"
                self.next_action_at = elapsed + 0.75
            return

        if self.phase == "joining-guests":
            if (
                self.observed_state == 15
                and elapsed >= self.next_action_at
                and self._command_finished(elapsed)
            ):
                commands = [
                    self._pulse_button(player, "START")
                    for player in range(2, self.players + 1)
                ]
                if self._send(
                    commands,
                    elapsed,
                    "join local guest players",
                    0.75,
                ):
                    self.phase = "waiting-for-guests"
            return

        if self.phase == "waiting-for-guests":
            if (
                self.observed_state == 15
                and self.observed_joined == self.players
                and self._command_finished(elapsed)
            ):
                command = self._pulse_button(1, "START")
                if self._send(
                    [command],
                    elapsed,
                    "start local match",
                    0.75,
                ):
                    self.phase = "starting-match"
            return

        if self.phase == "starting-match":
            if self._command_finished(elapsed):
                self.completed = True
                self.phase = "waiting-for-match-readiness"
                return

    def close(self) -> None:
        if self.command_fd >= 0:
            os.close(self.command_fd)
            self.command_fd = -1


def run_cycle(
    args: argparse.Namespace, suite_root: Path, number: int
) -> dict[str, Any]:
    global ACTIVE_PROCESS
    cycle_root = suite_root / f"cycle-{number:03d}"
    cycle_root.mkdir(parents=True)
    log_path = cycle_root / "raw.log"
    virtual_gamepad_read_fd: int | None = None
    virtual_gamepad_write_fd: int | None = None
    input_driver: LocalMultiplayerInputDriver | None = None
    if args.mode == "local-multiplayer":
        virtual_gamepad_read_fd, virtual_gamepad_write_fd = os.pipe()
    environment = build_environment(
        args, cycle_root, virtual_gamepad_fd=virtual_gamepad_read_fd
    )
    command = [
        str(args.executable),
        "--game_data_root",
        str(args.game_data),
        "--gpu",
        "metal",
    ]
    started = time.monotonic()
    started_epoch = time.time()
    ready = False
    ready_elapsed: float | None = None
    exited_before_ready: int | None = None
    exited_during_soak: int | None = None
    readiness_details: dict[str, Any] = {}
    pre_ready_abort_log_matches: list[str] = []
    soak_elapsed = 0.0
    soak_completed = False
    soak_abort_log_matches: list[str] = []
    launch_error: str | None = None
    with log_path.open("wb") as log:
        try:
            process_options: dict[str, Any] = {}
            if virtual_gamepad_read_fd is not None:
                process_options["pass_fds"] = (virtual_gamepad_read_fd,)
            process = subprocess.Popen(
                command,
                cwd=args.game_data,
                env=environment,
                stdout=log,
                stderr=subprocess.STDOUT,
                start_new_session=True,
                **process_options,
            )
            ACTIVE_PROCESS = process
            if virtual_gamepad_read_fd is not None:
                os.close(virtual_gamepad_read_fd)
                virtual_gamepad_read_fd = None
            if virtual_gamepad_write_fd is not None:
                input_driver = LocalMultiplayerInputDriver(
                    args.players, virtual_gamepad_write_fd
                )
                virtual_gamepad_write_fd = None
        except OSError as error:
            launch_error = str(error)
            process = None
            for descriptor in (
                virtual_gamepad_read_fd,
                virtual_gamepad_write_fd,
            ):
                if descriptor is not None:
                    os.close(descriptor)
            virtual_gamepad_read_fd = None
            virtual_gamepad_write_fd = None

        if process is not None:
            deadline = started + args.ready_timeout
            while time.monotonic() < deadline:
                log.flush()
                elapsed = time.monotonic() - started
                if process.poll() is not None:
                    exited_before_ready = process.returncode
                    break
                if input_driver:
                    input_driver.advance(log_path, elapsed)
                    if input_driver.error:
                        break
                pre_ready_abort_log_matches = detect_fatal_logs(cycle_root)
                if pre_ready_abort_log_matches:
                    break
                ready, readiness_details = readiness(
                    args.mode,
                    log_path,
                    elapsed,
                    args.menu_settle_seconds,
                    args.warmup_windows,
                    args.observe_windows,
                    args.players,
                    input_driver.last_sequence if input_driver else 0,
                )
                if ready:
                    ready_elapsed = elapsed
                    break
                time.sleep(args.poll_seconds)

            if ready:
                if input_driver:
                    input_driver.begin_post_ready_soak(
                        time.monotonic() - started,
                        heartbeat_enabled=args.post_ready_soak_seconds > 0,
                    )
                soak_started = time.monotonic()
                soak_deadline = soak_started + args.post_ready_soak_seconds
                while True:
                    now = time.monotonic()
                    soak_elapsed = now - soak_started
                    if now >= soak_deadline:
                        soak_completed = True
                        break
                    log.flush()
                    if process.poll() is not None:
                        exited_during_soak = process.returncode
                        break
                    if input_driver:
                        input_driver.advance(log_path, now - started)
                        if input_driver.error:
                            break
                    soak_abort_log_matches = detect_fatal_logs(cycle_root)
                    if soak_abort_log_matches:
                        break
                    time.sleep(min(args.poll_seconds, soak_deadline - now))

            if (
                input_driver
                and ready
                and soak_completed
                and exited_during_soak is None
                and not soak_abort_log_matches
            ):
                input_driver.finish_post_ready_soak(
                    log_path, time.monotonic() - started
                )
            if input_driver:
                input_driver.close()
            capture: str | None = None
            capture_error: str | None = None
            capture_skipped_reason: str | None = None
            capture_validation: dict[str, Any] | None = None
            regression: dict[str, Any] | None = None
            if ready and args.capture and process.poll() is not None:
                capture_skipped_reason = "application exited before capture"
            elif ready and args.capture:
                capture_path = cycle_root / f"{args.mode}.png"
                try:
                    capture_validation = rendering.capture_window(
                        ROOT,
                        process.pid,
                        capture_path,
                        args.capture_delay,
                        args.capture_retries,
                        min_luma_stddev=args.render_min_luma_stddev,
                    )
                    capture = str(capture_path)
                    if args.reference:
                        regression = compare_capture(
                            args.reference, capture_path, cycle_root, args
                        )
                except (
                    rendering.ImageError,
                    OSError,
                    subprocess.TimeoutExpired,
                ) as error:
                    capture_error = str(error)
            shutdown = terminate_process(
                process, args.quit_method, args.shutdown_timeout
            )
            ACTIVE_PROCESS = None
        else:
            capture = None
            capture_error = None
            capture_skipped_reason = None
            capture_validation = None
            regression = None
            shutdown = {"exit_code": None}
    input_script = {
        "sent": input_driver.sent if input_driver else [],
        "error": input_driver.error if input_driver else None,
        "phase": input_driver.phase if input_driver else None,
        "completed": input_driver.completed if input_driver else None,
        "soak_heartbeat_batches": (
            input_driver.soak_heartbeat_batches if input_driver else []
        ),
        "last_sequence": input_driver.last_sequence if input_driver else 0,
        "last_ack": input_driver.last_ack if input_driver else 0,
        "acknowledged_sequences": (
            sorted(input_driver.acknowledged_sequences) if input_driver else []
        ),
        "observed_state": input_driver.observed_state if input_driver else None,
        "observed_menu": input_driver.observed_menu if input_driver else None,
        "observed_joined": input_driver.observed_joined if input_driver else 0,
    }

    elapsed = time.monotonic() - started
    fatal_logs = sorted(
        set(pre_ready_abort_log_matches)
        .union(soak_abort_log_matches)
        .union(detect_fatal_logs(cycle_root))
    )
    crash_reports = collect_crash_reports(cycle_root, started_epoch)
    profile_artifacts: str | None = None
    final_profile_passed: bool | None = None
    final_profile_failures: list[str] = []
    final_windows, final_violations, final_counts = profile.parse_log(log_path)
    final_profile_failures = final_violations + profile.wait_reg_mem_violations(
        final_windows
    )
    if args.mode == "dam":
        final_selected = profile.select_windows(
            final_windows, args.warmup_windows, args.observe_windows
        )
        profile_root = cycle_root / "profile"
        final_profile_passed = profile.write_outputs(
            profile_root,
            final_windows,
            final_selected,
            final_violations,
            final_counts,
            args.warmup_windows,
            args.observe_windows,
        )
        profile_artifacts = str(profile_root)
    else:
        # Menu readiness was observed before capture and shutdown. Parse the
        # completed log too so a late fallback, malformed profile window, or
        # renderer failure cannot hide behind that earlier clean snapshot.
        final_profile_passed = not final_profile_failures
    failures: list[str] = []
    if launch_error:
        failures.append(f"launch failed: {launch_error}")
    if input_script["error"]:
        failures.append(f"virtual-gamepad input failed: {input_script['error']}")
    if (
        not ready
        and not input_script["error"]
        and not pre_ready_abort_log_matches
    ):
        if exited_before_ready is not None:
            failures.append(
                f"application exited with code {exited_before_ready} before {args.mode} readiness"
            )
        else:
            failures.append(
                f"{args.mode} readiness was not reached within {args.ready_timeout:.1f} seconds"
            )
    if exited_during_soak is not None:
        failures.append(
            f"application exited with code {exited_during_soak} during post-ready soak"
        )
    failures.extend(readiness_details.get("profile_failures", []))
    failures.extend(fatal_logs)
    if final_profile_passed is False:
        detail = "; see profile/summary.txt" if profile_artifacts else ""
        failures.append(f"final Metal profile validation failed{detail}")
        for profile_failure in final_profile_failures:
            if profile_failure not in failures:
                failures.append(profile_failure)
    failures.extend(
        f"macOS crash report captured: {Path(report).name}" for report in crash_reports
    )
    if capture_error:
        failures.append(f"capture failed: {capture_error}")
    if regression and regression["status"] != "pass":
        failures.extend(f"render regression: {item}" for item in regression["failures"])
    if shutdown.get("timed_out"):
        failures.append("application did not exit before the shutdown deadline")
    if shutdown.get("descendant_cleanup_required"):
        failures.append("application left a child process running after shutdown")
    if shutdown.get("process_exited_before_request") and exited_during_soak is None:
        failures.append(
            f"application exited before the requested shutdown (code {shutdown.get('exit_code')})"
        )
    if (
        args.quit_method == "native"
        and not shutdown.get("process_exited_before_request")
        and not shutdown.get("native_request_succeeded")
    ):
        failures.append("native clean-quit request was not accepted")
    if args.quit_method == "native" and shutdown.get("exit_code") not in (None, 0):
        failures.append(f"native clean quit returned {shutdown.get('exit_code')}")

    result = {
        "cycle": number,
        "status": "fail" if failures else "pass",
        "mode": args.mode,
        "command": command,
        "pid": process.pid if process is not None else None,
        "elapsed_seconds": elapsed,
        "ready": ready,
        "ready_seconds": ready_elapsed,
        "pre_ready_abort_log_matches": pre_ready_abort_log_matches,
        "post_ready_soak": {
            "requested_seconds": args.post_ready_soak_seconds,
            "elapsed_seconds": soak_elapsed if ready else None,
            "completed": soak_completed,
            "abort_log_matches": soak_abort_log_matches,
        },
        "paths": {
            "user_data": str(cycle_root / "user-data"),
            "cache": str(cycle_root / "cache"),
            "raw_log": str(log_path),
        },
        "readiness": readiness_details,
        "input_script": input_script,
        "capture": capture,
        "capture_skipped_reason": capture_skipped_reason,
        "capture_validation": capture_validation,
        "render_comparison": regression,
        "profile_artifacts": profile_artifacts,
        "final_profile_passed": final_profile_passed,
        "final_profile_failures": final_profile_failures,
        "shutdown": shutdown,
        "fatal_log_matches": fatal_logs,
        "crash_reports": crash_reports,
        "failures": failures,
    }
    (cycle_root / "cycle.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return result


def write_suite_summary(
    suite_root: Path, cycles: list[dict[str, Any]], args: argparse.Namespace
) -> bool:
    passed = sum(cycle["status"] == "pass" for cycle in cycles)
    readiness_times = [
        float(cycle["ready_seconds"])
        for cycle in cycles
        if cycle.get("ready_seconds") is not None
    ]
    mean_fps = []
    for cycle in cycles:
        fps = cycle.get("readiness", {}).get("aggregate", {}).get("real_window_fps")
        if fps:
            mean_fps.append(float(fps["mean"]))
    soak_cycles_completed = sum(
        bool(cycle.get("post_ready_soak", {}).get("completed"))
        for cycle in cycles
    )
    summary = {
        "status": "pass" if passed == len(cycles) else "fail",
        "mode": args.mode,
        "cycles_requested": len(cycles),
        "cycles_passed": passed,
        "native_clean_shutdowns": sum(
            cycle["shutdown"].get("native_request_succeeded", False)
            and cycle["shutdown"].get("exit_code") == 0
            for cycle in cycles
        ),
        "readiness_seconds": {
            "mean": statistics.fmean(readiness_times) if readiness_times else None,
            "min": min(readiness_times) if readiness_times else None,
            "max": max(readiness_times) if readiness_times else None,
        },
        "mean_window_fps_across_cycles": statistics.fmean(mean_fps)
        if mean_fps
        else None,
        "isolated_state": True,
        "game_data": str(args.game_data),
        "reference": str(args.reference) if args.reference else None,
        "post_ready_soak_seconds": args.post_ready_soak_seconds,
        "post_ready_soak_cycles_completed": soak_cycles_completed,
        "cycles": cycles,
    }
    (suite_root / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    lines = [
        f"status: {summary['status']}",
        f"mode: {args.mode}",
        f"cycles: {passed}/{len(cycles)} passed",
        f"native clean shutdowns: {summary['native_clean_shutdowns']}/{len(cycles)}",
        (
            f"requested post-ready soak: {args.post_ready_soak_seconds:g} seconds; "
            f"completed: {soak_cycles_completed}/{len(cycles)} cycles"
        ),
    ]
    if mean_fps:
        lines.append(
            f"mean 64-frame-window FPS across cycles: {statistics.fmean(mean_fps):.3f}"
        )
    for cycle in cycles:
        if cycle["failures"]:
            lines.append(f"cycle {cycle['cycle']}: " + "; ".join(cycle["failures"]))
    (suite_root / "summary.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return summary["status"] == "pass"


def main() -> int:
    global ACTIVE_PROCESS
    release_app_contents = (
        ROOT
        / "vendor/GoldenEye-Recomp/out/build/macos-arm64-release/dist"
        / "GoldenEye Metal.app/Contents"
    )
    multiplayer_test_app_contents = (
        ROOT
        / "vendor/GoldenEye-Recomp/out/build/macos-arm64-multiplayer-test/dist"
        / "GoldenEye Metal.app/Contents"
    )
    default_game_data = (
        Path.home() / "Library/Application Support/GoldenEye Metal/Game Data"
    )
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cycles", type=positive_integer, default=3)
    parser.add_argument(
        "--mode",
        choices=("menu", "dam", "local-multiplayer"),
        default="dam",
    )
    parser.add_argument(
        "--players",
        type=int,
        default=2,
        help="local multiplayer test player count (2-4)",
    )
    parser.add_argument("--executable", type=Path)
    parser.add_argument("--runtime-dir", type=Path)
    parser.add_argument("--game-data", type=Path, default=default_game_data)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--ready-timeout", type=finite_float, default=150.0)
    parser.add_argument(
        "--post-ready-soak-seconds",
        type=finite_float,
        default=10.0,
        help=(
            "keep each ready run alive for this many additional seconds while "
            "checking stability markers"
        ),
    )
    parser.add_argument("--menu-settle-seconds", type=finite_float, default=20.0)
    parser.add_argument("--poll-seconds", type=finite_float, default=1.0)
    parser.add_argument("--warmup-windows", type=int, default=1)
    parser.add_argument("--observe-windows", type=positive_integer, default=3)
    parser.add_argument("--shutdown-timeout", type=finite_float, default=20.0)
    parser.add_argument(
        "--quit-method",
        choices=("native", "signal"),
        default="native",
        help="native validates normal AppKit quit; signal is for harness self-tests",
    )
    parser.add_argument("--capture", action="store_true")
    parser.add_argument("--capture-delay", type=finite_float, default=0.0)
    parser.add_argument("--capture-retries", type=positive_integer, default=10)
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--render-pixel-threshold", type=int, default=16)
    parser.add_argument("--render-max-changed-ratio", type=finite_float, default=0.02)
    parser.add_argument("--render-max-mae", type=finite_float, default=3.0)
    parser.add_argument("--render-max-coarse-mae", type=finite_float, default=4.0)
    parser.add_argument("--render-min-luma-stddev", type=finite_float, default=1.0)
    parser.add_argument(
        "--render-ignore",
        type=rendering.parse_rectangle,
        action="append",
        default=[],
        metavar="X,Y,W,H",
    )
    parser.add_argument("--skip-metadata", action="store_true")
    parser.add_argument(
        "--allow-stale-build",
        action="store_true",
        help="allow artifacts older than tracked source files (automation self-tests only)",
    )
    args = parser.parse_args()

    selected_app_contents = (
        multiplayer_test_app_contents
        if args.mode == "local-multiplayer"
        else release_app_contents
    )
    if args.executable is None:
        args.executable = selected_app_contents / "MacOS/GoldenEye"
    if args.runtime_dir is None:
        args.runtime_dir = (
            selected_app_contents / "Frameworks"
            if args.executable == selected_app_contents / "MacOS/GoldenEye"
            else args.executable.parent.parent / "Frameworks"
        )
    args.executable = args.executable.expanduser().resolve()
    args.runtime_dir = args.runtime_dir.expanduser().resolve()
    args.game_data = args.game_data.expanduser().resolve()
    requested_output = args.output.expanduser().resolve() if args.output else None
    args.reference = args.reference.expanduser().resolve() if args.reference else None
    if args.reference and not args.capture:
        parser.error("--reference requires --capture")
    if args.reference and args.mode != "menu":
        parser.error(
            "Dam gameplay is not frame-synchronized; --reference is supported only with --mode menu"
        )
    if not args.executable.is_file() or not os.access(args.executable, os.X_OK):
        parser.error(f"executable is not runnable: {args.executable}")
    dylib = args.runtime_dir / "librexruntime.dylib"
    if not dylib.is_file():
        parser.error(f"runtime library does not exist: {dylib}")
    if args.mode == "local-multiplayer":
        if not binary_contains(
            dylib, b"REX_TEST_VIRTUAL_GAMEPADS"
        ) or not binary_contains(args.executable, b"[ge-test] menu state="):
            parser.error(
                "local multiplayer mode requires the developer harness build; "
                "configure preset macos-arm64-multiplayer-test and build target "
                "goldeneye_macos_app"
            )
    if not (args.game_data / "default.xex").is_file():
        parser.error(f"game-data directory has no default.xex: {args.game_data}")
    if args.reference and not args.reference.is_file():
        parser.error(f"reference does not exist: {args.reference}")
    if args.warmup_windows < 0:
        parser.error("--warmup-windows must not be negative")
    if args.players < 2 or args.players > 4:
        parser.error("--players must be between 2 and 4")
    if not 0 <= args.render_pixel_threshold <= 255:
        parser.error("--render-pixel-threshold must be between 0 and 255")
    if not 0 <= args.render_max_changed_ratio <= 1:
        parser.error("--render-max-changed-ratio must be between 0 and 1")
    if (
        min(
            args.render_max_mae,
            args.render_max_coarse_mae,
            args.render_min_luma_stddev,
        )
        < 0
    ):
        parser.error("render comparison thresholds must not be negative")
    if (
        args.ready_timeout <= 0
        or args.poll_seconds <= 0
        or args.shutdown_timeout <= 0
        or args.post_ready_soak_seconds < 0
        or args.menu_settle_seconds < 0
        or args.capture_delay < 0
    ):
        parser.error(
            "timeouts/polling must be positive and delays must not be negative"
        )
    if not args.allow_stale_build:
        freshness = profile.build_freshness_report(ROOT, dylib, args.executable)
        if not freshness["fresh"]:
            parser.error("; ".join(freshness["failures"]))
    if args.capture or args.quit_method == "native":
        try:
            rendering.prepare_macos_app_control(ROOT)
        except (rendering.ImageError, OSError, subprocess.TimeoutExpired) as error:
            parser.error(f"macOS application helper is unavailable: {error}")
    if requested_output:
        suite_root = requested_output
        suite_root.mkdir(parents=True, exist_ok=False)
    else:
        stability_root = ROOT / "out/stability"
        stability_root.mkdir(parents=True, exist_ok=True)
        suite_root = Path(
            tempfile.mkdtemp(prefix=f"{timestamp()}.", dir=stability_root)
        )

    if not args.skip_metadata:
        profile.write_metadata(
            SimpleNamespace(
                executable=str(args.executable),
                dylib=str(dylib),
                xex=str(args.game_data / "default.xex"),
                repo=str(ROOT),
                data_root=str(args.game_data),
                metadata=str(suite_root / "metadata.json"),
                effective_environment=build_environment(
                    args, suite_root / "cycle-001", create_directories=False
                ),
            )
        )
    print(f"stability-cycle: {args.cycles} {args.mode} cycle(s) -> {suite_root}")
    cycles: list[dict[str, Any]] = []
    try:
        for number in range(1, args.cycles + 1):
            cycle = run_cycle(args, suite_root, number)
            cycles.append(cycle)
            print(f"cycle {number}/{args.cycles}: {cycle['status']}")
    except BaseException:
        if ACTIVE_PROCESS is not None and ACTIVE_PROCESS.poll() is None:
            try:
                terminate_process(
                    ACTIVE_PROCESS, "signal", min(5.0, args.shutdown_timeout)
                )
            except BaseException:
                ACTIVE_PROCESS.kill()
        ACTIVE_PROCESS = None
        raise
    success = write_suite_summary(suite_root, cycles, args)
    print(f"{'PASS' if success else 'FAIL'}: {suite_root / 'summary.txt'}")
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
