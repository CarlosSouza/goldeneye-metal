#!/usr/bin/env python3

import argparse
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import metal_profile_parser as parser  # noqa: E402


def window_lines(index: int, fps: float, frame_ns: int) -> str:
    start = index * 64 + 1
    end = start + 63
    elapsed = frame_ns * 64
    return "\n".join(
        (
            f"[metal-profile] presenter attempts={start}-{end} sources=64 "
            "unchanged_sources=0 drawable_nil=0 uploads=0 upload_bytes=0 commits=64",
            f"[metal-profile] window swaps={start}-{end} elapsed_ns={elapsed} "
            f"avg_frame_ns={frame_ns} fps={fps}",
            f"[metal-profile] gpu-chain swaps={start}-{end} "
            "wptr_attempts=128 wptr_accepted=128 wptr_rejected=0 "
            "ring_batches=64 ring_commit_rejected=0",
            f"[metal-profile] command swaps={start}-{end} event=draw calls=640 "
            "avg_calls_per_swap=10 total_ns=64000000 avg_ns_per_swap=1000000 "
            "max_call_ns=2000000 max_swap_ns=3000000",
            f"[metal-profile] command swaps={start}-{end} event=copy calls=768 "
            "avg_calls_per_swap=12 total_ns=32000000 avg_ns_per_swap=500000 "
            "max_call_ns=1000000 max_swap_ns=2000000",
            f"[metal-profile] command swaps={start}-{end} event=swap calls=64 "
            "avg_calls_per_swap=1 total_ns=16000000 avg_ns_per_swap=250000 "
            "max_call_ns=500000 max_swap_ns=500000",
            f"[metal-profile] command swaps={start}-{end} event=wait_reg_mem calls=128 "
            "avg_calls_per_swap=2 total_ns=8000000 avg_ns_per_swap=125000 "
            "max_call_ns=200000 max_swap_ns=400000",
            f"[metal-profile] command swaps={start}-{end} "
            "event=texture_fallback_decode calls=0 avg_calls_per_swap=0 "
            "total_ns=0 avg_ns_per_swap=0 max_call_ns=0 max_swap_ns=0",
            f"[metal-profile] wait swaps={start}-{end} reason=swap calls=64 "
            "waited_calls=64 waited_submissions=64 total_ns=16000000 max_call_ns=500000",
            f"[metal-profile] wait-reg-mem swaps={start}-{end} rank=1 source=memory "
            "address=0x100 reference=0 mask=0xffffffff operation=3 wait=0x100 "
            "calls=64 polls=640 avg_polls_per_call=10 max_polls=12 total_ns=4000000 "
            "avg_ns_per_swap=62500 max_ns=100000 unmatched=0 timeouts=0",
        )
    )


def probe_queue_line(
    index: int,
    *,
    draw_batch: int = 256,
    commits: int = 10,
    blocking_waits: int = 2,
    blocking_wait_ns: int = 1000,
    signals: int = 60,
    timeouts: int = 4,
    counter_reset: int = 0,
    command_buffer_cap: int = 4,
) -> str:
    start = index * 64 + 1
    end = start + 63
    return (
        f"[metal-profile] probe-queue swaps={start}-{end} contexts=5 "
        f"commits={commits} backpressure_checks=8 nonblocking_reclaims=6 "
        f"blocking_waits={blocking_waits} blocking_wait_ns={blocking_wait_ns} "
        "lifetime_peak_committed_per_context=4 "
        "lifetime_peak_pending_per_context=1024 "
        f"draws_per_command_buffer={draw_batch} "
        f"committed_command_buffer_cap={command_buffer_cap} "
        f"counter_reset={counter_reset} configuration_mismatch=0 "
        f"wait_reg_mem_signals={signals} wait_reg_mem_timeouts={timeouts} "
        "wait_reg_mem_unavailable=0"
    )


def exact_om_line(
    index: int,
    *,
    enabled: int = 1,
    attempts: int = 10,
    enqueued: int = 7,
    completed: int = 1,
    rejected: int = 2,
    terminal_failures: int = 0,
    pending: int = 3,
    commits: int = 2,
    blocking_waits: int = 1,
    blocking_wait_ns: int = 500,
    lifetime_peak_committed: int = 3,
    lifetime_peak_pending: int = 9,
    draw_batch: int = 128,
    command_buffer_cap: int = 4,
    counter_reset: int = 0,
) -> str:
    start = index * 64 + 1
    end = start + 63
    return (
        f"[metal-profile] exact-om swaps={start}-{end} enabled={enabled} "
        f"attempts={attempts} enqueued={enqueued} completed={completed} "
        f"rejected={rejected} terminal_failures={terminal_failures} "
        "materializations=1 full_uploads=1 full_upload_bytes=10485760 "
        "full_downloads=1 full_download_bytes=10485760 "
        f"pending={pending} commits={commits} backpressure_checks=4 "
        "nonblocking_reclaims=2 "
        f"blocking_waits={blocking_waits} blocking_wait_ns={blocking_wait_ns} "
        f"lifetime_peak_committed={lifetime_peak_committed} "
        f"lifetime_peak_pending={lifetime_peak_pending} "
        f"draws_per_command_buffer={draw_batch} "
        f"command_buffer_cap={command_buffer_cap} counter_reset={counter_reset}"
    )


class MetalProfileParserTest(unittest.TestCase):
    def make_log(self, root: Path) -> Path:
        log = root / "raw.log"
        samples = ((60.0, 16_666_667), (58.0, 17_241_379), (56.0, 17_857_143), (54.0, 18_518_519))
        log.write_text(
            "\n".join(window_lines(index, *sample) for index, sample in enumerate(samples))
            + "\n",
            encoding="utf-8",
        )
        return log

    def test_frame_pacing_and_wait_aggregation(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = self.make_log(Path(temporary))
            windows, violations, counts = parser.parse_log(log)
            self.assertEqual(violations, [])
            self.assertEqual(counts, {})
            selected = parser.select_windows(windows, warmup=1, measure=3)
            aggregate = parser.aggregate(selected)
            self.assertAlmostEqual(aggregate["real_window_fps"]["mean"], 56.0)
            self.assertGreater(aggregate["real_window_fps"]["coefficient_of_variation_percent"], 2)
            self.assertEqual(aggregate["frame_pacing"]["sampled_frames"], 192)
            self.assertGreater(
                aggregate["frame_pacing"]["window_p99_frame_ms"],
                aggregate["frame_pacing"]["window_p50_frame_ms"],
            )
            self.assertEqual(aggregate["wait_reasons"]["swap"]["calls"], 192)
            self.assertGreater(aggregate["wait_reasons"]["swap"]["selected_elapsed_percent"], 1)
            waits = aggregate["wait_reg_mem"]
            self.assertEqual(waits["command_calls"], 384)
            self.assertEqual(waits["captured_ranked_calls"], 192)
            self.assertEqual(waits["unique_sites"], 1)
            self.assertEqual(waits["sites"][0]["address"], 0x100)
            self.assertAlmostEqual(waits["sites"][0]["average_polls_per_call"], 10)

    def test_probe_queue_telemetry_is_validated_and_aggregated_per_window(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            log = root / "raw.log"
            log.write_text(
                window_lines(0, 60.0, 16_666_667)
                + "\n"
                + probe_queue_line(0)
                + "\n"
                + window_lines(1, 59.0, 16_949_153)
                + "\n"
                + probe_queue_line(
                    1,
                    commits=12,
                    blocking_waits=3,
                    blocking_wait_ns=2400,
                    signals=61,
                    timeouts=3,
                )
                + "\n",
                encoding="utf-8",
            )
            windows, violations, counts = parser.parse_log(log)
            self.assertEqual(violations, [])
            self.assertEqual(counts, {})
            aggregate = parser.aggregate(
                parser.select_windows(windows, warmup=0, measure=2)
            )
            queue = aggregate["probe_queue"]
            self.assertEqual(queue["windows_reported"], 2)
            self.assertEqual(queue["commits"], 22)
            self.assertEqual(queue["blocking_waits"], 5)
            self.assertEqual(queue["blocking_wait_ns"], 3400)
            self.assertEqual(queue["blocking_wait_mean_ns"], 680)
            self.assertEqual(queue["wait_reg_mem_signals"], 121)
            self.assertEqual(queue["wait_reg_mem_timeouts"], 7)
            self.assertEqual(queue["draws_per_command_buffer"], 256)
            self.assertEqual(queue["committed_command_buffer_cap"], 4)
            self.assertEqual(parser.probe_queue_comparison_violations(aggregate), [])

            wrong_cap = dict(aggregate)
            wrong_cap["probe_queue"] = dict(queue)
            wrong_cap["probe_queue"]["committed_command_buffer_cap"] = 3
            self.assertIn(
                "probe queue command-buffer cap must be exactly 4",
                parser.probe_queue_comparison_violations(wrong_cap),
            )

    def test_probe_queue_comparison_rejects_missing_reset_and_invalid_config(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            missing_log = root / "missing.log"
            missing_log.write_text(
                window_lines(0, 60.0, 16_666_667) + "\n", encoding="utf-8"
            )
            windows, violations, _ = parser.parse_log(missing_log)
            self.assertEqual(violations, [])
            missing = parser.aggregate(parser.select_windows(windows, 0, 1))
            self.assertIn(
                "probe queue telemetry is missing",
                parser.probe_queue_comparison_violations(missing),
            )

            reset_log = root / "reset.log"
            reset_log.write_text(
                window_lines(0, 60.0, 16_666_667)
                + "\n"
                + probe_queue_line(0, counter_reset=1)
                + "\n",
                encoding="utf-8",
            )
            windows, violations, _ = parser.parse_log(reset_log)
            self.assertEqual(violations, [])
            reset = parser.aggregate(parser.select_windows(windows, 0, 1))
            self.assertTrue(
                any(
                    "cumulative counters reset" in problem
                    for problem in parser.probe_queue_comparison_violations(reset)
                )
            )

            invalid_log = root / "invalid.log"
            invalid_log.write_text(
                window_lines(0, 60.0, 16_666_667)
                + "\n"
                + probe_queue_line(0, draw_batch=96)
                + "\n",
                encoding="utf-8",
            )
            windows, violations, counts = parser.parse_log(invalid_log)
            self.assertFalse(windows[0]["complete"])
            self.assertEqual(counts["invalid_profile_windows"], 1)
            self.assertTrue(any("64, 128, or 256" in item for item in violations))

            wrong_cap_log = root / "wrong-cap.log"
            wrong_cap_log.write_text(
                window_lines(0, 60.0, 16_666_667)
                + "\n"
                + probe_queue_line(0, command_buffer_cap=3)
                + "\n",
                encoding="utf-8",
            )
            windows, violations, counts = parser.parse_log(wrong_cap_log)
            self.assertFalse(windows[0]["complete"])
            self.assertEqual(counts["invalid_profile_windows"], 1)
            self.assertTrue(
                any("cap must be exactly 4" in item for item in violations)
            )

    def test_exact_output_merger_telemetry_matches_windows_and_reaches_outputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            log = root / "raw.log"
            # Emit the optional ledgers in reverse order after both windows to
            # prove association is by the inclusive swap range, not adjacency.
            log.write_text(
                window_lines(0, 60.0, 16_666_667)
                + "\n"
                + window_lines(1, 59.0, 16_949_153)
                + "\n"
                + exact_om_line(
                    1,
                    attempts=5,
                    enqueued=4,
                    completed=0,
                    rejected=1,
                    pending=0,
                    commits=3,
                    blocking_waits=2,
                    blocking_wait_ns=1500,
                    lifetime_peak_committed=4,
                    lifetime_peak_pending=10,
                    counter_reset=1,
                )
                + "\n"
                + exact_om_line(0)
                + "\n",
                encoding="utf-8",
            )

            windows, violations, counts = parser.parse_log(log)
            self.assertEqual(violations, [])
            self.assertEqual(counts, {})
            self.assertEqual(windows[0]["exact_output_merger"]["attempts"], 10)
            self.assertEqual(windows[1]["exact_output_merger"]["attempts"], 5)

            selected = parser.select_windows(windows, warmup=0, measure=2)
            exact_om = parser.aggregate(selected)["exact_output_merger"]
            self.assertEqual(exact_om["windows_reported"], 2)
            self.assertEqual(exact_om["enabled_windows"], 2)
            self.assertEqual(exact_om["attempts"], 15)
            self.assertEqual(exact_om["enqueued"], 11)
            self.assertEqual(exact_om["commits"], 5)
            self.assertEqual(exact_om["pending"], 0)
            self.assertEqual(exact_om["pending_max"], 3)
            self.assertEqual(exact_om["lifetime_peak_committed"], 4)
            self.assertEqual(exact_om["lifetime_peak_pending"], 10)
            self.assertEqual(exact_om["draws_per_command_buffer"], 128)
            self.assertEqual(exact_om["command_buffer_cap"], 4)
            self.assertEqual(exact_om["counter_reset_windows"], 1)
            self.assertAlmostEqual(exact_om["blocking_wait_mean_ns"], 2000 / 3)

            output = root / "result"
            self.assertTrue(
                parser.write_outputs(
                    output,
                    windows,
                    selected,
                    violations,
                    counts,
                    warmup=0,
                    measure=2,
                )
            )
            summary = json.loads(
                (output / "summary.json").read_text(encoding="utf-8")
            )
            self.assertEqual(summary["aggregate"]["exact_output_merger"]["attempts"], 15)
            csv_lines = (output / "windows.csv").read_text(encoding="utf-8").splitlines()
            self.assertIn("exact_om_terminal_failures", csv_lines[0])
            self.assertIn("exact_om_command_buffer_cap", csv_lines[0])
            self.assertIn(
                "exact output merger: enabled-windows=2 attempts=15",
                (output / "summary.txt").read_text(encoding="utf-8"),
            )

    def test_disabled_exact_output_merger_window_does_not_require_activity_or_config(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "raw.log"
            disabled = exact_om_line(
                0,
                enabled=0,
                attempts=99,
                enqueued=0,
                completed=0,
                rejected=0,
                terminal_failures=0,
                draw_batch=0,
                command_buffer_cap=0,
            )
            log.write_text(
                window_lines(0, 60.0, 16_666_667) + "\n" + disabled + "\n",
                encoding="utf-8",
            )
            windows, violations, counts = parser.parse_log(log)
            self.assertEqual(violations, [])
            self.assertEqual(counts, {})
            self.assertTrue(windows[0]["complete"])
            exact_om = parser.aggregate(windows)["exact_output_merger"]
            self.assertEqual(exact_om["enabled_windows"], 0)
            self.assertEqual(exact_om["disabled_windows"], 1)
            self.assertEqual(exact_om["draws_per_command_buffer"], 0)
            self.assertEqual(exact_om["command_buffer_cap"], 0)

    def test_invalid_exact_output_merger_ledgers_fail_closed(self):
        base = exact_om_line(0)
        cases = {
            "missing": base.replace(" counter_reset=0", ""),
            "negative": base.replace("pending=3", "pending=-1"),
            "non-integer": base.replace("attempts=10", "attempts=1.5"),
            "bad-enabled": base.replace("enabled=1", "enabled=2"),
            "outcome-mismatch": base.replace("attempts=10", "attempts=11"),
            "bad-batch": base.replace("draws_per_command_buffer=128", "draws_per_command_buffer=64"),
            "bad-cap": base.replace("command_buffer_cap=4", "command_buffer_cap=3"),
            "pending-over-peak": base.replace("lifetime_peak_pending=9", "lifetime_peak_pending=2"),
            "orphan-upload-bytes": base.replace("full_uploads=1", "full_uploads=0"),
        }
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "raw.log"
            for name, ledger in cases.items():
                with self.subTest(name=name):
                    log.write_text(
                        window_lines(0, 60.0, 16_666_667) + "\n" + ledger + "\n",
                        encoding="utf-8",
                    )
                    windows, violations, counts = parser.parse_log(log)
                    self.assertFalse(windows[0]["complete"])
                    self.assertTrue(violations)
                    self.assertEqual(counts["invalid_profile_windows"], 1)

    def test_duplicate_and_orphan_exact_output_merger_ledgers_are_reported(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            duplicate_log = root / "duplicate.log"
            duplicate_log.write_text(
                window_lines(0, 60.0, 16_666_667)
                + "\n"
                + exact_om_line(0)
                + "\n"
                + exact_om_line(0)
                + "\n",
                encoding="utf-8",
            )
            windows, violations, counts = parser.parse_log(duplicate_log)
            self.assertFalse(windows[0]["complete"])
            self.assertTrue(violations)
            self.assertEqual(counts["duplicate_exact_output_merger_ledgers"], 1)

            orphan_log = root / "orphan.log"
            orphan_log.write_text(exact_om_line(0) + "\n", encoding="utf-8")
            windows, violations, counts = parser.parse_log(orphan_log)
            self.assertEqual(windows, [])
            self.assertTrue(violations)
            self.assertEqual(counts["orphan_exact_output_merger_ledgers"], 1)

    def test_exact_output_merger_telemetry_remains_optional_for_older_logs(self):
        with tempfile.TemporaryDirectory() as temporary:
            windows, violations, counts = parser.parse_log(
                self.make_log(Path(temporary))
            )
            self.assertEqual(violations, [])
            self.assertEqual(counts, {})
            self.assertIsNone(parser.aggregate(windows)["exact_output_merger"])

    def test_sampled_draw_stages_are_optional_and_aggregated_separately(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            log = root / "raw.log"
            log.write_text(
                window_lines(0, 60.0, 16_666_667)
                + "\n"
                + "[metal-profile] command swaps=1-64 event=draw_probe_sample calls=10 "
                "avg_calls_per_swap=0.156 total_ns=5000000 avg_ns_per_swap=78125 "
                "max_call_ns=700000 max_swap_ns=900000 max_calls_per_swap=1\n"
                + "[metal-profile] command swaps=1-64 event=draw_render_sample calls=10 "
                "avg_calls_per_swap=0.156 total_ns=3000000 avg_ns_per_swap=46875 "
                "max_call_ns=500000 max_swap_ns=600000 max_calls_per_swap=1\n",
                encoding="utf-8",
            )
            windows, violations, counts = parser.parse_log(log)
            self.assertEqual(violations, [])
            self.assertEqual(counts, {})
            self.assertTrue(windows[0]["complete"])
            aggregate = parser.aggregate(parser.select_windows(windows, warmup=0, measure=1))
            self.assertEqual(aggregate["sampled_stages"]["draw_probe_sample"]["total_samples"], 10)
            self.assertEqual(
                aggregate["sampled_stages"]["draw_probe_sample"]["mean_ns_per_sample"],
                500_000,
            )
            self.assertEqual(
                aggregate["sampled_stages"]["draw_probe_sample"][
                    "median_window_mean_ns_per_sample"
                ],
                500_000,
            )
            self.assertEqual(
                aggregate["sampled_stages"]["draw_render_sample"]["mean_ns_per_sample"],
                300_000,
            )

    def test_optional_performance_gates_fail_summary(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            log = self.make_log(root)
            windows, violations, counts = parser.parse_log(log)
            selected = parser.select_windows(windows, warmup=1, measure=3)
            output = root / "result"
            passed = parser.write_outputs(
                output,
                windows,
                selected,
                violations,
                counts,
                warmup=1,
                measure=3,
                min_mean_fps=59.0,
                max_window_p99_ms=18.0,
                max_window_fps_cv_percent=1.0,
            )
            self.assertFalse(passed)
            summary = json.loads((output / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["status"], "fail")
            self.assertTrue(any("mean FPS" in failure for failure in summary["failures"]))
            self.assertTrue(any("p99 frame time" in failure for failure in summary["failures"]))
            text_summary = (output / "summary.txt").read_text(encoding="utf-8")
            self.assertIn("WAIT_REG_MEM: command_calls=384", text_summary)
            self.assertIn("WAIT_REG_MEM site memory:0x00000100", text_summary)

    def test_ready_mode_stops_after_a_complete_capture_even_if_a_gate_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = self.make_log(Path(temporary))
            result = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools/metal_profile_parser.py"),
                    str(log),
                    "--warmup",
                    "1",
                    "--measure",
                    "3",
                    "--min-mean-fps",
                    "99",
                    "--ready",
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_incomplete_non_64_and_invalid_numeric_windows_are_rejected(self):
        cases = {
            "non-64": window_lines(0, 60.0, 16_666_667).replace("swaps=1-64", "swaps=1-63"),
            "missing-ledger": "\n".join(
                line
                for line in window_lines(0, 60.0, 16_666_667).splitlines()
                if "event=wait_reg_mem" not in line
            ),
            "non-finite": window_lines(0, 60.0, 16_666_667).replace("fps=60.0", "fps=nan"),
            "zero-header": window_lines(0, 60.0, 16_666_667).replace(
                "avg_frame_ns=16666667", "avg_frame_ns=0"
            ),
            "zero-elapsed": window_lines(0, 60.0, 16_666_667).replace(
                "elapsed_ns=1066666688", "elapsed_ns=0"
            ),
            "zero-fps": window_lines(0, 60.0, 16_666_667).replace("fps=60.0", "fps=0"),
            "short-swap-ledger": window_lines(0, 60.0, 16_666_667).replace(
                "event=swap calls=64", "event=swap calls=63"
            ),
        }
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name, contents in cases.items():
                with self.subTest(name=name):
                    log = root / f"{name}.log"
                    log.write_text(contents + "\n", encoding="utf-8")
                    windows, violations, counts = parser.parse_log(log)
                    self.assertEqual(len(windows), 1)
                    self.assertFalse(windows[0]["complete"])
                    self.assertFalse(windows[0]["dam_candidate"])
                    self.assertTrue(violations)
                    self.assertEqual(counts["invalid_profile_windows"], 1)

    def test_performance_gates_never_use_a_metric_subset(self):
        with tempfile.TemporaryDirectory() as temporary:
            windows, violations, _ = parser.parse_log(self.make_log(Path(temporary)))
            self.assertEqual(violations, [])
            selected = parser.select_windows(windows, warmup=0, measure=3)
            selected[1]["fps"] = None
            issues = parser.performance_violations(
                parser.aggregate(selected),
                min_mean_fps=1.0,
                max_window_p99_ms=None,
                max_window_fps_cv_percent=100.0,
            )
            self.assertEqual(sum("incomplete samples" in issue for issue in issues), 2)

    def test_dam_selection_cannot_skip_a_complete_bad_window(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            samples = [window_lines(index, 60.0, 16_666_667) for index in range(4)]
            samples[1] = samples[1].replace(
                "event=copy calls=768 avg_calls_per_swap=12",
                "event=copy calls=0 avg_calls_per_swap=0",
            )
            log = root / "raw.log"
            log.write_text("\n".join(samples) + "\n", encoding="utf-8")
            windows, violations, counts = parser.parse_log(log)
            self.assertEqual(violations, [])
            self.assertEqual(sum(window["dam_candidate"] for window in windows), 3)
            selected = parser.select_windows(windows, warmup=0, measure=3)
            self.assertEqual(
                [(window["swap_start"], window["swap_end"]) for window in selected],
                [(1, 64)],
            )
            output = root / "result"
            self.assertFalse(
                parser.write_outputs(
                    output,
                    windows,
                    selected,
                    violations,
                    counts,
                    warmup=0,
                    measure=3,
                )
            )
            summary = json.loads((output / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["observed"]["contiguous_dam_windows"], 1)
            self.assertTrue(
                any("insufficient contiguous Dam windows" in item for item in summary["failures"])
            )

    def test_release_profile_requires_positive_presenter_and_clean_gpu_chain(self):
        cases = {
            "missing-presenter": "\n".join(
                line
                for line in window_lines(0, 60.0, 16_666_667).splitlines()
                if " presenter " not in f" {line} "
            ),
            "zero-presenter": window_lines(0, 60.0, 16_666_667).replace(
                "sources=64", "sources=0"
            ),
            "rejected-wptr": window_lines(0, 60.0, 16_666_667).replace(
                "wptr_rejected=0", "wptr_rejected=1"
            ),
            "rejected-ring-commit": window_lines(0, 60.0, 16_666_667).replace(
                "ring_commit_rejected=0", "ring_commit_rejected=1"
            ),
            "missing-fallback-ledger": "\n".join(
                line
                for line in window_lines(0, 60.0, 16_666_667).splitlines()
                if "event=texture_fallback_decode" not in line
            ),
        }
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "raw.log"
            for name, contents in cases.items():
                with self.subTest(name=name):
                    log.write_text(contents + "\n", encoding="utf-8")
                    windows, violations, _ = parser.parse_log(log)
                    self.assertFalse(windows[0]["complete"])
                    self.assertTrue(violations)

    def test_polling_parse_defers_only_a_structurally_open_trailing_window(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "raw.log"
            complete = window_lines(0, 60.0, 16_666_667)
            trailing = window_lines(1, 60.0, 16_666_667).splitlines()
            log.write_text(
                complete + "\n" + "\n".join(trailing[:3]) + "\n",
                encoding="utf-8",
            )
            windows, violations, counts = parser.parse_log(
                log, defer_open_window=True
            )
            self.assertEqual(violations, [])
            self.assertEqual(counts, {})
            self.assertTrue(windows[0]["complete"])
            self.assertFalse(windows[1]["complete"])
            self.assertTrue(windows[1]["pending_ledgers"])

            # A later header closes the prior cohort deterministically, so the
            # missing ledgers become a real failure rather than staying deferred.
            third = window_lines(2, 60.0, 16_666_667).splitlines()
            with log.open("a", encoding="utf-8") as stream:
                stream.write("\n".join(third[:2]) + "\n")
            windows, violations, counts = parser.parse_log(
                log, defer_open_window=True
            )
            self.assertTrue(violations)
            self.assertEqual(counts["invalid_profile_windows"], 1)
            self.assertFalse(windows[1]["pending_ledgers"])
            self.assertTrue(windows[2]["pending_ledgers"])

    def test_texture_fallback_decode_is_a_capture_violation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            log = root / "raw.log"
            log.write_text(
                window_lines(0, 60.0, 16_666_667).replace(
                    "event=texture_fallback_decode calls=0 avg_calls_per_swap=0 "
                    "total_ns=0 avg_ns_per_swap=0 max_call_ns=0 max_swap_ns=0",
                    "event=texture_fallback_decode calls=2 avg_calls_per_swap=0.031 "
                    "total_ns=2000 avg_ns_per_swap=31 max_call_ns=1200 max_swap_ns=2000",
                )
                + "\n",
                encoding="utf-8",
            )
            windows, violations, counts = parser.parse_log(log)
            self.assertFalse(windows[0]["complete"])
            self.assertEqual(counts["texture_fallback_decodes"], 2)
            self.assertTrue(
                any("texture fallback decode calls=2" in item for item in violations)
            )

    def test_native_path_fallback_counters_are_capture_violations(self):
        cases = (
            (
                "[metal] async probe summary#1 fallbacks=0 depth_fallbacks=2",
                "depth_fallbacks",
                2,
            ),
            ("[metal] draw route summary#1 host_fallback=3", "host_fallback", 3),
            ("[metal] draw-window#1 ps_fallback=1", "ps_fallback", 1),
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for index, (line, field_name, expected_count) in enumerate(cases):
                with self.subTest(field=field_name):
                    log = root / f"fallback-{index}.log"
                    log.write_text(line + "\n", encoding="utf-8")
                    _, violations, counts = parser.parse_log(log)
                    self.assertEqual(counts[field_name], expected_count)
                    self.assertTrue(
                        any(f"{field_name}={expected_count}" in item for item in violations)
                    )

    def test_gpu_chain_ledger_is_required_and_aggregated(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            log = root / "raw.log"
            windows = []
            for index in range(2):
                start = index * 64 + 1
                end = start + 63
                chain = (
                    f"[metal-profile] gpu-chain swaps={start}-{end} "
                    f"wptr_attempts={10 + index} wptr_accepted={10 + index} "
                    f"wptr_rejected=0 ring_batches={8 + index} "
                    f"ring_commit_rejected=0 "
                    f"metal_pending_samples=4 metal_pending_total={8 + index * 4} "
                    f"metal_pending_max={3 + index} throttle_calls=8 "
                    f"throttle_actual_ns={960000 + index * 8000} "
                    f"throttle_max_ns={130000 + index * 1000} "
                    f"interrupt_to_wptr_samples=2 "
                    f"interrupt_to_wptr_total_ns={4000 + index * 1000} "
                    "interrupt_outstanding=0"
                )
                base = window_lines(index, 60.0, 16_666_667)
                base_chain = (
                    f"[metal-profile] gpu-chain swaps={start}-{end} "
                    "wptr_attempts=128 wptr_accepted=128 wptr_rejected=0 "
                    "ring_batches=64 ring_commit_rejected=0"
                )
                windows.append(base.replace(base_chain, chain))
            log.write_text("\n".join(windows) + "\n", encoding="utf-8")

            parsed, violations, counts = parser.parse_log(log)
            self.assertEqual(violations, [])
            self.assertEqual(counts, {})
            aggregate = parser.aggregate(parser.select_windows(parsed, 0, 2))
            chain = aggregate["gpu_chain"]
            self.assertEqual(chain["wptr_accepted"], 21)
            self.assertEqual(chain["ring_batches"], 17)
            self.assertEqual(chain["metal_pending_max"], 4)
            self.assertEqual(chain["metal_pending_mean"], 2.5)
            self.assertEqual(chain["throttle_actual_mean_ns"], 120500)
            self.assertEqual(chain["interrupt_to_wptr_mean_ns"], 2250)

    def test_resolve_mode_ledgers_are_aggregated_without_enabling_verbose_logging(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            log = root / "raw.log"
            per_window = (
                (10, 2, 2, 0, 0, 9),
                (12, 3, 2, 0, 1, 11),
                (16, 2, 1, 0, 0, 15),
            )
            chunks = []
            for index, values in enumerate(per_window):
                start = index * 64 + 1
                end = start + 63
                chunks.append(window_lines(index, 60.0, 16_666_667))
                chunks.append(
                    f"[metal-profile] resolve-mode swaps={start}-{end} "
                    f"async={values[0]} sync={values[1]} "
                    f"missing_snapshot={values[2]} verbose={values[3]} "
                    f"missing_alias={values[4]} exact_snapshot={values[5]}"
                )
            log.write_text("\n".join(chunks) + "\n", encoding="utf-8")

            windows, violations, counts = parser.parse_log(log)
            self.assertEqual(violations, [])
            self.assertEqual(counts, {})
            self.assertEqual(
                windows[1]["resolve_mode"],
                {
                    "async": 12,
                    "sync": 3,
                    "missing_snapshot": 2,
                    "verbose": 0,
                    "missing_alias": 1,
                    "exact_snapshot": 11,
                },
            )
            aggregate = parser.aggregate(parser.select_windows(windows, 1, 2))
            self.assertEqual(
                aggregate["resolve_mode"],
                {
                    "async": 28,
                    "sync": 5,
                    "missing_snapshot": 3,
                    "verbose": 0,
                    "missing_alias": 1,
                    "exact_snapshot": 26,
                    "windows_reported": 2,
                    "async_percent": 28 / 33 * 100.0,
                },
            )

            output = root / "result"
            self.assertTrue(
                parser.write_outputs(
                    output,
                    windows,
                    parser.select_windows(windows, 1, 2),
                    violations,
                    counts,
                    warmup=1,
                    measure=2,
                )
            )
            self.assertIn(
                "tiled resolve mode: async=28 sync=5",
                (output / "summary.txt").read_text(encoding="utf-8"),
            )
            windows_csv = (output / "windows.csv").read_text(encoding="utf-8")
            self.assertIn("resolve_async", windows_csv.splitlines()[0])
            self.assertIn(",12,3,2,0,1,11", windows_csv)

    def test_invalid_or_duplicate_resolve_mode_ledgers_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            base = window_lines(0, 60.0, 16_666_667)
            valid = (
                "[metal-profile] resolve-mode swaps=1-64 async=10 sync=2 "
                "missing_snapshot=2 verbose=0 missing_alias=0 exact_snapshot=9"
            )
            cases = {
                "duplicate": base + "\n" + valid + "\n" + valid,
                "missing": base + "\n" + valid.replace(" exact_snapshot=9", ""),
                "negative": base + "\n" + valid.replace("sync=2", "sync=-1"),
            }
            for name, contents in cases.items():
                with self.subTest(name=name):
                    log = root / f"{name}.log"
                    log.write_text(contents + "\n", encoding="utf-8")
                    _, violations, counts = parser.parse_log(log)
                    self.assertTrue(violations)
                    self.assertTrue(
                        counts.get("duplicate_resolve_mode_ledgers", 0)
                        or counts.get("invalid_resolve_mode_ledgers", 0)
                    )

    def test_resolve_mode_telemetry_remains_optional_for_older_logs(self):
        with tempfile.TemporaryDirectory() as temporary:
            windows, violations, counts = parser.parse_log(
                self.make_log(Path(temporary))
            )
            self.assertEqual(violations, [])
            self.assertEqual(counts, {})
            self.assertIsNone(parser.aggregate(windows)["resolve_mode"])

    def test_threshold_converter_rejects_non_finite_values(self):
        for value in ("nan", "NaN", "inf", "-inf"):
            with self.subTest(value=value):
                with self.assertRaises(argparse.ArgumentTypeError):
                    parser.finite_float(value)
        self.assertEqual(parser.finite_float("0"), 0.0)
        self.assertEqual(parser.finite_float("59.5"), 59.5)

    def test_cli_rejects_non_finite_threshold(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = self.make_log(Path(temporary))
            result = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools/metal_profile_parser.py"),
                    str(log),
                    "--min-mean-fps",
                    "nan",
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
                check=False,
            )
            self.assertEqual(result.returncode, 2)
            self.assertIn("must be finite", result.stderr)

    def test_external_failure_is_written_to_both_summaries(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            windows, violations, counts = parser.parse_log(self.make_log(root))
            selected = parser.select_windows(windows, warmup=1, measure=3)
            output = root / "result"
            self.assertFalse(
                parser.write_outputs(
                    output,
                    windows,
                    selected,
                    violations,
                    counts,
                    warmup=1,
                    measure=3,
                    external_failures=["game process exited with status 9"],
                )
            )
            summary = json.loads((output / "summary.json").read_text(encoding="utf-8"))
            self.assertIn("game process exited with status 9", summary["failures"])
            self.assertIn(
                "FAIL: game process exited with status 9",
                (output / "summary.txt").read_text(encoding="utf-8"),
            )

    def test_tracked_source_freshness_covers_code_headers_and_config_only(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            files = {
                "CMakeLists.txt": "runtime config",
                "src/runtime.cpp": "runtime code",
                "include/rex/runtime.h": "runtime header",
                "tests/runtime_test.cpp": "ignored test",
                "docs/status.md": "ignored docs",
                "thirdparty/CMakeLists.txt": "third-party config",
                "thirdparty/tiny-aes-c/aes.c": "third-party runtime code",
                "thirdparty/tiny-aes-c/aes.h": "third-party runtime header",
                "thirdparty/generated/generated.c": "ignored generated dependency",
                "thirdparty/out/output.c": "ignored output dependency",
                "thirdparty/README.md": "ignored third-party prose",
                "vendor/GoldenEye-Recomp/CMakeLists.txt": "app config",
                "vendor/GoldenEye-Recomp/CMakePresets.json": "app presets",
                "vendor/GoldenEye-Recomp/ge_config.toml": "app config",
                "vendor/GoldenEye-Recomp/ge_manifest.toml": "app manifest",
                "vendor/GoldenEye-Recomp/src/main.cpp": "app code",
                "vendor/GoldenEye-Recomp/src/ge_app.h": "app header",
                "vendor/GoldenEye-Recomp/packaging/macos/Info.plist.in": "plist template",
                "vendor/GoldenEye-Recomp/packaging/macos/stage_app.cmake": "staging logic",
                "vendor/GoldenEye-Recomp/packaging/macos/GoldenEyeMetal.icns": "icon",
                "launcher/build-app.sh": "launcher build logic",
                "tools/sign-notarize.sh": "release packaging logic",
                "vendor/GoldenEye-Recomp/tests/app_test.cpp": "ignored test",
            }
            for relative, contents in files.items():
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(contents, encoding="utf-8")
                os.utime(path, ns=(100, 100))
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            subprocess.run(["git", "-C", str(root), "add", "."], check=True)
            untracked_header = root / "include/rex/new_build_input.h"
            untracked_header.write_text("untracked runtime header", encoding="utf-8")
            release_manifest = root / "config/goldeneye-release.json"
            release_manifest.parent.mkdir()
            release_manifest.write_text(
                '{"version":"0.4.2","build":"10"}\n', encoding="utf-8"
            )
            untracked_package_asset = (
                root
                / "vendor/GoldenEye-Recomp/packaging/macos/GoldenEyeMetal-AppIcon.png"
            )
            untracked_package_asset.write_text("untracked package icon", encoding="utf-8")
            os.utime(untracked_header, ns=(100, 100))
            os.utime(release_manifest, ns=(100, 100))
            os.utime(untracked_package_asset, ns=(100, 100))
            dylib, executable = root / "build/runtime.dylib", root / "build/GoldenEye"
            dylib.parent.mkdir()
            dylib.write_bytes(b"runtime")
            executable.write_bytes(b"app")
            os.utime(dylib, ns=(200, 200))
            os.utime(executable, ns=(200, 200))

            groups = parser.tracked_build_sources(root)
            runtime_relative = {str(path.relative_to(root)) for path in groups["runtime"]}
            app_relative = {str(path.relative_to(root)) for path in groups["goldeneye_app"]}
            self.assertIn("include/rex/runtime.h", runtime_relative)
            self.assertIn("include/rex/new_build_input.h", runtime_relative)
            self.assertIn("thirdparty/CMakeLists.txt", runtime_relative)
            self.assertIn("thirdparty/tiny-aes-c/aes.c", runtime_relative)
            self.assertIn("thirdparty/tiny-aes-c/aes.h", runtime_relative)
            self.assertIn("vendor/GoldenEye-Recomp/src/ge_app.h", app_relative)
            self.assertIn("vendor/GoldenEye-Recomp/ge_config.toml", app_relative)
            self.assertIn("config/goldeneye-release.json", app_relative)
            self.assertIn(
                "vendor/GoldenEye-Recomp/packaging/macos/Info.plist.in",
                app_relative,
            )
            self.assertIn(
                "vendor/GoldenEye-Recomp/packaging/macos/stage_app.cmake",
                app_relative,
            )
            self.assertIn(
                "vendor/GoldenEye-Recomp/packaging/macos/GoldenEyeMetal-AppIcon.png",
                app_relative,
            )
            self.assertIn("launcher/build-app.sh", app_relative)
            self.assertIn("tools/sign-notarize.sh", app_relative)
            self.assertNotIn("tests/runtime_test.cpp", runtime_relative)
            self.assertNotIn("docs/status.md", runtime_relative)
            self.assertNotIn("thirdparty/generated/generated.c", runtime_relative)
            self.assertNotIn("thirdparty/out/output.c", runtime_relative)
            self.assertNotIn("thirdparty/README.md", runtime_relative)
            self.assertNotIn("vendor/GoldenEye-Recomp/tests/app_test.cpp", app_relative)
            self.assertTrue(parser.build_freshness_report(root, dylib, executable)["fresh"])
            cli = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools/metal_profile_parser.py"),
                    "--check-freshness",
                    "--repo",
                    str(root),
                    "--dylib",
                    str(dylib),
                    "--executable",
                    str(executable),
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
                check=False,
            )
            self.assertEqual(cli.returncode, 0, cli.stderr)
            self.assertTrue(json.loads(cli.stdout)["fresh"])

            dependency_source = root / "thirdparty/tiny-aes-c/aes.c"
            os.utime(dependency_source, ns=(300, 300))
            report = parser.build_freshness_report(root, dylib, executable)
            self.assertFalse(report["artifacts"]["runtime"]["fresh"])
            self.assertTrue(report["artifacts"]["goldeneye_app"]["fresh"])
            self.assertIn(
                "thirdparty/tiny-aes-c/aes.c",
                report["artifacts"]["runtime"]["stale_sources"],
            )
            os.utime(dependency_source, ns=(100, 100))

            os.utime(untracked_header, ns=(300, 300))
            report = parser.build_freshness_report(root, dylib, executable)
            self.assertFalse(report["artifacts"]["runtime"]["fresh"])
            self.assertIn(
                "include/rex/new_build_input.h",
                report["artifacts"]["runtime"]["stale_sources"],
            )
            os.utime(untracked_header, ns=(100, 100))

            os.utime(release_manifest, ns=(300, 300))
            report = parser.build_freshness_report(root, dylib, executable)
            self.assertFalse(report["artifacts"]["goldeneye_app"]["fresh"])
            self.assertIn(
                "config/goldeneye-release.json",
                report["artifacts"]["goldeneye_app"]["stale_sources"],
            )
            os.utime(release_manifest, ns=(100, 100))

            os.utime(untracked_package_asset, ns=(300, 300))
            report = parser.build_freshness_report(root, dylib, executable)
            self.assertFalse(report["artifacts"]["goldeneye_app"]["fresh"])
            self.assertIn(
                "vendor/GoldenEye-Recomp/packaging/macos/GoldenEyeMetal-AppIcon.png",
                report["artifacts"]["goldeneye_app"]["stale_sources"],
            )
            os.utime(untracked_package_asset, ns=(100, 100))

            launcher_build = root / "launcher/build-app.sh"
            os.utime(launcher_build, ns=(300, 300))
            report = parser.build_freshness_report(root, dylib, executable)
            self.assertFalse(report["artifacts"]["goldeneye_app"]["fresh"])
            self.assertIn(
                "launcher/build-app.sh",
                report["artifacts"]["goldeneye_app"]["stale_sources"],
            )
            os.utime(launcher_build, ns=(100, 100))

            app_header = root / "vendor/GoldenEye-Recomp/src/ge_app.h"
            os.utime(app_header, ns=(300, 300))
            report = parser.build_freshness_report(root, dylib, executable)
            self.assertTrue(report["artifacts"]["runtime"]["fresh"])
            self.assertFalse(report["artifacts"]["goldeneye_app"]["fresh"])
            self.assertIn(
                "vendor/GoldenEye-Recomp/src/ge_app.h",
                report["artifacts"]["goldeneye_app"]["stale_sources"],
            )

    def test_metadata_uses_explicit_effective_environment(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable, dylib, xex = root / "GoldenEye", root / "runtime.dylib", root / "default.xex"
            for path in (executable, dylib, xex):
                path.write_bytes(path.name.encode())
            destination = root / "metadata.json"
            args = SimpleNamespace(
                executable=str(executable),
                dylib=str(dylib),
                xex=str(xex),
                repo=str(root),
                data_root=str(root),
                metadata=str(destination),
            )
            effective = {
                "REX_GPU": "metal",
                "GOLDENEYE_BENCH_CACHE_MODE": "warm",
                "DYLD_LIBRARY_PATH": "/effective/runtime",
                "HOME": "/isolated/home",
                "TMPDIR": "/isolated/tmp",
                "IGNORED": "no",
            }
            with mock.patch.object(parser, "command_text", return_value=""):
                parser.write_metadata(args, effective_environment=effective)
            metadata = json.loads(destination.read_text(encoding="utf-8"))
            self.assertEqual(metadata["environment"]["REX_GPU"], "metal")
            self.assertEqual(metadata["environment"]["DYLD_LIBRARY_PATH"], "/effective/runtime")
            self.assertEqual(metadata["environment"]["HOME"], "/isolated/home")
            self.assertEqual(metadata["environment"]["TMPDIR"], "/isolated/tmp")
            self.assertNotIn("IGNORED", metadata["environment"])
            self.assertEqual(metadata["benchmark"]["cache_mode"], "warm")

    def test_metric_formatting_handles_zero_missing_and_non_finite(self):
        self.assertEqual(parser.format_metric(0), "0.000")
        self.assertEqual(parser.format_metric(None), "n/a")
        self.assertEqual(parser.format_metric(float("nan")), "n/a")


if __name__ == "__main__":
    unittest.main()
