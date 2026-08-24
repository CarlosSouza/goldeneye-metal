#!/usr/bin/env python3

import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "gameplay_scenario", ROOT / "tools/gameplay_scenario.py"
)
assert SPEC and SPEC.loader
gameplay = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = gameplay
SPEC.loader.exec_module(gameplay)


def gameplay_sample(
    number: int,
    *,
    x: float = 10.0,
    yaw: float = 0.0,
    pause: int = 0,
    disabled: int = 0,
    rt: int = 0,
    ly: int = 0,
    rx: int = 0,
    weapon: int = 3,
    ammo_valid: int = 1,
    ammo: int = 7,
) -> str:
    return (
        f"[ge-test] gameplay sample={number} poll={number} "
        f"frame={100 + number} present={200 + number} "
        "player=0x83001000 coords=0x83002000 pos_valid=1 "
        f"pos=({x:.3f},20.000,30.000) camera=({yaw:.4f},0.0000) "
        f"weapon={weapon} ammo_valid={ammo_valid} ammo={ammo} "
        f"pause={pause} disabled={disabled} watch=0 buttons=0x0000 "
        f"lt=0 rt={rt} lx=0 ly={ly} rx={rx} ry=0"
    )


def local_pad_sample(
    number: int,
    slot: int,
    *,
    connected: int = 1,
    device: int = 102,
    rt: int = 0,
    ly: int = 0,
) -> str:
    return (
        f"[ge-test] local-pad sample={number} poll={number} "
        f"frame={100 + number} present={200 + number} "
        f"slot={slot} connected={connected} "
        f"device={device if connected else 0} buttons=0x0000 "
        f"lt=0 rt={rt} lx=0 ly={ly} rx=0 ry=0"
    )


def local_player_sample(
    number: int,
    slot: int,
    *,
    player_valid: int = 1,
    x: float = 10.0,
    yaw: float = 0.0,
    ammo: int = 7,
) -> str:
    return (
        f"[ge-test] local-player sample={number} poll={number} "
        f"frame={100 + number} present={200 + number} slot={slot} "
        f"player=0x{0x83000000 + slot * 0x1000 if player_valid else 0:08X} "
        f"player_valid={player_valid} "
        f"coords=0x{0x84000000 + slot * 0x1000 if player_valid else 0:08X} "
        f"pos_valid={player_valid} pos=({x:.3f},20.000,30.000) "
        f"camera_valid={player_valid} camera=({yaw:.4f},0.0000) "
        f"weapon_valid={player_valid} weapon={3 if player_valid else 0} "
        f"ammo_valid={player_valid} ammo={ammo if player_valid else 0} "
        "pause=0 disabled=0 watch=0"
    )


HOST_PAUSE_EVENTS = (
    "[ge-test] host-pause open-request queued=1",
    "[ge-test] host-pause frozen open_generation=3 samples=3 "
    "duration_ms=1000 frame_delta=2 present_delta=2 world_stable=1 "
    "ui_open=1 owned=1",
    "[ge-test] host-pause close-request open_generation=3 queued=1",
    "[ge-test] host-pause resumed open_generation=3 resume_generation=4 "
    "samples=2 duration_ms=500 frame_delta=1 present_delta=1 "
    "ui_closed=1 pause_released=1 input_ready=1",
)


class GameplayScenarioTest(unittest.TestCase):
    def test_host_pause_parser_requires_exact_complete_sequence(self):
        evidence = gameplay.parse_host_pause_evidence(
            "\n".join(HOST_PAUSE_EVENTS)
        )
        self.assertTrue(evidence["validated"], evidence)
        self.assertEqual(evidence["status"], "pass")
        self.assertEqual(evidence["events"], [
            "open-request",
            "frozen",
            "close-request",
            "resumed",
        ])
        self.assertEqual(evidence["open_generation"], 3)
        self.assertEqual(evidence["resume_generation"], 4)
        self.assertEqual(evidence["frozen_duration_ms"], 1000)
        self.assertEqual(evidence["paused_present_delta"], 2)

        pending = gameplay.parse_host_pause_evidence(
            "\n".join(HOST_PAUSE_EVENTS[:2])
        )
        self.assertEqual(pending["status"], "pending")
        self.assertFalse(pending["validated"])

    def test_host_pause_parser_fails_closed_on_ambiguous_or_bad_evidence(self):
        cases = {
            "duplicate": HOST_PAUSE_EVENTS + HOST_PAUSE_EVENTS,
            "out-of-order": (
                HOST_PAUSE_EVENTS[0],
                HOST_PAUSE_EVENTS[2],
                HOST_PAUSE_EVENTS[1],
                HOST_PAUSE_EVENTS[3],
            ),
            "malformed": (
                HOST_PAUSE_EVENTS[0],
                "[ge-test] host-pause frozen trust-me=1",
            ),
            "runtime-failure": (
                HOST_PAUSE_EVENTS[0],
                "[ge-test] host-pause failed phase=waiting-for-pause "
                "reason=phase-timeout",
            ),
            "stalled-presentation": (
                HOST_PAUSE_EVENTS[0],
                HOST_PAUSE_EVENTS[1].replace("present_delta=2", "present_delta=0"),
                HOST_PAUSE_EVENTS[2],
                HOST_PAUSE_EVENTS[3],
            ),
            "wrong-generation": (
                HOST_PAUSE_EVENTS[0],
                HOST_PAUSE_EVENTS[1],
                HOST_PAUSE_EVENTS[2],
                HOST_PAUSE_EVENTS[3].replace("resume_generation=4", "resume_generation=9"),
            ),
        }
        for name, lines in cases.items():
            with self.subTest(name=name):
                evidence = gameplay.parse_host_pause_evidence("\n".join(lines))
                self.assertEqual(evidence["status"], "failed", evidence)
                self.assertFalse(evidence["validated"], evidence)
                self.assertIsNotNone(evidence["failure"])

    def test_host_pause_driver_waits_then_sends_bounded_post_resume_probe(self):
        read_fd, write_fd = os.pipe()
        os.set_blocking(read_fd, False)
        driver = gameplay.DamGameplayScenario(
            write_fd, host_pause_required=True
        )
        gameplay_state = "\n".join(
            (
                "[vpad] READY pads=1",
                "[ge-test] mission level=33 players=1 network=0",
                gameplay_sample(1),
                gameplay_sample(2),
            )
        )
        try:
            driver.advance(gameplay_state, 1.0)
            with self.assertRaises(BlockingIOError):
                os.read(read_fd, 4096)

            partial = "\n".join(HOST_PAUSE_EVENTS[:3])
            driver.advance(gameplay_state, 1.5, host_pause_text=partial)
            with self.assertRaises(BlockingIOError):
                os.read(read_fd, 4096)

            driver.advance(
                gameplay_state,
                2.0,
                host_pause_text="\n".join(HOST_PAUSE_EVENTS),
            )
            self.assertEqual(
                os.read(read_fd, 4096).decode("ascii"),
                "PULSE_AXIS 1 1 RX 16000 2000\n",
            )
            self.assertEqual(driver.phase, "look-active")
            self.assertTrue(driver.evidence["host_resume_probe_bounded"])
            self.assertEqual(driver.evidence["host_resume_probe_hold_ms"], 2000)
            self.assertEqual(driver.evidence["host_resume_probe_sequence"], 1)
            self.assertEqual(
                driver.evidence["host_resume_neutral_baseline_sample"], 2
            )
            self.assertEqual(
                driver.evidence["host_resume_baseline_position_drift"], 0.0
            )
            self.assertEqual(
                driver.evidence["host_resume_baseline_camera_drift"], 0.0
            )

            active = (
                gameplay_state
                + "\n[vpad] ACK seq=1\n"
                + gameplay_sample(3, rx=16000)
            )
            driver.advance(active, 2.5, host_pause_text="\n".join(HOST_PAUSE_EVENTS))
            self.assertTrue(driver.evidence["look_input_observed"])
            self.assertEqual(driver.evidence["look_input_sample"], 3)
            with self.assertRaises(BlockingIOError):
                os.read(read_fd, 4096)

            camera_effect = active + "\n" + gameplay_sample(4, yaw=0.2, rx=16000)
            driver.advance(
                camera_effect,
                3.0,
                host_pause_text="\n".join(HOST_PAUSE_EVENTS),
            )
            self.assertEqual(
                os.read(read_fd, 4096).decode("ascii"), "RESET 2 1\n"
            )
            self.assertEqual(driver.evidence["camera_effect_sample"], 4)
            self.assertGreaterEqual(driver.evidence["camera_delta"], 0.2)

            neutral = (
                camera_effect
                + "\n[vpad] ACK seq=2\n"
                + gameplay_sample(5, yaw=0.2)
            )
            driver.advance(
                neutral,
                3.5,
                host_pause_text="\n".join(HOST_PAUSE_EVENTS),
            )
            self.assertEqual(
                os.read(read_fd, 4096).decode("ascii"),
                "SET_AXIS 3 1 LY -24000\n",
            )
            self.assertTrue(driver.evidence["host_resume_probe_released"])
            self.assertLess(
                driver.evidence["host_resume_neutral_baseline_sample"],
                driver.evidence["look_input_sample"],
            )
            self.assertLess(
                driver.evidence["look_input_sample"],
                driver.evidence["camera_effect_sample"],
            )
        finally:
            driver.close()
            os.close(read_fd)

    def test_host_pause_driver_rejects_moving_post_resume_baseline(self):
        read_fd, write_fd = os.pipe()
        os.set_blocking(read_fd, False)
        driver = gameplay.DamGameplayScenario(
            write_fd, host_pause_required=True
        )
        prefix = (
            "[vpad] READY pads=1\n"
            "[ge-test] mission level=33 players=1 network=0\n"
        )
        proof = "\n".join(HOST_PAUSE_EVENTS)
        try:
            driver.advance(
                prefix + gameplay_sample(1, x=10.0) + "\n" + gameplay_sample(2, x=11.0),
                1.0,
                host_pause_text=proof,
            )
            with self.assertRaises(BlockingIOError):
                os.read(read_fd, 4096)
            self.assertEqual(driver.phase, "waiting-for-gameplay")

            driver.advance(
                prefix
                + gameplay_sample(1, x=10.0)
                + "\n"
                + gameplay_sample(2, x=11.0)
                + "\n"
                + gameplay_sample(3, x=11.0),
                1.5,
                host_pause_text=proof,
            )
            self.assertEqual(
                os.read(read_fd, 4096).decode("ascii"),
                "PULSE_AXIS 1 1 RX 16000 2000\n",
            )
            self.assertEqual(
                driver.evidence["host_resume_neutral_baseline_sample"], 3
            )
        finally:
            driver.close()
            os.close(read_fd)

    def test_parser_reads_mission_graphics_and_gameplay_state(self):
        observations = gameplay.parse_observations(
            "\n".join(
                (
                    "[vpad] READY pads=1",
                    "[vpad] ACK seq=4",
                    "[ge-test] mission level=33 players=1 network=0",
                    "[ge-test] graphics original=0 known=1",
                    gameplay_sample(7, x=12.5, yaw=0.25, rt=255, ly=24000),
                    local_pad_sample(7, 2, device=202, rt=255, ly=24000),
                    local_player_sample(7, 2, x=42.5, yaw=0.75, ammo=5),
                )
            )
        )
        self.assertEqual(observations.mission, gameplay.Mission(33, 1, False))
        self.assertEqual(observations.acknowledgements, frozenset({4}))
        self.assertEqual(observations.ready_pads, frozenset({1}))
        self.assertTrue(observations.graphics[-1].known)
        self.assertFalse(observations.graphics[-1].original)
        self.assertEqual(observations.samples[-1].right_trigger, 255)
        self.assertEqual(observations.samples[-1].left_y, 24000)
        self.assertEqual(observations.samples[-1].weapon, 3)
        self.assertTrue(observations.samples[-1].right_magazine_valid)
        self.assertEqual(observations.samples[-1].right_magazine, 7)
        self.assertAlmostEqual(observations.samples[-1].x, 12.5)
        self.assertEqual(observations.local_pads[-1].slot, 2)
        self.assertTrue(observations.local_pads[-1].connected)
        self.assertEqual(observations.local_pads[-1].device, 202)
        self.assertEqual(observations.local_pads[-1].right_trigger, 255)
        self.assertEqual(observations.local_players[-1].slot, 2)
        self.assertTrue(observations.local_players[-1].eligible())
        self.assertAlmostEqual(observations.local_players[-1].x, 42.5)
        self.assertAlmostEqual(observations.local_players[-1].yaw, 0.75)
        self.assertEqual(observations.local_players[-1].right_magazine, 5)

    def test_complete_dam_scenario_requires_fresh_observed_effects(self):
        with tempfile.TemporaryDirectory() as temporary:
            log_path = Path(temporary) / "raw.log"
            read_fd, write_fd = os.pipe()
            os.set_blocking(read_fd, False)
            driver = gameplay.DamGameplayScenario(write_fd)

            def append(*lines: str) -> str:
                with log_path.open("a", encoding="utf-8") as stream:
                    stream.write("\n".join(lines) + "\n")
                return log_path.read_text(encoding="utf-8")

            try:
                text = append(
                    "[vpad] READY pads=1",
                    "[ge-test] mission level=33 players=1 network=0",
                    "[ge-test] graphics original=0 known=1",
                    gameplay_sample(1),
                    gameplay_sample(2),
                    # Logged after the eligible baseline but before the command.
                    # Its input/effect-looking values must remain out of bounds.
                    gameplay_sample(3, yaw=9.0, rx=16000, disabled=1),
                )
                driver.advance(text, 1.0)
                self.assertEqual(
                    os.read(read_fd, 4096).decode("ascii"),
                    "SET_AXIS 1 1 RX 16000\n",
                )

                # The pre-command sample cannot satisfy look delivery or effect.
                driver.advance(append("[vpad] ACK seq=1"), 1.5)
                self.assertEqual(driver.phase, "look-active")
                self.assertFalse(driver.evidence["look_input_observed"])

                # Delivery alone is not a camera effect. A later sample while
                # the dispatched axis remains active must move the camera.
                driver.advance(append(gameplay_sample(4, rx=16000)), 2.0)
                self.assertEqual(driver.phase, "look-active")
                self.assertTrue(driver.evidence["look_input_observed"])
                self.assertIsNone(driver.evidence["camera_effect_sample"])

                driver.advance(append(gameplay_sample(5, yaw=0.2, rx=16000)), 2.2)
                self.assertEqual(os.read(read_fd, 4096).decode("ascii"), "RESET 2 1\n")

                driver.advance(
                    append("[vpad] ACK seq=2", gameplay_sample(6, yaw=0.2)), 2.5
                )
                self.assertEqual(
                    os.read(read_fd, 4096).decode("ascii"),
                    "SET_AXIS 3 1 LY -24000\n",
                )

                driver.advance(
                    append(
                        "[vpad] ACK seq=3",
                        gameplay_sample(7, x=10.0, yaw=0.2, ly=24000),
                    ),
                    3.0,
                )
                self.assertEqual(driver.phase, "move-active")
                self.assertIsNone(driver.evidence["movement_effect_sample"])

                driver.advance(
                    append(gameplay_sample(8, x=13.5, yaw=0.2, ly=24000)),
                    3.2,
                )
                self.assertEqual(os.read(read_fd, 4096).decode("ascii"), "RESET 4 1\n")

                driver.advance(
                    append("[vpad] ACK seq=4", gameplay_sample(9, x=13.5, yaw=0.2)),
                    3.5,
                )
                self.assertEqual(
                    os.read(read_fd, 4096).decode("ascii"),
                    "SET_AXIS 5 1 RT 32767\n",
                )

                driver.advance(
                    append(
                        "[vpad] ACK seq=5",
                        gameplay_sample(10, x=13.5, yaw=0.2, rt=255, ammo=7),
                    ),
                    4.0,
                )
                self.assertEqual(driver.phase, "fire-active")
                self.assertTrue(driver.evidence["fire_input_observed"])
                self.assertFalse(driver.evidence["fire_effect_observed"])

                driver.advance(
                    append(
                        gameplay_sample(11, x=13.5, yaw=0.2, rt=255, ammo=6)
                    ),
                    4.2,
                )
                self.assertEqual(os.read(read_fd, 4096).decode("ascii"), "RESET 6 1\n")

                driver.advance(
                    append(
                        "[vpad] ACK seq=6",
                        gameplay_sample(12, x=13.5, yaw=0.2, ammo=6),
                    ),
                    4.5,
                )
                self.assertEqual(
                    os.read(read_fd, 4096).decode("ascii"),
                    "PULSE_BUTTON 7 1 START 300\n",
                )

                driver.advance(
                    append(
                        "[vpad] ACK seq=7",
                        gameplay_sample(13, x=13.5, yaw=0.2, pause=1, ammo=6),
                    ),
                    5.0,
                )
                self.assertEqual(
                    os.read(read_fd, 4096).decode("ascii"),
                    "PULSE_BUTTON 8 1 START 300\n",
                )

                driver.advance(
                    append(
                        "[vpad] ACK seq=8",
                        gameplay_sample(14, x=13.5, yaw=0.2, ammo=6),
                    ),
                    5.5,
                )
                self.assertEqual(
                    os.read(read_fd, 4096).decode("ascii"),
                    "PULSE_BUTTON 9 1 RIGHT_SHOULDER 400\n",
                )

                driver.advance(
                    append(
                        "[vpad] ACK seq=9",
                        "[ge-test] graphics original=1 known=1",
                        gameplay_sample(15, x=13.5, yaw=0.2, ammo=6),
                    ),
                    6.0,
                )
                self.assertEqual(
                    os.read(read_fd, 4096).decode("ascii"),
                    "PULSE_BUTTON 10 1 RIGHT_SHOULDER 400\n",
                )

                driver.advance(
                    append(
                        "[vpad] ACK seq=10",
                        "[ge-test] graphics original=0 known=1",
                    ),
                    6.5,
                )
                self.assertEqual(os.read(read_fd, 4096).decode("ascii"), "RESET 11 1\n")

                driver.advance(
                    append(
                        "[vpad] ACK seq=11",
                        gameplay_sample(16, x=13.5, yaw=0.2, ammo=6),
                    ),
                    7.0,
                )
                self.assertTrue(driver.completed, driver.result())
                self.assertEqual(driver.phase, "complete")
                for key in (
                    "look_input_observed",
                    "movement_input_observed",
                    "fire_input_observed",
                    "fire_effect_observed",
                    "pause_observed",
                    "resume_observed",
                    "graphics_toggled",
                    "graphics_restored",
                    "final_neutral_observed",
                ):
                    self.assertTrue(driver.evidence[key], driver.evidence)
                self.assertGreaterEqual(driver.evidence["movement_distance"], 3.5)
                self.assertGreaterEqual(driver.evidence["camera_delta"], 0.2)
                self.assertEqual(driver.evidence["ammo_before"], 7)
                self.assertEqual(driver.evidence["ammo_after"], 6)
                self.assertEqual(driver.evidence["ammo_decrement"], 1)
            finally:
                driver.close()
                os.close(read_fd)

    def test_dam_scenario_rejects_generic_single_player_level_identity(self):
        read_fd, write_fd = os.pipe()
        os.set_blocking(read_fd, False)
        driver = gameplay.DamGameplayScenario(write_fd)
        try:
            driver.advance(
                "\n".join(
                    (
                        "[vpad] READY pads=1",
                        "[ge-test] mission level=34 players=1 network=0",
                        gameplay_sample(1),
                        gameplay_sample(2),
                    )
                ),
                1.0,
            )
            self.assertEqual(driver.phase, "waiting-for-gameplay")
            with self.assertRaises(BlockingIOError):
                os.read(read_fd, 4096)
        finally:
            driver.close()
            os.close(read_fd)

    def test_active_dam_scenario_fails_if_mission_identity_changes(self):
        read_fd, write_fd = os.pipe()
        os.set_blocking(read_fd, False)
        driver = gameplay.DamGameplayScenario(write_fd)
        try:
            text = "\n".join(
                (
                    "[vpad] READY pads=1",
                    "[ge-test] mission level=33 players=1 network=0",
                    gameplay_sample(1),
                    gameplay_sample(2),
                )
            )
            driver.advance(text, 1.0)
            os.read(read_fd, 4096)
            driver.advance(
                text + "\n[ge-test] mission level=34 players=1 network=0\n",
                1.5,
            )
            self.assertEqual(
                driver.error, "gameplay scenario left Dam mission (level=34)"
            )
        finally:
            driver.close()
            os.close(read_fd)

    def test_rejected_command_fails_immediately(self):
        read_fd, write_fd = os.pipe()
        driver = gameplay.DamGameplayScenario(write_fd)
        try:
            driver.advance(
                "[vpad] REJECT reason=invalid axis command=SET_AXIS 3 1 BAD 4\n",
                1.0,
            )
            self.assertEqual(driver.error, "virtual-gamepad command 3 was rejected")
        finally:
            driver.close()
            os.close(read_fd)


if __name__ == "__main__":
    unittest.main()
