#!/usr/bin/env python3

import importlib.util
import json
import os
import signal
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "stability_cycle", ROOT / "tools/stability-cycle.py"
)
assert SPEC and SPEC.loader
stability = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(stability)


def profile_window_lines(
    *,
    start: int = 1,
    fps: float = 60.0,
    frame_ns: int = 16_666_667,
    wait_unmatched: int = 0,
) -> tuple[str, ...]:
    end = start + 63
    elapsed = frame_ns * 64
    return (
        f"[metal-profile] presenter attempts={start}-{end} sources=64 "
        "unchanged_sources=0 drawable_nil=0 uploads=0 upload_bytes=0 commits=64",
        f"[metal-profile] window swaps={start}-{end} elapsed_ns={elapsed} "
        f"avg_frame_ns={frame_ns} fps={fps}",
        f"[metal-profile] gpu-chain swaps={start}-{end} wptr_attempts=128 "
        "wptr_accepted=128 wptr_rejected=0 ring_batches=64 "
        "ring_commit_rejected=0",
        f"[metal-profile] command swaps={start}-{end} event=draw calls=640 "
        "avg_calls_per_swap=10 total_ns=64000000 avg_ns_per_swap=1000000 "
        "max_call_ns=2000000 max_swap_ns=3000000",
        f"[metal-profile] command swaps={start}-{end} event=copy calls=768 "
        "avg_calls_per_swap=12 total_ns=32000000 avg_ns_per_swap=500000 "
        "max_call_ns=1000000 max_swap_ns=2000000",
        f"[metal-profile] command swaps={start}-{end} event=swap calls=64 "
        "avg_calls_per_swap=1 total_ns=16000000 avg_ns_per_swap=250000 "
        "max_call_ns=500000 max_swap_ns=500000",
        f"[metal-profile] command swaps={start}-{end} event=wait_reg_mem calls=64 "
        "avg_calls_per_swap=1 total_ns=8000000 avg_ns_per_swap=125000 "
        "max_call_ns=250000 max_swap_ns=250000",
        f"[metal-profile] command swaps={start}-{end} "
        "event=texture_fallback_decode calls=0 avg_calls_per_swap=0 total_ns=0 "
        "avg_ns_per_swap=0 max_call_ns=0 max_swap_ns=0",
        f"[metal-profile] wait-reg-mem swaps={start}-{end} rank=1 "
        f"source=memory unmatched={wait_unmatched} timeouts=0",
    )


def profile_emitter_source(
    *,
    start: int = 1,
    fps: float = 60.0,
    frame_ns: int = 16_666_667,
    wait_unmatched: int = 0,
) -> str:
    lines = profile_window_lines(
        start=start,
        fps=fps,
        frame_ns=frame_ns,
        wait_unmatched=wait_unmatched,
    )
    return "\n".join(f"print({line!r}, flush=True)" for line in lines)


def performance_readiness(
    *,
    level: int = 30,
    draw_calls_per_swap: float = 1000.0,
    draw_batch: int = 64,
    counter_reset_windows: int = 0,
    command_buffer_cap: int = 4,
    mean_fps: float = 60.0,
    one_percent_low_fps: float = 55.0,
    window_p99_frame_ms: float = 18.0,
    blocking_wait_ns: int = 5_000_000,
    wait_reg_mem_timeouts: int = 12,
) -> dict[str, object]:
    windows = 3
    sampled_frames = windows * 64
    return {
        "local_multiplayer": {"level": level, "players": 4, "stable_polls": 120},
        "aggregate": {
            "windows": windows,
            "real_window_fps": {"mean": mean_fps},
            "frame_pacing": {
                "sampled_frames": sampled_frames,
                "window_one_percent_low_fps": one_percent_low_fps,
                "window_p99_frame_ms": window_p99_frame_ms,
            },
            "stages": {
                "draw": {
                    "total_calls": int(draw_calls_per_swap * sampled_frames),
                }
            },
            "probe_queue": {
                "windows_reported": windows,
                "commits": 2000,
                "backpressure_checks": 400,
                "nonblocking_reclaims": 350,
                "blocking_waits": 50,
                "blocking_wait_ns": blocking_wait_ns,
                "wait_reg_mem_signals": 180,
                "wait_reg_mem_timeouts": wait_reg_mem_timeouts,
                "wait_reg_mem_unavailable": 0,
                "contexts": 5,
                "lifetime_peak_committed_per_context": 4,
                "lifetime_peak_pending_per_context": 1024,
                "draws_per_command_buffer": draw_batch,
                "draws_per_command_buffer_values": [draw_batch],
                "committed_command_buffer_cap": command_buffer_cap,
                "committed_command_buffer_cap_values": [command_buffer_cap],
                "counter_reset_windows": counter_reset_windows,
                "configuration_mismatch_windows": 0,
                "blocking_wait_mean_ns": 100_000,
            },
        },
    }


def host_pause_event_text() -> str:
    return "\n".join(
        (
            "[ge-test] host-pause open-request queued=1",
            "[ge-test] host-pause frozen open_generation=3 samples=3 "
            "duration_ms=1000 frame_delta=2 present_delta=2 world_stable=1 "
            "ui_open=1 owned=1",
            "[ge-test] host-pause close-request open_generation=3 queued=1",
            "[ge-test] host-pause resumed open_generation=3 "
            "resume_generation=4 samples=2 duration_ms=500 frame_delta=1 "
            "present_delta=1 ui_closed=1 pause_released=1 input_ready=1",
        )
    )


def make_fake_trace_replay(
    path: Path, *, mismatch_second_run: bool = False, sleep_seconds: float = 0.0
) -> Path:
    path.write_text(
        textwrap.dedent(
            f"""\
            #!/usr/bin/env python3
            import sys
            import time
            from pathlib import Path

            assert len(sys.argv) == 4
            trace = Path(sys.argv[1])
            output = Path(sys.argv[2])
            assert trace.is_file()
            assert sys.argv[3] == "0"
            time.sleep({sleep_seconds!r})
            output.parent.mkdir(parents=True, exist_ok=True)
            width, height = 2, 2
            rgba = bytes(range(16))
            if {mismatch_second_run!r} and output.parent.name == "run-2":
                rgba = rgba[:-1] + bytes((rgba[-1] ^ 0xFF,))
            header = bytearray(54)
            header[:2] = b"BM"
            header[2:6] = (70).to_bytes(4, "little")
            header[10:14] = (54).to_bytes(4, "little")
            header[14:18] = (40).to_bytes(4, "little")
            header[18:22] = width.to_bytes(4, "little", signed=True)
            header[22:26] = height.to_bytes(4, "little", signed=True)
            header[26:28] = (1).to_bytes(2, "little")
            header[28:30] = (24).to_bytes(2, "little")
            header[34:38] = (16).to_bytes(4, "little")
            output.with_suffix(".bmp").write_bytes(header + bytes(16))
            output.with_suffix(".rgba").write_bytes(rgba)
            """
        ),
        encoding="utf-8",
    )
    path.chmod(0o755)
    return path


class StabilityCycleTest(unittest.TestCase):
    def _cycle_fixture(
        self,
        root: Path,
        source: str,
        **overrides,
    ) -> tuple[SimpleNamespace, Path]:
        executable = root / "fake-game.py"
        executable.write_text(
            textwrap.dedent(source),
            encoding="utf-8",
        )
        executable.chmod(0o755)
        runtime = root / "runtime"
        runtime.mkdir()
        (runtime / "librexruntime.dylib").touch()
        game_data = root / "game-data"
        game_data.mkdir()
        (game_data / "default.xex").write_bytes(b"fixture")
        suite = root / "suite"
        suite.mkdir()
        values = {
            "mode": "menu",
            "players": 2,
            "executable": executable,
            "runtime_dir": runtime,
            "game_data": game_data,
            "ready_timeout": 5.0,
            "post_ready_soak_seconds": 0.5,
            "poll_seconds": 0.02,
            "menu_settle_seconds": 0.0,
            "warmup_windows": 0,
            "observe_windows": 1,
            "capture": False,
            "capture_delay": 0.0,
            "capture_retries": 1,
            "render_min_luma_stddev": 1.0,
            "reference": None,
            "quit_method": "signal",
            "shutdown_timeout": 1.0,
        }
        values.update(overrides)
        return SimpleNamespace(**values), suite

    def test_cli_rejects_non_finite_timeout(self):
        result = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools/stability-cycle.py"),
                "--ready-timeout",
                "nan",
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("finite number", result.stderr)

    def test_cli_rejects_non_finite_post_ready_soak(self):
        result = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools/stability-cycle.py"),
                "--post-ready-soak-seconds",
                "nan",
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("finite number", result.stderr)

    def test_cli_documents_conservative_release_performance_defaults(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/stability-cycle.py"), "--help"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--min-mean-fps", result.stdout)
        self.assertIn("default: 25", result.stdout)
        self.assertIn("--max-window-p99-ms", result.stdout)
        self.assertIn("default: 50 ms", result.stdout)
        self.assertIn("--disable-performance-gates", result.stdout)
        self.assertIn("--fixed-multiplayer-benchmark-arena", result.stdout)
        self.assertEqual(stability.DEFAULT_MIN_MEAN_FPS, 25.0)
        self.assertEqual(stability.DEFAULT_MAX_WINDOW_P99_MS, 50.0)

    def test_four_player_performance_comparison_requires_same_level_and_draw_cohort(self):
        reference, problems = stability.multiplayer_performance_cohort(
            performance_readiness(level=30, draw_batch=64)
        )
        self.assertEqual(problems, [])
        self.assertIsNotNone(reference)

        same_scene = stability.compare_multiplayer_performance_cohort(
            reference,
            performance_readiness(
                level=30,
                draw_calls_per_swap=1010.0,
                draw_batch=128,
                mean_fps=58.0,
                one_percent_low_fps=53.0,
                window_p99_frame_ms=19.0,
                blocking_wait_ns=6_000_000,
                wait_reg_mem_timeouts=15,
            ),
            2.0,
            128,
        )
        self.assertEqual(same_scene["comparability_status"], "pass", same_scene)
        self.assertNotIn("status", same_scene)
        self.assertAlmostEqual(same_scene["draw_calls_per_swap_delta_percent"], 1.0)
        self.assertEqual(same_scene["reference"]["draws_per_command_buffer"], 64)
        self.assertEqual(same_scene["candidate"]["draws_per_command_buffer"], 128)
        self.assertEqual(
            same_scene["observed_metrics"]["reference"]["mean_fps"], 60.0
        )
        self.assertEqual(
            same_scene["observed_metrics"]["candidate"]["mean_fps"], 58.0
        )
        self.assertEqual(
            same_scene["metric_deltas"]["candidate_minus_reference"][
                "mean_fps"
            ],
            -2.0,
        )
        self.assertEqual(
            same_scene["metric_deltas"]["candidate_minus_reference"][
                "window_one_percent_low_fps"
            ],
            -2.0,
        )
        self.assertEqual(
            same_scene["metric_deltas"]["candidate_minus_reference"][
                "window_p99_frame_ms"
            ],
            1.0,
        )
        self.assertEqual(
            same_scene["metric_deltas"]["candidate_minus_reference"][
                "probe_blocking_wait_ns"
            ],
            1_000_000.0,
        )
        self.assertEqual(
            same_scene["metric_deltas"]["candidate_minus_reference"][
                "wait_reg_mem_timeouts"
            ],
            3.0,
        )

        wrong_level = stability.compare_multiplayer_performance_cohort(
            reference,
            performance_readiness(level=38, draw_batch=128),
            2.0,
            128,
        )
        self.assertEqual(wrong_level["comparability_status"], "fail")
        self.assertTrue(
            any(
                "level mismatch" in failure
                for failure in wrong_level["comparability_failures"]
            )
        )

        wrong_draw_cohort = stability.compare_multiplayer_performance_cohort(
            reference,
            performance_readiness(
                level=30, draw_calls_per_swap=900.0, draw_batch=256
            ),
            2.0,
            256,
        )
        self.assertEqual(wrong_draw_cohort["comparability_status"], "fail")
        self.assertTrue(
            any(
                "draw-cohort mismatch" in failure
                for failure in wrong_draw_cohort["comparability_failures"]
            )
        )

    def test_four_player_performance_gate_requires_requested_batch_and_four_buffer_cap(self):
        cohort, problems = stability.multiplayer_performance_cohort(
            performance_readiness(draw_batch=256)
        )
        self.assertEqual(problems, [])
        self.assertEqual(
            stability.requested_probe_configuration_violations(cohort, 128),
            [
                "requested probe draw batch was not observed: "
                "requested=128 observed=256"
            ],
        )

        reference, problems = stability.multiplayer_performance_cohort(
            performance_readiness(draw_batch=64)
        )
        self.assertEqual(problems, [])
        ignored_knob = stability.compare_multiplayer_performance_cohort(
            reference,
            performance_readiness(draw_batch=256),
            2.0,
            128,
        )
        self.assertEqual(ignored_knob["comparability_status"], "fail")
        self.assertTrue(
            any(
                "requested probe draw batch was not observed" in failure
                for failure in ignored_knob["comparability_failures"]
            )
        )

        wrong_cap, problems = stability.multiplayer_performance_cohort(
            performance_readiness(command_buffer_cap=3)
        )
        self.assertIsNone(wrong_cap)
        self.assertTrue(
            any("cap must be exactly 4" in problem for problem in problems)
        )

        invalid_reference = dict(reference)
        invalid_reference["committed_command_buffer_cap"] = 3
        comparison = stability.compare_multiplayer_performance_cohort(
            invalid_reference,
            performance_readiness(draw_batch=128),
            2.0,
            128,
        )
        self.assertEqual(comparison["comparability_status"], "fail")
        self.assertTrue(
            any(
                "reference command-buffer cap must be exactly 4" in failure
                for failure in comparison["comparability_failures"]
            )
        )

    def test_four_player_performance_cohort_rejects_missing_or_reset_telemetry(self):
        missing = performance_readiness()
        missing["aggregate"]["probe_queue"] = None
        cohort, problems = stability.multiplayer_performance_cohort(missing)
        self.assertIsNone(cohort)
        self.assertIn("probe queue telemetry is missing", problems)

        cohort, problems = stability.multiplayer_performance_cohort(
            performance_readiness(counter_reset_windows=1)
        )
        self.assertIsNone(cohort)
        self.assertTrue(any("cumulative counters reset" in item for item in problems))

    def test_performance_reference_loader_requires_one_successful_four_player_cycle(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "summary.json"
            path.write_text(
                json.dumps(
                    {
                        "status": "pass",
                        "release_eligible": True,
                        "mode": "local-multiplayer",
                        "cycles": [
                            {
                                "status": "pass",
                                "release_eligible": True,
                                "failures": [],
                                "readiness": performance_readiness(
                                    level=30, draw_batch=64
                                ),
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            cohort = stability.load_multiplayer_performance_reference(path)
            self.assertEqual(cohort["level"], 30)
            self.assertEqual(cohort["players"], 4)
            self.assertEqual(cohort["draws_per_command_buffer"], 64)

            document = json.loads(path.read_text(encoding="utf-8"))
            document["cycles"].append(document["cycles"][0])
            path.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "exactly one cycle"):
                stability.load_multiplayer_performance_reference(path)

            document["cycles"] = document["cycles"][:1]
            document["status"] = "diagnostic-pass"
            document["release_eligible"] = False
            path.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "release-eligible pass"):
                stability.load_multiplayer_performance_reference(path)

    def test_wrong_expected_multiplayer_level_aborts_before_scenario(self):
        read_fd, write_fd = os.pipe()
        driver = stability.LocalMultiplayerInputDriver(
            4, write_fd, expected_level=30
        )
        driver.phase = "waiting-for-match-readiness"
        driver.action_phase_started_at = 0.0
        observations = SimpleNamespace(
            mission=SimpleNamespace(level=38, players=4, network=False)
        )
        try:
            with mock.patch.object(
                stability.gameplay, "parse_observations", return_value=observations
            ), mock.patch.object(
                stability.gameplay,
                "complete_local_multiplayer_batches",
                return_value=(),
            ):
                driver._advance_active_scenario("", 1.0)
            self.assertEqual(driver.expected_level, 30)
            self.assertEqual(driver.observed_level, 38)
            self.assertEqual(
                driver.error,
                "local multiplayer level mismatch: expected 30, observed 38",
            )
            self.assertFalse(driver.completed)
            self.assertEqual(driver.result()["expected_level"], 30)
            self.assertEqual(driver.result()["observed_level"], 38)
        finally:
            driver.close()
            os.close(read_fd)

    def test_fixed_four_player_benchmark_selects_arena_before_guest_join(self):
        with tempfile.TemporaryDirectory() as temporary:
            log_path = Path(temporary) / "raw.log"
            read_fd, write_fd = os.pipe()
            os.set_blocking(read_fd, False)
            driver = stability.LocalMultiplayerInputDriver(
                4, write_fd, fixed_benchmark_arena=True
            )
            driver.ready_elapsed = 0.0
            driver.phase = "waiting-for-create-local-game"

            def append(*lines: str) -> None:
                with log_path.open("a", encoding="utf-8") as stream:
                    stream.write("\n".join(lines) + "\n")

            try:
                append(
                    "[vpad] READY pads=4",
                    "[ge-test] menu state=15 name=create-local-game joined=1",
                )
                driver.advance(log_path, 1.0)
                self.assertEqual(
                    driver.phase, "benchmark-arena-move-to-scenario"
                )
                driver.advance(log_path, 1.8)
                self.assertEqual(
                    os.read(read_fd, 4096).decode("ascii"),
                    "PULSE_AXIS 1 1 LY 32767 80\n",
                )

                append("[vpad] ACK seq=1")
                driver.advance(log_path, 2.5)
                self.assertEqual(
                    os.read(read_fd, 4096).decode("ascii"),
                    "PULSE_AXIS 2 1 LY 32767 80\n",
                )

                append("[vpad] ACK seq=2")
                driver.advance(log_path, 3.2)
                self.assertEqual(
                    os.read(read_fd, 4096).decode("ascii"),
                    "PULSE_BUTTON 3 1 SOUTH 250\n",
                )

                append(
                    "[vpad] ACK seq=3",
                    "[ge-test] menu state=16 name=level-selector joined=0",
                )
                driver.advance(log_path, 4.0)
                self.assertEqual(
                    os.read(read_fd, 4096).decode("ascii"),
                    "PULSE_AXIS 4 1 LY 32767 80\n",
                )

                append("[vpad] ACK seq=4")
                driver.advance(log_path, 4.7)
                self.assertEqual(
                    os.read(read_fd, 4096).decode("ascii"),
                    "PULSE_BUTTON 5 1 SOUTH 250\n",
                )

                append("[vpad] ACK seq=5")
                driver.advance(log_path, 5.5)
                self.assertEqual(
                    driver.phase, "benchmark-arena-selection-settle"
                )
                self.assertFalse(
                    driver.result()["benchmark_arena_selection"][
                        "create_local_game_returned_after_confirmation"
                    ]
                )
                with self.assertRaises(BlockingIOError):
                    os.read(read_fd, 4096)

                append(
                    "[ge-test] menu state=15 name=create-local-game joined=1"
                )
                driver.advance(log_path, 5.6)
                self.assertEqual(driver.phase, "joining-guests")
                with self.assertRaises(BlockingIOError):
                    os.read(read_fd, 4096)

                driver.advance(log_path, 6.4)
                self.assertEqual(
                    os.read(read_fd, 4096).decode("ascii"),
                    "".join(
                        f"PULSE_BUTTON {sequence} {player} START 250\n"
                        for sequence, player in ((6, 2), (7, 3), (8, 4))
                    ),
                )
                append(
                    "[vpad] ACK seq=6",
                    "[vpad] ACK seq=7",
                    "[vpad] ACK seq=8",
                    "[ge-test] menu state=15 name=create-local-game joined=4",
                )
                driver.advance(log_path, 7.2)
                self.assertEqual(
                    os.read(read_fd, 4096).decode("ascii"),
                    "PULSE_BUTTON 9 1 START 250\n",
                )

                append(
                    "[vpad] ACK seq=9",
                    "[ge-test] mission level=27 players=4 network=0",
                    "[ge] local multiplayer ready level=27 players=4 "
                    "network=0 stable_polls=120",
                )
                driver.advance(log_path, 8.0)
                driver.advance(log_path, 8.1)
                evidence = driver.result()["benchmark_arena_selection"]
                self.assertTrue(evidence["move_to_scenario"]["acknowledged"])
                self.assertTrue(evidence["move_to_level"]["acknowledged"])
                self.assertTrue(evidence["open_level_selector"]["acknowledged"])
                self.assertTrue(evidence["move_to_fixed_level"]["acknowledged"])
                self.assertTrue(evidence["confirm_fixed_level"]["acknowledged"])
                self.assertTrue(
                    evidence["create_local_game_returned_after_confirmation"]
                )
                self.assertTrue(
                    evidence["selection_completed_before_guest_join"]
                )
                self.assertEqual(evidence["guest_join_first_sequence"], 6)
                self.assertEqual(evidence["start_match_sequence"], 9)
                self.assertEqual(evidence["mission_level_observations"], [27])
                self.assertEqual(evidence["ready_level_observations"], [27])
                self.assertEqual(evidence["selected_level_id"], 27)
                self.assertTrue(evidence["observed_level_stable"])

                readiness = performance_readiness(level=27)
                readiness["multiplayer_scenario"] = driver.result()
                cohort, problems = stability.multiplayer_performance_cohort(
                    readiness
                )
                self.assertEqual(problems, [])
                self.assertEqual(
                    cohort["benchmark_arena_selection"]["selected_level_id"],
                    27,
                )

                invalid_readiness = json.loads(json.dumps(readiness))
                invalid_readiness["multiplayer_scenario"][
                    "benchmark_arena_selection"
                ]["move_to_fixed_level"]["command"] = (
                    "PULSE_AXIS 4 1 LY -32768 80"
                )
                cohort, problems = stability.multiplayer_performance_cohort(
                    invalid_readiness
                )
                self.assertIsNone(cohort)
                self.assertTrue(
                    any("command is invalid" in problem for problem in problems)
                )

                invalid_order = json.loads(json.dumps(readiness))
                invalid_order["multiplayer_scenario"][
                    "benchmark_arena_selection"
                ]["confirm_fixed_level"]["sequence"] = 4
                invalid_order["multiplayer_scenario"][
                    "benchmark_arena_selection"
                ]["confirm_fixed_level"]["command"] = (
                    "PULSE_BUTTON 4 1 SOUTH 250"
                )
                cohort, problems = stability.multiplayer_performance_cohort(
                    invalid_order
                )
                self.assertIsNone(cohort)
                self.assertTrue(
                    any("selection ordering is invalid" in problem for problem in problems)
                )

                append("[ge-test] mission level=45 players=4 network=0")
                driver.advance(log_path, 8.2)
                self.assertEqual(
                    driver.error,
                    "fixed multiplayer benchmark arena was not stable: 27, 45",
                )
            finally:
                driver.close()
                os.close(read_fd)

    def test_fixed_benchmark_arena_rejects_non_four_player_driver(self):
        read_fd, write_fd = os.pipe()
        try:
            with self.assertRaisesRegex(ValueError, "requires 4 players"):
                stability.LocalMultiplayerInputDriver(
                    3, write_fd, fixed_benchmark_arena=True
                )
        finally:
            os.close(write_fd)
            os.close(read_fd)

    def test_fixed_benchmark_arena_cli_requires_four_player_local_mode(self):
        result = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools/stability-cycle.py"),
                "--mode",
                "local-multiplayer",
                "--players",
                "3",
                "--fixed-multiplayer-benchmark-arena",
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--fixed-multiplayer-benchmark-arena requires "
            "--mode local-multiplayer --players 4",
            result.stderr,
        )

    def test_gpu_capture_cli_is_restricted_to_dam_gameplay(self):
        result = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools/stability-cycle.py"),
                "--mode",
                "menu",
                "--capture-gpu-frame",
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--capture-gpu-frame requires --mode dam-gameplay", result.stderr
        )

    def test_host_pause_cli_is_restricted_to_dam_gameplay(self):
        result = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools/stability-cycle.py"),
                "--mode",
                "menu",
                "--validate-host-pause",
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--validate-host-pause requires --mode dam-gameplay", result.stderr
        )

    def test_host_pause_integration_requires_bounded_ordered_camera_effect(self):
        scenario_evidence = {
            "host_resume_probe_bounded": True,
            "host_resume_probe_hold_ms": 2000,
            "host_resume_probe_sequence": 1,
            "host_resume_neutral_baseline_sample": 20,
            "host_resume_baseline_position_drift": 0.0,
            "host_resume_baseline_camera_drift": 0.0,
            "look_input_observed": True,
            "look_input_sample": 21,
            "camera_effect_sample": 22,
            "camera_delta": 0.2,
            "host_resume_probe_released": True,
            "movement_input_observed": True,
            "fire_input_observed": True,
            "fire_effect_observed": True,
            "final_neutral_observed": True,
        }

        def evidence(**updates):
            current = dict(scenario_evidence)
            current.update(updates)
            return stability.host_pause_integration_evidence(
                host_pause_event_text(),
                {"completed": True, "scenario": {"evidence": current}},
            )

        result = evidence()
        self.assertTrue(result["validated"], result)
        self.assertTrue(result["input_after_resume"], result)
        self.assertTrue(result["post_resume_input"]["validated"], result)
        self.assertEqual(result["post_resume_input"]["neutral_baseline_sample"], 20)
        self.assertEqual(result["post_resume_input"]["input_sample"], 21)
        self.assertEqual(result["post_resume_input"]["camera_effect_sample"], 22)

        failures = {
            "unbounded": {"host_resume_probe_bounded": False},
            "wrong-duration": {"host_resume_probe_hold_ms": 999},
            "moving-baseline": {"host_resume_baseline_position_drift": 1.0},
            "camera-drift-baseline": {"host_resume_baseline_camera_drift": 0.5},
            "input-before-baseline": {"look_input_sample": 19},
            "effect-before-input": {"camera_effect_sample": 21},
            "no-camera-effect": {"camera_delta": 0.0},
            "non-finite-camera-effect": {"camera_delta": float("nan")},
            "not-released": {"host_resume_probe_released": False},
        }
        for name, updates in failures.items():
            with self.subTest(name=name):
                failed = evidence(**updates)
                self.assertFalse(failed["validated"], failed)
                self.assertFalse(failed["input_after_resume"], failed)
                self.assertEqual(
                    failed["failure"], "post-resume gameplay input was not proven"
                )

    def test_host_pause_event_source_collapses_only_coherent_mirrors(self):
        event_lines = host_pause_event_text().splitlines()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            raw_log = root / "raw.log"
            runtime_logs = root / "user-data" / "Logs"
            runtime_logs.mkdir(parents=True)
            runtime_log = runtime_logs / "ge_001.log"

            raw_log.write_text(host_pause_event_text() + "\n", encoding="utf-8")
            runtime_log.write_text(
                "\n".join(f"[timestamp] {line}" for line in event_lines) + "\n",
                encoding="utf-8",
            )
            selected = stability.host_pause_event_source_text(raw_log)
            self.assertEqual(selected, host_pause_event_text())
            self.assertTrue(
                stability.gameplay.parse_host_pause_evidence(selected)["validated"]
            )

            raw_log.write_text("\n".join(event_lines[:2]) + "\n", encoding="utf-8")
            selected = stability.host_pause_event_source_text(raw_log)
            self.assertEqual(selected, host_pause_event_text())

            raw_log.write_text(
                host_pause_event_text()
                + "\n[ge-test] host-pause open-request queued=1\n",
                encoding="utf-8",
            )
            selected = stability.host_pause_event_source_text(raw_log)
            duplicate = stability.gameplay.parse_host_pause_evidence(selected)
            self.assertEqual(duplicate["status"], "failed", duplicate)

            raw_log.write_text(
                "[ge-test] host-pause open-request queued=0\n",
                encoding="utf-8",
            )
            selected = stability.host_pause_event_source_text(raw_log)
            divergent = stability.gameplay.parse_host_pause_evidence(selected)
            self.assertEqual(divergent["status"], "failed", divergent)
            self.assertEqual(divergent["malformed"], ["conflicting-event-sinks"])

    def test_performance_gates_can_be_overridden_or_disabled_for_diagnostics(self):
        args = SimpleNamespace(
            min_mean_fps=15.0,
            max_window_p99_ms=60.0,
            disable_performance_gates=False,
        )
        self.assertEqual(
            stability.performance_thresholds(args, "dam"), (15.0, 60.0)
        )
        args.disable_performance_gates = True
        self.assertEqual(
            stability.performance_thresholds(args, "dam-gameplay"), (None, None)
        )
        self.assertEqual(
            stability.performance_thresholds(args, "menu"), (None, None)
        )

    def test_release_eligibility_rejects_every_diagnostic_bypass(self):
        args = SimpleNamespace(
            quit_method="native",
            disable_performance_gates=False,
            require_soak_progress=True,
            post_ready_soak_seconds=10.0,
            allow_stale_build=False,
            skip_metadata=False,
        )
        self.assertEqual(
            stability.release_eligibility(args), {"eligible": True, "reasons": []}
        )
        mutations = (
            ("quit_method", "signal", "visible-window"),
            ("disable_performance_gates", True, "performance gates"),
            ("require_soak_progress", False, "progress gate"),
            ("post_ready_soak_seconds", 0.0, "duration was zero"),
            ("allow_stale_build", True, "freshness"),
            ("skip_metadata", True, "metadata"),
        )
        for field, value, expected in mutations:
            with self.subTest(field=field):
                original = getattr(args, field)
                setattr(args, field, value)
                result = stability.release_eligibility(args)
                self.assertFalse(result["eligible"])
                self.assertTrue(
                    any(expected in reason for reason in result["reasons"]), result
                )
                setattr(args, field, original)

    def test_failed_level_or_comparability_evidence_is_release_ineligible(self):
        args = SimpleNamespace(
            quit_method="native",
            disable_performance_gates=False,
            require_soak_progress=True,
            post_ready_soak_seconds=10.0,
            allow_stale_build=False,
            skip_metadata=False,
        )
        clean = stability.cycle_release_eligibility(args, [])
        self.assertEqual(clean, {"eligible": True, "reasons": []})

        failed = stability.cycle_release_eligibility(
            args,
            ["virtual-gamepad input failed"],
            expected_multiplayer_level=27,
            observed_multiplayer_level=45,
            performance_comparison={
                "comparability_status": "fail",
                "comparability_failures": [
                    "no complete selected candidate cohort was produced"
                ],
            },
        )
        self.assertFalse(failed["eligible"])
        self.assertIn(
            "local multiplayer level mismatch: expected 27, observed 45",
            failed["reasons"],
        )
        self.assertIn(
            "4-player performance comparison was not workload-comparable",
            failed["reasons"],
        )
        self.assertIn("cycle validation failed", failed["reasons"])

        suite = stability.suite_release_eligibility(
            args,
            [
                {
                    "cycle": 1,
                    "status": "fail",
                    "release_eligible": failed["eligible"],
                    "release_ineligibility_reasons": failed["reasons"],
                }
            ],
        )
        self.assertFalse(suite["eligible"])
        self.assertIn(
            "cycle 1: local multiplayer level mismatch: expected 27, observed 45",
            suite["reasons"],
        )
        self.assertIn(
            "cycle 1: 4-player performance comparison was not workload-comparable",
            suite["reasons"],
        )

    def test_failed_cycle_cannot_leave_suite_release_eligible_true(self):
        args = SimpleNamespace(
            quit_method="native",
            disable_performance_gates=False,
            require_soak_progress=True,
            post_ready_soak_seconds=10.0,
            allow_stale_build=False,
            skip_metadata=False,
        )
        suite = stability.suite_release_eligibility(
            args,
            [
                {
                    "cycle": 1,
                    "status": "fail",
                    # Guard old/malformed cycle records too: suite status is
                    # authoritative even if this field says true.
                    "release_eligible": True,
                    "release_ineligibility_reasons": [],
                }
            ],
        )
        self.assertFalse(suite["eligible"])
        self.assertEqual(suite["reasons"], ["cycle 1: cycle validation failed"])

    def test_serious_stability_markers_fail_log_scan(self):
        markers = (
            "GEWATCHDOG STALL: ring rpi=0x1 wpi=0x2 [PENDING]",
            "[ge] GENOPRESENT STALL rpi=0x00000001 wpi=0x00000002 PENDING",
            ("[GE-PLAYER-STUCK-v1] suspected live-render movement stall: samples=16"),
            (
                "[GE-GUARD-AUDIO-CALLBACK-v1] repaired callback ABI "
                "hit=1 site=0x823E4BA8"
            ),
            (
                "[GE-GUARD-823DFB70-v1] recovered site=0x823DFBA4 "
                "hit=1 reason=null-object"
            ),
            (
                "[GE-GUARD-823CFC00-v2] repaired callback ABI hit=1 "
                "changed=0x02"
            ),
            (
                "WAIT_REG_MEM stalled >60ms (poll=1FC9B006 ref=00000000); "
                "proceeding to avoid a CPU/GPU sync deadlock"
            ),
            "Metal MSL translation failed: invalid SPIR-V byte size 3",
            "[metal] render pipeline failed#1 vs=1 ps=2 rt=0: fixture",
            "[metal] native depth resolve failed#1 copy=2: fixture",
            "MetalDrawRenderer: diagnostic command buffer failed: fixture",
            "Metal API Validation Error: fixture",
            "Error Domain=MTLCommandBufferErrorDomain Code=3 fixture",
            "Command Buffer execution failed: fixture",
            "Execution of the command buffer was aborted: fixture",
            "IOAF code 4 (page fault)",
            "kIOAccelCommandBufferCallbackErrorPageFault fixture",
            "GPU Address Fault Error: fixture",
            "MTLDebugRenderCommandEncoder validation error: fixture",
            "[metal] probe context submission with error: fixture",
            "Metal vertex pipeline compilation failed: fixture",
            "Metal shader cache shutdown: 20 shaders cached, 19 translated, 1 failed",
            "[metal] CPU fallback for resolve fixture",
            (
                "[GE-GUARD-AUDIO-CALLBACK-v1] snapshot overflow "
                "hit=1 depth=8 guest_sp=0x1"
            ),
            (
                "[GE-GUARD-823CFC00-v2] callback snapshot underflow "
                "hit=2 guest_sp=0x1"
            ),
            (
                "[ge] GENOPRESENT CSLEDGER owner=0x1 cs=0x2 found=1 "
                "depth=1 enters=1 incomplete=1 dropped=0 leave_mismatches=0"
            ),
            (
                "[ge] GENOPRESENT CSLEDGER owner=0x1 cs=0x2 found=1 "
                "depth=1 enters=1 incomplete=0 dropped=0 leave_mismatches=2"
            ),
            (
                "[ge] GENOPRESENT CSLEDGER matching_leave_mismatch "
                "cs=0x00000002 leave_lr=0x823E4BAC"
            ),
            (
                "GENOPRESENT CSLEDGER latest owner-thread leave mismatch "
                "cs=0x2 leave_lr=0x823E4BAC"
            ),
            (
                "GENOPRESENT CSLEDGER owner=0x1 cs=0x2 found=true "
                "incomplete=true dropped=0 leave_mismatches=0"
            ),
            (
                "[metal] true MRT submission required failed#1 draw=42 active=2 "
                "mask=0x00ff vs=1 ps=2; legacy per-target replay is disabled"
            ),
            (
                "[metal] production MRT route failed#1 draw=42 active=2 "
                "mask=0x00ff vs=1 ps=2 reason=fixture; "
                "legacy per-target replay is disabled"
            ),
        )
        with tempfile.TemporaryDirectory() as temporary:
            cycle_root = Path(temporary)
            log_path = cycle_root / "raw.log"
            for marker in markers:
                with self.subTest(marker=marker):
                    log_path.write_text(marker + "\n", encoding="utf-8")
                    matches = stability.detect_fatal_logs(cycle_root)
                    self.assertTrue(matches, marker)

    def test_benign_stability_summaries_do_not_fail_log_scan(self):
        with tempfile.TemporaryDirectory() as temporary:
            cycle_root = Path(temporary)
            (cycle_root / "raw.log").write_text(
                "\n".join(
                    (
                        "summary: GEWATCHDOG STALL count=0",
                        "summary: GENOPRESENT STALL count=0",
                        "[GE-PLAYER-STUCK-v1] reports=0",
                        "[GE-GUARD-AUDIO-CALLBACK-v1] repairs=0",
                        "[GE-GUARD-823DFB70-v1] active",
                        "Metal shader cache shutdown: 20 shaders cached, 20 translated, 0 failed",
                        "[metal-profile] command swaps=1-64 event=texture_fallback_decode calls=0",
                        "[metal-profile] wait-reg-mem swaps=1-64 unmatched=0 timeouts=0",
                        (
                            "[metal] production MRT submit#1 draw=42 outputs=0x3 "
                            "targets=2 encoded=1 depth=1 completed=1 samples=1 "
                            "sync=1 vs_memexport=0 ps_memexport=0"
                        ),
                        (
                            "[ge] GENOPRESENT CSLEDGER owner=0x1 cs=0x2 found=1 "
                            "depth=1 enters=1 incomplete=0 dropped=0 "
                            "leave_mismatches=0"
                        ),
                        (
                            "GENOPRESENT CSLEDGER owner=0x1 cs=0x2 found=true "
                            "incomplete=false dropped=0 leave_mismatches=0"
                        ),
                        "summary: CSLEDGER matching_leave_mismatch count=0",
                    )
                )
                + "\n",
                encoding="utf-8",
            )
            self.assertEqual(stability.detect_fatal_logs(cycle_root), [])

    def test_every_dummy_texture_is_a_failure_in_every_mode(self):
        line = (
            "[metal] probe texture dummy#1 ps shader=5d448681ef02a235 "
            "binding=0 fetch=0 type=2 fmt=6 dim=1 "
            "dwords=80024802 1ebfe086 03ffdfff 00800c14 00000000 00000200"
        )
        with tempfile.TemporaryDirectory() as temporary:
            log_path = Path(temporary) / "raw.log"
            log_path.write_text(line + "\n", encoding="utf-8")
            for mode in ("menu", "dam", "dam-gameplay", "local-multiplayer"):
                with self.subTest(mode=mode):
                    self.assertTrue(stability.dummy_texture_violations(log_path, mode))
            self.assertTrue(stability.detect_fatal_logs(Path(temporary)))

    def test_process_group_cleanup_detects_and_kills_surviving_child(self):
        with tempfile.TemporaryDirectory() as temporary:
            child_ready = Path(temporary) / "child-ready"
            parent_source = textwrap.dedent(
                """\
                import subprocess, sys, time
                child_code = (
                    "import pathlib,signal,sys,time; "
                    "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                    "pathlib.Path(sys.argv[1]).write_text('ready'); "
                    "time.sleep(60)"
                )
                subprocess.Popen([sys.executable, "-c", child_code, sys.argv[1]])
                while True:
                    time.sleep(0.05)
                """
            )
            process = subprocess.Popen(
                [sys.executable, "-c", parent_source, str(child_ready)],
                start_new_session=True,
            )
            try:
                deadline = time.monotonic() + 3
                while not child_ready.is_file() and time.monotonic() < deadline:
                    time.sleep(0.02)
                self.assertTrue(child_ready.is_file())
                result = stability.terminate_process(process, "signal", 0.2)
                self.assertTrue(result["descendant_cleanup_required"])
                self.assertTrue(result["descendant_force_killed"])
            finally:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except (ProcessLookupError, PermissionError):
                    # terminate_process already proved and reported descendant
                    # cleanup above. Under a test runner, the dead parent's PID
                    # may be recycled for a process group we don't own before
                    # this defensive finalizer runs.
                    pass
                process.wait(timeout=3)

    def test_local_multiplayer_uses_only_virtual_sdl_controller_input(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args = type(
                "Args",
                (),
                {
                    "mode": "local-multiplayer",
                    "players": 2,
                    "runtime_dir": root / "runtime",
                    "probe_draws_per_command_buffer": 128,
                },
            )()
            old_environment = {
                name: os.environ.get(name)
                for name in (
                    "GOLDENEYE_AUTO_START",
                    "GOLDENEYE_AUTO_MISSION",
                    "GOLDENEYE_TEST_MENU_TRACE",
                    "GOLDENEYE_TEST_CAPTURE_DAM_FRAME",
                    "REX_INPUT_TEST_HARNESS",
                )
            }
            os.environ["GOLDENEYE_AUTO_START"] = "periodic"
            os.environ["GOLDENEYE_AUTO_MISSION"] = "dam"
            os.environ["GOLDENEYE_TEST_CAPTURE_DAM_FRAME"] = "unexpected"
            os.environ["REX_INPUT_TEST_HARNESS"] = "unexpected"
            try:
                environment = stability.build_environment(
                    args,
                    root / "cycle",
                    virtual_gamepad_fd=17,
                )
            finally:
                for name, value in old_environment.items():
                    if value is None:
                        os.environ.pop(name, None)
                    else:
                        os.environ[name] = value
            self.assertEqual(environment["REX_INPUT_BACKEND"], "sdl")
            self.assertEqual(environment["REX_INPUT_TEST_HARNESS"], "1")
            self.assertEqual(environment["REX_TEST_VIRTUAL_GAMEPADS"], "2")
            self.assertEqual(environment["REX_TEST_VIRTUAL_GAMEPAD_FD"], "17")
            self.assertEqual(environment["GOLDENEYE_TEST_MENU_TRACE"], "1")
            self.assertEqual(environment["GOLDENEYE_TEST_MULTIPLAYER_TRACE"], "1")
            self.assertEqual(
                environment[
                    "GOLDENEYE_METAL_PROBE_DRAWS_PER_COMMAND_BUFFER"
                ],
                "128",
            )
            self.assertNotIn("GOLDENEYE_AUTO_START", environment)
            self.assertNotIn("GOLDENEYE_AUTO_MISSION", environment)
            self.assertNotIn("GOLDENEYE_TEST_CAPTURE_DAM_FRAME", environment)

            log_path = root / "raw.log"
            log_path.write_text("[vpad] READY pads=2\n", encoding="utf-8")

            def append_log(*lines):
                with log_path.open("a", encoding="utf-8") as stream:
                    stream.write("\n".join(lines) + "\n")

            read_fd, write_fd = os.pipe()
            os.set_blocking(read_fd, False)
            driver = stability.LocalMultiplayerInputDriver(2, write_fd)
            driver.advance(log_path, 10.0)
            with self.assertRaises(BlockingIOError):
                os.read(read_fd, 4096)

            append_log("[ge-test] menu state=5 name=title-ready joined=0")
            driver.advance(log_path, 10.3)
            self.assertEqual(
                os.read(read_fd, 4096).decode("ascii"),
                "PULSE_BUTTON 1 1 START 250\n",
            )

            append_log(
                "[vpad] ACK seq=1",
                "[ge-test] menu state=7 name=dossier joined=0",
            )
            driver.advance(log_path, 11.1)
            driver.advance(log_path, 11.9)
            self.assertEqual(
                os.read(read_fd, 4096).decode("ascii"),
                "PULSE_AXIS 2 1 LY 32767 80\n",
            )

            append_log("[vpad] ACK seq=2")
            driver.advance(log_path, 12.6)
            self.assertEqual(
                os.read(read_fd, 4096).decode("ascii"),
                "PULSE_BUTTON 3 1 SOUTH 250\n",
            )

            append_log(
                "[vpad] ACK seq=3",
                "[ge-test] menu state=27 name=multiplayer-modes joined=0",
            )
            driver.advance(log_path, 13.3)
            driver.advance(log_path, 14.1)
            self.assertEqual(
                os.read(read_fd, 4096).decode("ascii"),
                "PULSE_AXIS 4 1 LY -32768 80\n",
            )

            append_log("[vpad] ACK seq=4")
            driver.advance(log_path, 14.8)
            self.assertEqual(
                os.read(read_fd, 4096).decode("ascii"),
                "PULSE_BUTTON 5 1 SOUTH 250\n",
            )

            append_log(
                "[vpad] ACK seq=5",
                "[ge-test] menu state=15 name=create-local-game joined=1",
            )
            driver.advance(log_path, 15.1)
            driver.advance(log_path, 15.9)
            self.assertEqual(
                os.read(read_fd, 4096).decode("ascii"),
                "PULSE_BUTTON 6 2 START 250\n",
            )

            append_log(
                "[vpad] ACK seq=6",
                "[ge-test] menu state=15 name=create-local-game joined=2",
            )
            driver.advance(log_path, 16.7)
            self.assertEqual(
                os.read(read_fd, 4096).decode("ascii"),
                "PULSE_BUTTON 7 1 START 250\n",
            )
            append_log("[vpad] ACK seq=7")
            driver.advance(log_path, 17.5)
            driver.close()
            os.close(read_fd)
            self.assertFalse(driver.completed)
            self.assertEqual(driver.phase, "waiting-for-match-readiness")
            self.assertEqual(len(driver.sent), 7)

            profile_lines = [
                "[vpad] READY pads=2",
                *[f"[vpad] ACK seq={sequence}" for sequence in range(1, 8)],
                (
                    "[ge] local multiplayer ready level=7 players=2 "
                    "network=0 stable_polls=120"
                ),
                (
                    "[metal-profile] presenter attempts=1-64 sources=64 "
                    "unchanged_sources=0 drawable_nil=0 uploads=0 "
                    "upload_bytes=0 commits=64"
                ),
                (
                    "[metal-profile] window swaps=1-64 "
                    "elapsed_ns=1066666688 avg_frame_ns=16666667 fps=60.0"
                ),
                (
                    "[metal-profile] gpu-chain swaps=1-64 wptr_attempts=128 "
                    "wptr_accepted=128 wptr_rejected=0 ring_batches=64 "
                    "ring_commit_rejected=0"
                ),
                (
                    "[metal-profile] command swaps=1-64 event=draw calls=640 "
                    "avg_calls_per_swap=10 total_ns=64000000 "
                    "avg_ns_per_swap=1000000 max_call_ns=2000000 "
                    "max_swap_ns=3000000"
                ),
                (
                    "[metal-profile] command swaps=1-64 event=copy calls=64 "
                    "avg_calls_per_swap=1 total_ns=32000000 "
                    "avg_ns_per_swap=500000 max_call_ns=1000000 "
                    "max_swap_ns=2000000"
                ),
                (
                    "[metal-profile] command swaps=1-64 event=swap calls=64 "
                    "avg_calls_per_swap=1 total_ns=16000000 "
                    "avg_ns_per_swap=250000 max_call_ns=500000 "
                    "max_swap_ns=500000"
                ),
                (
                    "[metal-profile] command swaps=1-64 event=wait_reg_mem "
                    "calls=64 avg_calls_per_swap=1 total_ns=8000000 "
                    "avg_ns_per_swap=125000 max_call_ns=250000 "
                    "max_swap_ns=250000"
                ),
                (
                    "[metal-profile] command swaps=1-64 "
                    "event=texture_fallback_decode calls=0 avg_calls_per_swap=0 "
                    "total_ns=0 avg_ns_per_swap=0 max_call_ns=0 max_swap_ns=0"
                ),
                (
                    "[metal-profile] wait-reg-mem swaps=1-64 rank=1 "
                    "source=memory unmatched=0 timeouts=0"
                ),
            ]
            log_path.write_text("\n".join(profile_lines) + "\n", encoding="utf-8")
            ready, details = stability.readiness(
                "local-multiplayer", log_path, 40.0, 0.0, 0, 1, 2, 7, 0
            )
            self.assertTrue(ready, details)
            self.assertEqual(details["local_multiplayer"]["players"], 2)
            self.assertEqual(details["selected_profile_ranges"], [[1, 64]])

            ready, details = stability.readiness(
                "local-multiplayer", log_path, 40.0, 0.0, 0, 1, 2, 7, 64
            )
            self.assertFalse(ready)
            self.assertEqual(details["post_match_profile_windows"], 0)

            log_path.write_text(
                "\n".join(line for line in profile_lines if line != "[vpad] ACK seq=6")
                + "\n",
                encoding="utf-8",
            )
            ready, details = stability.readiness(
                "local-multiplayer", log_path, 40.0, 0.0, 0, 1, 2, 7, 0
            )
            self.assertFalse(ready)
            self.assertEqual(details["missing_virtual_gamepad_acks"], [6])

    def test_local_multiplayer_defaults_to_validated_128_draw_batch(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args = type(
                "Args",
                (),
                {
                    "mode": "local-multiplayer",
                    "players": 4,
                    "runtime_dir": root / "runtime",
                },
            )()
            environment = stability.build_environment(args, root / "cycle")
            self.assertEqual(
                environment["GOLDENEYE_METAL_PROBE_DRAWS_PER_COMMAND_BUFFER"],
                "128",
            )

    def test_stability_environment_scrubs_inherited_runtime_experiments(self):
        contaminants = {
            "GOLDENEYE_FORCE_PRESENTED_ON_VDSWAP": "1",
            "GOLDENEYE_METAL_STICKY_TEX": "1",
            "GOLDENEYE_METAL_FALLBACK_RESOLVE": "1",
            "GOLDENEYE_METAL_HEURISTIC_PRESENT": "1",
            "GOLDENEYE_METAL_MAGENTA_RESOLVE": "1",
            "GOLDENEYE_METAL_GPU_TILED_RESOLVE": "0",
            "GOLDENEYE_METAL_DUMP_FRAMES": "all",
            "GOLDENEYE_METAL_DUMP_SHADERS": "1",
            "GOLDENEYE_METAL_PIPELINE_PROBE": "1",
            "GOLDENEYE_METAL_HOST_RT_SOLID_TEST": "1",
            "GOLDENEYE_METAL_VDSWAP_SCAVENGE": "1",
            "GOLDENEYE_TRACE_FRAME": "1",
            "GOLDENEYE_TRACE_IO": "1",
            "GOLDENEYE_FUTURE_DIAGNOSTIC": "1",
            "REX_INPUT_TEST_HARNESS": "1",
            "REX_METAL_EXACT_OUTPUT_MERGER": "true",
            "REX_FUTURE_DIAGNOSTIC": "1",
            "SPDLOG_LEVEL": "off",
        }
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args = SimpleNamespace(
                mode="menu", players=2, runtime_dir=root / "runtime"
            )
            with mock.patch.dict(os.environ, contaminants, clear=False):
                environment = stability.build_environment(args, root / "cycle")
        goldeneye_keys = {
            key for key in environment if key.startswith("GOLDENEYE_")
        }
        self.assertEqual(
            goldeneye_keys,
            {
                "GOLDENEYE_AUTO_START",
                "GOLDENEYE_LAUNCHER_BYPASS_UI",
                "GOLDENEYE_METAL_PROFILE",
            },
        )
        self.assertNotIn("REX_FUTURE_DIAGNOSTIC", environment)
        self.assertNotIn("REX_INPUT_TEST_HARNESS", environment)
        self.assertNotIn("REX_METAL_EXACT_OUTPUT_MERGER", environment)
        self.assertNotIn("SPDLOG_LEVEL", environment)

    def test_exact_output_merger_diagnostic_is_explicit_and_proven(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args = SimpleNamespace(
                mode="dam",
                players=2,
                runtime_dir=root / "runtime",
                exact_output_merger=True,
                post_ready_soak_seconds=10.0,
            )
            cycle_root = root / "cycle"
            environment = stability.build_environment(args, cycle_root)
            self.assertEqual(
                environment["REX_METAL_EXACT_OUTPUT_MERGER"], "true"
            )

            log_path = cycle_root / "raw.log"
            log_path.write_text(
                "[metal] exact output-merger draw enqueued#1 draw=4\n"
                "[metal] exact output-merger draw enqueued#256 draw=300\n",
                encoding="utf-8",
            )
            evidence = stability.exact_output_merger_evidence(log_path)
            self.assertEqual(
                evidence,
                {
                    "validated": True,
                    "logged_draw_count": 2,
                    "highest_logged_draw_index": 256,
                },
            )
            eligibility = stability.release_eligibility(args)
            self.assertFalse(eligibility["eligible"])
            self.assertIn(
                "experimental exact output-merger route was enabled",
                eligibility["reasons"],
            )

    def test_multiplayer_profile_gate_rejects_a_missing_post_match_cohort(self):
        with tempfile.TemporaryDirectory() as temporary:
            log_path = Path(temporary) / "raw.log"
            control = (
                "[vpad] READY pads=2",
                "[ge] local multiplayer ready level=7 players=2 "
                "network=0 stable_polls=120",
            )
            log_path.write_text(
                "\n".join(
                    control
                    + profile_window_lines(start=1)
                    + profile_window_lines(start=65)
                    + profile_window_lines(start=193)
                )
                + "\n",
                encoding="utf-8",
            )
            ready, details = stability.readiness(
                "local-multiplayer", log_path, 40.0, 0.0, 0, 2, 2, 0, 64
            )
            self.assertFalse(ready)
            self.assertEqual(details["post_match_profile_windows"], 1)
            self.assertEqual(details["selected_profile_ranges"], [[65, 128]])

            log_path.write_text(
                "\n".join(
                    control
                    + profile_window_lines(start=1)
                    + profile_window_lines(start=65)
                    + profile_window_lines(start=129)
                )
                + "\n",
                encoding="utf-8",
            )
            ready, details = stability.readiness(
                "local-multiplayer", log_path, 40.0, 0.0, 0, 2, 2, 0, 64
            )
            self.assertTrue(ready, details)
            self.assertEqual(details["selected_profile_ranges"], [[65, 128], [129, 192]])

    def test_dam_gameplay_profile_gate_cannot_use_pre_anchor_windows(self):
        with tempfile.TemporaryDirectory() as temporary:
            log_path = Path(temporary) / "raw.log"
            log_path.write_text(
                "\n".join(
                    profile_window_lines(start=1) + profile_window_lines(start=65)
                )
                + "\n",
                encoding="utf-8",
            )
            ready, details = stability.readiness(
                "dam-gameplay", log_path, 40.0, 0.0, 0, 1, 2, 0, 128
            )
            self.assertFalse(ready)
            self.assertEqual(details["post_dam_profile_windows"], 0)

            with log_path.open("a", encoding="utf-8") as stream:
                stream.write("\n".join(profile_window_lines(start=129)) + "\n")
            ready, details = stability.readiness(
                "dam-gameplay", log_path, 40.0, 0.0, 0, 1, 2, 0, 128
            )
            self.assertTrue(ready, details)
            self.assertEqual(details["selected_profile_ranges"], [[129, 192]])

    def test_dam_gameplay_uses_one_virtual_controller_and_real_mission_navigation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args = type(
                "Args",
                (),
                {
                    "mode": "dam-gameplay",
                    "players": 2,
                    "runtime_dir": root / "runtime",
                },
            )()
            environment = stability.build_environment(
                args,
                root / "cycle",
                virtual_gamepad_fd=17,
            )
            self.assertEqual(environment["REX_INPUT_BACKEND"], "sdl")
            self.assertEqual(environment["REX_INPUT_TEST_HARNESS"], "1")
            self.assertEqual(environment["REX_TEST_VIRTUAL_GAMEPADS"], "1")
            self.assertEqual(environment["REX_TEST_VIRTUAL_GAMEPAD_FD"], "17")
            self.assertEqual(environment["GOLDENEYE_AUTO_START"], "menu")
            self.assertEqual(environment["GOLDENEYE_AUTO_MISSION"], "dam")
            self.assertEqual(environment["GOLDENEYE_TEST_GAMEPLAY_TRACE"], "1")
            self.assertNotIn("GOLDENEYE_TEST_CAPTURE_DAM_FRAME", environment)
            self.assertNotIn("GOLDENEYE_TEST_HOST_PAUSE", environment)
            self.assertNotIn("GOLDENEYE_TEST_MENU_TRACE", environment)

            args.capture_gpu_frame = True
            capture_environment = stability.build_environment(
                args,
                root / "capture-cycle",
                virtual_gamepad_fd=19,
            )
            self.assertEqual(
                capture_environment["GOLDENEYE_TEST_CAPTURE_DAM_FRAME"], "1"
            )
            self.assertNotIn("GOLDENEYE_TEST_HOST_PAUSE", capture_environment)

            args.capture_gpu_frame = False
            args.validate_host_pause = True
            pause_environment = stability.build_environment(
                args,
                root / "host-pause-cycle",
                virtual_gamepad_fd=23,
            )
            self.assertEqual(pause_environment["GOLDENEYE_TEST_HOST_PAUSE"], "1")
            self.assertNotIn(
                "GOLDENEYE_TEST_CAPTURE_DAM_FRAME", pause_environment
            )

    def test_host_pause_runtime_failure_is_a_fatal_log_marker(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "raw.log").write_text(
                "[ge-test] host-pause failed phase=observing-pause "
                "reason=world-advanced-while-paused\n",
                encoding="utf-8",
            )
            matches = stability.detect_fatal_logs(root)
            self.assertEqual(len(matches), 1, matches)
            self.assertIn("[ge-test] host-pause failed", matches[0])

    def test_gpu_capture_evidence_requires_matching_reader_validated_marker(self):
        with tempfile.TemporaryDirectory() as temporary:
            log_path = Path(temporary) / "raw.log"
            log_path.write_text(
                "\n".join(
                    (
                        "[ge-test] gpu-capture requested token=7 dam_ready=1 "
                        "frames_progressed=1 presents_progressed=1",
                        "[ge-test] gpu-capture validated token=7 state=Complete "
                        "bytes=4096 frames=1 private=1 partial=0",
                    )
                )
                + "\n",
                encoding="utf-8",
            )
            evidence = stability.gpu_capture_evidence(log_path)
            self.assertTrue(evidence["requested"])
            self.assertTrue(evidence["validated"])
            self.assertEqual(evidence["request_token"], 7)
            self.assertEqual(evidence["byte_count"], 4096)
            self.assertEqual(evidence["frame_count"], 1)
            self.assertTrue(evidence["private_permissions"])
            self.assertTrue(evidence["partial_absent"])

            log_path.write_text(
                "[ge-test] gpu-capture requested token=7 dam_ready=1 "
                "frames_progressed=1 presents_progressed=1\n"
                "[ge-test] gpu-capture validated token=8 state=Complete "
                "bytes=4096 frames=1 private=1 partial=0\n",
                encoding="utf-8",
            )
            self.assertFalse(
                stability.gpu_capture_evidence(log_path)["validated"]
            )

            with log_path.open("a", encoding="utf-8") as stream:
                stream.write(
                    "[ge-test] gpu-capture failed token=7 state=Failed "
                    "validation=0\n"
                )
            evidence = stability.gpu_capture_evidence(log_path)
            self.assertFalse(evidence["validated"])
            self.assertEqual(evidence["failure_state"], "Failed")
            self.assertTrue(stability.detect_fatal_logs(Path(temporary)))

    def test_private_gpu_capture_is_strictly_replayed_twice_with_identical_rgba(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cycle_root = root / "cycle"
            trace_path = cycle_root / stability.TRACE_CAPTURE_RELATIVE_PATH
            trace_path.parent.mkdir(parents=True)
            trace_path.write_bytes(b"private deterministic trace")
            replay = make_fake_trace_replay(root / "trace_dump_metal")

            report = stability.run_capture_replay_gate(
                trace_path,
                replay,
                cycle_root,
                None,
                expected_trace_bytes=trace_path.stat().st_size,
                timeout_seconds=2.0,
            )

            self.assertEqual(report["status"], "pass", report)
            self.assertTrue(report["strict_deterministic_preflight"])
            self.assertFalse(report["best_effort"])
            self.assertEqual(len(report["runs"]), 2)
            self.assertTrue(report["comparison"]["dimensions_equal"])
            self.assertTrue(report["comparison"]["rgba_byte_counts_equal"])
            self.assertTrue(report["comparison"]["rgba_sha256_equal"])
            for run in report["runs"]:
                self.assertEqual(run["status"], "pass")
                self.assertEqual(run["command"][-1], "0")
                self.assertNotIn("--best-effort", run["command"])
                self.assertEqual(run["bmp"]["width"], 2)
                self.assertEqual(run["bmp"]["height"], 2)
                self.assertEqual(run["rgba"]["byte_count"], 16)
                self.assertEqual(run["rgba"]["expected_byte_count"], 16)

    def test_gpu_capture_replay_rejects_nondeterministic_rgba(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cycle_root = root / "cycle"
            trace_path = cycle_root / stability.TRACE_CAPTURE_RELATIVE_PATH
            trace_path.parent.mkdir(parents=True)
            trace_path.write_bytes(b"private deterministic trace")
            replay = make_fake_trace_replay(
                root / "trace_dump_metal", mismatch_second_run=True
            )

            report = stability.run_capture_replay_gate(
                trace_path,
                replay,
                cycle_root,
                None,
                expected_trace_bytes=trace_path.stat().st_size,
                timeout_seconds=2.0,
            )

            self.assertEqual(report["status"], "fail")
            self.assertFalse(report["comparison"]["rgba_sha256_equal"])
            self.assertIn(
                "Metal replay RGBA hashes differ between runs", report["failures"]
            )

    def test_gpu_capture_replay_timeout_is_bounded_and_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cycle_root = root / "cycle"
            trace_path = cycle_root / stability.TRACE_CAPTURE_RELATIVE_PATH
            trace_path.parent.mkdir(parents=True)
            trace_path.write_bytes(b"private deterministic trace")
            replay = make_fake_trace_replay(
                root / "trace_dump_metal", sleep_seconds=0.5
            )

            started = time.monotonic()
            report = stability.run_capture_replay_gate(
                trace_path,
                replay,
                cycle_root,
                None,
                expected_trace_bytes=trace_path.stat().st_size,
                timeout_seconds=0.05,
            )

            self.assertLess(time.monotonic() - started, 1.0)
            self.assertEqual(report["status"], "fail")
            self.assertTrue(all(run["timed_out"] for run in report["runs"]))
            self.assertTrue(
                any("exceeded 0.05 seconds" in item for item in report["failures"])
            )

    def test_gpu_capture_replay_failure_is_persisted_and_fails_the_cycle(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = (
                "#!/usr/bin/env python3\n"
                "import os, signal, sys, time\n"
                "from pathlib import Path\n"
                "signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))\n"
                "payload = b'private deterministic trace'\n"
                "capture = Path(os.environ['REX_USER_DATA_ROOT']) / "
                "'GPU Captures' / 'latest.xtr'\n"
                "capture.parent.mkdir(mode=0o700, parents=True)\n"
                "capture.write_bytes(payload)\n"
                "capture.chmod(0o600)\n"
                "print('[ge] GOLDENEYE_AUTO_START=menu injecting Start', flush=True)\n"
                "print('[ge-test] gpu-capture requested token=9 dam_ready=1 "
                "frames_progressed=1 presents_progressed=1', flush=True)\n"
                "print(f'[ge-test] gpu-capture validated token=9 state=Complete "
                "bytes={len(payload)} frames=1 private=1 partial=0', flush=True)\n"
                + profile_emitter_source()
                + "\nwhile True:\n    time.sleep(0.05)\n"
            )
            replay = make_fake_trace_replay(
                root / "trace_dump_metal", mismatch_second_run=True
            )
            args, suite = self._cycle_fixture(
                root,
                source,
                capture_gpu_frame=True,
                trace_replay_executable=replay,
                trace_replay_timeout=2.0,
                post_ready_soak_seconds=0.0,
            )

            cycle = stability.run_cycle(args, suite, 1)
            stored = json.loads(
                (suite / "cycle-001/cycle.json").read_text(encoding="utf-8")
            )

            self.assertEqual(cycle["status"], "fail", cycle)
            self.assertEqual(stored["gpu_replay"]["status"], "fail")
            self.assertEqual(len(stored["gpu_replay"]["runs"]), 2)
            self.assertTrue(
                any(
                    "GPU capture replay: Metal replay RGBA hashes differ"
                    in failure
                    for failure in stored["failures"]
                )
            )

    def test_local_multiplayer_stops_on_rejected_virtual_input(self):
        with tempfile.TemporaryDirectory() as temporary:
            log_path = Path(temporary) / "raw.log"
            log_path.write_text(
                "\n".join(
                    (
                        "[vpad] READY pads=2",
                        (
                            "[vpad] REJECT reason=invalid pulse duration "
                            "command=PULSE_AXIS 2 1 LY 32767 0"
                        ),
                        "[vpad] ACK seq=3",
                    )
                )
                + "\n",
                encoding="utf-8",
            )
            read_fd, write_fd = os.pipe()
            driver = stability.LocalMultiplayerInputDriver(2, write_fd)
            try:
                driver.last_sequence = 3
                driver.advance(log_path, 1.0)
                self.assertEqual(
                    driver.error,
                    "virtual-gamepad command 2 was rejected",
                )
                self.assertFalse(driver.completed)
                self.assertFalse(driver._command_finished(10.0))
            finally:
                driver.close()
                os.close(read_fd)

    def test_post_ready_multiplayer_heartbeats_require_fresh_acks(self):
        with tempfile.TemporaryDirectory() as temporary:
            log_path = Path(temporary) / "raw.log"
            log_path.write_text("[vpad] READY pads=2\n", encoding="utf-8")
            read_fd, write_fd = os.pipe()
            os.set_blocking(read_fd, False)
            driver = stability.LocalMultiplayerInputDriver(2, write_fd)
            driver.ready_elapsed = 1.0
            try:
                driver.begin_post_ready_soak(10.0, heartbeat_enabled=True)
                self.assertEqual(
                    os.read(read_fd, 4096).decode("ascii"),
                    "RESET 1 1\nRESET 2 2\n",
                )

                with log_path.open("a", encoding="utf-8") as stream:
                    stream.write("[vpad] ACK seq=1\n[vpad] ACK seq=2\n")
                driver.advance(log_path, 10.5)
                driver.finish_post_ready_soak(log_path, 10.5)
                self.assertIsNone(driver.error)
                with self.assertRaises(BlockingIOError):
                    os.read(read_fd, 4096)

                driver.advance(log_path, 11.1)
                self.assertEqual(
                    os.read(read_fd, 4096).decode("ascii"),
                    "RESET 3 1\nRESET 4 2\n",
                )
                with log_path.open("a", encoding="utf-8") as stream:
                    stream.write("[vpad] ACK seq=3\n")
                driver.finish_post_ready_soak(log_path, 11.5)
                self.assertEqual(
                    driver.error,
                    ("post-ready virtual-gamepad heartbeat was not acknowledged: 4"),
                )
                self.assertEqual(driver.soak_heartbeat_batches, [[1, 2], [3, 4]])
            finally:
                driver.close()
                os.close(read_fd)

    def test_binary_capability_scan_handles_chunk_boundaries(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "binary"
            marker = b"REX_TEST_VIRTUAL_GAMEPADS"
            path.write_bytes(b"x" * (1024 * 1024 - 3) + marker)
            self.assertTrue(stability.binary_contains(path, marker))
            self.assertFalse(
                stability.binary_contains(path, b"GOLDENEYE_TEST_MENU_TRACE")
            )

    def test_cycle_uses_private_state_and_collects_artifacts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "fake-game.py"
            executable.write_text(
                textwrap.dedent(
                    """\
                    #!/usr/bin/env python3
                    import os, signal, sys, time
                    user = os.environ["REX_USER_DATA_ROOT"]
                    os.makedirs(user, exist_ok=True)
                    open(os.path.join(user, "state-root-seen.txt"), "w").write(
                        "\\n".join((user, os.environ["REX_CACHE_PATH"], os.environ["HOME"]))
                    )
                    for index in range(2):
                        start = index * 64 + 1
                        end = start + 63
                        print(f"[metal-profile] presenter attempts={start}-{end} sources=64 unchanged_sources=0 drawable_nil=0 uploads=0 upload_bytes=0 commits=64", flush=True)
                        print(f"[metal-profile] window swaps={start}-{end} elapsed_ns=1066666688 avg_frame_ns=16666667 fps=60.0", flush=True)
                        print(f"[metal-profile] gpu-chain swaps={start}-{end} wptr_attempts=128 wptr_accepted=128 wptr_rejected=0 ring_batches=64 ring_commit_rejected=0", flush=True)
                        print(f"[metal-profile] command swaps={start}-{end} event=copy calls=768 avg_calls_per_swap=12 total_ns=32000000 avg_ns_per_swap=500000 max_call_ns=1000000 max_swap_ns=2000000", flush=True)
                        print(f"[metal-profile] command swaps={start}-{end} event=draw calls=640 avg_calls_per_swap=10 total_ns=64000000 avg_ns_per_swap=1000000 max_call_ns=2000000 max_swap_ns=3000000", flush=True)
                        print(f"[metal-profile] command swaps={start}-{end} event=swap calls=64 avg_calls_per_swap=1 total_ns=16000000 avg_ns_per_swap=250000 max_call_ns=500000 max_swap_ns=500000", flush=True)
                        print(f"[metal-profile] command swaps={start}-{end} event=wait_reg_mem calls=64 avg_calls_per_swap=1 total_ns=8000000 avg_ns_per_swap=125000 max_call_ns=250000 max_swap_ns=250000", flush=True)
                        print(f"[metal-profile] command swaps={start}-{end} event=texture_fallback_decode calls=0 avg_calls_per_swap=0 total_ns=0 avg_ns_per_swap=0 max_call_ns=0 max_swap_ns=0", flush=True)
                        print(f"[metal-profile] wait-reg-mem swaps={start}-{end} rank=1 source=memory unmatched=0 timeouts=0", flush=True)
                    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
                    while True:
                        time.sleep(0.05)
                    """
                ),
                encoding="utf-8",
            )
            executable.chmod(0o755)
            runtime = root / "runtime"
            runtime.mkdir()
            (runtime / "librexruntime.dylib").touch()
            game_data = root / "game-data"
            game_data.mkdir()
            (game_data / "default.xex").write_bytes(b"fixture")
            suite = root / "suite"
            command = [
                sys.executable,
                str(ROOT / "tools/stability-cycle.py"),
                "--cycles",
                "2",
                "--mode",
                "dam",
                "--executable",
                str(executable),
                "--runtime-dir",
                str(runtime),
                "--game-data",
                str(game_data),
                "--output",
                str(suite),
                "--ready-timeout",
                "5",
                "--post-ready-soak-seconds",
                "0.15",
                "--poll-seconds",
                "0.05",
                "--warmup-windows",
                "0",
                "--observe-windows",
                "2",
                "--shutdown-timeout",
                "2",
                "--quit-method",
                "signal",
                "--skip-metadata",
                "--allow-stale-build",
                "--no-soak-progress-gate",
            ]
            environment = os.environ.copy()
            environment["PYTHONDONTWRITEBYTECODE"] = "1"
            result = subprocess.run(
                command,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=environment,
            )
            detail = result.stdout + result.stderr
            if (suite / "summary.txt").is_file():
                detail += (suite / "summary.txt").read_text(encoding="utf-8")
            self.assertEqual(result.returncode, 0, detail)
            self.assertIn("DIAGNOSTIC PASS:", result.stdout)
            self.assertNotIn("\nPASS:", "\n" + result.stdout)
            summary = json.loads((suite / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["status"], "diagnostic-pass")
            self.assertEqual(summary["diagnostic_status"], "pass")
            self.assertFalse(summary["release_eligible"])
            self.assertTrue(summary["release_ineligibility_reasons"])
            self.assertTrue(
                all(
                    cycle["status"] == "diagnostic-pass"
                    and not cycle["release_eligible"]
                    for cycle in summary["cycles"]
                )
            )
            self.assertEqual(summary["cycles_passed"], 2)
            self.assertEqual(summary["post_ready_soak_cycles_completed"], 2)
            self.assertTrue(summary["isolated_state"])
            self.assertIn(
                ("requested post-ready soak: 0.15 seconds; completed: 2/2 cycles"),
                (suite / "summary.txt").read_text(encoding="utf-8"),
            )
            for number in (1, 2):
                cycle_root = suite / f"cycle-{number:03d}"
                state_root = cycle_root / "user-data"
                observed_roots = (
                    (state_root / "state-root-seen.txt")
                    .read_text(encoding="utf-8")
                    .splitlines()
                )
                self.assertEqual(observed_roots[0], str(state_root.resolve()))
                self.assertEqual(
                    observed_roots[1], str((cycle_root / "cache").resolve())
                )
                self.assertEqual(
                    observed_roots[2], str((cycle_root / "home").resolve())
                )
                self.assertTrue((cycle_root / "raw.log").is_file())
                self.assertTrue((cycle_root / "cycle.json").is_file())
                cycle = json.loads(
                    (cycle_root / "cycle.json").read_text(encoding="utf-8")
                )
                self.assertTrue(cycle["post_ready_soak"]["completed"])
                self.assertEqual(cycle["post_ready_soak"]["requested_seconds"], 0.15)
                self.assertGreaterEqual(
                    cycle["post_ready_soak"]["elapsed_seconds"], 0.15
                )
            self.assertEqual(
                sorted(path.name for path in game_data.iterdir()), ["default.xex"]
            )

    def test_fatal_marker_before_readiness_aborts_without_timeout(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args, suite = self._cycle_fixture(
                root,
                """\
                #!/usr/bin/env python3
                import signal, sys, time
                signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
                print(
                    "GEWATCHDOG STALL: ring rpi=0x1 wpi=0x2 [PENDING]",
                    flush=True,
                )
                while True:
                    time.sleep(0.05)
                """,
                ready_timeout=5.0,
                post_ready_soak_seconds=2.0,
            )
            started = time.monotonic()
            cycle = stability.run_cycle(args, suite, 1)
            self.assertLess(time.monotonic() - started, 2.0)
            self.assertFalse(cycle["ready"])
            self.assertTrue(cycle["pre_ready_abort_log_matches"])
            self.assertTrue(
                any("GEWATCHDOG STALL" in failure for failure in cycle["failures"])
            )
            self.assertFalse(
                any(
                    "readiness was not reached" in failure
                    for failure in cycle["failures"]
                )
            )

            self.assertFalse(stability.write_suite_summary(suite, [cycle], args))
            summary = json.loads((suite / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["post_ready_soak_cycles_completed"], 0)
            self.assertIn(
                "completed: 0/1 cycles",
                (suite / "summary.txt").read_text(encoding="utf-8"),
            )

    def test_profile_violation_before_readiness_aborts_without_timeout(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args, suite = self._cycle_fixture(
                root,
                """\
                #!/usr/bin/env python3
                import signal, sys, time
                signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
                print(
                    "[metal-profile] presenter attempts=1-64 sources=64 "
                    "unchanged_sources=0 drawable_nil=0 uploads=1 "
                    "upload_bytes=4096 commits=64",
                    flush=True,
                )
                while True:
                    time.sleep(0.05)
                """,
                ready_timeout=5.0,
                post_ready_soak_seconds=2.0,
            )
            started = time.monotonic()
            cycle = stability.run_cycle(args, suite, 1)
            self.assertLess(time.monotonic() - started, 2.0)
            self.assertFalse(cycle["ready"])
            self.assertTrue(cycle["pre_ready_profile_failures"])
            self.assertTrue(
                any("full-frame uploads=1" in failure for failure in cycle["failures"])
            )
            self.assertFalse(
                any(
                    "readiness was not reached" in failure
                    for failure in cycle["failures"]
                )
            )

    def test_delayed_trailing_ledgers_complete_without_false_early_abort(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            emitter = profile_emitter_source().splitlines()
            source = (
                "#!/usr/bin/env python3\n"
                "import signal, sys, time\n"
                "signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))\n"
                "print('[ge] GOLDENEYE_AUTO_START=menu injecting Start', flush=True)\n"
                + "\n".join(emitter[:3])
                + "\ntime.sleep(0.25)\n"
                + "\n".join(emitter[3:])
                + "\nwhile True:\n    time.sleep(0.05)\n"
            )
            args, suite = self._cycle_fixture(
                root,
                source,
                ready_timeout=3.0,
                post_ready_soak_seconds=0.0,
            )
            started = time.monotonic()
            cycle = stability.run_cycle(args, suite, 1)
            duration = time.monotonic() - started
            self.assertGreaterEqual(duration, 0.20)
            self.assertLess(duration, 2.0)
            self.assertTrue(cycle["ready"], cycle["failures"])
            self.assertEqual(cycle["pre_ready_profile_failures"], [])
            self.assertTrue(cycle["final_profile_passed"])

    def test_default_performance_gate_fails_slow_gameplay_and_override_passes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = (
                "#!/usr/bin/env python3\n"
                "import signal, sys, time\n"
                "signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))\n"
                + profile_emitter_source(fps=20.0, frame_ns=50_000_000)
                + "\nwhile True:\n    time.sleep(0.05)\n"
            )
            args, suite = self._cycle_fixture(
                root,
                source,
                mode="dam",
                ready_timeout=2.0,
                post_ready_soak_seconds=0.0,
            )
            cycle = stability.run_cycle(args, suite, 1)
            self.assertFalse(cycle["final_profile_passed"])
            self.assertTrue(
                any("mean FPS 20.000" in item for item in cycle["final_profile_failures"])
            )

            args.min_mean_fps = 15.0
            args.max_window_p99_ms = 60.0
            args.disable_performance_gates = False
            suite_override = root / "suite-override"
            suite_override.mkdir()
            cycle = stability.run_cycle(args, suite_override, 1)
            self.assertEqual(cycle["status"], "diagnostic-pass", cycle["failures"])
            self.assertFalse(cycle["release_eligible"])
            self.assertTrue(cycle["final_profile_passed"])

    def test_wait_and_dummy_failures_match_profile_artifacts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            dummy = (
                "[metal] probe texture dummy#1 ps shader=5d448681ef02a235 "
                "binding=0 fetch=0 type=2 fmt=6 dim=1 "
                "dwords=80024802 1ebfe086 03ffdfff 00800c14 00000000 00000200"
            )
            source = (
                "#!/usr/bin/env python3\n"
                "import signal, sys, time\n"
                "signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))\n"
                + profile_emitter_source(wait_unmatched=1)
                + f"\nprint({dummy!r}, flush=True)\n"
                + "while True:\n    time.sleep(0.05)\n"
            )
            args, suite = self._cycle_fixture(
                root,
                source,
                mode="dam",
                ready_timeout=2.0,
                post_ready_soak_seconds=0.0,
            )
            cycle = stability.run_cycle(args, suite, 1)
            summary = json.loads(
                (suite / "cycle-001/profile/summary.json").read_text(encoding="utf-8")
            )
            self.assertFalse(cycle["final_profile_passed"])
            self.assertEqual(cycle["final_profile_failures"], summary["failures"])
            self.assertTrue(any("WAIT_REG_MEM unmatched=1" in item for item in summary["failures"]))
            self.assertTrue(any("dummy texture bindings observed" in item for item in summary["failures"]))

    def test_static_soak_fails_without_new_renderer_progress(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = (
                "#!/usr/bin/env python3\n"
                "import signal, sys, time\n"
                "signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))\n"
                "print('[ge] GOLDENEYE_AUTO_START=menu injecting Start', flush=True)\n"
                + profile_emitter_source()
                + "\nwhile True:\n    time.sleep(0.05)\n"
            )
            args, suite = self._cycle_fixture(
                root,
                source,
                post_ready_soak_seconds=0.2,
            )
            cycle = stability.run_cycle(args, suite, 1)
            self.assertTrue(cycle["ready"])
            self.assertTrue(cycle["post_ready_soak"]["completed"])
            self.assertTrue(
                any("no new complete Metal profile window" in item for item in cycle["failures"])
            )

    def test_native_mode_requires_a_real_visible_host_window(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = (
                "#!/usr/bin/env python3\n"
                "import signal, sys, time\n"
                "signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))\n"
                "print('[ge] GOLDENEYE_AUTO_START=menu injecting Start', flush=True)\n"
                + profile_emitter_source()
                + "\nwhile True:\n    time.sleep(0.05)\n"
            )
            args, suite = self._cycle_fixture(
                root,
                source,
                quit_method="native",
                # Process startup can overlap a renderer build on CI or a
                # developer machine. Keep this focused on the window proof,
                # not sub-second scheduler timing.
                ready_timeout=1.0,
                post_ready_soak_seconds=0.0,
            )

            def app_control(_root, command, _pid):
                if command == "window-id":
                    raise stability.rendering.ImageError("no visible top-level window")
                return ""

            with mock.patch.object(
                stability.rendering, "macos_app_control", side_effect=app_control
            ):
                cycle = stability.run_cycle(args, suite, 1)
            self.assertFalse(cycle["ready"])
            self.assertIsNone(cycle["host_window_id"])
            self.assertIn("no visible top-level window", cycle["host_window_error"])
            self.assertTrue(
                any("no visible GoldenEye host window" in item for item in cycle["failures"])
            )

    def test_exit_during_soak_skips_capture(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args, suite = self._cycle_fixture(
                root,
                """\
                #!/usr/bin/env python3
                import time
                print(
                    "[ge] GOLDENEYE_AUTO_START=menu injecting Start",
                    flush=True,
                )
                print(
                    "[metal-profile] presenter attempts=1-64 sources=64 "
                    "unchanged_sources=0 drawable_nil=0 uploads=0 "
                    "upload_bytes=0 commits=64",
                    flush=True,
                )
                print(
                    "[metal-profile] window swaps=1-64 "
                    "elapsed_ns=1066666688 avg_frame_ns=16666667 fps=60.0",
                    flush=True,
                )
                print(
                    "[metal-profile] gpu-chain swaps=1-64 wptr_attempts=128 "
                    "wptr_accepted=128 wptr_rejected=0 ring_batches=64 "
                    "ring_commit_rejected=0",
                    flush=True,
                )
                for event, calls in (
                    ("draw", 640),
                    ("copy", 64),
                    ("swap", 64),
                    ("wait_reg_mem", 64),
                    ("texture_fallback_decode", 0),
                ):
                    print(
                        f"[metal-profile] command swaps=1-64 event={event} "
                        f"calls={calls} avg_calls_per_swap=1 total_ns=64000000 "
                        "avg_ns_per_swap=1000000 max_call_ns=2000000 "
                        "max_swap_ns=3000000",
                        flush=True,
                    )
                time.sleep(0.15)
                """,
                capture=True,
                post_ready_soak_seconds=1.0,
            )
            with mock.patch.object(
                stability.rendering, "capture_window"
            ) as capture_window:
                cycle = stability.run_cycle(args, suite, 1)
            capture_window.assert_not_called()
            self.assertTrue(cycle["ready"])
            self.assertEqual(
                cycle["capture_skipped_reason"],
                "application exited before capture",
            )
            self.assertTrue(
                any(
                    "during post-ready soak" in failure for failure in cycle["failures"]
                )
            )
            self.assertFalse(
                any("capture failed" in failure for failure in cycle["failures"])
            )

    def test_failure_logged_during_shutdown_fails_final_profile(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "fake-game.py"
            executable.write_text(
                textwrap.dedent(
                    """\
                    #!/usr/bin/env python3
                    import signal, sys, time
                    def stop(*_):
                        print("GPU tiled resolve failed during shutdown", flush=True)
                        sys.exit(0)
                    signal.signal(signal.SIGTERM, stop)
                    for index in range(2):
                        start = index * 64 + 1
                        end = start + 63
                        print(f"[metal-profile] presenter attempts={start}-{end} sources=64 unchanged_sources=0 drawable_nil=0 uploads=0 upload_bytes=0 commits=64", flush=True)
                        print(f"[metal-profile] window swaps={start}-{end} elapsed_ns=1066666688 avg_frame_ns=16666667 fps=60.0", flush=True)
                        print(f"[metal-profile] gpu-chain swaps={start}-{end} wptr_attempts=128 wptr_accepted=128 wptr_rejected=0 ring_batches=64 ring_commit_rejected=0", flush=True)
                        for event, calls, average in (("draw", 640, 10), ("copy", 768, 12), ("swap", 64, 1), ("wait_reg_mem", 64, 1), ("texture_fallback_decode", 0, 0)):
                            print(f"[metal-profile] command swaps={start}-{end} event={event} calls={calls} avg_calls_per_swap={average} total_ns=64000000 avg_ns_per_swap=1000000 max_call_ns=2000000 max_swap_ns=3000000", flush=True)
                        print(f"[metal-profile] wait-reg-mem swaps={start}-{end} rank=1 source=memory unmatched=0 timeouts=0", flush=True)
                    while True:
                        time.sleep(0.05)
                    """
                ),
                encoding="utf-8",
            )
            executable.chmod(0o755)
            runtime = root / "runtime"
            runtime.mkdir()
            (runtime / "librexruntime.dylib").touch()
            game_data = root / "game-data"
            game_data.mkdir()
            (game_data / "default.xex").write_bytes(b"fixture")
            suite = root / "suite"
            result = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools/stability-cycle.py"),
                    "--cycles",
                    "1",
                    "--mode",
                    "dam",
                    "--executable",
                    str(executable),
                    "--runtime-dir",
                    str(runtime),
                    "--game-data",
                    str(game_data),
                    "--output",
                    str(suite),
                    "--ready-timeout",
                    "5",
                    "--post-ready-soak-seconds",
                    "0",
                    "--poll-seconds",
                    "0.05",
                    "--warmup-windows",
                    "0",
                    "--observe-windows",
                    "2",
                    "--shutdown-timeout",
                    "2",
                    "--quit-method",
                    "signal",
                    "--skip-metadata",
                    "--allow-stale-build",
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=15,
            )
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            cycle = json.loads(
                (suite / "cycle-001/cycle.json").read_text(encoding="utf-8")
            )
            self.assertFalse(cycle["final_profile_passed"])
            self.assertTrue(
                any(
                    "GPU tiled resolve failed" in match
                    for match in cycle["fatal_log_matches"]
                )
            )
            self.assertTrue(
                any(
                    "final Metal profile validation failed" in failure
                    for failure in cycle["failures"]
                )
            )

    def test_menu_failure_logged_after_readiness_fails_final_profile(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "fake-game.py"
            executable.write_text(
                textwrap.dedent(
                    """\
                    #!/usr/bin/env python3
                    import signal, sys, time
                    def stop(*_):
                        print("[metal] async probe summary fallbacks=1", flush=True)
                        sys.exit(0)
                    signal.signal(signal.SIGTERM, stop)
                    print("[metal-profile] presenter attempts=1-64 sources=64 unchanged_sources=0 drawable_nil=0 uploads=0 upload_bytes=0 commits=64", flush=True)
                    print("[ge] GOLDENEYE_AUTO_START=menu injecting Start", flush=True)
                    print("[metal-profile] window swaps=1-64 elapsed_ns=1066666688 avg_frame_ns=16666667 fps=60.0", flush=True)
                    print("[metal-profile] gpu-chain swaps=1-64 wptr_attempts=128 wptr_accepted=128 wptr_rejected=0 ring_batches=64 ring_commit_rejected=0", flush=True)
                    for event, calls, average in (("draw", 640, 10), ("copy", 64, 1), ("swap", 64, 1), ("wait_reg_mem", 64, 1), ("texture_fallback_decode", 0, 0)):
                        print(f"[metal-profile] command swaps=1-64 event={event} calls={calls} avg_calls_per_swap={average} total_ns=64000000 avg_ns_per_swap=1000000 max_call_ns=2000000 max_swap_ns=3000000", flush=True)
                    while True:
                        time.sleep(0.05)
                    """
                ),
                encoding="utf-8",
            )
            executable.chmod(0o755)
            runtime = root / "runtime"
            runtime.mkdir()
            (runtime / "librexruntime.dylib").touch()
            game_data = root / "game-data"
            game_data.mkdir()
            (game_data / "default.xex").write_bytes(b"fixture")
            suite = root / "suite"
            result = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools/stability-cycle.py"),
                    "--cycles",
                    "1",
                    "--mode",
                    "menu",
                    "--executable",
                    str(executable),
                    "--runtime-dir",
                    str(runtime),
                    "--game-data",
                    str(game_data),
                    "--output",
                    str(suite),
                    "--ready-timeout",
                    "5",
                    "--post-ready-soak-seconds",
                    "0",
                    "--menu-settle-seconds",
                    "0",
                    "--poll-seconds",
                    "0.05",
                    "--shutdown-timeout",
                    "2",
                    "--quit-method",
                    "signal",
                    "--skip-metadata",
                    "--allow-stale-build",
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=15,
            )
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            cycle = json.loads(
                (suite / "cycle-001/cycle.json").read_text(encoding="utf-8")
            )
            self.assertFalse(cycle["final_profile_passed"])
            self.assertTrue(
                any(
                    "fallbacks=1" in failure
                    for failure in cycle["final_profile_failures"]
                ),
                cycle["final_profile_failures"],
            )


if __name__ == "__main__":
    unittest.main()
