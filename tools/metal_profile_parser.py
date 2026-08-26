#!/usr/bin/env python3
"""Parse GoldenEye's complete ``[metal-profile]`` windows without dependencies.

The renderer writes one command window followed by its command/wait ledgers.  This
tool deliberately keys every ledger on its inclusive swap range, rather than on
line order, so interleaved presenter output cannot corrupt a benchmark result.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import re
import statistics
import subprocess
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any, Mapping, Sequence


PROFILE = "[metal-profile]"
KV = re.compile(r"([A-Za-z_][A-Za-z0-9_-]*)=([^\s]+)")
RANGE = re.compile(r"(?:swaps|attempts)=(\d+)-(\d+)")
RESOLVE_MODE_FIELDS = (
    "async",
    "sync",
    "missing_snapshot",
    "verbose",
    "missing_alias",
    "exact_snapshot",
)
STAGES = ("draw", "copy", "swap", "wait_reg_mem")
REQUIRED_ZERO_STAGES = ("texture_fallback_decode",)
SAMPLED_STAGES = ("draw_probe_sample", "draw_render_sample")
EXPORTED_STAGES = STAGES + REQUIRED_ZERO_STAGES + SAMPLED_STAGES
PROFILE_WINDOW_SIZE = 64
WINDOW_FIELDS = ("elapsed_ns", "avg_frame_ns", "fps")
COMMAND_FIELDS = (
    "calls",
    "avg_calls_per_swap",
    "total_ns",
    "avg_ns_per_swap",
    "max_call_ns",
    "max_swap_ns",
)
GPU_CHAIN_MAX_FIELDS = (
    "wptr_pending_dwords_max",
    "wptr_to_worker_max_ns",
    "isr_max_ns",
    "interrupt_to_wptr_max_ns",
    "metal_pending_max",
    "throttle_max_ns",
)
GPU_CHAIN_LIVE_FIELDS = ("interrupt_outstanding",)
PROBE_QUEUE_SUM_FIELDS = (
    "commits",
    "backpressure_checks",
    "nonblocking_reclaims",
    "blocking_waits",
    "blocking_wait_ns",
    "wait_reg_mem_signals",
    "wait_reg_mem_timeouts",
    "wait_reg_mem_unavailable",
)
PROBE_QUEUE_MAX_FIELDS = (
    "contexts",
    "lifetime_peak_committed_per_context",
    "lifetime_peak_pending_per_context",
)
PROBE_QUEUE_CONFIG_FIELDS = (
    "draws_per_command_buffer",
    "committed_command_buffer_cap",
)
PROBE_QUEUE_FLAG_FIELDS = ("counter_reset", "configuration_mismatch")
PROBE_QUEUE_FIELDS = (
    PROBE_QUEUE_SUM_FIELDS
    + PROBE_QUEUE_MAX_FIELDS
    + PROBE_QUEUE_CONFIG_FIELDS
    + PROBE_QUEUE_FLAG_FIELDS
)
EXACT_OM_SUM_FIELDS = (
    "attempts",
    "enqueued",
    "completed",
    "rejected",
    "terminal_failures",
    "materializations",
    "full_uploads",
    "full_upload_bytes",
    "full_downloads",
    "full_download_bytes",
    "commits",
    "backpressure_checks",
    "nonblocking_reclaims",
    "blocking_waits",
    "blocking_wait_ns",
)
EXACT_OM_GAUGE_FIELDS = ("pending",)
EXACT_OM_MAX_FIELDS = ("lifetime_peak_committed", "lifetime_peak_pending")
EXACT_OM_CONFIG_FIELDS = ("draws_per_command_buffer", "command_buffer_cap")
EXACT_OM_FLAG_FIELDS = ("enabled", "counter_reset")
EXACT_OM_FIELDS = (
    EXACT_OM_FLAG_FIELDS[:1]
    + EXACT_OM_SUM_FIELDS[:10]
    + EXACT_OM_GAUGE_FIELDS
    + EXACT_OM_SUM_FIELDS[10:]
    + EXACT_OM_MAX_FIELDS
    + EXACT_OM_CONFIG_FIELDS
    + EXACT_OM_FLAG_FIELDS[1:]
)


def is_finite_number(value: Any) -> bool:
    return (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(float(value))
    )


def finite_float(value: str) -> float:
    """Argparse converter which does not silently accept NaN or infinity."""
    try:
        parsed = float(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"{value!r} is not a number") from error
    if not math.isfinite(parsed):
        raise argparse.ArgumentTypeError(f"{value!r} must be finite")
    return parsed


def number(value: str) -> int | float | str:
    try:
        return float(value) if any(c in value.lower() for c in ".e") else int(value, 0)
    except ValueError:
        return value


def fields(line: str) -> dict[str, Any]:
    return {key: number(value) for key, value in KV.findall(line)}


def swap_range(line: str) -> tuple[int, int] | None:
    match = RANGE.search(line)
    return (int(match.group(1)), int(match.group(2))) if match else None


def empty_window(
    start: int, end: int, data: dict[str, Any], source_line: int
) -> dict[str, Any]:
    return {
        "swap_start": start,
        "swap_end": end,
        "swap_count": end - start + 1,
        "elapsed_ns": data.get("elapsed_ns"),
        "avg_frame_ns": data.get("avg_frame_ns"),
        "fps": data.get("fps"),
        "stages": {},
        "wait_reasons": {},
        "wait_reg_mem": [],
        "gpu_chain": None,
        "probe_queue": None,
        "exact_output_merger": None,
        "resolve_mode": None,
        "presenter": [],
        "source_line": source_line,
        "complete": False,
        "validation_errors": [],
        "_header_count": 1,
        "_stage_counts": defaultdict(int),
        "_presenter_summary_count": 0,
        "_probe_queue_count": 0,
        "_exact_om_count": 0,
        "_resolve_mode_count": 0,
        "dam_candidate": False,
        "selected": False,
    }


def validate_window(window: dict[str, Any]) -> list[str]:
    """Validate one renderer window as an indivisible 64-swap sample."""
    errors: list[str] = []
    label = f"swaps {window['swap_start']}-{window['swap_end']}"
    if window["swap_count"] != PROFILE_WINDOW_SIZE:
        errors.append(
            f"{label}: expected exactly {PROFILE_WINDOW_SIZE} swaps, got {window['swap_count']}"
        )
    if window.get("_header_count") != 1:
        errors.append(f"{label}: duplicate window headers")
    for name in WINDOW_FIELDS:
        value = window.get(name)
        if not is_finite_number(value):
            errors.append(f"{label}: window {name} is missing or non-finite")
        elif value <= 0:
            errors.append(f"{label}: window {name} must be positive")
    for stage in STAGES:
        count = window.get("_stage_counts", {}).get(stage, 0)
        if count != 1:
            qualifier = "missing" if count == 0 else f"duplicated {count} times"
            errors.append(f"{label}: command ledger {stage} is {qualifier}")
            continue
        ledger = window["stages"].get(stage, {})
        missing = [name for name in COMMAND_FIELDS if not is_finite_number(ledger.get(name))]
        if missing:
            errors.append(
                f"{label}: command ledger {stage} has missing or non-finite fields: "
                + ", ".join(missing)
            )
        elif any(ledger[name] < 0 for name in COMMAND_FIELDS):
            errors.append(f"{label}: command ledger {stage} contains a negative metric")
        if stage == "swap" and is_finite_number(ledger.get("calls")):
            if float(ledger["calls"]) != PROFILE_WINDOW_SIZE:
                errors.append(
                    f"{label}: swap command ledger must contain {PROFILE_WINDOW_SIZE} calls, "
                    f"got {ledger['calls']}"
                )
    for stage in REQUIRED_ZERO_STAGES:
        count = window.get("_stage_counts", {}).get(stage, 0)
        if count != 1:
            qualifier = "missing" if count == 0 else f"duplicated {count} times"
            errors.append(f"{label}: command ledger {stage} is {qualifier}")
            continue
        ledger = window["stages"].get(stage, {})
        missing = [
            name for name in COMMAND_FIELDS if not is_finite_number(ledger.get(name))
        ]
        if missing:
            errors.append(
                f"{label}: command ledger {stage} has missing or non-finite fields: "
                + ", ".join(missing)
            )
        elif any(ledger[name] < 0 for name in COMMAND_FIELDS):
            errors.append(f"{label}: command ledger {stage} contains a negative metric")
        elif float(ledger["calls"]) != 0:
            errors.append(
                f"{label}: command ledger {stage} must contain zero calls, "
                f"got {ledger['calls']}"
            )

    presenter_count = window.get("_presenter_summary_count", 0)
    if presenter_count != 1:
        qualifier = "missing" if presenter_count == 0 else f"duplicated {presenter_count} times"
        errors.append(f"{label}: presenter summary is {qualifier}")
    else:
        presenter = window["presenter"][0]
        required_presenter = ("sources", "commits", "drawable_nil", "uploads", "upload_bytes")
        missing = [
            name
            for name in required_presenter
            if not is_finite_number(presenter.get(name))
        ]
        if missing:
            errors.append(
                f"{label}: presenter summary has missing or non-finite fields: "
                + ", ".join(missing)
            )
        elif float(presenter["sources"]) <= 0 or float(presenter["commits"]) <= 0:
            errors.append(
                f"{label}: presenter made no source/commit progress "
                f"(sources={presenter['sources']} commits={presenter['commits']})"
            )

    gpu_chain = window.get("gpu_chain")
    if gpu_chain is None:
        errors.append(f"{label}: GPU-chain ledger is missing")
    else:
        required_chain = (
            "wptr_attempts",
            "wptr_accepted",
            "wptr_rejected",
            "ring_batches",
            "ring_commit_rejected",
        )
        missing = [
            name for name in required_chain if not is_finite_number(gpu_chain.get(name))
        ]
        if missing:
            errors.append(
                f"{label}: GPU-chain ledger has missing or non-finite fields: "
                + ", ".join(missing)
            )
        else:
            if float(gpu_chain["wptr_accepted"]) <= 0 or float(gpu_chain["ring_batches"]) <= 0:
                errors.append(
                    f"{label}: GPU-chain made no WPTR/ring progress "
                    f"(accepted={gpu_chain['wptr_accepted']} "
                    f"batches={gpu_chain['ring_batches']})"
                )
            if float(gpu_chain["wptr_attempts"]) < float(gpu_chain["wptr_accepted"]):
                errors.append(
                    f"{label}: GPU-chain accepted more WPTR writes than attempted"
                )
            for name in ("wptr_rejected", "ring_commit_rejected"):
                if float(gpu_chain[name]) != 0:
                    errors.append(f"{label}: GPU-chain {name}={gpu_chain[name]}")
    probe_queue = window.get("probe_queue")
    if probe_queue is not None:
        missing = [
            name
            for name in PROBE_QUEUE_FIELDS
            if not is_finite_number(probe_queue.get(name))
        ]
        if missing:
            errors.append(
                f"{label}: probe queue ledger has missing or non-finite fields: "
                + ", ".join(missing)
            )
        elif any(
            float(probe_queue[name]) < 0
            or not float(probe_queue[name]).is_integer()
            for name in PROBE_QUEUE_FIELDS
        ):
            errors.append(
                f"{label}: probe queue ledger contains a negative or non-integer metric"
            )
        else:
            if int(probe_queue["contexts"]) <= 0:
                errors.append(f"{label}: probe queue ledger has no contexts")
            if int(probe_queue["draws_per_command_buffer"]) not in (64, 128, 256):
                errors.append(
                    f"{label}: probe queue draw batch must be 64, 128, or 256"
                )
            if int(probe_queue["committed_command_buffer_cap"]) != 4:
                errors.append(
                    f"{label}: probe queue committed command-buffer cap must be exactly 4"
                )
            for name in PROBE_QUEUE_FLAG_FIELDS:
                if int(probe_queue[name]) not in (0, 1):
                    errors.append(f"{label}: probe queue {name} must be 0 or 1")
    exact_om_count = window.get("_exact_om_count", 0)
    if exact_om_count > 1:
        errors.append(
            f"{label}: exact output-merger ledger is duplicated {exact_om_count} times"
        )
    exact_om = window.get("exact_output_merger")
    if exact_om is not None:
        missing = [
            name
            for name in EXACT_OM_FIELDS
            if not is_finite_number(exact_om.get(name))
        ]
        if missing:
            errors.append(
                f"{label}: exact output-merger ledger has missing or non-finite fields: "
                + ", ".join(missing)
            )
        elif any(
            float(exact_om[name]) < 0
            or not float(exact_om[name]).is_integer()
            for name in EXACT_OM_FIELDS
        ):
            errors.append(
                f"{label}: exact output-merger ledger contains a negative or non-integer metric"
            )
        else:
            for name in EXACT_OM_FLAG_FIELDS:
                if int(exact_om[name]) not in (0, 1):
                    errors.append(f"{label}: exact output-merger {name} must be 0 or 1")
            if int(exact_om["enabled"]) == 1:
                outcomes = sum(
                    int(exact_om[name])
                    for name in (
                        "enqueued",
                        "completed",
                        "rejected",
                        "terminal_failures",
                    )
                )
                if int(exact_om["attempts"]) != outcomes:
                    errors.append(
                        f"{label}: exact output-merger attempts must equal "
                        "enqueued + completed + rejected + terminal_failures"
                    )
                if int(exact_om["draws_per_command_buffer"]) != 128:
                    errors.append(
                        f"{label}: exact output-merger draw batch must be exactly 128"
                    )
                if int(exact_om["command_buffer_cap"]) != 4:
                    errors.append(
                        f"{label}: exact output-merger command-buffer cap must be exactly 4"
                    )
            if int(exact_om["pending"]) > int(exact_om["lifetime_peak_pending"]):
                errors.append(
                    f"{label}: exact output-merger pending exceeds its lifetime peak"
                )
            if int(exact_om["lifetime_peak_committed"]) > int(
                exact_om["command_buffer_cap"]
            ) and int(exact_om["command_buffer_cap"]) > 0:
                errors.append(
                    f"{label}: exact output-merger committed lifetime peak exceeds its cap"
                )
            for count_name, bytes_name in (
                ("full_uploads", "full_upload_bytes"),
                ("full_downloads", "full_download_bytes"),
            ):
                if int(exact_om[count_name]) == 0 and int(exact_om[bytes_name]) != 0:
                    errors.append(
                        f"{label}: exact output-merger {bytes_name} is nonzero without "
                        f"a matching {count_name} operation"
                    )
    return errors


def window_is_structurally_closed(window: dict[str, Any]) -> bool:
    """Return whether every required ledger marker for a window has arrived."""
    stage_counts = window.get("_stage_counts", {})
    return (
        all(stage_counts.get(stage, 0) >= 1 for stage in STAGES + REQUIRED_ZERO_STAGES)
        and window.get("_presenter_summary_count", 0) >= 1
        and window.get("gpu_chain") is not None
    )


def parse_log(
    path: Path, *, defer_open_window: bool = False
) -> tuple[list[dict[str, Any]], list[str], dict[str, int]]:
    windows: dict[tuple[int, int], dict[str, Any]] = {}
    ordered: list[tuple[int, int]] = []
    violations: list[str] = []
    counts: dict[str, int] = defaultdict(int)
    presenter_summaries: dict[tuple[int, int], list[dict[str, Any]]] = defaultdict(list)

    with path.open("r", encoding="utf-8", errors="replace") as log:
        lines = log.readlines()
    # A polling reader may observe the writer in the middle of one physical
    # line. That byte fragment is not evidence and will be parsed on the next
    # poll once its newline has arrived.
    if defer_open_window and lines and not lines[-1].endswith(("\n", "\r")):
        lines.pop()
    for line_number, line in enumerate(lines, 1):
            # Fallback counters are emitted by the resolver outside the profile ledger.
            values = fields(line)
            fallback = values.get("fallbacks")
            if isinstance(fallback, (int, float)) and fallback > 0:
                violations.append(f"line {line_number}: fallbacks={fallback}")
                counts["fallbacks"] += int(fallback)
            for field_name in ("depth_fallbacks", "host_fallback", "ps_fallback"):
                fallback_value = values.get(field_name)
                if isinstance(fallback_value, (int, float)) and fallback_value > 0:
                    violations.append(
                        f"line {line_number}: {field_name}={fallback_value}"
                    )
                    counts[field_name] += int(fallback_value)

            if PROFILE not in line:
                if re.search(r"drawable[^\n]*(?:nil|fail(?:ed|ure)?)", line, re.I):
                    violations.append(f"line {line_number}: drawable failure: {line.strip()}")
                    counts["drawable_failures"] += 1
                if re.search(
                    r"(?:direct guest output copy did not complete|"
                    r"GPU tiled resolve failed|shared-memory upload failed)",
                    line,
                    re.I,
                ):
                    violations.append(f"line {line_number}: Metal execution failure: {line.strip()}")
                    counts["metal_execution_failures"] += 1
                continue

            key = swap_range(line)
            if " window " in f" {line} ":
                if not key:
                    violations.append(f"line {line_number}: malformed profile window range")
                    counts["malformed_profile_lines"] += 1
                    continue
                if key not in windows:
                    windows[key] = empty_window(*key, values, line_number)
                    ordered.append(key)
                else:
                    windows[key]["_header_count"] += 1
                continue

            if " command " in f" {line} ":
                if not key:
                    violations.append(f"line {line_number}: malformed command ledger range")
                    counts["malformed_profile_lines"] += 1
                    continue
                window = windows.get(key)
                if window:
                    event = str(values.get("event", "unknown"))
                    window["stages"][event] = values
                    window["_stage_counts"][event] += 1
                    if event == "texture_fallback_decode":
                        calls = values.get("calls", 0)
                        if isinstance(calls, (int, float)) and calls > 0:
                            violations.append(
                                f"line {line_number}: texture fallback decode calls={calls}"
                            )
                            counts["texture_fallback_decodes"] += int(calls)
                elif str(values.get("event", "unknown")) in EXPORTED_STAGES:
                    violations.append(
                        f"line {line_number}: command ledger has no matching window: "
                        f"swaps {key[0]}-{key[1]}"
                    )
                    counts["orphan_command_ledgers"] += 1
                continue

            if " gpu-chain " in f" {line} ":
                if not key:
                    violations.append(f"line {line_number}: malformed GPU chain ledger range")
                    counts["malformed_profile_lines"] += 1
                    continue
                window = windows.get(key)
                if not window:
                    violations.append(
                        f"line {line_number}: GPU chain ledger has no matching window: "
                        f"swaps {key[0]}-{key[1]}"
                    )
                    counts["orphan_gpu_chain_ledgers"] += 1
                    continue
                if window["gpu_chain"] is not None:
                    violations.append(
                        f"line {line_number}: duplicate GPU chain ledger: "
                        f"swaps {key[0]}-{key[1]}"
                    )
                    counts["duplicate_gpu_chain_ledgers"] += 1
                    continue
                ledger = {name: value for name, value in values.items() if name != "swaps"}
                invalid = [
                    name
                    for name, value in ledger.items()
                    if not is_finite_number(value) or value < 0
                ]
                if invalid:
                    violations.append(
                        f"line {line_number}: GPU chain ledger has invalid fields: "
                        + ", ".join(invalid)
                    )
                    counts["invalid_gpu_chain_ledgers"] += 1
                window["gpu_chain"] = ledger
                continue

            if " probe-queue " in f" {line} ":
                if not key:
                    violations.append(
                        f"line {line_number}: malformed probe queue ledger range"
                    )
                    counts["malformed_profile_lines"] += 1
                    continue
                window = windows.get(key)
                if not window:
                    violations.append(
                        f"line {line_number}: probe queue ledger has no matching window: "
                        f"swaps {key[0]}-{key[1]}"
                    )
                    counts["orphan_probe_queue_ledgers"] += 1
                    continue
                window["_probe_queue_count"] += 1
                if window["_probe_queue_count"] > 1:
                    violations.append(
                        f"line {line_number}: duplicate probe queue ledger: "
                        f"swaps {key[0]}-{key[1]}"
                    )
                    counts["duplicate_probe_queue_ledgers"] += 1
                    continue
                window["probe_queue"] = {
                    name: values.get(name) for name in PROBE_QUEUE_FIELDS
                }
                continue

            if " exact-om " in f" {line} ":
                if not key:
                    violations.append(
                        f"line {line_number}: malformed exact output-merger ledger range"
                    )
                    counts["malformed_profile_lines"] += 1
                    continue
                window = windows.get(key)
                if not window:
                    violations.append(
                        f"line {line_number}: exact output-merger ledger has no matching window: "
                        f"swaps {key[0]}-{key[1]}"
                    )
                    counts["orphan_exact_output_merger_ledgers"] += 1
                    continue
                window["_exact_om_count"] += 1
                if window["_exact_om_count"] > 1:
                    violations.append(
                        f"line {line_number}: duplicate exact output-merger ledger: "
                        f"swaps {key[0]}-{key[1]}"
                    )
                    counts["duplicate_exact_output_merger_ledgers"] += 1
                    continue
                window["exact_output_merger"] = {
                    name: values.get(name) for name in EXACT_OM_FIELDS
                }
                continue

            if " resolve-mode " in f" {line} ":
                if not key:
                    violations.append(
                        f"line {line_number}: malformed resolve-mode ledger range"
                    )
                    counts["malformed_profile_lines"] += 1
                    continue
                window = windows.get(key)
                if not window:
                    violations.append(
                        f"line {line_number}: resolve-mode ledger has no matching window: "
                        f"swaps {key[0]}-{key[1]}"
                    )
                    counts["orphan_resolve_mode_ledgers"] += 1
                    continue
                window["_resolve_mode_count"] += 1
                if window["_resolve_mode_count"] > 1:
                    violations.append(
                        f"line {line_number}: duplicate resolve-mode ledger: "
                        f"swaps {key[0]}-{key[1]}"
                    )
                    counts["duplicate_resolve_mode_ledgers"] += 1
                    continue
                ledger = {name: values.get(name) for name in RESOLVE_MODE_FIELDS}
                invalid = [
                    name
                    for name, value in ledger.items()
                    if not is_finite_number(value) or value < 0
                ]
                if invalid:
                    violations.append(
                        f"line {line_number}: resolve-mode ledger has invalid fields: "
                        + ", ".join(invalid)
                    )
                    counts["invalid_resolve_mode_ledgers"] += 1
                    continue
                window["resolve_mode"] = {
                    name: int(ledger[name]) for name in RESOLVE_MODE_FIELDS
                }
                continue

            if " wait swaps=" in line and key:
                window = windows.get(key)
                if window:
                    window["wait_reasons"][str(values.get("reason", "unknown"))] = values
                continue

            if " wait-reg-mem " in f" {line} " and key:
                window = windows.get(key)
                if window:
                    window["wait_reg_mem"].append(values)
                continue

            if " presenter " in f" {line} " and key:
                # Presenter attempts are independent of swaps. Keep them for evidence,
                # and apply their failure counters globally below.
                drawable_nil = values.get("drawable_nil", 0)
                if isinstance(drawable_nil, (int, float)) and drawable_nil > 0:
                    violations.append(f"line {line_number}: drawable_nil={drawable_nil}")
                    counts["drawable_nil"] += int(drawable_nil)
                uploads, upload_bytes = values.get("uploads", 0), values.get("upload_bytes", 0)
                if isinstance(uploads, (int, float)) and uploads > 0:
                    violations.append(f"line {line_number}: presenter full-frame uploads={uploads}")
                    counts["presenter_uploads"] += int(uploads)
                if isinstance(upload_bytes, (int, float)) and upload_bytes > 0:
                    violations.append(
                        f"line {line_number}: presenter full-frame upload_bytes={upload_bytes}"
                    )
                    counts["presenter_upload_bytes"] += int(upload_bytes)
                if "event" not in values:
                    presenter_summaries[key].append(values)
                continue

    result = [windows[key] for key in ordered]
    for index, window in enumerate(result):
        key = (int(window["swap_start"]), int(window["swap_end"]))
        window["presenter"] = presenter_summaries.get(key, [])
        window["_presenter_summary_count"] = len(window["presenter"])
        errors = validate_window(window)
        window["validation_errors"] = errors
        window["complete"] = not errors
        is_open_tail = (
            defer_open_window
            and index == len(result) - 1
            and not window_is_structurally_closed(window)
        )
        window["pending_ledgers"] = is_open_tail
        if errors and not is_open_tail:
            violations.extend(errors)
            counts["invalid_profile_windows"] += 1
        copy = window["stages"].get("copy", {})
        average = copy.get("avg_calls_per_swap", 0)
        window["dam_candidate"] = (
            window["complete"] and is_finite_number(average) and average >= 10
        )
        # Internal parser bookkeeping should not leak into JSON/CSV consumers.
        window.pop("_header_count", None)
        window.pop("_stage_counts", None)
        window.pop("_presenter_summary_count", None)
        window.pop("_probe_queue_count", None)
        window.pop("_exact_om_count", None)
        window.pop("_resolve_mode_count", None)
    return result, violations, dict(counts)


def stage_value(window: dict[str, Any], stage: str, name: str) -> Any:
    return window["stages"].get(stage, {}).get(name, "")


def percentile(values: list[float], quantile: float) -> float | None:
    """Return an interpolated percentile for a non-empty numeric sample."""
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * quantile
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    fraction = position - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def contiguous_dam_windows(windows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    # Lock on to the first complete Dam window, then require every warm-up and
    # measured window to be the immediately following 64-swap window. Filtering
    # all candidates first would silently remove a complete low-work/broken
    # window and replace it with a later healthy sample.
    contiguous_dam: list[dict[str, Any]] = []
    expected_start: int | None = None
    for window in windows:
        if not contiguous_dam:
            if not window["dam_candidate"]:
                continue
            contiguous_dam.append(window)
            expected_start = int(window["swap_end"]) + 1
            continue
        if not window["dam_candidate"] or int(window["swap_start"]) != expected_start:
            break
        contiguous_dam.append(window)
        expected_start = int(window["swap_end"]) + 1
    return contiguous_dam


def select_windows(windows: list[dict[str, Any]], warmup: int, measure: int) -> list[dict[str, Any]]:
    for window in windows:
        window["selected"] = False

    contiguous_dam = contiguous_dam_windows(windows)
    selected = contiguous_dam[warmup : warmup + measure]
    for window in selected:
        window["selected"] = True
    return selected


def contiguous_dam_window_count(windows: list[dict[str, Any]]) -> int:
    """Return the first uninterrupted Dam run used by ``select_windows``."""
    return len(contiguous_dam_windows(windows))


def wait_reg_mem_violations(windows: list[dict[str, Any]]) -> list[str]:
    problems: list[str] = []
    for window in windows:
        for entry in window["wait_reg_mem"]:
            unmatched, timeouts = entry.get("unmatched", 0), entry.get("timeouts", 0)
            if isinstance(unmatched, (int, float)) and unmatched > 0:
                problems.append(f"swaps {window['swap_start']}-{window['swap_end']}: WAIT_REG_MEM unmatched={unmatched}")
            if isinstance(timeouts, (int, float)) and timeouts > 0:
                problems.append(f"swaps {window['swap_start']}-{window['swap_end']}: WAIT_REG_MEM timeouts={timeouts}")
    return problems


def finite_values(values: Sequence[Any]) -> list[float]:
    return [float(value) for value in values if is_finite_number(value)]


def wait_reg_mem_site_key(entry: dict[str, Any]) -> tuple[Any, ...]:
    return tuple(
        entry.get(name)
        for name in ("source", "address", "reference", "mask", "operation", "wait")
    )


def aggregate_wait_reg_mem(
    selected: list[dict[str, Any]], elapsed_ns: float
) -> dict[str, Any]:
    """Aggregate ranked WAIT_REG_MEM evidence without implying it is exhaustive."""
    grouped: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
    entries = [entry for window in selected for entry in window.get("wait_reg_mem", [])]
    for entry in entries:
        grouped[wait_reg_mem_site_key(entry)].append(entry)

    def summed(group: Sequence[dict[str, Any]], name: str) -> float:
        return sum(finite_values([entry.get(name) for entry in group]))

    command_calls = sum(
        finite_values(
            [window.get("stages", {}).get("wait_reg_mem", {}).get("calls") for window in selected]
        )
    )
    command_ns = sum(
        finite_values(
            [
                window.get("stages", {}).get("wait_reg_mem", {}).get("total_ns")
                for window in selected
            ]
        )
    )
    sites: list[dict[str, Any]] = []
    for key, group in grouped.items():
        calls = summed(group, "calls")
        polls = summed(group, "polls")
        total_ns = summed(group, "total_ns")
        sites.append(
            {
                "source": key[0],
                "address": key[1],
                "reference": key[2],
                "mask": key[3],
                "operation": key[4],
                "wait": key[5],
                "windows_reported": len(group),
                "calls": int(calls),
                "polls": int(polls),
                "average_polls_per_call": polls / calls if calls > 0 else None,
                "total_ns": int(total_ns),
                "average_ns_per_call": total_ns / calls if calls > 0 else None,
                "selected_elapsed_percent": total_ns / elapsed_ns * 100.0
                if elapsed_ns > 0
                else None,
                "max_polls": max(finite_values([entry.get("max_polls") for entry in group]), default=0),
                "max_ns": max(finite_values([entry.get("max_ns") for entry in group]), default=0),
                "unmatched": int(summed(group, "unmatched")),
                "timeouts": int(summed(group, "timeouts")),
            }
        )
    sites.sort(key=lambda site: (-site["total_ns"], -site["calls"], str(site["address"])))
    captured_calls = sum(site["calls"] for site in sites)
    captured_ns = sum(site["total_ns"] for site in sites)
    return {
        "command_calls": int(command_calls),
        "command_total_ns": int(command_ns),
        "ranked_entries": len(entries),
        "unique_sites": len(sites),
        "captured_ranked_calls": captured_calls,
        "captured_ranked_total_ns": captured_ns,
        "captured_call_coverage_percent": captured_calls / command_calls * 100.0
        if command_calls > 0
        else None,
        "captured_time_coverage_percent": captured_ns / command_ns * 100.0
        if command_ns > 0
        else None,
        "unmatched": sum(site["unmatched"] for site in sites),
        "timeouts": sum(site["timeouts"] for site in sites),
        "sites": sites,
    }


def aggregate(selected: list[dict[str, Any]]) -> dict[str, Any]:
    output: dict[str, Any] = {
        "windows": len(selected),
        "metric_samples": {},
        "stages": {},
        "sampled_stages": {},
        "wait_reasons": {},
        "gpu_chain": None,
        "probe_queue": None,
        "exact_output_merger": None,
        "resolve_mode": None,
    }
    fps = finite_values([window.get("fps") for window in selected])
    output["metric_samples"]["fps"] = len(fps)
    output["real_window_fps"] = (
        {
            "mean": statistics.fmean(fps),
            "min": min(fps),
            "max": max(fps),
            "stddev": statistics.pstdev(fps),
            "coefficient_of_variation_percent": (
                statistics.pstdev(fps) / statistics.fmean(fps) * 100.0
                if statistics.fmean(fps) > 0
                else None
            ),
        }
        if fps
        else None
    )

    frame_ns = [value for value in finite_values([w.get("avg_frame_ns") for w in selected]) if value > 0]
    elapsed_samples = finite_values([window.get("elapsed_ns") for window in selected])
    elapsed_ns = sum(elapsed_samples)
    output["metric_samples"]["avg_frame_ns"] = len(frame_ns)
    output["metric_samples"]["elapsed_ns"] = len(elapsed_samples)
    sampled_frames = sum(int(w.get("swap_count", 0)) for w in selected)
    if frame_ns:
        p50_ns = percentile(frame_ns, 0.50)
        p95_ns = percentile(frame_ns, 0.95)
        p99_ns = percentile(frame_ns, 0.99)
        median_ns = p50_ns or 0.0
        output["frame_pacing"] = {
            # These are 64-frame-window statistics, not individual-frame
            # percentiles. The explicit names prevent benchmark reports from
            # overstating their precision.
            "sampled_frames": sampled_frames,
            "window_samples": len(frame_ns),
            "weighted_mean_frame_ms": (
                elapsed_ns / sampled_frames / 1_000_000.0 if sampled_frames else None
            ),
            "window_p50_frame_ms": p50_ns / 1_000_000.0 if p50_ns is not None else None,
            "window_p95_frame_ms": p95_ns / 1_000_000.0 if p95_ns is not None else None,
            "window_p99_frame_ms": p99_ns / 1_000_000.0 if p99_ns is not None else None,
            "window_worst_frame_ms": max(frame_ns) / 1_000_000.0,
            "window_frame_ms_stddev": statistics.pstdev(frame_ns) / 1_000_000.0,
            "window_one_percent_low_fps": 1_000_000_000.0 / p99_ns
            if p99_ns and p99_ns > 0
            else None,
            "windows_over_10_percent_slower_than_median": sum(
                value > median_ns * 1.10 for value in frame_ns
            ),
        }
    else:
        output["frame_pacing"] = None

    for stage in STAGES + REQUIRED_ZERO_STAGES:
        metrics = [w["stages"].get(stage, {}) for w in selected]
        calls = finite_values([metric.get("calls") for metric in metrics])
        total_ns = finite_values([metric.get("total_ns") for metric in metrics])
        avg_per_swap = finite_values([metric.get("avg_ns_per_swap") for metric in metrics])
        output["stages"][stage] = {
            "total_calls": sum(calls),
            "total_ns": sum(total_ns),
            "mean_avg_ns_per_swap": sum(avg_per_swap) / len(avg_per_swap) if avg_per_swap else None,
            "max_call_ns": max(finite_values([m.get("max_call_ns") for m in metrics]), default=0),
            "selected_elapsed_percent": (
                sum(total_ns) / elapsed_ns * 100.0 if elapsed_ns > 0 else None
            ),
        }

    for stage in SAMPLED_STAGES:
        metrics = [w["stages"].get(stage, {}) for w in selected]
        calls = finite_values([metric.get("calls") for metric in metrics])
        total_ns = finite_values([metric.get("total_ns") for metric in metrics])
        total_calls = sum(calls)
        if not total_calls:
            continue
        sampled_total_ns = sum(total_ns)
        window_mean_ns = [
            float(metric["total_ns"]) / float(metric["calls"])
            for metric in metrics
            if is_finite_number(metric.get("total_ns"))
            and is_finite_number(metric.get("calls"))
            and float(metric["calls"]) > 0
        ]
        output["sampled_stages"][stage] = {
            "total_samples": total_calls,
            "total_ns": sampled_total_ns,
            "mean_ns_per_sample": sampled_total_ns / total_calls,
            "median_window_mean_ns_per_sample": percentile(window_mean_ns, 0.5),
            "p95_window_mean_ns_per_sample": percentile(window_mean_ns, 0.95),
            "max_sample_ns": max(
                finite_values([metric.get("max_call_ns") for metric in metrics]),
                default=0,
            ),
        }

    wait_names = sorted(
        {name for window in selected for name in window.get("wait_reasons", {}).keys()}
    )
    for name in wait_names:
        metrics = [
            window["wait_reasons"][name]
            for window in selected
            if name in window.get("wait_reasons", {})
        ]
        numeric_sum = lambda key: sum(finite_values([metric.get(key) for metric in metrics]))
        total_wait_ns = numeric_sum("total_ns")
        output["wait_reasons"][name] = {
            "calls": int(numeric_sum("calls")),
            "waited_calls": int(numeric_sum("waited_calls")),
            "waited_submissions": int(numeric_sum("waited_submissions")),
            "total_ns": int(total_wait_ns),
            "selected_elapsed_percent": (
                total_wait_ns / elapsed_ns * 100.0 if elapsed_ns > 0 else None
            ),
            "max_call_ns": max(
                finite_values([metric.get("max_call_ns") for metric in metrics]), default=0
            ),
        }

    gpu_chain_ledgers = [
        window["gpu_chain"] for window in selected if window.get("gpu_chain") is not None
    ]
    if gpu_chain_ledgers:
        names = sorted({name for ledger in gpu_chain_ledgers for name in ledger})
        gpu_chain: dict[str, Any] = {}
        for name in names:
            values = finite_values([ledger.get(name) for ledger in gpu_chain_ledgers])
            if not values:
                continue
            if name in GPU_CHAIN_MAX_FIELDS:
                gpu_chain[name] = max(values)
            elif name in GPU_CHAIN_LIVE_FIELDS:
                gpu_chain[name] = values[-1]
            else:
                gpu_chain[name] = sum(values)
        worker_samples = gpu_chain.get("wptr_to_worker_samples", 0)
        interrupt_samples = gpu_chain.get("interrupt_to_wptr_samples", 0)
        metal_samples = gpu_chain.get("metal_pending_samples", 0)
        throttle_calls = gpu_chain.get("throttle_calls", 0)
        gpu_chain["wptr_to_worker_mean_ns"] = (
            gpu_chain.get("wptr_to_worker_total_ns", 0) / worker_samples
            if worker_samples
            else None
        )
        gpu_chain["interrupt_to_wptr_mean_ns"] = (
            gpu_chain.get("interrupt_to_wptr_total_ns", 0) / interrupt_samples
            if interrupt_samples
            else None
        )
        gpu_chain["metal_pending_mean"] = (
            gpu_chain.get("metal_pending_total", 0) / metal_samples
            if metal_samples
            else None
        )
        gpu_chain["throttle_actual_mean_ns"] = (
            gpu_chain.get("throttle_actual_ns", 0) / throttle_calls
            if throttle_calls
            else None
        )
        output["gpu_chain"] = gpu_chain
    probe_queue_ledgers = [
        window["probe_queue"]
        for window in selected
        if window.get("probe_queue") is not None
    ]
    if probe_queue_ledgers:
        probe_queue: dict[str, Any] = {
            "windows_reported": len(probe_queue_ledgers),
        }
        for name in PROBE_QUEUE_SUM_FIELDS:
            probe_queue[name] = int(
                sum(finite_values([ledger.get(name) for ledger in probe_queue_ledgers]))
            )
        for name in PROBE_QUEUE_MAX_FIELDS:
            probe_queue[name] = int(
                max(
                    finite_values([ledger.get(name) for ledger in probe_queue_ledgers]),
                    default=0,
                )
            )
        for name in PROBE_QUEUE_CONFIG_FIELDS:
            values = sorted(
                {
                    int(value)
                    for value in finite_values(
                        [ledger.get(name) for ledger in probe_queue_ledgers]
                    )
                }
            )
            probe_queue[name] = values[0] if len(values) == 1 else None
            probe_queue[f"{name}_values"] = values
        probe_queue["counter_reset_windows"] = int(
            sum(
                finite_values(
                    [ledger.get("counter_reset") for ledger in probe_queue_ledgers]
                )
            )
        )
        probe_queue["configuration_mismatch_windows"] = int(
            sum(
                finite_values(
                    [
                        ledger.get("configuration_mismatch")
                        for ledger in probe_queue_ledgers
                    ]
                )
            )
        )
        blocking_waits = probe_queue["blocking_waits"]
        probe_queue["blocking_wait_mean_ns"] = (
            probe_queue["blocking_wait_ns"] / blocking_waits
            if blocking_waits
            else None
        )
        output["probe_queue"] = probe_queue
    exact_om_ledgers = [
        window["exact_output_merger"]
        for window in selected
        if window.get("exact_output_merger") is not None
    ]
    if exact_om_ledgers:
        exact_om: dict[str, Any] = {
            "windows_reported": len(exact_om_ledgers),
            "enabled_windows": sum(
                int(ledger["enabled"]) for ledger in exact_om_ledgers
            ),
        }
        exact_om["disabled_windows"] = (
            exact_om["windows_reported"] - exact_om["enabled_windows"]
        )
        for name in EXACT_OM_SUM_FIELDS:
            exact_om[name] = int(
                sum(finite_values([ledger.get(name) for ledger in exact_om_ledgers]))
            )
        pending_values = finite_values(
            [ledger.get("pending") for ledger in exact_om_ledgers]
        )
        exact_om["pending"] = int(pending_values[-1]) if pending_values else None
        exact_om["pending_max"] = (
            int(max(pending_values)) if pending_values else None
        )
        for name in EXACT_OM_MAX_FIELDS:
            exact_om[name] = int(
                max(
                    finite_values([ledger.get(name) for ledger in exact_om_ledgers]),
                    default=0,
                )
            )
        enabled_ledgers = [
            ledger for ledger in exact_om_ledgers if int(ledger["enabled"]) == 1
        ]
        configuration_ledgers = enabled_ledgers or exact_om_ledgers
        for name in EXACT_OM_CONFIG_FIELDS:
            values = sorted(
                {
                    int(value)
                    for value in finite_values(
                        [ledger.get(name) for ledger in configuration_ledgers]
                    )
                }
            )
            exact_om[name] = values[0] if len(values) == 1 else None
            exact_om[f"{name}_values"] = values
        exact_om["counter_reset_windows"] = int(
            sum(
                finite_values(
                    [ledger.get("counter_reset") for ledger in exact_om_ledgers]
                )
            )
        )
        blocking_waits = exact_om["blocking_waits"]
        exact_om["blocking_wait_mean_ns"] = (
            exact_om["blocking_wait_ns"] / blocking_waits
            if blocking_waits
            else None
        )
        output["exact_output_merger"] = exact_om
    resolve_modes = [
        window["resolve_mode"]
        for window in selected
        if window.get("resolve_mode") is not None
    ]
    if resolve_modes:
        resolve_mode = {
            name: int(sum(mode[name] for mode in resolve_modes))
            for name in RESOLVE_MODE_FIELDS
        }
        resolve_mode["windows_reported"] = len(resolve_modes)
        total = resolve_mode["async"] + resolve_mode["sync"]
        resolve_mode["async_percent"] = (
            resolve_mode["async"] / total * 100.0 if total else None
        )
        output["resolve_mode"] = resolve_mode
    output["wait_reg_mem"] = aggregate_wait_reg_mem(selected, elapsed_ns)
    return output


def probe_queue_comparison_violations(
    aggregate_data: Mapping[str, Any],
) -> list[str]:
    """Reject incomplete or reset queue telemetry before an A/B comparison."""
    expected = int(aggregate_data.get("windows", 0))
    queue = aggregate_data.get("probe_queue")
    if not isinstance(queue, Mapping):
        return ["probe queue telemetry is missing"]
    problems: list[str] = []
    if int(queue.get("windows_reported", 0)) != expected:
        problems.append(
            "probe queue telemetry has incomplete windows: "
            f"expected {expected}, got {queue.get('windows_reported', 0)}"
        )
    if int(queue.get("counter_reset_windows", 0)):
        problems.append(
            "probe queue cumulative counters reset or the context cohort changed "
            f"in {queue['counter_reset_windows']} selected window(s)"
        )
    if int(queue.get("configuration_mismatch_windows", 0)):
        problems.append(
            "probe queue configuration differed between contexts in "
            f"{queue['configuration_mismatch_windows']} selected window(s)"
        )
    if queue.get("draws_per_command_buffer") not in (64, 128, 256):
        problems.append("probe queue draw-batch configuration is missing or invalid")
    cap = queue.get("committed_command_buffer_cap")
    if not is_finite_number(cap) or int(cap) != 4:
        problems.append("probe queue command-buffer cap must be exactly 4")
    return problems


def performance_violations(
    aggregate_data: dict[str, Any],
    min_mean_fps: float | None,
    max_window_p99_ms: float | None,
    max_window_fps_cv_percent: float | None,
) -> list[str]:
    problems: list[str] = []
    fps = aggregate_data.get("real_window_fps")
    pacing = aggregate_data.get("frame_pacing")
    expected = int(aggregate_data.get("windows", 0))
    samples = aggregate_data.get("metric_samples", {})
    if min_mean_fps is not None:
        if samples.get("fps") != expected:
            problems.append(
                f"mean FPS has incomplete samples: expected {expected}, got {samples.get('fps', 0)}"
            )
        elif not fps or not is_finite_number(fps.get("mean")):
            problems.append("mean FPS is unavailable")
        elif fps["mean"] < min_mean_fps:
            problems.append(f"mean FPS {fps['mean']:.3f} is below required {min_mean_fps:.3f}")
    if max_window_p99_ms is not None:
        if samples.get("avg_frame_ns") != expected or samples.get("elapsed_ns") != expected:
            problems.append(
                "64-frame-window p99 frame time has incomplete samples: "
                f"expected {expected}, got frame={samples.get('avg_frame_ns', 0)} "
                f"elapsed={samples.get('elapsed_ns', 0)}"
            )
        elif not pacing or not is_finite_number(pacing.get("window_p99_frame_ms")):
            problems.append("64-frame-window p99 frame time is unavailable")
        elif pacing["window_p99_frame_ms"] > max_window_p99_ms:
            problems.append(
                "64-frame-window p99 frame time "
                f"{pacing['window_p99_frame_ms']:.3f} ms exceeds {max_window_p99_ms:.3f} ms"
            )
    if max_window_fps_cv_percent is not None:
        if samples.get("fps") != expected:
            problems.append(
                "window FPS coefficient of variation has incomplete samples: "
                f"expected {expected}, got {samples.get('fps', 0)}"
            )
        elif not fps or not is_finite_number(fps.get("coefficient_of_variation_percent")):
            problems.append("window FPS coefficient of variation is unavailable")
        elif fps["coefficient_of_variation_percent"] > max_window_fps_cv_percent:
            problems.append(
                "window FPS coefficient of variation "
                f"{fps['coefficient_of_variation_percent']:.3f}% exceeds "
                f"{max_window_fps_cv_percent:.3f}%"
            )
    return problems


def format_metric(value: Any, digits: int = 3, suffix: str = "") -> str:
    """Format report metrics without throwing on zero, missing, or non-finite data."""
    if not is_finite_number(value):
        return "n/a"
    return f"{float(value):.{digits}f}{suffix}"


def write_outputs(
    output: Path,
    windows: list[dict[str, Any]],
    selected: list[dict[str, Any]],
    violations: list[str],
    counts: dict[str, int],
    warmup: int,
    measure: int,
    min_mean_fps: float | None = None,
    max_window_p99_ms: float | None = None,
    max_window_fps_cv_percent: float | None = None,
    external_failures: Sequence[str] = (),
) -> bool:
    output.mkdir(parents=True, exist_ok=True)
    enough = len(selected) == measure
    # A timeout or unmatched compare indicates an execution-path failure, even
    # if it happened while the selected Dam samples were warming up.
    aggregate_data = aggregate(selected)
    issues = list(violations) + wait_reg_mem_violations(windows) + list(external_failures)
    if not enough:
        issues.append(
            "insufficient contiguous Dam windows: "
            f"need {warmup + measure}, found {contiguous_dam_window_count(windows)} "
            f"({sum(w['dam_candidate'] for w in windows)} candidates total)"
        )
    else:
        issues.extend(
            performance_violations(
                aggregate_data,
                min_mean_fps,
                max_window_p99_ms,
                max_window_fps_cv_percent,
            )
        )
    summary = {
        "status": "pass" if not issues else "fail",
        "requirements": {
            "dam_copy_avg_calls_per_swap_at_least": 10,
            "presenter_sources_and_commits_positive": True,
            "gpu_chain_required_and_rejects_zero": True,
            "texture_fallback_decode_calls": 0,
            "discard_dam_windows": warmup,
            "measure_dam_windows": measure,
            "min_mean_fps": min_mean_fps,
            "max_64_frame_window_p99_ms": max_window_p99_ms,
            "max_window_fps_coefficient_of_variation_percent": max_window_fps_cv_percent,
        },
        "observed": {
            "profile_windows": len(windows),
            "dam_windows": sum(w["dam_candidate"] for w in windows),
            "contiguous_dam_windows": contiguous_dam_window_count(windows),
            "selected_windows": len(selected),
            "failure_counts": counts,
        },
        "aggregate": aggregate_data,
        "failures": issues,
    }
    with (output / "summary.json").open("w", encoding="utf-8") as handle:
        json.dump(summary, handle, indent=2, sort_keys=True)
        handle.write("\n")
    reported_stages = STAGES + REQUIRED_ZERO_STAGES + tuple(
        stage
        for stage in SAMPLED_STAGES
        if any(stage in window.get("stages", {}) for window in windows)
    )
    columns = ["swap_start", "swap_end", "dam_candidate", "selected", "fps", "elapsed_ns", "avg_frame_ns"]
    for stage in reported_stages:
        columns += [f"{stage}_{field}" for field in ("calls", "avg_calls_per_swap", "total_ns", "avg_ns_per_swap", "max_call_ns", "max_swap_ns")]
    columns += [f"probe_queue_{name}" for name in PROBE_QUEUE_FIELDS]
    columns += [f"exact_om_{name}" for name in EXACT_OM_FIELDS]
    columns += [f"resolve_{name}" for name in RESOLVE_MODE_FIELDS]
    with (output / "windows.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        for window in windows:
            row = {key: window.get(key, "") for key in columns}
            for stage in reported_stages:
                for field in ("calls", "avg_calls_per_swap", "total_ns", "avg_ns_per_swap", "max_call_ns", "max_swap_ns"):
                    row[f"{stage}_{field}"] = stage_value(window, stage, field)
            probe_queue = window.get("probe_queue") or {}
            for name in PROBE_QUEUE_FIELDS:
                row[f"probe_queue_{name}"] = probe_queue.get(name, "")
            exact_om = window.get("exact_output_merger") or {}
            for name in EXACT_OM_FIELDS:
                row[f"exact_om_{name}"] = exact_om.get(name, "")
            resolve_mode = window.get("resolve_mode") or {}
            for name in RESOLVE_MODE_FIELDS:
                row[f"resolve_{name}"] = resolve_mode.get(name, "")
            writer.writerow(row)
    with (output / "summary.txt").open("w", encoding="utf-8") as handle:
        handle.write(f"status: {summary['status']}\nDam windows: {summary['observed']['dam_windows']} (selected {len(selected)}/{measure})\n")
        if summary["aggregate"]["real_window_fps"]:
            fps = summary["aggregate"]["real_window_fps"]
            handle.write(
                f"real window FPS: mean={format_metric(fps.get('mean'))} "
                f"min={format_metric(fps.get('min'))} max={format_metric(fps.get('max'))} "
                f"stddev={format_metric(fps.get('stddev'))} "
                f"cv={format_metric(fps.get('coefficient_of_variation_percent'), suffix='%')}\n"
            )
        if summary["aggregate"]["frame_pacing"]:
            pacing = summary["aggregate"]["frame_pacing"]
            handle.write(
                "64-frame-window pacing: "
                f"p50={format_metric(pacing.get('window_p50_frame_ms'), suffix='ms')} "
                f"p95={format_metric(pacing.get('window_p95_frame_ms'), suffix='ms')} "
                f"p99={format_metric(pacing.get('window_p99_frame_ms'), suffix='ms')} "
                f"window-1%-low={format_metric(pacing.get('window_one_percent_low_fps'), suffix='fps')}\n"
            )
        if summary["aggregate"]["gpu_chain"]:
            chain = summary["aggregate"]["gpu_chain"]
            handle.write(
                "GPU chain: "
                f"WPTR={int(chain.get('wptr_accepted', 0))} "
                f"batches={int(chain.get('ring_batches', 0))} "
                f"coalesced={int(chain.get('wptr_coalesced', 0))} "
                f"concurrent={int(chain.get('ring_concurrent_wptr', 0))} "
                f"interrupt-pairs={int(chain.get('interrupt_to_wptr_samples', 0))} "
                f"mean-handoff={format_metric(chain.get('interrupt_to_wptr_mean_ns') / 1_000.0 if is_finite_number(chain.get('interrupt_to_wptr_mean_ns')) else None, suffix='us')} "
                f"mean-handoff-wait={format_metric(chain.get('throttle_actual_mean_ns') / 1_000.0 if is_finite_number(chain.get('throttle_actual_mean_ns')) else None, suffix='us')} "
                f"handoff-wakes={int(chain.get('throttle_wptr_wakes', 0))} "
                f"handoff-timeouts={int(chain.get('throttle_timeouts', 0))} "
                f"Metal-pending-max={int(chain.get('metal_pending_max', 0))}\n"
            )
        if summary["aggregate"]["probe_queue"]:
            queue = summary["aggregate"]["probe_queue"]
            blocking_wait_ns = queue.get("blocking_wait_ns")
            handle.write(
                "probe queue: "
                f"draw-batch={queue.get('draws_per_command_buffer', 'n/a')} "
                f"command-buffer-cap={queue.get('committed_command_buffer_cap', 'n/a')} "
                f"commits={int(queue.get('commits', 0))} "
                f"blocking-waits={int(queue.get('blocking_waits', 0))} "
                f"blocking-time={format_metric(float(blocking_wait_ns) / 1_000_000.0 if is_finite_number(blocking_wait_ns) else None, suffix='ms')} "
                f"WAIT_REG_MEM-signals={int(queue.get('wait_reg_mem_signals', 0))} "
                f"timeouts={int(queue.get('wait_reg_mem_timeouts', 0))} "
                f"resets={int(queue.get('counter_reset_windows', 0))}\n"
            )
        if summary["aggregate"]["exact_output_merger"]:
            exact_om = summary["aggregate"]["exact_output_merger"]
            blocking_wait_ns = exact_om.get("blocking_wait_ns")
            handle.write(
                "exact output merger: "
                f"enabled-windows={int(exact_om.get('enabled_windows', 0))} "
                f"attempts={int(exact_om.get('attempts', 0))} "
                f"enqueued={int(exact_om.get('enqueued', 0))} "
                f"completed={int(exact_om.get('completed', 0))} "
                f"rejected={int(exact_om.get('rejected', 0))} "
                f"terminal-failures={int(exact_om.get('terminal_failures', 0))} "
                f"materializations={int(exact_om.get('materializations', 0))} "
                f"pending={exact_om.get('pending', 'n/a')} "
                f"commits={int(exact_om.get('commits', 0))} "
                f"blocking-waits={int(exact_om.get('blocking_waits', 0))} "
                f"blocking-time={format_metric(float(blocking_wait_ns) / 1_000_000.0 if is_finite_number(blocking_wait_ns) else None, suffix='ms')} "
                f"draw-batch={exact_om.get('draws_per_command_buffer', 'n/a')} "
                f"command-buffer-cap={exact_om.get('command_buffer_cap', 'n/a')} "
                f"resets={int(exact_om.get('counter_reset_windows', 0))}\n"
            )
        if summary["aggregate"]["resolve_mode"]:
            resolve_mode = summary["aggregate"]["resolve_mode"]
            handle.write(
                "tiled resolve mode: "
                f"async={resolve_mode['async']} sync={resolve_mode['sync']} "
                f"async-share={format_metric(resolve_mode.get('async_percent'), digits=2, suffix='%')} "
                f"missing-snapshot={resolve_mode['missing_snapshot']} "
                f"verbose={resolve_mode['verbose']} "
                f"missing-alias={resolve_mode['missing_alias']} "
                f"exact-snapshot={resolve_mode['exact_snapshot']}\n"
            )
        for stage, values in summary["aggregate"]["sampled_stages"].items():
            median_window_ns = values.get("median_window_mean_ns_per_sample")
            median_window_ms = (
                float(median_window_ns) / 1_000_000.0
                if is_finite_number(median_window_ns)
                else None
            )
            handle.write(
                f"sample {stage}: count={int(values['total_samples'])} "
                f"mean={format_metric(values.get('mean_ns_per_sample') / 1_000_000.0, suffix='ms')} "
                f"median-window={format_metric(median_window_ms, suffix='ms')} "
                f"max={format_metric(values.get('max_sample_ns') / 1_000_000.0, suffix='ms')}\n"
            )
        for reason, values in summary["aggregate"]["wait_reasons"].items():
            total_ns = values.get("total_ns")
            total_ms = float(total_ns) / 1_000_000.0 if is_finite_number(total_ns) else None
            handle.write(
                f"wait {reason}: {format_metric(total_ms, suffix='ms')} "
                f"({format_metric(values.get('selected_elapsed_percent'), digits=2, suffix='%')} "
                "selected elapsed)\n"
            )
        wait_reg_mem = summary["aggregate"]["wait_reg_mem"]
        handle.write(
            "WAIT_REG_MEM: "
            f"command_calls={wait_reg_mem['command_calls']} "
            f"command_time={format_metric(wait_reg_mem['command_total_ns'] / 1_000_000.0, suffix='ms')} "
            f"ranked_call_coverage={format_metric(wait_reg_mem.get('captured_call_coverage_percent'), digits=2, suffix='%')} "
            f"unmatched={wait_reg_mem['unmatched']} timeouts={wait_reg_mem['timeouts']}\n"
        )
        for site in wait_reg_mem["sites"][:3]:
            address = site.get("address")
            address_text = f"0x{address:08x}" if isinstance(address, int) else str(address or "unknown")
            handle.write(
                f"WAIT_REG_MEM site {site.get('source', 'unknown')}:{address_text}: "
                f"calls={site['calls']} polls/call={format_metric(site.get('average_polls_per_call'))} "
                f"time={format_metric(site['total_ns'] / 1_000_000.0, suffix='ms')}\n"
            )
        for issue in issues:
            handle.write(f"FAIL: {issue}\n")
    return not issues


def command_text(argv: list[str]) -> str:
    result = subprocess.run(argv, text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False)
    return result.stdout.strip()


def tracked_build_sources(repo: Path) -> dict[str, list[Path]]:
    """Return worktree sources which can affect the runtime or app build.

    Both tracked and non-ignored untracked files are considered. This matters
    during development: a newly added header or release manifest can already be
    compiled into (or required by) the app before its first commit.
    """
    repo = repo.resolve()
    result = subprocess.run(
        [
            "git",
            "-C",
            str(repo),
            "ls-files",
            "-z",
            "--cached",
            "--others",
            "--exclude-standard",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"cannot enumerate tracked build sources: {detail or 'git failed'}")
    candidates = [
        Path(value.decode("utf-8", errors="surrogateescape"))
        for value in result.stdout.split(b"\0")
        if value
    ]
    runtime: list[Path] = []
    app: list[Path] = []
    runtime_root_configs = {"CMakeLists.txt", "CMakePresets.json"}
    app_root = Path("vendor/GoldenEye-Recomp")
    app_configs = {
        Path("CMakeLists.txt"),
        Path("CMakePresets.json"),
        Path("ge_config.toml"),
        Path("ge_manifest.toml"),
    }
    release_manifest = Path("config/goldeneye-release.json")
    root_app_build_inputs = {
        Path("launcher/build-app.sh"),
        Path("tools/sign-notarize.sh"),
    }

    def excluded(relative: Path) -> bool:
        lowered_parts = {part.lower() for part in relative.parts}
        stem = relative.stem.lower()
        return (
            bool(lowered_parts & {"tests", "test", "docs", "doc"})
            or stem.endswith("_test")
            or stem.startswith("test_")
            or stem == "test_support"
            or "test_pipeline" in stem
        )

    def thirdparty_build_input(relative: Path) -> bool:
        # Only version-controlled source/configuration inputs belong in the
        # freshness set. Generated build trees and third-party prose do not.
        thirdparty_relative = Path(*relative.parts[1:])
        lowered_parts = {part.lower() for part in thirdparty_relative.parts}
        if lowered_parts & {"build", "generated", "out", "docs", "doc", "tests", "test"}:
            return False
        if relative.name in {"CMakeLists.txt", "meson.build"}:
            return True
        return relative.suffix.lower() in {
            ".c",
            ".cc",
            ".cmake",
            ".cpp",
            ".cxx",
            ".def",
            ".h",
            ".hh",
            ".hpp",
            ".hxx",
            ".inc",
            ".inl",
            ".m",
            ".metal",
            ".mm",
            ".s",
        }

    for relative in candidates:
        parts = relative.parts
        if not parts or excluded(relative):
            continue
        if relative.as_posix() in runtime_root_configs or parts[0] in {"src", "include", "cmake"}:
            runtime.append(repo / relative)
            continue
        if parts[0] == "thirdparty" and thirdparty_build_input(relative):
            runtime.append(repo / relative)
            continue
        if relative == release_manifest:
            app.append(repo / relative)
            continue
        if relative in root_app_build_inputs:
            app.append(repo / relative)
            continue
        try:
            app_relative = relative.relative_to(app_root)
        except ValueError:
            continue
        app_parts = app_relative.parts
        if not app_parts:
            continue
        if (
            app_relative in app_configs
            or app_parts[0] in {"src", "generated", "packaging"}
            or (
                app_parts[0] == "assets"
                and relative.suffix.lower()
                in {".icns", ".json", ".metal", ".png", ".plist", ".xcassets"}
            )
        ):
            app.append(repo / relative)
    return {
        "runtime": sorted(set(runtime)),
        "goldeneye_app": sorted(set(app)),
    }


def build_freshness_report(repo: Path, dylib: Path, executable: Path) -> dict[str, Any]:
    """Compare build artifacts with every relevant tracked source by nanosecond mtime."""
    repo, dylib, executable = repo.resolve(), dylib.resolve(), executable.resolve()
    try:
        groups = tracked_build_sources(repo)
    except RuntimeError as error:
        return {"fresh": False, "artifacts": {}, "failures": [str(error)]}
    artifacts = {"runtime": dylib, "goldeneye_app": executable}
    results: dict[str, Any] = {}
    failures: list[str] = []
    for name, artifact in artifacts.items():
        sources = groups[name]
        missing_sources = [source for source in sources if not source.is_file()]
        stale_sources: list[Path] = []
        artifact_mtime_ns: int | None = None
        if artifact.is_file():
            artifact_mtime_ns = artifact.stat().st_mtime_ns
            stale_sources = [
                source
                for source in sources
                if source.is_file() and source.stat().st_mtime_ns > artifact_mtime_ns
            ]
        else:
            failures.append(f"{name} artifact is missing: {artifact}")
        if not sources:
            failures.append(f"no tracked {name} build sources were found under {repo}")
        for source in missing_sources:
            failures.append(f"tracked {name} build source is missing: {source.relative_to(repo)}")
        for source in stale_sources:
            failures.append(
                f"{name} artifact is older than {source.relative_to(repo)}; rebuild before testing"
            )
        newest = max(
            (source for source in sources if source.is_file()),
            key=lambda source: source.stat().st_mtime_ns,
            default=None,
        )
        results[name] = {
            "artifact": str(artifact),
            "artifact_mtime_ns": artifact_mtime_ns,
            "source_count": len(sources),
            # Retained for downstream readers written before untracked
            # build-critical inputs were included.
            "tracked_source_count": len(sources),
            "newest_source": str(newest.relative_to(repo)) if newest else None,
            "newest_source_mtime_ns": newest.stat().st_mtime_ns if newest else None,
            "stale_sources": [str(source.relative_to(repo)) for source in stale_sources],
            "missing_sources": [str(source.relative_to(repo)) for source in missing_sources],
            "fresh": bool(artifact_mtime_ns is not None and sources)
            and not stale_sources
            and not missing_sources,
        }
    return {"fresh": not failures, "artifacts": results, "failures": failures}


def sha256(path: Path) -> str | None:
    if not path.is_file():
        return None
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def filtered_environment(environment: Mapping[str, Any]) -> dict[str, str]:
    prefixes = ("REX_", "GOLDENEYE_", "DYLD_LIBRARY_PATH")
    return {
        str(key): str(value)
        for key, value in sorted(environment.items())
        if (str(key).startswith(prefixes) or str(key) in {"HOME", "TMPDIR"})
        and value is not None
    }


def write_metadata(
    args: argparse.Namespace, effective_environment: Mapping[str, Any] | None = None
) -> None:
    executable, dylib, xex, repo = Path(args.executable), Path(args.dylib), Path(args.xex), Path(args.repo)
    effective = effective_environment
    if effective is None:
        effective = getattr(args, "effective_environment", None)
    if effective is None:
        effective = os.environ
    recorded_environment = filtered_environment(effective)
    sysctls = {key: command_text(["sysctl", "-n", key]) for key in ("hw.model", "machdep.cpu.brand_string", "hw.physicalcpu", "hw.logicalcpu", "hw.nperflevels", "hw.perflevel0.physicalcpu", "hw.perflevel1.physicalcpu", "hw.memsize")}
    metadata = {
        "executable": str(executable), "dylib": str(dylib), "xex": str(xex), "game_data_root": args.data_root,
        "hashes_sha256": {"executable": sha256(executable), "dylib": sha256(dylib), "xex": sha256(xex)},
        "macos": command_text(["sw_vers"]), "hardware": {"model": sysctls["hw.model"], "chip": sysctls["machdep.cpu.brand_string"], "physical_cores": sysctls["hw.physicalcpu"], "logical_cores": sysctls["hw.logicalcpu"], "performance_cores": sysctls["hw.perflevel0.physicalcpu"], "efficiency_cores": sysctls["hw.perflevel1.physicalcpu"], "memory_bytes": sysctls["hw.memsize"], "raw_sysctl": sysctls},
        "power_source": command_text(["pmset", "-g", "batt"]) or command_text(["pmset", "-g", "ups"]),
        "git": {"commit": command_text(["git", "-C", str(repo), "rev-parse", "HEAD"]), "dirty": bool(command_text(["git", "-C", str(repo), "status", "--porcelain"]))},
        "environment": recorded_environment,
        "benchmark": {"cache_mode": recorded_environment.get("GOLDENEYE_BENCH_CACHE_MODE")},
    }
    destination = Path(args.metadata)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def load_environment_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read effective environment JSON: {error}") from error
    if not isinstance(value, dict):
        raise ValueError("effective environment JSON must contain an object")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", nargs="?", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--warmup", type=int, default=8)
    parser.add_argument("--measure", type=int, default=48)
    parser.add_argument(
        "--validate-options",
        action="store_true",
        help="validate benchmark sampling and threshold options without reading a log",
    )
    parser.add_argument("--harness-timeout-seconds", type=int)
    parser.add_argument("--harness-poll-seconds", type=finite_float)
    parser.add_argument("--min-mean-fps", type=finite_float)
    parser.add_argument("--max-window-p99-ms", type=finite_float)
    parser.add_argument("--max-window-fps-cv-percent", type=finite_float)
    parser.add_argument(
        "--external-failure",
        action="append",
        default=[],
        metavar="TEXT",
        help="record an external harness/process failure in the final result",
    )
    parser.add_argument("--ready", action="store_true", help="exit success once enough clean data exists")
    parser.add_argument("--metadata", type=Path)
    parser.add_argument(
        "--effective-environment-json",
        type=Path,
        help="JSON object containing the environment effective for the tested process",
    )
    parser.add_argument(
        "--check-freshness",
        action="store_true",
        help="verify tracked runtime and GoldenEye sources are not newer than build artifacts",
    )
    parser.add_argument("--executable")
    parser.add_argument("--dylib")
    parser.add_argument("--xex")
    parser.add_argument("--repo")
    parser.add_argument("--data-root")
    args = parser.parse_args()
    if args.warmup < 0:
        parser.error("--warmup must not be negative")
    if args.measure <= 0:
        parser.error("--measure must be positive")
    for name in ("min_mean_fps", "max_window_p99_ms", "max_window_fps_cv_percent"):
        value = getattr(args, name)
        if value is not None and value < 0:
            parser.error(f"--{name.replace('_', '-')} must not be negative")
    for name in ("harness_timeout_seconds", "harness_poll_seconds"):
        value = getattr(args, name)
        if value is not None and value <= 0:
            parser.error(f"--{name.replace('_', '-')} must be positive")
    if args.validate_options:
        if args.log or args.output or args.metadata or args.check_freshness:
            parser.error("--validate-options cannot be combined with an input or output mode")
        return 0
    if args.check_freshness:
        required = (args.executable, args.dylib, args.repo)
        if not all(required):
            parser.error("--check-freshness requires --executable, --dylib, and --repo")
        report = build_freshness_report(
            Path(args.repo), Path(args.dylib), Path(args.executable)
        )
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0 if report["fresh"] else 1
    if args.metadata:
        required = (args.executable, args.dylib, args.xex, args.repo, args.data_root)
        if not all(required):
            parser.error("--metadata requires --executable, --dylib, --xex, --repo, and --data-root")
        try:
            effective_environment = (
                load_environment_json(args.effective_environment_json)
                if args.effective_environment_json
                else None
            )
        except ValueError as error:
            parser.error(str(error))
        write_metadata(args, effective_environment=effective_environment)
        return 0
    if args.effective_environment_json:
        parser.error("--effective-environment-json requires --metadata")
    if not args.log:
        parser.error("log is required unless --metadata is used")
    windows, violations, counts = parse_log(args.log)
    selected = select_windows(windows, args.warmup, args.measure)
    aggregate_data = aggregate(selected)
    capture_issues = violations + wait_reg_mem_violations(windows) + list(args.external_failure)
    issues = list(capture_issues)
    if len(selected) == args.measure:
        issues += performance_violations(
            aggregate_data,
            args.min_mean_fps,
            args.max_window_p99_ms,
            args.max_window_fps_cv_percent,
        )
    # Selection always uses the first contiguous Dam run, so performance gate
    # failures cannot become healthy by waiting for later windows. Stop the
    # harness once the requested clean capture exists; final output still
    # applies and reports every performance gate.
    ready = len(selected) == args.measure and not capture_issues
    if args.output:
        success = write_outputs(
            args.output,
            windows,
            selected,
            violations,
            counts,
            args.warmup,
            args.measure,
            args.min_mean_fps,
            args.max_window_p99_ms,
            args.max_window_fps_cv_percent,
            args.external_failure,
        )
        return 0 if success else 1
    if args.ready:
        return 0 if ready else 1
    print(
        json.dumps(
            {
                "ready": ready,
                "profile_windows": len(windows),
                "dam_windows": sum(w["dam_candidate"] for w in windows),
                "contiguous_dam_windows": contiguous_dam_window_count(windows),
                "selected_windows": len(selected),
                "failures": issues,
            },
            sort_keys=True,
        )
    )
    return 0 if ready else 1


if __name__ == "__main__":
    raise SystemExit(main())
