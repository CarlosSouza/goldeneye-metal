#!/usr/bin/env python3
"""Repeat GoldenEye boot/menu/Dam/shutdown cycles in isolated state roots."""

from __future__ import annotations

import argparse
import hashlib
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
from typing import Any, Mapping, Sequence

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import gameplay_scenario as gameplay  # noqa: E402
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
    re.compile(r"completion event wait failed; disabling direct", re.I),
    re.compile(r"Metal direct presentation copy failed", re.I),
    re.compile(r"WAIT_REG_MEM stalled >60ms", re.I),
    re.compile(r"Metal MSL translation (?:failed|unavailable)", re.I),
    re.compile(r"MetalDrawRenderer[^\r\n]*(?:compile|pipeline|command buffer) failed", re.I),
    re.compile(
        r"\b(?:MTLCommandBufferError[A-Za-z0-9_]*|"
        r"Command Buffer execution failed|"
        r"Execution of the command buffer was aborted|"
        r"IOAF code|"
        r"kIOAccelCommandBufferCallbackError[A-Za-z0-9_]*|"
        r"GPU (?:Address Fault|Hang|Page Fault) Error)\b",
        re.I,
    ),
    re.compile(
        r"(?:\bMetal(?:\s+API)? Validation Error\b|"
        r"\bMTLDebug[A-Za-z0-9_]*\b[^\r\n]*"
        r"\b(?:failed assertion|validation error)\b)",
        re.I,
    ),
    re.compile(
        r"\[metal\]\s+(?:"
        r"native depth resolve|GPU tiled resolve|host RT resolve-clear|"
        r"host depth/stencil resolve-clear|readback resolve write|"
        r"GoldenEye restore color snapshot|GoldenEye restore snapshot allocation|"
        r"asynchronous probe wait"
        r") failed#",
        re.I,
    ),
    re.compile(
        r"\[metal\]\s+(?:render|fullscreen|host|solid|memexport|persistent|guest)"
        r"[^\r\n]{0,120}(?:pipeline|shader|compile|render|residency|publication|"
        r"context|setup)[^\r\n]*failed#",
        re.I,
    ),
    re.compile(
        r"^\[metal\](?![^\r\n]*\b0 failed\b)[^\r\n]*"
        r"(?:\bfailed(?:#\d+)?(?=\s|:|$)|"
        r"\bunavailable(?=\s*(?:[:;#]|$))|"
        r"\bwith error(?=\s*:))[^\r\n]*$",
        re.I | re.M,
    ),
    re.compile(
        r"^(?!\[metal\])[^\r\n]*\bMetal"
        r"(?! shader cache shutdown:)[^\r\n]*"
        r"(?:\bfailed\b|\bunavailable(?=\s*(?:[:;#]|$)))[^\r\n]*$",
        re.I | re.M,
    ),
    re.compile(
        r"\bMetal shader cache shutdown:[^\r\n]*,\s*[1-9]\d*\s+failed\b",
        re.I,
    ),
    re.compile(
        r"^\[metal\]\s+(?:wrote fallback resolve|"
        r"presenting latest pipeline probe fallback|CPU fallback|"
        r"preferred fallback candidate|skipped untextured fallback resolve)\b"
        r"[^\r\n]*$",
        re.I | re.M,
    ),
    re.compile(r"\[vpad\] FAILED", re.I),
    re.compile(r"\[ge-test\] gpu-capture failed\b", re.I),
    re.compile(r"\[ge-test\] host-pause failed\b", re.I),
    re.compile(r"\[metal\] probe texture dummy#[1-9]\d*", re.I),
    # Match only emitted stall records, not aggregate text such as
    # "GEWATCHDOG STALL count=0".
    re.compile(r"\bGEWATCHDOG STALL(?=:\s+ring\b|\s+rpi=0x[0-9a-f]+\b)", re.I),
    re.compile(r"\bGENOPRESENT STALL(?=:\s+ring\b|\s+rpi=0x[0-9a-f]+\b)", re.I),
    re.compile(
        r"\[GE-PLAYER-STUCK-v1\]\s+"
        r"(?:suspected live-render movement stall:|pipeline ring=|sample=\d+/\d+\s)",
        re.I,
    ),
    # Snapshot overflow/underflow means a callback-stack invariant was lost,
    # even if a later recovery happened to keep the process alive.
    re.compile(
        r"\[GE-GUARD-(?:823CFC00-v2|AUDIO-CALLBACK-v1)\]\s+"
        r"(?:callback )?snapshot (?:overflow|underflow)\s+hit=[1-9]\d*\b",
        re.I,
    ),
    # Guard activation and diagnostic snapshots are expected. A concrete
    # recovery means the run survived a guest lifecycle bug and is degraded,
    # so it must not pass the release gate silently.
    re.compile(r"\[GE-GUARD-[^\]]+\]\s+recovered\b", re.I),
    re.compile(
        r"\[GE-GUARD-[^\]]+\]\s+repaired callback ABI hit=[1-9]\d*\b",
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

DUMMY_TEXTURE = re.compile(
    r"^\[metal\] probe texture dummy#(?P<index>[1-9]\d*) "
    r"(?P<stage>vs|ps) shader=(?P<shader>[0-9a-f]{16}) "
    r"binding=(?P<binding>\d+) fetch=(?P<fetch>\d+) "
    r"type=(?P<type>\d+) fmt=(?P<fmt>\d+) dim=(?P<dim>\d+) "
    r"dwords=(?P<words>(?:[0-9a-f]{8}\s+){5}[0-9a-f]{8})$",
    re.I | re.M,
)
DEFAULT_MIN_MEAN_FPS = 25.0
DEFAULT_MAX_WINDOW_P99_MS = 50.0
PROFILE_WINDOW_SIZE = profile.PROFILE_WINDOW_SIZE
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
GPU_CAPTURE_REQUESTED = re.compile(
    r"\[ge-test\] gpu-capture requested token=(?P<token>\d+) "
    r"dam_ready=1 frames_progressed=1 presents_progressed=1"
)
GPU_CAPTURE_VALIDATED = re.compile(
    r"\[ge-test\] gpu-capture validated token=(?P<token>\d+) "
    r"state=Complete bytes=(?P<bytes>\d+) frames=1 private=1 partial=0"
)
GPU_CAPTURE_FAILED = re.compile(
    r"\[ge-test\] gpu-capture failed token=(?P<token>\d+) "
    r"state=(?P<state>[A-Za-z0-9_-]+) validation=0"
)
EXACT_OUTPUT_MERGER_DRAW = re.compile(
    r"\[metal\] exact output-merger draw (?:enqueued|completed)#(?P<index>\d+)",
    re.I,
)
TRACE_REPLAY_TIMEOUT_SECONDS = 120.0
TRACE_CAPTURE_RELATIVE_PATH = Path("user-data") / "GPU Captures" / "latest.xtr"


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


def host_pause_event_source_text(raw_log_path: Path) -> str:
    """Select one coherent Host Settings event stream across mirrored sinks."""
    candidates = [raw_log_path]
    runtime_logs = raw_log_path.parent / "user-data" / "Logs"
    if runtime_logs.is_dir():
        candidates.extend(sorted(runtime_logs.glob("*.log")))

    sequences: list[tuple[str, ...]] = []
    for candidate in candidates:
        payloads = tuple(
            line.split(gameplay.HOST_PAUSE_EVENT_PREFIX, 1)[1].strip()
            for line in safe_text(candidate).splitlines()
            if gameplay.HOST_PAUSE_EVENT_PREFIX in line
        )
        if payloads:
            sequences.append(payloads)
    if not sequences:
        return ""

    longest = max(sequences, key=len)
    if any(sequence != longest[: len(sequence)] for sequence in sequences):
        # Feed an intentionally malformed event to the exact parser. Divergent
        # sinks are ambiguous evidence and must never be silently reconciled.
        return gameplay.HOST_PAUSE_EVENT_PREFIX + "conflicting-event-sinks"
    return "\n".join(
        gameplay.HOST_PAUSE_EVENT_PREFIX + payload for payload in longest
    )


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


def dummy_texture_violations(log_path: Path, mode: str | None = None) -> list[str]:
    """Reject every dummy texture binding in every mode."""
    matches = list(DUMMY_TEXTURE.finditer(safe_text(log_path)))
    if not matches:
        return []
    first = matches[0]
    return [
        "dummy texture bindings observed: "
        f"count={len(matches)} first={first.group('stage').lower()}:"
        f"{first.group('shader').lower()} binding={first.group('binding')}"
    ]


def gpu_capture_evidence(log_path: Path) -> dict[str, Any]:
    """Return bounded proof emitted after the app validates its private trace."""
    text = combined_cycle_text(log_path)
    requests = list(GPU_CAPTURE_REQUESTED.finditer(text))
    validated = list(GPU_CAPTURE_VALIDATED.finditer(text))
    failed = list(GPU_CAPTURE_FAILED.finditer(text))
    evidence: dict[str, Any] = {
        "requested": bool(requests),
        "validated": False,
        "request_token": None,
        "byte_count": 0,
        "frame_count": 0,
        "private_permissions": False,
        "partial_absent": False,
        "failure_state": failed[-1].group("state") if failed else None,
    }
    if not requests or not validated or failed:
        return evidence
    request_token = int(requests[-1].group("token"))
    validation_token = int(validated[-1].group("token"))
    byte_count = int(validated[-1].group("bytes"))
    if request_token != validation_token or byte_count <= 0:
        return evidence
    evidence.update(
        {
            "validated": True,
            "request_token": request_token,
            "byte_count": byte_count,
            "frame_count": 1,
            "private_permissions": True,
            "partial_absent": True,
        }
    )
    return evidence


def locate_trace_replay_executable(
    explicit: Path | None, game_executable: Path | None = None
) -> Path | None:
    """Locate the source-tree-only Metal trace runner without building it."""
    if explicit is not None:
        return explicit.expanduser().resolve()

    candidates: list[Path] = []
    if game_executable is not None:
        resolved_game = game_executable.expanduser().resolve()
        if resolved_game.is_relative_to(ROOT):
            for parent in resolved_game.parents:
                candidates.append(parent / "trace_dump_metal")
                if parent == ROOT:
                    break
    candidates.extend(
        (
            ROOT / "out/tools/metal-trace-dump/trace_dump_metal",
            ROOT
            / "vendor/GoldenEye-Recomp/out/build/macos-arm64-release/trace_dump_metal",
        )
    )
    seen: set[Path] = set()
    for candidate in candidates:
        resolved = candidate.resolve()
        if resolved in seen:
            continue
        seen.add(resolved)
        if resolved.is_file() and os.access(resolved, os.X_OK):
            return resolved
    return None


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def bmp_dimensions(path: Path) -> tuple[int, int] | None:
    try:
        with path.open("rb") as stream:
            header = stream.read(54)
    except OSError:
        return None
    if len(header) != 54 or header[:2] != b"BM":
        return None
    dib_size = int.from_bytes(header[14:18], "little")
    width = int.from_bytes(header[18:22], "little", signed=True)
    height = int.from_bytes(header[22:26], "little", signed=True)
    planes = int.from_bytes(header[26:28], "little")
    bits_per_pixel = int.from_bytes(header[28:30], "little")
    if dib_size < 40 or width <= 0 or height == 0 or planes != 1 or bits_per_pixel != 24:
        return None
    return width, abs(height)


def _private_directory(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True, mode=0o700)
    path.chmod(0o700)


def capture_replay_environment(
    replay_root: Path,
    runtime_dir: Path | None,
    *,
    exact_output_merger: bool = False,
) -> dict[str, str]:
    environment = os.environ.copy()
    for variable in tuple(environment):
        if variable.startswith(("GOLDENEYE_", "REX_")):
            environment.pop(variable, None)
    for directory_name in ("home", "tmp", "cache", "user-data"):
        _private_directory(replay_root / directory_name)
    environment.update(
        {
            "HOME": str(replay_root / "home"),
            "TMPDIR": str(replay_root / "tmp"),
            "REX_GPU": "metal",
            "REX_USER_DATA_ROOT": str(replay_root / "user-data"),
            "REX_CACHE_PATH": str(replay_root / "cache"),
            "REX_METAL_SHOW_FPS": "false",
        }
    )
    if exact_output_merger:
        environment["REX_METAL_EXACT_OUTPUT_MERGER"] = "true"
    if runtime_dir is not None:
        environment["DYLD_LIBRARY_PATH"] = str(runtime_dir)
    return environment


def run_capture_replay_gate(
    trace_path: Path,
    replay_executable: Path | None,
    cycle_root: Path,
    runtime_dir: Path | None,
    *,
    expected_trace_bytes: int,
    timeout_seconds: float = TRACE_REPLAY_TIMEOUT_SECONDS,
    exact_output_merger: bool = False,
) -> dict[str, Any]:
    """Strictly preflight and replay one private trace twice through Metal."""
    replay_root = cycle_root / "gpu-replay"
    _private_directory(replay_root)
    report: dict[str, Any] = {
        "status": "fail",
        "strict_deterministic_preflight": True,
        "best_effort": False,
        "frame_index": 0,
        "timeout_seconds": timeout_seconds,
        "trace": {
            "path": str(trace_path),
            "expected_byte_count": expected_trace_bytes,
            "byte_count": 0,
            "sha256": None,
        },
        "replay_executable": str(replay_executable) if replay_executable else None,
        "runs": [],
        "comparison": {
            "dimensions_equal": False,
            "rgba_byte_counts_equal": False,
            "rgba_sha256_equal": False,
        },
        "failures": [],
    }
    failures: list[str] = report["failures"]

    try:
        canonical_cycle_root = cycle_root.resolve(strict=True)
        canonical_trace = trace_path.resolve(strict=True)
        if not canonical_trace.is_relative_to(canonical_cycle_root):
            failures.append("private trace resolved outside the cycle output")
            return report
        capture_root = cycle_root / "user-data" / "GPU Captures"
        if any(path.is_symlink() for path in (capture_root, trace_path)):
            failures.append("private trace path contains a symbolic link")
            return report
        trace_bytes = canonical_trace.stat().st_size
        if trace_bytes <= 0:
            failures.append("private trace is empty")
            return report
        report["trace"]["byte_count"] = trace_bytes
        report["trace"]["sha256"] = file_sha256(canonical_trace)
        if trace_bytes != expected_trace_bytes:
            failures.append(
                "private trace byte count changed after in-app validation: "
                f"expected {expected_trace_bytes}, found {trace_bytes}"
            )
            return report
    except OSError as error:
        failures.append(f"private trace is unavailable after shutdown: {error}")
        return report

    if replay_executable is None:
        failures.append("trace_dump_metal executable was not found")
        return report
    replay_executable = replay_executable.expanduser().resolve()
    if not replay_executable.is_file() or not os.access(replay_executable, os.X_OK):
        failures.append(f"trace_dump_metal is not runnable: {replay_executable}")
        return report
    report["replay_executable"] = str(replay_executable)

    environment = capture_replay_environment(
        replay_root,
        runtime_dir,
        exact_output_merger=exact_output_merger,
    )
    for run_number in (1, 2):
        run_root = replay_root / f"run-{run_number}"
        _private_directory(run_root)
        output_base = run_root / "frame"
        log_path = run_root / "replay.log"
        command = [
            str(replay_executable),
            str(trace_path),
            str(output_base),
            "0",
        ]
        run_report: dict[str, Any] = {
            "run": run_number,
            "status": "fail",
            "command": command,
            "output_base": str(output_base),
            "log_path": str(log_path),
            "exit_code": None,
            "timed_out": False,
            "elapsed_seconds": None,
            "bmp": None,
            "rgba": None,
            "failures": [],
        }
        report["runs"].append(run_report)
        started = time.monotonic()
        process: subprocess.Popen[bytes] | None = None
        try:
            descriptor = os.open(
                log_path,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL,
                0o600,
            )
            with os.fdopen(descriptor, "wb") as log:
                process = subprocess.Popen(
                    command,
                    cwd=replay_root,
                    env=environment,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    start_new_session=True,
                    umask=0o077,
                )
                try:
                    run_report["exit_code"] = process.wait(timeout=timeout_seconds)
                except subprocess.TimeoutExpired:
                    run_report["timed_out"] = True
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    run_report["exit_code"] = process.wait()
        except OSError as error:
            run_report["failures"].append(f"could not launch replay: {error}")
        finally:
            run_report["elapsed_seconds"] = time.monotonic() - started

        if run_report["timed_out"]:
            run_report["failures"].append(
                f"replay exceeded {timeout_seconds:g} seconds"
            )
        elif not run_report["failures"] and run_report["exit_code"] != 0:
            run_report["failures"].append(
                f"strict Metal replay exited with code {run_report['exit_code']}"
            )

        bmp_path = output_base.with_suffix(".bmp")
        rgba_path = output_base.with_suffix(".rgba")
        dimensions = (
            bmp_dimensions(bmp_path)
            if bmp_path.is_file() and not bmp_path.is_symlink()
            else None
        )
        if dimensions is None:
            run_report["failures"].append("replay did not produce a valid nonzero BMP")
        else:
            bmp_path.chmod(0o600)
            run_report["bmp"] = {
                "path": str(bmp_path),
                "width": dimensions[0],
                "height": dimensions[1],
                "byte_count": bmp_path.stat().st_size,
                "sha256": file_sha256(bmp_path),
            }
        if not rgba_path.is_file() or rgba_path.is_symlink():
            run_report["failures"].append("replay did not produce a regular RGBA output")
        else:
            rgba_path.chmod(0o600)
            rgba_bytes = rgba_path.stat().st_size
            expected_rgba_bytes = (
                dimensions[0] * dimensions[1] * 4 if dimensions else 0
            )
            run_report["rgba"] = {
                "path": str(rgba_path),
                "byte_count": rgba_bytes,
                "expected_byte_count": expected_rgba_bytes,
                "sha256": file_sha256(rgba_path),
            }
            if rgba_bytes <= 0 or rgba_bytes != expected_rgba_bytes:
                run_report["failures"].append(
                    "RGBA byte count does not match the replay dimensions: "
                    f"expected {expected_rgba_bytes}, found {rgba_bytes}"
                )
        if not run_report["failures"]:
            run_report["status"] = "pass"
        else:
            failures.extend(
                f"replay {run_number}: {failure}"
                for failure in run_report["failures"]
            )

    if len(report["runs"]) == 2:
        first, second = report["runs"]
        first_bmp, second_bmp = first.get("bmp"), second.get("bmp")
        first_rgba, second_rgba = first.get("rgba"), second.get("rgba")
        comparison = report["comparison"]
        comparison["dimensions_equal"] = bool(
            first_bmp
            and second_bmp
            and (first_bmp["width"], first_bmp["height"])
            == (second_bmp["width"], second_bmp["height"])
        )
        comparison["rgba_byte_counts_equal"] = bool(
            first_rgba
            and second_rgba
            and first_rgba["byte_count"] == second_rgba["byte_count"]
        )
        comparison["rgba_sha256_equal"] = bool(
            first_rgba
            and second_rgba
            and first_rgba["sha256"] == second_rgba["sha256"]
        )
        if not comparison["dimensions_equal"]:
            failures.append("Metal replay dimensions differ between runs")
        if not comparison["rgba_byte_counts_equal"]:
            failures.append("Metal replay RGBA byte counts differ between runs")
        if not comparison["rgba_sha256_equal"]:
            failures.append("Metal replay RGBA hashes differ between runs")

    if not failures:
        report["status"] = "pass"
    return report


def exact_output_merger_evidence(log_path: Path) -> dict[str, Any]:
    """Return bounded proof that a title draw crossed the exact route boundary."""
    indices = {
        int(match.group("index"))
        for match in EXACT_OUTPUT_MERGER_DRAW.finditer(combined_cycle_text(log_path))
    }
    return {
        "validated": bool(indices),
        "logged_draw_count": len(indices),
        "highest_logged_draw_index": max(indices) if indices else None,
    }


def host_pause_integration_evidence(
    event_text: str, input_script: dict[str, Any]
) -> dict[str, Any]:
    """Combine the runtime pause proof with a bounded post-resume input proof."""
    result = gameplay.parse_host_pause_evidence(event_text)
    scenario = input_script.get("scenario")
    scenario_evidence = (
        scenario.get("evidence", {}) if isinstance(scenario, dict) else {}
    )
    neutral_sample = scenario_evidence.get("host_resume_neutral_baseline_sample")
    input_sample = scenario_evidence.get("look_input_sample")
    camera_effect_sample = scenario_evidence.get("camera_effect_sample")
    camera_delta = scenario_evidence.get("camera_delta")
    baseline_position_drift = scenario_evidence.get(
        "host_resume_baseline_position_drift"
    )
    baseline_camera_drift = scenario_evidence.get(
        "host_resume_baseline_camera_drift"
    )

    def integer(value: object) -> bool:
        return isinstance(value, int) and not isinstance(value, bool)

    def finite_number(value: object) -> bool:
        return (
            isinstance(value, (int, float))
            and not isinstance(value, bool)
            and math.isfinite(float(value))
        )

    ordered_camera_effect = bool(
        integer(neutral_sample)
        and integer(input_sample)
        and integer(camera_effect_sample)
        and neutral_sample < input_sample < camera_effect_sample
    )
    post_resume_input = {
        "bounded": scenario_evidence.get("host_resume_probe_bounded") is True,
        "hold_ms": scenario_evidence.get("host_resume_probe_hold_ms"),
        "sequence": scenario_evidence.get("host_resume_probe_sequence"),
        "neutral_baseline_sample": neutral_sample,
        "baseline_position_drift": baseline_position_drift,
        "baseline_camera_drift": baseline_camera_drift,
        "input_sample": input_sample,
        "camera_effect_sample": camera_effect_sample,
        "camera_delta": camera_delta,
        "released_to_neutral": (
            scenario_evidence.get("host_resume_probe_released") is True
        ),
        "ordered_camera_effect": ordered_camera_effect,
    }
    post_resume_input["validated"] = bool(
        post_resume_input["bounded"]
        and post_resume_input["hold_ms"]
        == gameplay.DamGameplayScenario.HOST_RESUME_PULSE_MS
        and integer(post_resume_input["sequence"])
        and post_resume_input["sequence"] > 0
        and finite_number(baseline_position_drift)
        and 0.0
        <= float(baseline_position_drift)
        <= gameplay.DamGameplayScenario.HOST_RESUME_BASELINE_POSITION_DRIFT
        and finite_number(baseline_camera_drift)
        and 0.0
        <= float(baseline_camera_drift)
        <= gameplay.DamGameplayScenario.HOST_RESUME_BASELINE_CAMERA_DRIFT
        and scenario_evidence.get("look_input_observed") is True
        and ordered_camera_effect
        and finite_number(camera_delta)
        and float(camera_delta) >= gameplay.DamGameplayScenario.CAMERA_DELTA
        and post_resume_input["released_to_neutral"]
    )
    result["post_resume_input"] = post_resume_input
    input_after_resume = bool(
        input_script.get("completed") is True
        and post_resume_input["validated"]
        and scenario_evidence.get("movement_input_observed") is True
        and scenario_evidence.get("fire_input_observed") is True
        and scenario_evidence.get("fire_effect_observed") is True
        and scenario_evidence.get("final_neutral_observed") is True
    )
    result["input_after_resume"] = input_after_resume
    if result["validated"] and not input_after_resume:
        result["validated"] = False
        result["status"] = "failed"
        result["failure"] = "post-resume gameplay input was not proven"
    return result


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
    profile_anchor_swap_end: int | None = None,
) -> tuple[bool, dict[str, Any]]:
    windows, violations, counts = profile.parse_log(
        log_path, defer_open_window=True
    )
    complete_windows = [window for window in windows if window["complete"]]
    log_text = combined_cycle_text(log_path)
    details: dict[str, Any] = {
        "profile_windows": len(windows),
        "complete_profile_windows": len(complete_windows),
        "pending_profile_windows": sum(
            bool(window.get("pending_ledgers")) for window in windows
        ),
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
        details["profile_anchor_swap_end"] = profile_anchor_swap_end
        post_match_windows = contiguous_profile_windows_after_anchor(
            windows, profile_anchor_swap_end
        )
        selected = post_match_windows[warmup : warmup + observe]
        details["post_match_profile_windows"] = len(post_match_windows)
        details["selected_windows"] = len(selected)
        details["selected_profile_ranges"] = [
            [int(window["swap_start"]), int(window["swap_end"])]
            for window in selected
        ]
        if len(selected) == observe:
            details["aggregate"] = profile.aggregate(selected)
        return (
            bool(matching)
            and details["virtual_gamepads_ready"]
            and not missing_acknowledgements
            and len(selected) == observe,
            details,
        )
    if mode == "dam-gameplay":
        post_dam_windows = contiguous_profile_windows_after_anchor(
            windows,
            profile_anchor_swap_end,
            require_dam_candidate=True,
        )
        selected = post_dam_windows[warmup : warmup + observe]
        details["profile_anchor_swap_end"] = profile_anchor_swap_end
        details["post_dam_profile_windows"] = len(post_dam_windows)
        details["selected_windows"] = len(selected)
        details["selected_profile_ranges"] = [
            [int(window["swap_start"]), int(window["swap_end"])]
            for window in selected
        ]
        if len(selected) == observe:
            details["aggregate"] = profile.aggregate(selected)
            return True, details
        return False, details
    if mode == "menu":
        injected = "GOLDENEYE_AUTO_START=menu injecting Start" in log_text
        details["auto_start_seen"] = injected
        return (
            injected
            and elapsed >= menu_settle_seconds
            and bool(complete_windows),
            details,
        )
    selected = profile.select_windows(windows, warmup, observe)
    details["selected_windows"] = len(selected)
    if len(selected) == observe:
        details["aggregate"] = profile.aggregate(selected)
        return True, details
    return False, details


def contiguous_profile_windows_after_anchor(
    windows: list[dict[str, Any]],
    anchor_swap_end: int | None,
    *,
    require_dam_candidate: bool = False,
) -> list[dict[str, Any]]:
    """Return the uninterrupted complete profile run strictly after an anchor."""
    if anchor_swap_end is None:
        return []
    expected_start = int(anchor_swap_end) + 1
    contiguous: list[dict[str, Any]] = []
    for window in windows:
        start = int(window["swap_start"])
        end = int(window["swap_end"])
        if end <= anchor_swap_end or start <= anchor_swap_end:
            continue
        if start != expected_start:
            break
        if not window["complete"] or (
            require_dam_candidate and not window.get("dam_candidate", False)
        ):
            break
        contiguous.append(window)
        expected_start = end + 1
    return contiguous


def profile_anchor_after_live_frontier(log_path: Path) -> int:
    """Exclude every complete cohort plus the cohort currently in flight."""
    windows, _, _ = profile.parse_log(log_path, defer_open_window=True)
    completed_frontier = max(
        (int(window["swap_end"]) for window in windows if window["complete"]),
        default=0,
    )
    return completed_frontier + PROFILE_WINDOW_SIZE


def runtime_progress_snapshot(log_path: Path, mode: str) -> dict[str, Any]:
    """Capture guest and renderer counters used to prove a live soak."""
    windows, _, _ = profile.parse_log(log_path, defer_open_window=True)
    complete = [window for window in windows if window["complete"]]
    snapshot: dict[str, Any] = {
        "complete_profile_windows": len(complete),
        "profile_swap_end": max(
            (int(window["swap_end"]) for window in complete), default=0
        ),
    }
    if mode not in ("dam-gameplay", "local-multiplayer"):
        return snapshot
    observations = gameplay.parse_observations(combined_cycle_text(log_path))
    if observations.mission is not None:
        snapshot["mission"] = {
            "level": observations.mission.level,
            "players": observations.mission.players,
            "network": observations.mission.network,
        }
    if mode == "dam-gameplay" and observations.samples:
        latest = observations.samples[-1]
        snapshot["guest"] = {
            "sample": latest.sample,
            "poll": latest.poll,
            "frame": latest.frame,
            "present": latest.present,
        }
    elif mode == "local-multiplayer":
        batches = gameplay.complete_local_pad_batches(observations, 4)
        if not batches:
            # The parser requires an exact player set, so use the newest pad-1
            # sample when fewer than four local players are active.
            pad_one = [pad for pad in observations.local_pads if pad.slot == 1]
            if pad_one:
                latest = pad_one[-1]
                snapshot["guest"] = {
                    "sample": latest.sample,
                    "poll": latest.poll,
                    "frame": latest.frame,
                    "present": latest.present,
                }
        else:
            latest = batches[-1][1]
            snapshot["guest"] = {
                "sample": latest.sample,
                "poll": latest.poll,
                "frame": latest.frame,
                "present": latest.present,
            }
    return snapshot


def soak_progress_violations(
    before: dict[str, Any], after: dict[str, Any], mode: str
) -> list[str]:
    problems: list[str] = []
    if int(after.get("profile_swap_end", 0)) <= int(before.get("profile_swap_end", 0)):
        problems.append(
            "post-ready soak emitted no new complete Metal profile window"
        )
    if mode not in ("dam-gameplay", "local-multiplayer"):
        return problems
    if before.get("mission") is None or after.get("mission") is None:
        problems.append("post-ready soak has no mission telemetry")
    elif before["mission"] != after["mission"]:
        problems.append(
            "post-ready soak mission identity changed unexpectedly: "
            f"{before['mission']} -> {after['mission']}"
        )
    before_guest, after_guest = before.get("guest"), after.get("guest")
    if not before_guest or not after_guest:
        problems.append("post-ready soak has no guest progress telemetry")
        return problems
    for counter in ("poll", "frame", "present"):
        if not gameplay._counter_advanced(
            int(before_guest[counter]), int(after_guest[counter])
        ):
            problems.append(
                f"post-ready soak guest {counter} did not advance "
                f"({before_guest[counter]} -> {after_guest[counter]})"
            )
    return problems


def performance_thresholds(
    args: argparse.Namespace, mode: str
) -> tuple[float | None, float | None]:
    """Return release thresholds, with menu and diagnostic runs exempt."""
    if mode == "menu" or getattr(args, "disable_performance_gates", False):
        return None, None
    return (
        getattr(args, "min_mean_fps", DEFAULT_MIN_MEAN_FPS),
        getattr(args, "max_window_p99_ms", DEFAULT_MAX_WINDOW_P99_MS),
    )


def fixed_multiplayer_benchmark_arena_evidence(
    readiness_details: Mapping[str, Any], level: int | None
) -> tuple[dict[str, Any] | None, list[str]]:
    """Validate the input-only fixed-arena selection proof when enabled."""
    scenario = readiness_details.get("multiplayer_scenario")
    selection = (
        scenario.get("benchmark_arena_selection")
        if isinstance(scenario, Mapping)
        else None
    )
    if not isinstance(selection, Mapping) or selection.get("enabled") is not True:
        return None, []

    problems: list[str] = []
    sequences: dict[str, int] = {}
    for key, command_kind, control, value, hold_ms in (
        ("move_to_scenario", "PULSE_AXIS", "LY", 32767, 80),
        ("move_to_level", "PULSE_AXIS", "LY", 32767, 80),
        ("open_level_selector", "PULSE_BUTTON", "SOUTH", None, 250),
        ("move_to_fixed_level", "PULSE_AXIS", "LY", 32767, 80),
        ("confirm_fixed_level", "PULSE_BUTTON", "SOUTH", None, 250),
    ):
        pulse = selection.get(key)
        sequence = pulse.get("sequence") if isinstance(pulse, Mapping) else None
        command = pulse.get("command") if isinstance(pulse, Mapping) else None
        acknowledged = (
            pulse.get("acknowledged") if isinstance(pulse, Mapping) else None
        )
        if not isinstance(sequence, int) or sequence <= 0:
            problems.append(f"fixed benchmark arena {key} sequence is invalid")
            continue
        sequences[key] = sequence
        expected_command = (
            f"{command_kind} {sequence} 1 {control} {hold_ms}"
            if value is None
            else f"{command_kind} {sequence} 1 {control} {value} {hold_ms}"
        )
        if command != expected_command:
            problems.append(
                f"fixed benchmark arena {key} command is invalid: {command}"
            )
        if acknowledged is not True:
            problems.append(
                f"fixed benchmark arena {key} was not acknowledged"
            )

    guest_join_sequence = selection.get("guest_join_first_sequence")
    start_match_sequence = selection.get("start_match_sequence")
    if selection.get("selection_completed_before_guest_join") is not True:
        problems.append(
            "fixed benchmark arena selection did not complete before guest join"
        )
    if (
        not isinstance(guest_join_sequence, int)
        or guest_join_sequence <= sequences.get("confirm_fixed_level", 0)
    ):
        problems.append("fixed benchmark arena guest-join ordering is invalid")
    if not (
        sequences.get("move_to_scenario", 0)
        < sequences.get("move_to_level", 0)
        < sequences.get("open_level_selector", 0)
        < sequences.get("move_to_fixed_level", 0)
        < sequences.get("confirm_fixed_level", 0)
    ):
        problems.append("fixed benchmark arena selection ordering is invalid")
    if selection.get("create_local_game_returned_after_confirmation") is not True:
        problems.append(
            "fixed benchmark arena did not return to create-local-game after confirmation"
        )
    if (
        not isinstance(start_match_sequence, int)
        or not isinstance(guest_join_sequence, int)
        or start_match_sequence <= guest_join_sequence
    ):
        problems.append("fixed benchmark arena start ordering is invalid")

    mission_levels = selection.get("mission_level_observations")
    ready_levels = selection.get("ready_level_observations")
    if (
        not isinstance(level, int)
        or not isinstance(mission_levels, list)
        or not mission_levels
        or any(observed != level for observed in mission_levels)
        or not isinstance(ready_levels, list)
        or not ready_levels
        or any(observed != level for observed in ready_levels)
        or selection.get("selected_level_id") != level
        or selection.get("observed_level_stable") is not True
    ):
        problems.append(
            "fixed benchmark arena observed-level stability proof is invalid"
        )
    return dict(selection), problems


def multiplayer_performance_cohort(
    readiness_details: Mapping[str, Any],
) -> tuple[dict[str, Any] | None, list[str]]:
    """Extract only the identity and workload data needed for a fair A/B."""
    problems: list[str] = []
    mission = readiness_details.get("local_multiplayer")
    aggregate = readiness_details.get("aggregate")
    if not isinstance(mission, Mapping):
        problems.append("local multiplayer mission identity is missing")
    if not isinstance(aggregate, Mapping):
        problems.append("selected local multiplayer aggregate is missing")
    if problems:
        return None, problems

    level = mission.get("level")
    players = mission.get("players")
    if not isinstance(level, int) or not 0 < level < 90:
        problems.append("local multiplayer level identity is invalid")
    if players != 4:
        problems.append(f"performance comparison requires 4 players, got {players}")

    windows = aggregate.get("windows")
    pacing = aggregate.get("frame_pacing")
    stages = aggregate.get("stages")
    draw = stages.get("draw") if isinstance(stages, Mapping) else None
    sampled_frames = pacing.get("sampled_frames") if isinstance(pacing, Mapping) else None
    draw_calls = draw.get("total_calls") if isinstance(draw, Mapping) else None
    fps = aggregate.get("real_window_fps")
    mean_fps = fps.get("mean") if isinstance(fps, Mapping) else None
    window_one_percent_low_fps = (
        pacing.get("window_one_percent_low_fps")
        if isinstance(pacing, Mapping)
        else None
    )
    window_p99_frame_ms = (
        pacing.get("window_p99_frame_ms")
        if isinstance(pacing, Mapping)
        else None
    )
    if not isinstance(windows, int) or windows <= 0:
        problems.append("selected performance window count is missing or invalid")
    if (
        not profile.is_finite_number(sampled_frames)
        or float(sampled_frames) <= 0
    ):
        problems.append("selected sampled-frame count is missing or invalid")
    elif isinstance(windows, int) and int(sampled_frames) != windows * profile.PROFILE_WINDOW_SIZE:
        problems.append(
            "selected sampled-frame count does not match the 64-swap windows"
        )
    if not profile.is_finite_number(draw_calls) or float(draw_calls) <= 0:
        problems.append("selected draw-call count is missing or invalid")
    for label, value in (
        ("mean FPS", mean_fps),
        ("window 1% low FPS", window_one_percent_low_fps),
        ("window p99 frame time", window_p99_frame_ms),
    ):
        if not profile.is_finite_number(value) or float(value) <= 0:
            problems.append(f"selected {label} is missing or invalid")
    benchmark_arena, benchmark_arena_problems = (
        fixed_multiplayer_benchmark_arena_evidence(readiness_details, level)
    )
    problems.extend(benchmark_arena_problems)
    problems.extend(profile.probe_queue_comparison_violations(aggregate))
    queue = aggregate.get("probe_queue")
    if isinstance(queue, Mapping):
        for metric in (
            "backpressure_checks",
            "nonblocking_reclaims",
            "blocking_waits",
            "blocking_wait_ns",
            "wait_reg_mem_signals",
            "wait_reg_mem_timeouts",
            "wait_reg_mem_unavailable",
        ):
            value = queue.get(metric)
            if (
                not profile.is_finite_number(value)
                or float(value) < 0
                or not float(value).is_integer()
            ):
                problems.append(
                    f"selected probe queue {metric} is missing or invalid"
                )
    if problems:
        return None, problems

    queue = aggregate["probe_queue"]
    sampled_frame_count = int(sampled_frames)
    performance_metrics = {
        "mean_fps": float(mean_fps),
        "window_one_percent_low_fps": float(window_one_percent_low_fps),
        "window_p99_frame_ms": float(window_p99_frame_ms),
        "probe_blocking_waits": int(queue["blocking_waits"]),
        "probe_blocking_waits_per_swap": (
            float(queue["blocking_waits"]) / sampled_frame_count
        ),
        "probe_blocking_wait_ns": int(queue["blocking_wait_ns"]),
        "probe_blocking_wait_ns_per_swap": (
            float(queue["blocking_wait_ns"]) / sampled_frame_count
        ),
        "probe_backpressure_checks": int(queue["backpressure_checks"]),
        "probe_nonblocking_reclaims": int(queue["nonblocking_reclaims"]),
        "wait_reg_mem_signals": int(queue["wait_reg_mem_signals"]),
        "wait_reg_mem_signals_per_swap": (
            float(queue["wait_reg_mem_signals"]) / sampled_frame_count
        ),
        "wait_reg_mem_timeouts": int(queue["wait_reg_mem_timeouts"]),
        "wait_reg_mem_timeouts_per_swap": (
            float(queue["wait_reg_mem_timeouts"]) / sampled_frame_count
        ),
        "wait_reg_mem_unavailable": int(queue["wait_reg_mem_unavailable"]),
    }
    cohort = {
        "level": level,
        "players": players,
        "windows": windows,
        "sampled_frames": int(sampled_frames),
        "draw_calls": int(draw_calls),
        "draw_calls_per_swap": float(draw_calls) / float(sampled_frames),
        "draws_per_command_buffer": queue["draws_per_command_buffer"],
        "committed_command_buffer_cap": queue[
            "committed_command_buffer_cap"
        ],
        "performance_metrics": performance_metrics,
        "probe_queue": dict(queue),
    }
    if benchmark_arena is not None:
        cohort["benchmark_arena_selection"] = benchmark_arena
    return cohort, []


def requested_probe_configuration_violations(
    cohort: Mapping[str, Any] | None,
    requested_draws_per_command_buffer: int,
) -> list[str]:
    """Prove the harness actually applied the sole A/B configuration knob."""
    if requested_draws_per_command_buffer not in (64, 128, 256):
        return [
            "requested probe draw batch is invalid: "
            f"{requested_draws_per_command_buffer}"
        ]
    if not isinstance(cohort, Mapping):
        return []
    observed = cohort.get("draws_per_command_buffer")
    if observed != requested_draws_per_command_buffer:
        return [
            "requested probe draw batch was not observed: "
            f"requested={requested_draws_per_command_buffer} observed={observed}"
        ]
    return []


def performance_metric_deltas(
    reference_metrics: Mapping[str, Any],
    candidate_metrics: Mapping[str, Any],
) -> tuple[dict[str, float], dict[str, float | None]]:
    """Return descriptive candidate-reference deltas without grading them."""
    absolute: dict[str, float] = {}
    percent: dict[str, float | None] = {}
    for name in sorted(set(reference_metrics) & set(candidate_metrics)):
        reference_value = reference_metrics[name]
        candidate_value = candidate_metrics[name]
        if not (
            profile.is_finite_number(reference_value)
            and profile.is_finite_number(candidate_value)
        ):
            continue
        reference_number = float(reference_value)
        candidate_number = float(candidate_value)
        absolute[name] = candidate_number - reference_number
        percent[name] = (
            (candidate_number - reference_number) / reference_number * 100.0
            if reference_number != 0
            else None
        )
    return absolute, percent


def load_multiplayer_performance_reference(path: Path) -> dict[str, Any]:
    try:
        summary = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read performance reference: {error}") from error
    if not isinstance(summary, Mapping):
        raise ValueError("performance reference summary is not a JSON object")
    if summary.get("mode") != "local-multiplayer":
        raise ValueError("performance reference is not a local-multiplayer suite")
    if summary.get("status") != "pass" or summary.get("release_eligible") is not True:
        raise ValueError(
            "performance reference suite is not a release-eligible pass"
        )
    cycles = summary.get("cycles")
    if not isinstance(cycles, list) or len(cycles) != 1:
        raise ValueError("performance reference must contain exactly one cycle")
    cycle = cycles[0]
    if (
        not isinstance(cycle, Mapping)
        or cycle.get("status") != "pass"
        or cycle.get("release_eligible") is not True
    ):
        raise ValueError(
            "performance reference cycle is not a release-eligible pass"
        )
    if cycle.get("failures"):
        raise ValueError("performance reference cycle contains failures")
    cohort, problems = multiplayer_performance_cohort(cycle.get("readiness", {}))
    if problems or cohort is None:
        raise ValueError("; ".join(problems or ["performance cohort is missing"]))
    return cohort


def compare_multiplayer_performance_cohort(
    reference: Mapping[str, Any],
    candidate_details: Mapping[str, Any],
    draw_tolerance_percent: float,
    requested_draws_per_command_buffer: int | None = None,
) -> dict[str, Any]:
    candidate, problems = multiplayer_performance_cohort(candidate_details)
    failures = list(problems)
    draw_delta_percent: float | None = None
    reference_metrics = reference.get("performance_metrics")
    candidate_metrics = (
        candidate.get("performance_metrics") if candidate is not None else None
    )
    absolute_metric_deltas: dict[str, float] | None = None
    percent_metric_deltas: dict[str, float | None] | None = None
    if not isinstance(reference_metrics, Mapping):
        failures.append("reference performance metrics are missing or invalid")
    if candidate is not None and not isinstance(candidate_metrics, Mapping):
        failures.append("candidate performance metrics are missing or invalid")
    if isinstance(reference_metrics, Mapping) and isinstance(candidate_metrics, Mapping):
        absolute_metric_deltas, percent_metric_deltas = performance_metric_deltas(
            reference_metrics, candidate_metrics
        )
    if reference.get("committed_command_buffer_cap") != 4:
        failures.append(
            "reference command-buffer cap must be exactly 4, got "
            f"{reference.get('committed_command_buffer_cap')}"
        )
    if candidate is not None:
        reference_arena = reference.get("benchmark_arena_selection")
        candidate_arena = candidate.get("benchmark_arena_selection")
        if isinstance(reference_arena, Mapping) != isinstance(
            candidate_arena, Mapping
        ):
            failures.append(
                "fixed benchmark arena selection mismatch between reference "
                "and candidate"
            )
        if requested_draws_per_command_buffer is not None:
            failures.extend(
                requested_probe_configuration_violations(
                    candidate, requested_draws_per_command_buffer
                )
            )
        if candidate["players"] != reference.get("players"):
            failures.append(
                "player-count mismatch: "
                f"reference={reference.get('players')} candidate={candidate['players']}"
            )
        if candidate["level"] != reference.get("level"):
            failures.append(
                "level mismatch: "
                f"reference={reference.get('level')} candidate={candidate['level']}"
            )
        if candidate["windows"] != reference.get("windows"):
            failures.append(
                "selected-window mismatch: "
                f"reference={reference.get('windows')} candidate={candidate['windows']}"
            )
        if candidate["committed_command_buffer_cap"] != 4:
            failures.append(
                "candidate command-buffer cap must be exactly 4, got "
                f"{candidate['committed_command_buffer_cap']}"
            )
        reference_draws = reference.get("draw_calls_per_swap")
        if profile.is_finite_number(reference_draws) and float(reference_draws) > 0:
            draw_delta_percent = (
                abs(candidate["draw_calls_per_swap"] - float(reference_draws))
                / float(reference_draws)
                * 100.0
            )
            if draw_delta_percent > draw_tolerance_percent:
                failures.append(
                    "draw-cohort mismatch: "
                    f"reference={float(reference_draws):.3f} calls/swap "
                    f"candidate={candidate['draw_calls_per_swap']:.3f} calls/swap "
                    f"delta={draw_delta_percent:.3f}% exceeds "
                    f"{draw_tolerance_percent:.3f}%"
                )
        else:
            failures.append("reference draw cohort is missing or invalid")
    return {
        "comparability_status": "pass" if not failures else "fail",
        "draw_cohort_tolerance_percent": draw_tolerance_percent,
        "draw_calls_per_swap_delta_percent": draw_delta_percent,
        "reference": dict(reference),
        "candidate": candidate,
        "observed_metrics": {
            "reference": dict(reference_metrics)
            if isinstance(reference_metrics, Mapping)
            else None,
            "candidate": dict(candidate_metrics)
            if isinstance(candidate_metrics, Mapping)
            else None,
        },
        "metric_deltas": {
            "candidate_minus_reference": absolute_metric_deltas,
            "candidate_vs_reference_percent": percent_metric_deltas,
        },
        "comparability_failures": failures,
    }


def release_eligibility(args: argparse.Namespace) -> dict[str, Any]:
    """Classify diagnostic bypasses separately from release-grade evidence."""
    reasons: list[str] = []
    if getattr(args, "quit_method", "native") != "native":
        reasons.append("native visible-window and clean-quit validation was disabled")
    if getattr(args, "disable_performance_gates", False):
        reasons.append("performance gates were disabled")
    if not getattr(args, "require_soak_progress", True):
        reasons.append("post-ready soak progress gate was disabled")
    if float(getattr(args, "post_ready_soak_seconds", 0.0)) <= 0:
        reasons.append("post-ready soak duration was zero")
    if getattr(args, "allow_stale_build", False):
        reasons.append("build freshness validation was disabled")
    if getattr(args, "skip_metadata", False):
        reasons.append("reproducibility metadata was disabled")
    if getattr(args, "exact_output_merger", False):
        reasons.append("experimental exact output-merger route was enabled")
    return {"eligible": not reasons, "reasons": reasons}


def cycle_release_eligibility(
    args: argparse.Namespace,
    failures: Sequence[str],
    *,
    expected_multiplayer_level: int | None = None,
    observed_multiplayer_level: int | None = None,
    performance_comparison: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    """Combine configuration eligibility with this cycle's actual evidence."""
    configuration = release_eligibility(args)
    reasons = list(configuration["reasons"])
    if (
        expected_multiplayer_level is not None
        and observed_multiplayer_level is not None
        and expected_multiplayer_level != observed_multiplayer_level
    ):
        reasons.append(
            "local multiplayer level mismatch: "
            f"expected {expected_multiplayer_level}, "
            f"observed {observed_multiplayer_level}"
        )
    if (
        isinstance(performance_comparison, Mapping)
        and performance_comparison.get("comparability_status") != "pass"
    ):
        reasons.append(
            "4-player performance comparison was not workload-comparable"
        )
    if failures:
        reasons.append("cycle validation failed")
    reasons = list(dict.fromkeys(reasons))
    return {"eligible": not reasons, "reasons": reasons}


def suite_release_eligibility(
    args: argparse.Namespace, cycles: Sequence[Mapping[str, Any]]
) -> dict[str, Any]:
    """A suite is release-eligible only when every cycle is a release pass."""
    configuration = release_eligibility(args)
    reasons = list(configuration["reasons"])
    for index, cycle in enumerate(cycles, start=1):
        if cycle.get("status") == "pass" and cycle.get("release_eligible") is True:
            continue
        cycle_number = cycle.get("cycle", index)
        cycle_reasons = cycle.get("release_ineligibility_reasons")
        material_reasons = (
            [
                str(reason)
                for reason in cycle_reasons
                if reason not in configuration["reasons"]
            ]
            if isinstance(cycle_reasons, list)
            else []
        )
        if not material_reasons and cycle.get("status") == "fail":
            material_reasons = ["cycle validation failed"]
        reasons.extend(
            f"cycle {cycle_number}: {reason}" for reason in material_reasons
        )
    reasons = list(dict.fromkeys(reasons))
    all_release_passed = bool(cycles) and all(
        cycle.get("status") == "pass"
        and cycle.get("release_eligible") is True
        for cycle in cycles
    )
    return {
        "eligible": configuration["eligible"] and all_release_passed,
        "reasons": reasons,
    }


def successful_status(eligible: bool) -> str:
    return "pass" if eligible else "diagnostic-pass"


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
    # A stability run must not inherit a developer's renderer experiments.
    # Several GOLDENEYE_* switches deliberately replace textures, force guest
    # presentation state, disable the native tiled resolve, or enable expensive
    # diagnostics. Remove the complete private namespaces, then add back only
    # the deterministic settings required by this harness and its selected mode.
    for variable in tuple(environment):
        if variable.startswith(("GOLDENEYE_", "REX_")):
            environment.pop(variable, None)
    # spdlog also honors this generic variable; inheriting a restrictive value
    # could hide evidence that the log gate is expected to inspect.
    environment.pop("SPDLOG_LEVEL", None)
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
    if getattr(args, "exact_output_merger", False):
        environment["REX_METAL_EXACT_OUTPUT_MERGER"] = "true"
    if args.mode == "local-multiplayer":
        environment["REX_INPUT_BACKEND"] = "sdl"
        environment.pop("GOLDENEYE_AUTO_START", None)
        environment.pop("GOLDENEYE_AUTO_MISSION", None)
        environment["REX_CONTROLLER_LAYOUT"] = "modern"
        environment["REX_CONTROLLER_BUTTON_MAP"] = ""
        environment["GOLDENEYE_TEST_MENU_TRACE"] = "1"
        environment["GOLDENEYE_TEST_MULTIPLAYER_TRACE"] = "1"
        environment["GOLDENEYE_METAL_PROBE_DRAWS_PER_COMMAND_BUFFER"] = str(
            getattr(args, "probe_draws_per_command_buffer", 128)
        )
        if virtual_gamepad_fd is not None:
            environment["REX_INPUT_TEST_HARNESS"] = "1"
            environment["REX_TEST_VIRTUAL_GAMEPADS"] = str(args.players)
            environment["REX_TEST_VIRTUAL_GAMEPAD_FD"] = str(virtual_gamepad_fd)
    elif args.mode == "dam-gameplay":
        environment["REX_INPUT_BACKEND"] = "sdl"
        environment["REX_CONTROLLER_LAYOUT"] = "modern"
        environment["REX_CONTROLLER_BUTTON_MAP"] = ""
        environment["GOLDENEYE_AUTO_MISSION"] = "dam"
        environment["GOLDENEYE_TEST_GAMEPLAY_TRACE"] = "1"
        if getattr(args, "capture_gpu_frame", False):
            environment["GOLDENEYE_TEST_CAPTURE_DAM_FRAME"] = "1"
        if getattr(args, "validate_host_pause", False):
            environment["GOLDENEYE_TEST_HOST_PAUSE"] = "1"
        if virtual_gamepad_fd is not None:
            environment["REX_INPUT_TEST_HARNESS"] = "1"
            environment["REX_TEST_VIRTUAL_GAMEPADS"] = "1"
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
    ACTION_PHASE_TIMEOUT_SECONDS = 15.0
    MATCH_READY_TIMEOUT_SECONDS = 60.0
    ACTIVE_AXIS_THRESHOLD = 12_000
    NEUTRAL_AXIS_THRESHOLD = 2_000
    MOVEMENT_DISTANCE = 1.0
    CAMERA_DELTA = 0.01
    NON_TARGET_POSITION_DRIFT = 0.35
    NON_TARGET_CAMERA_DRIFT = 0.005

    def __init__(
        self,
        players: int,
        command_fd: int,
        expected_level: int | None = None,
        fixed_benchmark_arena: bool = False,
    ):
        if fixed_benchmark_arena and players != 4:
            raise ValueError("fixed multiplayer benchmark arena requires 4 players")
        self.players = players
        self.command_fd = command_fd
        self.expected_level = expected_level
        self.fixed_benchmark_arena = fixed_benchmark_arena
        self.observed_level: int | None = None
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
        self.action_phase_started_at = 0.0
        self.action_player = 1
        self.minimum_pad_sample = 0
        self.action_world_baseline: dict[int, gameplay.LocalPlayerSample] = {}
        self.action_is_post_reconnect = False
        self.hotplug_player = 2
        self.hotplug_devices_before: dict[int, int] = {}
        self.hotplug_devices_after: dict[int, int] = {}
        self.match_readiness_observed = False
        self.profile_anchor_swap_end: int | None = None
        self.benchmark_mission_observation_start_index = 0
        self.benchmark_arena_evidence: dict[str, Any] = {
            "enabled": fixed_benchmark_arena,
            "menu_precondition": {
                "cursor": "CREATE LOCAL GAME / Press your start buttons header",
                "scenario": "Normal",
                "level": "Random",
            },
            "fixed_selection": "first fixed item after Random in Level selector",
            "move_to_scenario": {
                "sequence": None,
                "command": None,
                "acknowledged": False,
            },
            "move_to_level": {
                "sequence": None,
                "command": None,
                "acknowledged": False,
            },
            "open_level_selector": {
                "sequence": None,
                "command": None,
                "acknowledged": False,
            },
            "move_to_fixed_level": {
                "sequence": None,
                "command": None,
                "acknowledged": False,
            },
            "confirm_fixed_level": {
                "sequence": None,
                "command": None,
                "acknowledged": False,
            },
            "create_local_game_returned_after_confirmation": False,
            "selection_completed_before_guest_join": False,
            "guest_join_first_sequence": None,
            "start_match_sequence": None,
            "mission_level_observations": [],
            "ready_level_observations": [],
            "selected_level_id": None,
            "observed_level_stable": False,
        }
        self.action_evidence: dict[str, Any] = {
            "players": {
                str(player): {
                    "movement_sequence": None,
                    "camera_sequence": None,
                    "movement_sample": None,
                    "world_effect_sample": None,
                    "movement_distance": 0.0,
                    "camera_delta": 0.0,
                    "non_target_max_position_drift": 0.0,
                    "non_target_max_camera_drift": 0.0,
                    "fire_sequence": None,
                    "fire_sample": None,
                    "fire_effect_required": None,
                    "fire_effect_observed": False,
                    "ammo_before": None,
                    "ammo_after": None,
                    "ammo_decrement": 0,
                    "neutral_sample": None,
                }
                for player in range(1, players + 1)
            },
            "hotplug": {
                "player": self.hotplug_player,
                "disconnect_sequence": None,
                "disconnect_sample": None,
                "connect_sequence": None,
                "connect_sample": None,
                "old_device": None,
                "new_device": None,
                "unaffected_devices_preserved": False,
                "post_reconnect_movement_sequence": None,
                "post_reconnect_camera_sequence": None,
                "post_reconnect_movement_sample": None,
                "post_reconnect_world_effect_sample": None,
                "post_reconnect_movement_distance": 0.0,
                "post_reconnect_camera_delta": 0.0,
                "post_reconnect_fire_sequence": None,
                "post_reconnect_fire_sample": None,
                "post_reconnect_fire_effect_required": None,
                "post_reconnect_fire_effect_observed": False,
                "post_reconnect_ammo_before": None,
                "post_reconnect_ammo_after": None,
                "post_reconnect_ammo_decrement": 0,
            },
            "final_reset_sequence": None,
            "final_neutral_sample": None,
        }

    def _pulse_button(self, player: int, button: str, hold_ms: int = 250) -> str:
        self.sequence += 1
        return f"PULSE_BUTTON {self.sequence} {player} {button} {hold_ms}\n"

    def _pulse_axis(
        self, player: int, axis: str, value: int, hold_ms: int = 350
    ) -> str:
        self.sequence += 1
        return f"PULSE_AXIS {self.sequence} {player} {axis} {value} {hold_ms}\n"

    def _set_axis(self, player: int, axis: str, value: int) -> str:
        self.sequence += 1
        return f"SET_AXIS {self.sequence} {player} {axis} {value}\n"

    def _lifecycle(self, operation: str, player: int) -> str:
        self.sequence += 1
        return f"{operation} {self.sequence} {player}\n"

    def _reset(self, player: int) -> str:
        self.sequence += 1
        return f"RESET {self.sequence} {player}\n"

    def _reset_all(self) -> str:
        self.sequence += 1
        return f"RESET {self.sequence} ALL\n"

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

    def _set_action_phase(self, phase: str, elapsed: float) -> None:
        self.phase = phase
        self.action_phase_started_at = elapsed

    def _refresh_benchmark_arena_ack_evidence(self) -> None:
        if not self.fixed_benchmark_arena:
            return
        for key in (
            "move_to_scenario",
            "move_to_level",
            "open_level_selector",
            "move_to_fixed_level",
            "confirm_fixed_level",
        ):
            sequence = self.benchmark_arena_evidence[key]["sequence"]
            self.benchmark_arena_evidence[key]["acknowledged"] = (
                isinstance(sequence, int)
                and sequence in self.acknowledged_sequences
            )

    def _observe_benchmark_arena_level(
        self, log_text: str
    ) -> bool:
        if not self.fixed_benchmark_arena:
            return True
        mission_matches = list(gameplay.MISSION_STATE.finditer(log_text))[
            self.benchmark_mission_observation_start_index :
        ]
        mission_levels = [
            int(match.group(1))
            for match in mission_matches
            if 0 < int(match.group(1)) < 90
            and int(match.group(2)) == self.players
            and int(match.group(3)) == 0
        ]
        ready_levels = [
            int(match.group(1))
            for match in LOCAL_MULTIPLAYER_READY.finditer(log_text)
            if int(match.group(2)) == self.players and int(match.group(3)) >= 120
        ]
        evidence = self.benchmark_arena_evidence
        evidence["mission_level_observations"] = mission_levels
        evidence["ready_level_observations"] = ready_levels
        observed_levels = set(mission_levels + ready_levels)
        evidence["selected_level_id"] = (
            next(iter(observed_levels)) if len(observed_levels) == 1 else None
        )
        evidence["observed_level_stable"] = bool(
            mission_levels and ready_levels and len(observed_levels) == 1
        )
        if len(observed_levels) > 1:
            self.error = (
                "fixed multiplayer benchmark arena was not stable: "
                + ", ".join(str(level) for level in sorted(observed_levels))
            )
            return False
        return True

    def _fresh_multiplayer_batches(
        self, observations: gameplay.Observations
    ) -> tuple[
        tuple[
            dict[int, gameplay.LocalPadSample],
            dict[int, gameplay.LocalPlayerSample],
        ],
        ...,
    ]:
        return tuple(
            batch
            for batch in gameplay.complete_local_multiplayer_batches(
                observations, self.players
            )
            if batch[0][1].sample > self.minimum_pad_sample
        )

    def _all_connected_neutral(self, batch: dict[int, gameplay.LocalPadSample]) -> bool:
        return all(
            pad.connected
            and pad.device != 0
            and pad.neutral(self.NEUTRAL_AXIS_THRESHOLD)
            for pad in batch.values()
        )

    def _other_pads_neutral(
        self,
        batch: dict[int, gameplay.LocalPadSample],
        target: int,
    ) -> bool:
        return all(
            slot == target
            or (
                pad.connected
                and pad.device != 0
                and pad.neutral(self.NEUTRAL_AXIS_THRESHOLD)
            )
            for slot, pad in batch.items()
        )

    def _world_cohort_eligible(
        self, world: dict[int, gameplay.LocalPlayerSample]
    ) -> bool:
        if set(world) != set(range(1, self.players + 1)):
            return False
        if not all(player.eligible() for player in world.values()):
            return False
        # A duplicated pointer would make apparent slot isolation meaningless.
        return len({player.player for player in world.values()}) == self.players

    @staticmethod
    def _same_world_identity(
        current: dict[int, gameplay.LocalPlayerSample],
        baseline: dict[int, gameplay.LocalPlayerSample],
    ) -> bool:
        return set(current) == set(baseline) and all(
            current[slot].eligible()
            and current[slot].player == baseline[slot].player
            and current[slot].coordinates == baseline[slot].coordinates
            for slot in baseline
        )

    @staticmethod
    def _position_distance(
        first: gameplay.LocalPlayerSample,
        second: gameplay.LocalPlayerSample,
    ) -> float:
        return math.sqrt(
            (second.x - first.x) ** 2
            + (second.y - first.y) ** 2
            + (second.z - first.z) ** 2
        )

    @staticmethod
    def _camera_distance(
        first: gameplay.LocalPlayerSample,
        second: gameplay.LocalPlayerSample,
    ) -> float:
        return math.hypot(second.yaw - first.yaw, second.pitch - first.pitch)

    def _world_effect(
        self,
        current: dict[int, gameplay.LocalPlayerSample],
        target: int,
    ) -> tuple[float, float, float, float] | None:
        baseline = self.action_world_baseline
        if not baseline or not self._same_world_identity(current, baseline):
            return None
        movement = self._position_distance(baseline[target], current[target])
        camera = self._camera_distance(baseline[target], current[target])
        other_position = max(
            (
                self._position_distance(baseline[slot], current[slot])
                for slot in baseline
                if slot != target
            ),
            default=0.0,
        )
        other_camera = max(
            (
                self._camera_distance(baseline[slot], current[slot])
                for slot in baseline
                if slot != target
            ),
            default=0.0,
        )
        if (
            other_position > self.NON_TARGET_POSITION_DRIFT
            or other_camera > self.NON_TARGET_CAMERA_DRIFT
        ):
            return None
        return movement, camera, other_position, other_camera

    def _send_player_movement(
        self,
        player: int,
        baseline: tuple[
            dict[int, gameplay.LocalPadSample],
            dict[int, gameplay.LocalPlayerSample],
        ],
        elapsed: float,
        *,
        post_reconnect: bool = False,
    ) -> None:
        pads, world = baseline
        self.action_player = player
        self.minimum_pad_sample = pads[1].sample
        self.action_world_baseline = dict(world)
        self.action_is_post_reconnect = post_reconnect
        movement_command = self._set_axis(player, "LY", -24_000)
        movement_sequence = self.sequence
        camera_command = self._set_axis(player, "RX", 16_000)
        camera_sequence = self.sequence
        evidence = self.action_evidence["players"][str(player)]
        if post_reconnect:
            hotplug = self.action_evidence["hotplug"]
            hotplug["post_reconnect_movement_sequence"] = movement_sequence
            hotplug["post_reconnect_camera_sequence"] = camera_sequence
        else:
            evidence["movement_sequence"] = movement_sequence
            evidence["camera_sequence"] = camera_sequence
        if self._send(
            [movement_command, camera_command],
            elapsed,
            (
                f"verify reconnected player {player} movement and camera effects"
                if post_reconnect
                else f"verify player {player} movement and camera effects"
            ),
            0.0,
        ):
            self._set_action_phase(
                "hotplug-move-active" if post_reconnect else "player-move-active",
                elapsed,
            )

    def _advance_active_scenario(self, log_text: str, elapsed: float) -> None:
        observations = gameplay.parse_observations(log_text)
        if not self._observe_benchmark_arena_level(log_text):
            return
        timeout = (
            self.MATCH_READY_TIMEOUT_SECONDS
            if self.phase == "waiting-for-match-readiness"
            else self.ACTION_PHASE_TIMEOUT_SECONDS
        )
        if elapsed - self.action_phase_started_at > timeout:
            self.error = f"local multiplayer scenario timed out in phase {self.phase}"
            return

        batches = gameplay.complete_local_multiplayer_batches(
            observations, self.players
        )
        if self.phase == "waiting-for-match-readiness":
            mission = observations.mission
            if (
                mission is not None
                and 0 < mission.level < 90
                and mission.players == self.players
                and not mission.network
            ):
                self.observed_level = mission.level
                if (
                    self.expected_level is not None
                    and mission.level != self.expected_level
                ):
                    self.error = (
                        "local multiplayer level mismatch: "
                        f"expected {self.expected_level}, observed {mission.level}"
                    )
                    return
            ready_matches = [
                match
                for match in LOCAL_MULTIPLAYER_READY.finditer(log_text)
                if int(match.group(2)) == self.players and int(match.group(3)) >= 120
            ]
            if (
                mission is None
                or mission.level <= 0
                or mission.level >= 90
                or mission.players != self.players
                or mission.network
                or not ready_matches
                or len(batches) < 2
            ):
                return
            if (
                self.fixed_benchmark_arena
                and not self.benchmark_arena_evidence["observed_level_stable"]
            ):
                return
            previous_pads, previous_world = batches[-2]
            current_pads, current_world = batches[-1]
            if (
                not self._all_connected_neutral(previous_pads)
                or not self._all_connected_neutral(current_pads)
                or not self._world_cohort_eligible(previous_world)
                or not self._world_cohort_eligible(current_world)
                or not self._same_world_identity(current_world, previous_world)
                or not gameplay._counter_advanced(
                    previous_pads[1].frame, current_pads[1].frame
                )
                or not gameplay._counter_advanced(
                    previous_pads[1].present, current_pads[1].present
                )
                or any(
                    self._position_distance(previous_world[slot], current_world[slot])
                    > self.NON_TARGET_POSITION_DRIFT
                    or self._camera_distance(previous_world[slot], current_world[slot])
                    > self.NON_TARGET_CAMERA_DRIFT
                    for slot in current_world
                )
            ):
                return
            self.hotplug_devices_before = {
                slot: pad.device for slot, pad in current_pads.items()
            }
            self.action_evidence["hotplug"]["old_device"] = current_pads[
                self.hotplug_player
            ].device
            self.match_readiness_observed = True
            self._send_player_movement(1, batches[-1], elapsed)
            return

        fresh = self._fresh_multiplayer_batches(observations)
        if not fresh or not self._command_finished(elapsed):
            return

        player = self.action_player
        evidence = self.action_evidence["players"][str(player)]

        if self.phase in ("player-move-active", "hotplug-move-active"):
            matching: tuple[
                dict[int, gameplay.LocalPadSample],
                dict[int, gameplay.LocalPlayerSample],
                tuple[float, float, float, float],
            ] | None = None
            for pads, world in fresh:
                target_pad = pads[player]
                if not (
                    target_pad.connected
                    and abs(target_pad.left_y) >= self.ACTIVE_AXIS_THRESHOLD
                    and abs(target_pad.right_x) >= self.ACTIVE_AXIS_THRESHOLD
                    and target_pad.buttons == 0
                    and target_pad.left_trigger == 0
                    and target_pad.right_trigger == 0
                    and abs(target_pad.left_x) < self.NEUTRAL_AXIS_THRESHOLD
                    and abs(target_pad.right_y) < self.NEUTRAL_AXIS_THRESHOLD
                    and self._other_pads_neutral(pads, player)
                ):
                    continue
                if self.phase == "player-move-active":
                    if evidence["movement_sample"] is None:
                        evidence["movement_sample"] = pads[1].sample
                elif self.action_evidence["hotplug"][
                    "post_reconnect_movement_sample"
                ] is None:
                    self.action_evidence["hotplug"][
                        "post_reconnect_movement_sample"
                    ] = pads[1].sample
                effect = self._world_effect(world, player)
                if effect is None:
                    continue
                movement, camera, other_position, other_camera = effect
                if self.phase == "player-move-active":
                    evidence["movement_distance"] = max(
                        evidence["movement_distance"], movement
                    )
                    evidence["camera_delta"] = max(
                        evidence["camera_delta"], camera
                    )
                    evidence["non_target_max_position_drift"] = max(
                        evidence["non_target_max_position_drift"], other_position
                    )
                    evidence["non_target_max_camera_drift"] = max(
                        evidence["non_target_max_camera_drift"], other_camera
                    )
                else:
                    hotplug = self.action_evidence["hotplug"]
                    hotplug["post_reconnect_movement_distance"] = max(
                        hotplug["post_reconnect_movement_distance"], movement
                    )
                    hotplug["post_reconnect_camera_delta"] = max(
                        hotplug["post_reconnect_camera_delta"], camera
                    )
                if movement >= self.MOVEMENT_DISTANCE and camera >= self.CAMERA_DELTA:
                    matching = (pads, world, effect)
                    break
            if matching is None:
                return
            pads, _, _ = matching
            if self.phase == "player-move-active":
                evidence["world_effect_sample"] = pads[1].sample
            else:
                self.action_evidence["hotplug"][
                    "post_reconnect_world_effect_sample"
                ] = pads[1].sample
            self.minimum_pad_sample = pads[1].sample
            command = self._reset(player)
            if self._send(
                [command], elapsed, f"return player {player} controls to neutral", 0.0
            ):
                self._set_action_phase(
                    "hotplug-move-neutral"
                    if self.phase == "hotplug-move-active"
                    else "player-move-neutral",
                    elapsed,
                )
            return

        if self.phase in ("player-move-neutral", "hotplug-move-neutral"):
            neutral = next(
                (
                    (pads, world)
                    for pads, world in fresh
                    if self._all_connected_neutral(pads)
                    and self._world_cohort_eligible(world)
                    and self._same_world_identity(
                        world, self.action_world_baseline
                    )
                ),
                None,
            )
            if neutral is None:
                return
            neutral_pads, neutral_world = neutral
            self.minimum_pad_sample = neutral_pads[1].sample
            self.action_world_baseline = dict(neutral_world)
            fire_required = (
                neutral_world[player].weapon_valid
                and neutral_world[player].right_magazine_valid
                and neutral_world[player].right_magazine > 0
            )
            command = self._set_axis(player, "RT", 32_767)
            if self.phase == "player-move-neutral":
                evidence["fire_sequence"] = self.sequence
                evidence["fire_effect_required"] = fire_required
                evidence["ammo_before"] = (
                    neutral_world[player].right_magazine if fire_required else None
                )
            else:
                hotplug = self.action_evidence["hotplug"]
                hotplug["post_reconnect_fire_sequence"] = self.sequence
                hotplug["post_reconnect_fire_effect_required"] = fire_required
                hotplug["post_reconnect_ammo_before"] = (
                    neutral_world[player].right_magazine if fire_required else None
                )
            if self._send(
                [command], elapsed, f"verify player {player} fire gameplay effect", 0.0
            ):
                self._set_action_phase(
                    "hotplug-fire-active"
                    if self.phase == "hotplug-move-neutral"
                    else "player-fire-active",
                    elapsed,
                )
            return

        if self.phase in ("player-fire-active", "hotplug-fire-active"):
            hotplug_action = self.phase == "hotplug-fire-active"
            fire_required = (
                self.action_evidence["hotplug"][
                    "post_reconnect_fire_effect_required"
                ]
                if hotplug_action
                else evidence["fire_effect_required"]
            )
            matching: tuple[
                dict[int, gameplay.LocalPadSample],
                dict[int, gameplay.LocalPlayerSample],
                bool,
            ] | None = None
            for pads, world in fresh:
                target_pad = pads[player]
                if not (
                    target_pad.connected
                    and target_pad.right_trigger > 0
                    and target_pad.buttons == 0
                    and target_pad.left_trigger == 0
                    and abs(target_pad.left_x) < self.NEUTRAL_AXIS_THRESHOLD
                    and abs(target_pad.left_y) < self.NEUTRAL_AXIS_THRESHOLD
                    and abs(target_pad.right_x) < self.NEUTRAL_AXIS_THRESHOLD
                    and abs(target_pad.right_y) < self.NEUTRAL_AXIS_THRESHOLD
                    and self._other_pads_neutral(pads, player)
                ):
                    continue
                if hotplug_action:
                    if self.action_evidence["hotplug"][
                        "post_reconnect_fire_sample"
                    ] is None:
                        self.action_evidence["hotplug"][
                            "post_reconnect_fire_sample"
                        ] = pads[1].sample
                elif evidence["fire_sample"] is None:
                    evidence["fire_sample"] = pads[1].sample
                if self._world_effect(world, player) is None:
                    continue
                effect_observed = not fire_required
                if fire_required:
                    before = self.action_world_baseline[player]
                    after = world[player]
                    other_ammo_preserved = all(
                        slot == player
                        or not (
                            self.action_world_baseline[slot].weapon_valid
                            and self.action_world_baseline[
                                slot
                            ].right_magazine_valid
                            and world[slot].weapon_valid
                            and world[slot].right_magazine_valid
                            and world[slot].weapon
                            == self.action_world_baseline[slot].weapon
                            and world[slot].right_magazine
                            < self.action_world_baseline[slot].right_magazine
                        )
                        for slot in world
                    )
                    effect_observed = (
                        after.weapon_valid
                        and after.right_magazine_valid
                        and after.weapon == before.weapon
                        and after.right_magazine < before.right_magazine
                        and other_ammo_preserved
                    )
                if effect_observed:
                    matching = (pads, world, bool(fire_required))
                    break
            if matching is None:
                return
            pads, world, observed_ammo_effect = matching
            if hotplug_action:
                hotplug = self.action_evidence["hotplug"]
                hotplug["post_reconnect_fire_effect_observed"] = observed_ammo_effect
                if observed_ammo_effect:
                    hotplug["post_reconnect_ammo_after"] = world[
                        player
                    ].right_magazine
                    hotplug["post_reconnect_ammo_decrement"] = (
                        self.action_world_baseline[player].right_magazine
                        - world[player].right_magazine
                    )
            else:
                evidence["fire_effect_observed"] = observed_ammo_effect
                if observed_ammo_effect:
                    evidence["ammo_after"] = world[player].right_magazine
                    evidence["ammo_decrement"] = (
                        self.action_world_baseline[player].right_magazine
                        - world[player].right_magazine
                    )
            self.minimum_pad_sample = pads[1].sample
            command = self._reset(player)
            if self._send(
                [command], elapsed, f"return player {player} fire to neutral", 0.0
            ):
                self._set_action_phase(
                    "hotplug-final-neutral"
                    if self.phase == "hotplug-fire-active"
                    else "player-fire-neutral",
                    elapsed,
                )
            return

        if self.phase == "player-fire-neutral":
            neutral = next(
                (
                    (pads, world)
                    for pads, world in fresh
                    if self._all_connected_neutral(pads)
                    and self._world_cohort_eligible(world)
                    and self._same_world_identity(
                        world, self.action_world_baseline
                    )
                ),
                None,
            )
            if neutral is None:
                return
            neutral_pads, neutral_world = neutral
            evidence["neutral_sample"] = neutral_pads[1].sample
            if player < self.players:
                self._send_player_movement(player + 1, neutral, elapsed)
                return
            self.minimum_pad_sample = neutral_pads[1].sample
            self.action_world_baseline = dict(neutral_world)
            command = self._lifecycle("DISCONNECT", self.hotplug_player)
            self.action_evidence["hotplug"]["disconnect_sequence"] = self.sequence
            if self._send(
                [command],
                elapsed,
                f"disconnect player {self.hotplug_player} without compacting survivors",
                0.0,
            ):
                self._set_action_phase("hotplug-disconnecting", elapsed)
            return

        if self.phase == "hotplug-disconnecting":
            disconnected = next(
                (
                    (pads, world)
                    for pads, world in fresh
                    if not pads[self.hotplug_player].connected
                    and pads[self.hotplug_player].device == 0
                    and pads[self.hotplug_player].neutral(self.NEUTRAL_AXIS_THRESHOLD)
                    and all(
                        slot == self.hotplug_player
                        or (
                            pad.connected
                            and pad.device == self.hotplug_devices_before[slot]
                            and pad.neutral(self.NEUTRAL_AXIS_THRESHOLD)
                        )
                        for slot, pad in pads.items()
                    )
                    and self._world_cohort_eligible(world)
                    and self._world_effect(world, self.hotplug_player) is not None
                ),
                None,
            )
            if disconnected is None:
                return
            disconnected_pads, disconnected_world = disconnected
            self.action_evidence["hotplug"]["disconnect_sample"] = (
                disconnected_pads[1].sample
            )
            self.minimum_pad_sample = disconnected_pads[1].sample
            self.action_world_baseline = dict(disconnected_world)
            command = self._lifecycle("CONNECT", self.hotplug_player)
            self.action_evidence["hotplug"]["connect_sequence"] = self.sequence
            if self._send(
                [command], elapsed, f"reconnect player {self.hotplug_player}", 0.0
            ):
                self._set_action_phase("hotplug-connecting", elapsed)
            return

        if self.phase == "hotplug-connecting":
            reconnected = next(
                (
                    (pads, world)
                    for pads, world in fresh
                    if self._all_connected_neutral(pads)
                    and pads[self.hotplug_player].device != 0
                    and pads[self.hotplug_player].device
                    != self.hotplug_devices_before[self.hotplug_player]
                    and all(
                        slot == self.hotplug_player
                        or pad.device == self.hotplug_devices_before[slot]
                        for slot, pad in pads.items()
                    )
                    and self._world_cohort_eligible(world)
                    and self._same_world_identity(
                        world, self.action_world_baseline
                    )
                ),
                None,
            )
            if reconnected is None:
                return
            reconnected_pads, _ = reconnected
            self.hotplug_devices_after = {
                slot: pad.device for slot, pad in reconnected_pads.items()
            }
            self.action_evidence["hotplug"]["connect_sample"] = (
                reconnected_pads[1].sample
            )
            self.action_evidence["hotplug"]["new_device"] = reconnected_pads[
                self.hotplug_player
            ].device
            self.action_evidence["hotplug"]["unaffected_devices_preserved"] = all(
                self.hotplug_devices_after[slot] == self.hotplug_devices_before[slot]
                for slot in range(1, self.players + 1)
                if slot != self.hotplug_player
            )
            self._send_player_movement(
                self.hotplug_player, reconnected, elapsed, post_reconnect=True
            )
            return

        if self.phase == "hotplug-final-neutral":
            neutral = next(
                (
                    (pads, world)
                    for pads, world in fresh
                    if self._all_connected_neutral(pads)
                    and self._world_cohort_eligible(world)
                    and self._same_world_identity(
                        world, self.action_world_baseline
                    )
                ),
                None,
            )
            if neutral is None:
                return
            neutral_pads, neutral_world = neutral
            self.minimum_pad_sample = neutral_pads[1].sample
            self.action_world_baseline = dict(neutral_world)
            command = self._reset_all()
            self.action_evidence["final_reset_sequence"] = self.sequence
            if self._send(
                [command],
                elapsed,
                "finish local multiplayer with every pad neutral",
                0.0,
            ):
                self._set_action_phase("final-neutral", elapsed)
            return

        if self.phase == "final-neutral":
            neutral = next(
                (
                    (pads, world)
                    for pads, world in fresh
                    if self._all_connected_neutral(pads)
                    and self._world_cohort_eligible(world)
                    and self._same_world_identity(
                        world, self.action_world_baseline
                    )
                ),
                None,
            )
            if neutral is None:
                return
            neutral_pads, _ = neutral
            self.action_evidence["final_neutral_sample"] = neutral_pads[1].sample
            self.completed = True
            self._set_action_phase("complete", elapsed)

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
            self.next_soak_heartbeat_at = elapsed + self.SOAK_HEARTBEAT_INTERVAL_SECONDS

    def begin_post_ready_soak(self, elapsed: float, *, heartbeat_enabled: bool) -> None:
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
        self._refresh_benchmark_arena_ack_evidence()
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

        if self.phase in {
            "waiting-for-match-readiness",
            "player-move-active",
            "player-move-neutral",
            "player-fire-active",
            "player-fire-neutral",
            "hotplug-disconnecting",
            "hotplug-connecting",
            "hotplug-move-active",
            "hotplug-move-neutral",
            "hotplug-fire-active",
            "hotplug-final-neutral",
            "final-neutral",
            "complete",
        }:
            if self.phase != "complete":
                self._advance_active_scenario(log_text, elapsed)
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
                self.phase = (
                    "benchmark-arena-move-to-scenario"
                    if self.fixed_benchmark_arena
                    else "joining-guests"
                )
                if self.fixed_benchmark_arena:
                    self.action_phase_started_at = elapsed
                self.next_action_at = elapsed + 0.75
            return

        if self.phase == "benchmark-arena-move-to-scenario":
            if (
                elapsed - self.action_phase_started_at
                > self.ACTION_PHASE_TIMEOUT_SECONDS
            ):
                self.error = (
                    "fixed multiplayer benchmark arena move to Scenario timed out"
                )
                return
            if self.observed_state != 15 or self.observed_joined != 1:
                self.error = (
                    "fixed multiplayer benchmark arena lost the create-game "
                    "header precondition before selecting Scenario"
                )
                return
            if (
                elapsed >= self.next_action_at
                and self._command_finished(elapsed)
            ):
                command = self._pulse_axis(1, "LY", 32767, 80)
                evidence = self.benchmark_arena_evidence["move_to_scenario"]
                evidence["sequence"] = self.sequence
                evidence["command"] = command.rstrip()
                if self._send(
                    [command],
                    elapsed,
                    "move from create-game header to Scenario for fixed 4-player benchmark",
                    0.6,
                ):
                    self._set_action_phase(
                        "benchmark-arena-move-to-level", elapsed
                    )
            return

        if self.phase == "benchmark-arena-move-to-level":
            if (
                elapsed - self.action_phase_started_at
                > self.ACTION_PHASE_TIMEOUT_SECONDS
            ):
                self.error = (
                    "fixed multiplayer benchmark arena move to Level timed out"
                )
                return
            if self.observed_state != 15 or self.observed_joined != 1:
                self.error = (
                    "fixed multiplayer benchmark arena left create-game "
                    "before selecting Level"
                )
                return
            if self._command_finished(elapsed):
                command = self._pulse_axis(1, "LY", 32767, 80)
                evidence = self.benchmark_arena_evidence["move_to_level"]
                evidence["sequence"] = self.sequence
                evidence["command"] = command.rstrip()
                if self._send(
                    [command],
                    elapsed,
                    "move from Scenario to Level for fixed 4-player benchmark",
                    0.6,
                ):
                    self._set_action_phase(
                        "benchmark-arena-open-level-selector", elapsed
                    )
            return

        if self.phase == "benchmark-arena-open-level-selector":
            if (
                elapsed - self.action_phase_started_at
                > self.ACTION_PHASE_TIMEOUT_SECONDS
            ):
                self.error = (
                    "fixed multiplayer benchmark Level selector open timed out"
                )
                return
            if self.observed_state != 15 or self.observed_joined != 1:
                self.error = (
                    "fixed multiplayer benchmark arena left create-game "
                    "before opening the Level selector"
                )
                return
            if self._command_finished(elapsed):
                command = self._pulse_button(1, "SOUTH")
                evidence = self.benchmark_arena_evidence["open_level_selector"]
                evidence["sequence"] = self.sequence
                evidence["command"] = command.rstrip()
                if self._send(
                    [command],
                    elapsed,
                    "open the multiplayer Level selector",
                    0.75,
                ):
                    self._set_action_phase(
                        "benchmark-arena-move-to-fixed-level", elapsed
                    )
            return

        if self.phase == "benchmark-arena-move-to-fixed-level":
            if (
                elapsed - self.action_phase_started_at
                > self.ACTION_PHASE_TIMEOUT_SECONDS
            ):
                self.error = (
                    "fixed multiplayer benchmark Level selector move timed out"
                )
                return
            if self._command_finished(elapsed):
                command = self._pulse_axis(1, "LY", 32767, 80)
                evidence = self.benchmark_arena_evidence["move_to_fixed_level"]
                evidence["sequence"] = self.sequence
                evidence["command"] = command.rstrip()
                if self._send(
                    [command],
                    elapsed,
                    "move Random to the first fixed multiplayer level",
                    0.6,
                ):
                    self._set_action_phase(
                        "benchmark-arena-confirm-fixed-level", elapsed
                    )
            return

        if self.phase == "benchmark-arena-confirm-fixed-level":
            if (
                elapsed - self.action_phase_started_at
                > self.ACTION_PHASE_TIMEOUT_SECONDS
            ):
                self.error = (
                    "fixed multiplayer benchmark Level confirmation timed out"
                )
                return
            if self._command_finished(elapsed):
                command = self._pulse_button(1, "SOUTH")
                evidence = self.benchmark_arena_evidence["confirm_fixed_level"]
                evidence["sequence"] = self.sequence
                evidence["command"] = command.rstrip()
                if self._send(
                    [command],
                    elapsed,
                    "confirm the first fixed multiplayer level",
                    0.75,
                ):
                    self._set_action_phase(
                        "benchmark-arena-selection-settle", elapsed
                    )
            return

        if self.phase == "benchmark-arena-selection-settle":
            if (
                elapsed - self.action_phase_started_at
                > self.ACTION_PHASE_TIMEOUT_SECONDS
            ):
                self.error = (
                    "fixed multiplayer benchmark arena acknowledgement timed out"
                )
                return
            if self._command_finished(elapsed):
                self._refresh_benchmark_arena_ack_evidence()
                evidence = self.benchmark_arena_evidence
                if not (
                    evidence["move_to_scenario"]["acknowledged"]
                    and evidence["move_to_level"]["acknowledged"]
                    and evidence["open_level_selector"]["acknowledged"]
                    and evidence["move_to_fixed_level"]["acknowledged"]
                    and evidence["confirm_fixed_level"]["acknowledged"]
                ):
                    self.error = (
                        "fixed multiplayer benchmark arena selection was not "
                        "fully acknowledged"
                    )
                    return
                if self.observed_state != 15 or self.observed_joined != 1:
                    # Confirmation is asynchronous. Stay fail-closed until the
                    # create-local-game screen returns or this phase times out.
                    return
                evidence[
                    "create_local_game_returned_after_confirmation"
                ] = True
                evidence["selection_completed_before_guest_join"] = True
                self.phase = "joining-guests"
                self.next_action_at = elapsed + 0.75
            return

        if self.phase == "joining-guests":
            if (
                self.observed_state == 15
                and elapsed >= self.next_action_at
                and self._command_finished(elapsed)
            ):
                if self.fixed_benchmark_arena:
                    evidence = self.benchmark_arena_evidence
                    if not evidence["selection_completed_before_guest_join"]:
                        self.error = (
                            "guest join attempted before fixed benchmark arena "
                            "selection completed"
                        )
                        return
                    evidence["guest_join_first_sequence"] = self.sequence + 1
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
                if self.fixed_benchmark_arena:
                    self.benchmark_mission_observation_start_index = len(
                        list(gameplay.MISSION_STATE.finditer(log_text))
                    )
                command = self._pulse_button(1, "START")
                if self.fixed_benchmark_arena:
                    self.benchmark_arena_evidence[
                        "start_match_sequence"
                    ] = self.sequence
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
                self._set_action_phase("waiting-for-match-readiness", elapsed)
                return

    def result(self) -> dict[str, Any]:
        self._refresh_benchmark_arena_ack_evidence()
        return {
            "completed": self.completed,
            "phase": self.phase,
            "error": self.error,
            "evidence": self.action_evidence,
            "devices_before_hotplug": self.hotplug_devices_before,
            "devices_after_hotplug": self.hotplug_devices_after,
            "profile_anchor_swap_end": self.profile_anchor_swap_end,
            "expected_level": self.expected_level,
            "observed_level": self.observed_level,
            "benchmark_arena_selection": dict(
                self.benchmark_arena_evidence
            ),
        }

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
    input_driver: LocalMultiplayerInputDriver | gameplay.DamGameplayScenario | None = (
        None
    )
    if args.mode in ("local-multiplayer", "dam-gameplay"):
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
    pre_ready_profile_failures: list[str] = []
    host_window_id: int | None = None
    host_window_error: str | None = None
    dam_profile_anchor_swap_end: int | None = None
    soak_elapsed = 0.0
    soak_completed = False
    soak_abort_log_matches: list[str] = []
    soak_progress_before: dict[str, Any] | None = None
    soak_progress_after: dict[str, Any] | None = None
    soak_progress_failures: list[str] = []
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
                input_driver = (
                    LocalMultiplayerInputDriver(
                        args.players,
                        virtual_gamepad_write_fd,
                        getattr(args, "expected_multiplayer_level", None),
                        getattr(
                            args, "fixed_multiplayer_benchmark_arena", False
                        ),
                    )
                    if args.mode == "local-multiplayer"
                    else gameplay.DamGameplayScenario(
                        virtual_gamepad_write_fd,
                        host_pause_required=getattr(
                            args, "validate_host_pause", False
                        ),
                    )
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
                    if isinstance(input_driver, gameplay.DamGameplayScenario):
                        input_driver.advance(
                            combined_cycle_text(log_path),
                            elapsed,
                            # raw.log is the primary process stream. Runtime
                            # file logs may mirror it or may be the only sink.
                            # Select one coherent event stream without ever
                            # merging duplicate or divergent evidence.
                            host_pause_text=host_pause_event_source_text(log_path),
                        )
                    else:
                        input_driver.advance(log_path, elapsed)
                    if input_driver.error:
                        break
                    if (
                        isinstance(input_driver, gameplay.DamGameplayScenario)
                        and input_driver.baseline is not None
                        and dam_profile_anchor_swap_end is None
                    ):
                        # The first eligible gameplay baseline proves exact Dam
                        # mission readiness. Exclude the renderer cohort already
                        # in flight at that instant so menu/transition work can
                        # never satisfy the gameplay performance gate.
                        dam_profile_anchor_swap_end = profile_anchor_after_live_frontier(
                            log_path
                        )
                    if (
                        isinstance(input_driver, LocalMultiplayerInputDriver)
                        and input_driver.match_readiness_observed
                        and input_driver.profile_anchor_swap_end is None
                    ):
                        # The renderer emits a window header only after all 64
                        # swaps have elapsed. Exclude one additional cohort so
                        # a window already in flight when match readiness was
                        # observed cannot be counted as post-match evidence.
                        input_driver.profile_anchor_swap_end = (
                            profile_anchor_after_live_frontier(log_path)
                        )
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
                    (
                        input_driver.profile_anchor_swap_end
                        if isinstance(input_driver, LocalMultiplayerInputDriver)
                        else dam_profile_anchor_swap_end
                        if isinstance(input_driver, gameplay.DamGameplayScenario)
                        else None
                    ),
                )
                if isinstance(input_driver, gameplay.DamGameplayScenario):
                    readiness_details["gameplay_scenario"] = input_driver.result()
                    ready = ready and input_driver.completed
                elif isinstance(input_driver, LocalMultiplayerInputDriver):
                    readiness_details["multiplayer_scenario"] = input_driver.result()
                    ready = ready and input_driver.completed
                if getattr(args, "capture_gpu_frame", False):
                    live_gpu_capture = gpu_capture_evidence(log_path)
                    readiness_details["gpu_capture"] = live_gpu_capture
                    ready = ready and live_gpu_capture["validated"]
                pre_ready_profile_failures = list(
                    readiness_details.get("profile_failures", [])
                )
                if pre_ready_profile_failures:
                    break
                if ready and args.quit_method == "native":
                    try:
                        candidate = rendering.macos_app_control(
                            ROOT, "window-id", process.pid
                        )
                        parsed_window_id = int(candidate)
                        if parsed_window_id <= 0:
                            raise ValueError(f"invalid window ID {candidate!r}")
                        host_window_id = parsed_window_id
                        host_window_error = None
                        readiness_details["host_window_id"] = host_window_id
                    except (
                        ValueError,
                        rendering.ImageError,
                        OSError,
                        subprocess.TimeoutExpired,
                    ) as error:
                        host_window_error = str(error)
                        readiness_details["host_window_error"] = host_window_error
                        ready = False
                if ready:
                    ready_elapsed = elapsed
                    break
                time.sleep(args.poll_seconds)

            if ready:
                soak_progress_before = runtime_progress_snapshot(log_path, args.mode)
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
                        if isinstance(input_driver, gameplay.DamGameplayScenario):
                            input_driver.advance(
                                combined_cycle_text(log_path),
                                now - started,
                                host_pause_text=host_pause_event_source_text(
                                    log_path
                                ),
                            )
                        else:
                            input_driver.advance(log_path, now - started)
                        if input_driver.error:
                            break
                    soak_abort_log_matches = detect_fatal_logs(cycle_root)
                    if soak_abort_log_matches:
                        break
                    time.sleep(min(args.poll_seconds, soak_deadline - now))

                if soak_completed and args.post_ready_soak_seconds > 0:
                    log.flush()
                    soak_progress_after = runtime_progress_snapshot(log_path, args.mode)
                    if getattr(args, "require_soak_progress", True):
                        soak_progress_failures = soak_progress_violations(
                            soak_progress_before, soak_progress_after, args.mode
                        )

            if (
                input_driver
                and ready
                and soak_completed
                and exited_during_soak is None
                and not soak_abort_log_matches
            ):
                if isinstance(input_driver, gameplay.DamGameplayScenario):
                    input_driver.finish_post_ready_soak(
                        combined_cycle_text(log_path), time.monotonic() - started
                    )
                else:
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
        "scenario": input_driver.result() if input_driver else None,
    }

    gpu_replay: dict[str, Any] | None = None
    if getattr(args, "capture_gpu_frame", False):
        replay_capture_evidence = gpu_capture_evidence(log_path)
        if replay_capture_evidence["validated"]:
            gpu_replay = run_capture_replay_gate(
                cycle_root / TRACE_CAPTURE_RELATIVE_PATH,
                getattr(args, "trace_replay_executable", None),
                cycle_root,
                getattr(args, "runtime_dir", None),
                expected_trace_bytes=int(replay_capture_evidence["byte_count"]),
                timeout_seconds=float(
                    getattr(
                        args,
                        "trace_replay_timeout",
                        TRACE_REPLAY_TIMEOUT_SECONDS,
                    )
                ),
                exact_output_merger=getattr(args, "exact_output_merger", False),
            )
        else:
            gpu_replay = {
                "status": "not-run",
                "strict_deterministic_preflight": True,
                "best_effort": False,
                "runs": [],
                "failures": [
                    "in-app private trace validation did not complete"
                ],
            }

    final_host_pause = (
        host_pause_integration_evidence(
            host_pause_event_source_text(log_path), input_script
        )
        if getattr(args, "validate_host_pause", False)
        else None
    )

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
    performance_cohort_report: dict[str, Any] | None = None
    performance_comparison: dict[str, Any] | None = None
    final_windows, final_violations, final_counts = profile.parse_log(log_path)
    dummy_failures = dummy_texture_violations(log_path, args.mode)
    final_gpu_capture = (
        gpu_capture_evidence(log_path)
        if getattr(args, "capture_gpu_frame", False)
        else None
    )
    final_exact_output_merger = (
        exact_output_merger_evidence(log_path)
        if getattr(args, "exact_output_merger", False)
        else None
    )
    final_profile_failures = (
        final_violations
        + profile.wait_reg_mem_violations(final_windows)
        + dummy_failures
    )
    min_mean_fps, max_window_p99_ms = performance_thresholds(args, args.mode)
    if args.mode in ("dam", "dam-gameplay"):
        selection_failures: list[str] = []
        if args.mode == "dam-gameplay":
            post_dam = contiguous_profile_windows_after_anchor(
                final_windows,
                dam_profile_anchor_swap_end,
                require_dam_candidate=True,
            )
            for window in final_windows:
                window["selected"] = False
            final_selected = post_dam[
                args.warmup_windows : args.warmup_windows + args.observe_windows
            ]
            for window in final_selected:
                window["selected"] = True
            if len(final_selected) != args.observe_windows:
                selection_failures.append(
                    "insufficient contiguous post-Dam-readiness Metal profile windows: "
                    f"need {args.warmup_windows + args.observe_windows}, "
                    f"found {len(post_dam)} after anchor "
                    f"{dam_profile_anchor_swap_end}"
                )
        else:
            final_selected = profile.select_windows(
                final_windows, args.warmup_windows, args.observe_windows
            )
        if len(final_selected) == args.observe_windows:
            final_profile_failures.extend(
                profile.performance_violations(
                    profile.aggregate(final_selected),
                    min_mean_fps,
                    max_window_p99_ms,
                    None,
                )
            )
        else:
            if selection_failures:
                final_profile_failures.extend(selection_failures)
            else:
                final_profile_failures.append(
                    "insufficient contiguous Dam windows: "
                    f"need {args.warmup_windows + args.observe_windows}, found "
                    f"{profile.contiguous_dam_window_count(final_windows)}"
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
            min_mean_fps,
            max_window_p99_ms,
            None,
            dummy_failures + selection_failures,
        )
        profile_artifacts = str(profile_root)
        # Keep cycle.json and the profile artifacts on one exact failure set.
        # This prevents WAIT_REG_MEM, dummy textures, or future external gates
        # from failing one surface while accidentally passing the other.
        try:
            profile_summary = json.loads(
                (profile_root / "summary.json").read_text(encoding="utf-8")
            )
            final_profile_failures = list(profile_summary.get("failures", []))
        except (OSError, json.JSONDecodeError):
            final_profile_failures.append(
                "Metal profile summary could not be read after validation"
            )
            final_profile_passed = False
    elif args.mode == "local-multiplayer":
        anchor = (
            input_driver.profile_anchor_swap_end
            if isinstance(input_driver, LocalMultiplayerInputDriver)
            else None
        )
        post_match = contiguous_profile_windows_after_anchor(final_windows, anchor)
        final_selected = post_match[
            args.warmup_windows : args.warmup_windows + args.observe_windows
        ]
        if len(final_selected) != args.observe_windows:
            final_profile_failures.append(
                "insufficient post-match Metal profile windows: "
                f"need {args.warmup_windows + args.observe_windows}, "
                f"found {len(post_match)}"
            )
        else:
            final_aggregate = profile.aggregate(final_selected)
            final_profile_failures.extend(
                profile.performance_violations(
                    final_aggregate,
                    min_mean_fps,
                    max_window_p99_ms,
                    None,
                )
            )
            candidate_details = dict(readiness_details)
            candidate_details["aggregate"] = final_aggregate
            candidate_cohort, candidate_cohort_failures = (
                multiplayer_performance_cohort(candidate_details)
            )
            requested_draw_batch = int(
                getattr(args, "probe_draws_per_command_buffer", 128)
            )
            candidate_cohort_failures.extend(
                requested_probe_configuration_violations(
                    candidate_cohort, requested_draw_batch
                )
            )
            performance_cohort_report = {
                "status": "pass" if not candidate_cohort_failures else "fail",
                "requested_draws_per_command_buffer": requested_draw_batch,
                "observed_draws_per_command_buffer": (
                    candidate_cohort.get("draws_per_command_buffer")
                    if candidate_cohort is not None
                    else None
                ),
                "cohort": candidate_cohort,
                "failures": candidate_cohort_failures,
            }
            final_profile_failures.extend(
                "performance cohort: " + failure
                for failure in candidate_cohort_failures
            )
            performance_reference = getattr(
                args, "performance_reference_cohort", None
            )
            if performance_reference is not None:
                performance_comparison = compare_multiplayer_performance_cohort(
                    performance_reference,
                    candidate_details,
                    getattr(args, "draw_cohort_tolerance_percent", 2.0),
                    requested_draw_batch,
                )
                performance_comparison["expected_level"] = getattr(
                    args, "expected_multiplayer_level", None
                )
                performance_comparison["observed_level"] = getattr(
                    input_driver, "observed_level", None
                )
                final_profile_failures.extend(
                    "performance comparison: " + failure
                    for failure in performance_comparison[
                        "comparability_failures"
                    ]
                    if failure not in candidate_cohort_failures
                )
        if (
            getattr(args, "performance_reference_cohort", None) is not None
            and performance_comparison is None
        ):
            performance_comparison = {
                "comparability_status": "fail",
                "draw_cohort_tolerance_percent": getattr(
                    args, "draw_cohort_tolerance_percent", 2.0
                ),
                "draw_calls_per_swap_delta_percent": None,
                "reference": dict(args.performance_reference_cohort),
                "candidate": None,
                "observed_metrics": {
                    "reference": dict(
                        args.performance_reference_cohort.get(
                            "performance_metrics", {}
                        )
                    ),
                    "candidate": None,
                },
                "metric_deltas": {
                    "candidate_minus_reference": None,
                    "candidate_vs_reference_percent": None,
                },
                "expected_level": getattr(
                    args, "expected_multiplayer_level", None
                ),
                "observed_level": getattr(input_driver, "observed_level", None),
                "comparability_failures": [
                    "no complete selected candidate cohort was produced"
                ],
            }
            final_profile_failures.append(
                "performance comparison: no complete selected candidate cohort was produced"
            )
        final_profile_passed = not final_profile_failures
    else:
        # Menu readiness was observed before capture and shutdown. Parse the
        # completed log too so a late fallback, malformed profile window, or
        # renderer failure cannot hide behind that earlier clean snapshot.
        if not any(window["complete"] for window in final_windows):
            final_profile_failures.append(
                "no complete presenter-backed Metal profile window was emitted"
            )
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
        and not pre_ready_profile_failures
    ):
        if exited_before_ready is not None:
            failures.append(
                f"application exited with code {exited_before_ready} before {args.mode} readiness"
            )
        else:
            failures.append(
                f"{args.mode} readiness was not reached within {args.ready_timeout:.1f} seconds"
            )
    if not ready and args.quit_method == "native" and host_window_error:
        failures.append(f"no visible GoldenEye host window: {host_window_error}")
    if exited_during_soak is not None:
        failures.append(
            f"application exited with code {exited_during_soak} during post-ready soak"
        )
    failures.extend(readiness_details.get("profile_failures", []))
    failures.extend(soak_progress_failures)
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
    if final_gpu_capture is not None and not final_gpu_capture["validated"]:
        failures.append(
            "one-frame Dam GPU capture was not validated through the private trace reader"
        )
    if (
        final_gpu_capture is not None
        and final_gpu_capture["validated"]
        and (gpu_replay is None or gpu_replay.get("status") != "pass")
    ):
        replay_failures = (
            gpu_replay.get("failures", []) if gpu_replay is not None else []
        )
        if replay_failures:
            failures.extend(
                f"GPU capture replay: {failure}" for failure in replay_failures
            )
        else:
            failures.append("GPU capture replay did not pass")
    if (
        final_exact_output_merger is not None
        and not final_exact_output_merger["validated"]
    ):
        failures.append("no title draw crossed the exact output-merger route")
    if final_host_pause is not None and not final_host_pause["validated"]:
        failures.append(
            "Host Settings pause/resume integration proof failed: "
            + str(final_host_pause.get("failure") or "incomplete event sequence")
        )
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

    eligibility = cycle_release_eligibility(
        args,
        failures,
        expected_multiplayer_level=getattr(
            args, "expected_multiplayer_level", None
        ),
        observed_multiplayer_level=getattr(input_driver, "observed_level", None),
        performance_comparison=performance_comparison,
    )
    result = {
        "cycle": number,
        "status": (
            "fail" if failures else successful_status(bool(eligibility["eligible"]))
        ),
        "release_eligible": eligibility["eligible"],
        "release_ineligibility_reasons": eligibility["reasons"],
        "mode": args.mode,
        "command": command,
        "pid": process.pid if process is not None else None,
        "elapsed_seconds": elapsed,
        "ready": ready,
        "ready_seconds": ready_elapsed,
        "pre_ready_abort_log_matches": pre_ready_abort_log_matches,
        "pre_ready_profile_failures": pre_ready_profile_failures,
        "host_window_id": host_window_id,
        "host_window_error": host_window_error,
        "dam_profile_anchor_swap_end": dam_profile_anchor_swap_end,
        "post_ready_soak": {
            "requested_seconds": args.post_ready_soak_seconds,
            "elapsed_seconds": soak_elapsed if ready else None,
            "completed": soak_completed,
            "abort_log_matches": soak_abort_log_matches,
            "progress_gate_enabled": getattr(args, "require_soak_progress", True),
            "progress_before": soak_progress_before,
            "progress_after": soak_progress_after,
            "progress_failures": soak_progress_failures,
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
        "gpu_capture": final_gpu_capture,
        "gpu_replay": gpu_replay,
        "exact_output_merger": final_exact_output_merger,
        "host_pause": final_host_pause,
        "render_comparison": regression,
        "profile_artifacts": profile_artifacts,
        "final_profile_passed": final_profile_passed,
        "final_profile_failures": final_profile_failures,
        "performance_cohort": performance_cohort_report,
        "performance_comparison": performance_comparison,
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
    diagnostic_passed = sum(
        cycle["status"] in ("pass", "diagnostic-pass") for cycle in cycles
    )
    release_passed = sum(cycle["status"] == "pass" for cycle in cycles)
    eligibility = suite_release_eligibility(args, cycles)
    suite_status = (
        "fail"
        if diagnostic_passed != len(cycles)
        else successful_status(bool(eligibility["eligible"]))
    )
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
        bool(cycle.get("post_ready_soak", {}).get("completed")) for cycle in cycles
    )
    summary = {
        "status": suite_status,
        "diagnostic_status": (
            "pass" if diagnostic_passed == len(cycles) else "fail"
        ),
        "release_eligible": eligibility["eligible"],
        "release_ineligibility_reasons": eligibility["reasons"],
        "mode": args.mode,
        "cycles_requested": len(cycles),
        "cycles_passed": diagnostic_passed,
        "release_cycles_passed": release_passed,
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
        "performance_reference_summary": (
            str(args.performance_reference_summary)
            if getattr(args, "performance_reference_summary", None)
            else None
        ),
        "expected_multiplayer_level": getattr(
            args, "expected_multiplayer_level", None
        ),
        "fixed_multiplayer_benchmark_arena": getattr(
            args, "fixed_multiplayer_benchmark_arena", False
        ),
        "probe_draws_per_command_buffer": getattr(
            args, "probe_draws_per_command_buffer", 128
        ),
        "draw_cohort_tolerance_percent": getattr(
            args, "draw_cohort_tolerance_percent", 2.0
        ),
        "post_ready_soak_seconds": args.post_ready_soak_seconds,
        "post_ready_soak_cycles_completed": soak_cycles_completed,
        "cycles": cycles,
    }
    (suite_root / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    lines = [
        f"status: {summary['status']}",
        f"release eligible: {'yes' if summary['release_eligible'] else 'no'}",
        f"mode: {args.mode}",
        f"cycles: {diagnostic_passed}/{len(cycles)} passed",
        f"native clean shutdowns: {summary['native_clean_shutdowns']}/{len(cycles)}",
        (
            f"requested post-ready soak: {args.post_ready_soak_seconds:g} seconds; "
            f"completed: {soak_cycles_completed}/{len(cycles)} cycles"
        ),
    ]
    for reason in summary["release_ineligibility_reasons"]:
        lines.append(f"release ineligible: {reason}")
    if mean_fps:
        lines.append(
            f"mean 64-frame-window FPS across cycles: {statistics.fmean(mean_fps):.3f}"
        )
    for cycle in cycles:
        if cycle["failures"]:
            lines.append(f"cycle {cycle['cycle']}: " + "; ".join(cycle["failures"]))
    (suite_root / "summary.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    # Diagnostic/self-test runs still return success when their actual checks
    # pass, while their JSON/text status can never be mistaken for release proof.
    return summary["diagnostic_status"] == "pass"


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
        choices=("menu", "dam", "dam-gameplay", "local-multiplayer"),
        default="dam",
    )
    parser.add_argument(
        "--players",
        type=int,
        default=2,
        help="local multiplayer test player count (2-4)",
    )
    parser.add_argument(
        "--fixed-multiplayer-benchmark-arena",
        action="store_true",
        help=(
            "4-player benchmark only: move from the create-game header through "
            "Scenario to Level, then change Random before guests join"
        ),
    )
    parser.add_argument(
        "--probe-draws-per-command-buffer",
        type=int,
        choices=(64, 128, 256),
        default=128,
        help=(
            "developer-harness Metal draw batch for local-multiplayer "
            "performance comparison (default: 128)"
        ),
    )
    parser.add_argument(
        "--performance-reference-summary",
        type=Path,
        help=(
            "one-cycle 4-player stability summary used to pin the exact level "
            "and draw cohort for an A/B candidate"
        ),
    )
    parser.add_argument(
        "--draw-cohort-tolerance-percent",
        type=finite_float,
        default=2.0,
        help="maximum draw-calls-per-swap difference from the A/B reference",
    )
    parser.add_argument(
        "--expected-multiplayer-level",
        type=int,
        help=(
            "fail immediately if the started local match is not this "
            "level; set automatically by --performance-reference-summary"
        ),
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
    parser.add_argument(
        "--min-mean-fps",
        type=finite_float,
        default=DEFAULT_MIN_MEAN_FPS,
        help=(
            "minimum measured gameplay mean FPS for a release pass "
            f"(default: {DEFAULT_MIN_MEAN_FPS:g}; override for a target device)"
        ),
    )
    parser.add_argument(
        "--max-window-p99-ms",
        type=finite_float,
        default=DEFAULT_MAX_WINDOW_P99_MS,
        help=(
            "maximum p99 of the 64-frame-window average frame times "
            f"(default: {DEFAULT_MAX_WINDOW_P99_MS:g} ms; not an individual-frame p99)"
        ),
    )
    parser.add_argument(
        "--disable-performance-gates",
        action="store_true",
        help=(
            "disable FPS and frame-time release thresholds for diagnostic runs; "
            "renderer correctness gates remain mandatory"
        ),
    )
    parser.add_argument("--shutdown-timeout", type=finite_float, default=20.0)
    parser.add_argument(
        "--quit-method",
        choices=("native", "signal"),
        default="native",
        help=(
            "native validates a visible AppKit window and clean quit; signal "
            "disables host-window proof and is only for headless harness self-tests"
        ),
    )
    parser.add_argument(
        "--no-soak-progress-gate",
        dest="require_soak_progress",
        action="store_false",
        help=(
            "diagnostic-only: do not require new profile windows and guest/render "
            "counters during the post-ready soak"
        ),
    )
    parser.set_defaults(require_soak_progress=True)
    parser.add_argument("--capture", action="store_true")
    parser.add_argument(
        "--capture-gpu-frame",
        action="store_true",
        help=(
            "developer harness only: after live Dam gameplay is proven, capture "
            "and strictly replay exactly one private GPU frame twice"
        ),
    )
    parser.add_argument(
        "--trace-replay-executable",
        type=Path,
        help=(
            "trace_dump_metal binary used by --capture-gpu-frame; defaults to "
            "a matching source-tree build when one is available"
        ),
    )
    parser.add_argument(
        "--trace-replay-timeout",
        type=finite_float,
        default=TRACE_REPLAY_TIMEOUT_SECONDS,
        help=(
            "maximum seconds for each strict Metal trace replay "
            f"(default: {TRACE_REPLAY_TIMEOUT_SECONDS:g})"
        ),
    )
    parser.add_argument(
        "--validate-host-pause",
        action="store_true",
        help=(
            "developer harness only: open the real Host Settings during Dam, "
            "prove world freeze with live presentation, close it, and prove "
            "resume before gameplay input validation"
        ),
    )
    parser.add_argument(
        "--exact-output-merger",
        action="store_true",
        help=(
            "developer diagnostic: enable the default-off exact Xenos EDRAM "
            "route and require proof that at least one title draw used it"
        ),
    )
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
        if args.mode in ("dam-gameplay", "local-multiplayer")
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
    args.trace_replay_executable = locate_trace_replay_executable(
        args.trace_replay_executable,
        args.executable,
    )
    requested_output = args.output.expanduser().resolve() if args.output else None
    args.reference = args.reference.expanduser().resolve() if args.reference else None
    args.performance_reference_summary = (
        args.performance_reference_summary.expanduser().resolve()
        if args.performance_reference_summary
        else None
    )
    args.performance_reference_cohort = None
    if args.reference and not args.capture:
        parser.error("--reference requires --capture")
    if args.reference and args.mode != "menu":
        parser.error(
            "Dam gameplay is not frame-synchronized; --reference is supported only with --mode menu"
        )
    if args.performance_reference_summary and (
        args.mode != "local-multiplayer" or args.players != 4
    ):
        parser.error(
            "--performance-reference-summary requires "
            "--mode local-multiplayer --players 4"
        )
    if args.fixed_multiplayer_benchmark_arena and (
        args.mode != "local-multiplayer" or args.players != 4
    ):
        parser.error(
            "--fixed-multiplayer-benchmark-arena requires "
            "--mode local-multiplayer --players 4"
        )
    if args.expected_multiplayer_level is not None and args.mode != "local-multiplayer":
        parser.error("--expected-multiplayer-level requires --mode local-multiplayer")
    if args.probe_draws_per_command_buffer != 128 and args.mode != "local-multiplayer":
        parser.error(
            "--probe-draws-per-command-buffer is configurable only with "
            "--mode local-multiplayer"
        )
    if args.capture_gpu_frame and args.mode != "dam-gameplay":
        parser.error("--capture-gpu-frame requires --mode dam-gameplay")
    if args.validate_host_pause and args.mode != "dam-gameplay":
        parser.error("--validate-host-pause requires --mode dam-gameplay")
    if args.capture_gpu_frame and args.validate_host_pause:
        parser.error(
            "--capture-gpu-frame cannot be combined with --validate-host-pause"
        )
    if not args.executable.is_file() or not os.access(args.executable, os.X_OK):
        parser.error(f"executable is not runnable: {args.executable}")
    dylib = args.runtime_dir / "librexruntime.dylib"
    if not dylib.is_file():
        parser.error(f"runtime library does not exist: {dylib}")
    if args.mode in ("dam-gameplay", "local-multiplayer"):
        expected_markers = (
            (b"[ge-test] gameplay sample=",)
            if args.mode == "dam-gameplay"
            else (b"[ge-test] menu state=", b"[ge-test] local-pad sample=")
        )
        if (
            not binary_contains(dylib, b"REX_TEST_VIRTUAL_GAMEPADS")
            or (
                args.mode == "local-multiplayer"
                and not binary_contains(dylib, b"DISCONNECT")
            )
            or not all(
                binary_contains(args.executable, marker) for marker in expected_markers
            )
        ):
            parser.error(
                f"{args.mode} mode requires the developer harness build; "
                "configure preset macos-arm64-multiplayer-test and build target "
                "goldeneye_macos_app"
            )
    if args.capture_gpu_frame and not binary_contains(
        args.executable, b"GOLDENEYE_TEST_CAPTURE_DAM_FRAME"
    ):
        parser.error(
            "--capture-gpu-frame requires a current developer harness build"
        )
    if args.capture_gpu_frame and (
        args.trace_replay_executable is None
        or not args.trace_replay_executable.is_file()
        or not os.access(args.trace_replay_executable, os.X_OK)
    ):
        parser.error(
            "--capture-gpu-frame requires a runnable trace_dump_metal; "
            "build the source-tree Metal trace tool or pass "
            "--trace-replay-executable"
        )
    if args.validate_host_pause and not binary_contains(
        args.executable, b"GOLDENEYE_TEST_HOST_PAUSE"
    ):
        parser.error(
            "--validate-host-pause requires a current developer harness build"
        )
    if not (args.game_data / "default.xex").is_file():
        parser.error(f"game-data directory has no default.xex: {args.game_data}")
    if args.reference and not args.reference.is_file():
        parser.error(f"reference does not exist: {args.reference}")
    if (
        args.performance_reference_summary
        and not args.performance_reference_summary.is_file()
    ):
        parser.error(
            "performance reference does not exist: "
            f"{args.performance_reference_summary}"
        )
    if args.performance_reference_summary:
        try:
            args.performance_reference_cohort = (
                load_multiplayer_performance_reference(
                    args.performance_reference_summary
                )
            )
        except ValueError as error:
            parser.error(str(error))
        reference_level = args.performance_reference_cohort["level"]
        if (
            args.expected_multiplayer_level is not None
            and args.expected_multiplayer_level != reference_level
        ):
            parser.error(
                "--expected-multiplayer-level does not match the performance "
                f"reference ({args.expected_multiplayer_level} != {reference_level})"
            )
        args.expected_multiplayer_level = reference_level
    if args.warmup_windows < 0:
        parser.error("--warmup-windows must not be negative")
    if args.min_mean_fps < 0:
        parser.error("--min-mean-fps must not be negative")
    if args.max_window_p99_ms <= 0:
        parser.error("--max-window-p99-ms must be positive")
    if args.players < 2 or args.players > 4:
        parser.error("--players must be between 2 and 4")
    if (
        args.expected_multiplayer_level is not None
        and not 0 < args.expected_multiplayer_level < 90
    ):
        parser.error("--expected-multiplayer-level must be between 1 and 89")
    if args.draw_cohort_tolerance_percent < 0:
        parser.error("--draw-cohort-tolerance-percent must not be negative")
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
        or args.trace_replay_timeout <= 0
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
    if success:
        label = "PASS" if release_eligibility(args)["eligible"] else "DIAGNOSTIC PASS"
    else:
        label = "FAIL"
    print(f"{label}: {suite_root / 'summary.txt'}")
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
