#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
SPEC = importlib.util.spec_from_file_location(
    "stability_cycle", ROOT / "tools/stability-cycle.py"
)
assert SPEC and SPEC.loader
stability = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(stability)
gameplay = stability.gameplay


def pad_batch(
    sample: int,
    players: int,
    *,
    active_slot: int = 0,
    left_y: int = 0,
    right_trigger: int = 0,
    right_x: int = 0,
    disconnected_slot: int = 0,
    devices: dict[int, int] | None = None,
    unexpected_connected_slot: int = 0,
    unexpected_active_slot: int = 0,
) -> list[str]:
    devices = devices or {slot: 100 + slot for slot in range(1, players + 1)}
    lines = []
    for slot in range(1, gameplay.LOCAL_PAD_SLOT_COUNT + 1):
        requested = slot <= players
        connected = (requested and slot != disconnected_slot) or (
            slot == unexpected_connected_slot
        )
        slot_active = slot == active_slot or slot == unexpected_active_slot
        lines.append(
            f"[ge-test] local-pad sample={sample} poll={1000 + sample} "
            f"frame={2000 + sample} present={3000 + sample} "
            f"slot={slot} connected={1 if connected else 0} "
            f"device={devices.get(slot, 900 + slot) if connected else 0} buttons=0x0000 "
            f"lt=0 rt={right_trigger if slot_active else 0} "
            f"lx=0 ly={left_y if slot_active else 0} "
            f"rx={right_x if slot_active else 0} ry=0"
        )
    return lines


def player_batch(
    sample: int,
    players: int,
    *,
    positions: dict[int, float] | None = None,
    yaws: dict[int, float] | None = None,
    ammo: dict[int, int] | None = None,
    invalid_slot: int = 0,
) -> list[str]:
    positions = positions or {slot: float(slot * 10) for slot in range(1, players + 1)}
    yaws = yaws or {slot: 0.0 for slot in range(1, players + 1)}
    ammo = ammo or {slot: 7 for slot in range(1, players + 1)}
    lines = []
    for slot in range(1, gameplay.LOCAL_PAD_SLOT_COUNT + 1):
        valid = slot <= players and slot != invalid_slot
        lines.append(
            f"[ge-test] local-player sample={sample} poll={1000 + sample} "
            f"frame={2000 + sample} present={3000 + sample} slot={slot} "
            f"player=0x{0x83000000 + slot * 0x1000 if valid else 0:08X} "
            f"player_valid={1 if valid else 0} "
            f"coords=0x{0x84000000 + slot * 0x1000 if valid else 0:08X} "
            f"pos_valid={1 if valid else 0} "
            f"pos=({positions.get(slot, 0.0):.3f},20.000,30.000) "
            f"camera_valid={1 if valid else 0} "
            f"camera=({yaws.get(slot, 0.0):.4f},0.0000) "
            f"weapon_valid={1 if valid else 0} weapon={3 if valid else 0} "
            f"ammo_valid={1 if valid else 0} ammo={ammo.get(slot, 0)} "
            "pause=0 disabled=0 watch=0"
        )
    return lines


def multiplayer_batch(
    sample: int,
    players: int,
    **kwargs: object,
) -> list[str]:
    pad_keys = {
        "active_slot",
        "left_y",
        "right_trigger",
        "right_x",
        "disconnected_slot",
        "devices",
        "unexpected_connected_slot",
        "unexpected_active_slot",
    }
    return [
        *pad_batch(
            sample,
            players,
            **{key: value for key, value in kwargs.items() if key in pad_keys},
        ),
        *player_batch(
            sample,
            players,
            **{key: value for key, value in kwargs.items() if key not in pad_keys},
        ),
    ]


class LocalMultiplayerScenarioTest(unittest.TestCase):
    def test_two_to_four_players_prove_isolated_input_and_hotplug_recovery(self):
        for players in (2, 3, 4):
            with (
                self.subTest(players=players),
                tempfile.TemporaryDirectory() as temporary,
            ):
                log_path = Path(temporary) / "raw.log"
                read_fd, write_fd = os.pipe()
                os.set_blocking(read_fd, False)
                driver = stability.LocalMultiplayerInputDriver(players, write_fd)
                driver.ready_elapsed = 0.0
                driver.phase = "waiting-for-match-readiness"
                driver.action_phase_started_at = 0.0
                driver.sequence = 7
                driver.last_sequence = 7
                now = 1.0
                sample = 2
                devices = {slot: 100 + slot for slot in range(1, players + 1)}
                positions = {
                    slot: float(slot * 10) for slot in range(1, players + 1)
                }
                yaws = {slot: 0.0 for slot in range(1, players + 1)}
                ammo = {slot: 7 for slot in range(1, players + 1)}

                def append(*lines: str) -> None:
                    with log_path.open("a", encoding="utf-8") as stream:
                        stream.write("\n".join(lines) + "\n")

                def advance(*lines: str) -> str:
                    nonlocal now
                    append(*lines)
                    now += 0.5
                    driver.advance(log_path, now)
                    try:
                        return os.read(read_fd, 4096).decode("ascii")
                    except BlockingIOError:
                        return ""

                def acknowledgements() -> list[str]:
                    return [
                        f"[vpad] ACK seq={sequence}"
                        for sequence in range(1, driver.last_sequence + 1)
                    ]

                try:
                    append(
                        f"[vpad] READY pads={players}",
                        *[f"[vpad] ACK seq={sequence}" for sequence in range(1, 8)],
                        f"[ge-test] mission level=7 players={players} network=0",
                        (
                            f"[ge] local multiplayer ready level=7 players={players} "
                            "network=0 stable_polls=120"
                        ),
                        *multiplayer_batch(
                            1,
                            players,
                            devices=devices,
                            positions=positions,
                            yaws=yaws,
                            ammo=ammo,
                        ),
                        *multiplayer_batch(
                            2,
                            players,
                            devices=devices,
                            positions=positions,
                            yaws=yaws,
                            ammo=ammo,
                        ),
                    )
                    driver.advance(log_path, now)
                    self.assertEqual(
                        os.read(read_fd, 4096).decode("ascii"),
                        "SET_AXIS 8 1 LY -24000\nSET_AXIS 9 1 RX 16000\n",
                    )

                    for player in range(1, players + 1):
                        positions[player] += 1.5
                        yaws[player] += 0.02
                        sample += 1
                        command = advance(
                            *acknowledgements(),
                            *multiplayer_batch(
                                sample,
                                players,
                                active_slot=player,
                                left_y=23999,
                                right_x=16000,
                                devices=devices,
                                positions=positions,
                                yaws=yaws,
                                ammo=ammo,
                            ),
                        )
                        self.assertEqual(
                            command, f"RESET {driver.last_sequence} {player}\n"
                        )

                        sample += 1
                        command = advance(
                            *acknowledgements(),
                            *multiplayer_batch(
                                sample,
                                players,
                                devices=devices,
                                positions=positions,
                                yaws=yaws,
                                ammo=ammo,
                            ),
                        )
                        self.assertEqual(
                            command,
                            f"SET_AXIS {driver.last_sequence} {player} RT 32767\n",
                        )

                        ammo[player] -= 1
                        sample += 1
                        command = advance(
                            *acknowledgements(),
                            *multiplayer_batch(
                                sample,
                                players,
                                active_slot=player,
                                right_trigger=255,
                                devices=devices,
                                positions=positions,
                                yaws=yaws,
                                ammo=ammo,
                            ),
                        )
                        self.assertEqual(
                            command, f"RESET {driver.last_sequence} {player}\n"
                        )

                        sample += 1
                        command = advance(
                            *acknowledgements(),
                            *multiplayer_batch(
                                sample,
                                players,
                                devices=devices,
                                positions=positions,
                                yaws=yaws,
                                ammo=ammo,
                            ),
                        )
                        if player < players:
                            self.assertEqual(
                                command,
                                f"SET_AXIS {driver.last_sequence - 1} {player + 1} LY -24000\n"
                                f"SET_AXIS {driver.last_sequence} {player + 1} RX 16000\n",
                            )
                        else:
                            self.assertEqual(
                                command,
                                f"DISCONNECT {driver.last_sequence} 2\n",
                            )

                    sample += 1
                    command = advance(
                        *acknowledgements(),
                        *multiplayer_batch(
                            sample,
                            players,
                            disconnected_slot=2,
                            devices=devices,
                            positions=positions,
                            yaws=yaws,
                            ammo=ammo,
                        ),
                    )
                    self.assertEqual(command, f"CONNECT {driver.last_sequence} 2\n")

                    reconnected_devices = dict(devices)
                    reconnected_devices[2] = 902
                    sample += 1
                    command = advance(
                        *acknowledgements(),
                        *multiplayer_batch(
                            sample,
                            players,
                            devices=reconnected_devices,
                            positions=positions,
                            yaws=yaws,
                            ammo=ammo,
                        ),
                    )
                    self.assertEqual(
                        command,
                        f"SET_AXIS {driver.last_sequence - 1} 2 LY -24000\n"
                        f"SET_AXIS {driver.last_sequence} 2 RX 16000\n",
                    )

                    positions[2] += 1.5
                    yaws[2] += 0.02
                    sample += 1
                    command = advance(
                        *acknowledgements(),
                        *multiplayer_batch(
                            sample,
                            players,
                            active_slot=2,
                            left_y=23999,
                            right_x=16000,
                            devices=reconnected_devices,
                            positions=positions,
                            yaws=yaws,
                            ammo=ammo,
                        ),
                    )
                    self.assertEqual(command, f"RESET {driver.last_sequence} 2\n")

                    sample += 1
                    command = advance(
                        *acknowledgements(),
                        *multiplayer_batch(
                            sample,
                            players,
                            devices=reconnected_devices,
                            positions=positions,
                            yaws=yaws,
                            ammo=ammo,
                        ),
                    )
                    self.assertEqual(
                        command,
                        f"SET_AXIS {driver.last_sequence} 2 RT 32767\n",
                    )

                    ammo[2] -= 1
                    sample += 1
                    command = advance(
                        *acknowledgements(),
                        *multiplayer_batch(
                            sample,
                            players,
                            active_slot=2,
                            right_trigger=255,
                            devices=reconnected_devices,
                            positions=positions,
                            yaws=yaws,
                            ammo=ammo,
                        ),
                    )
                    self.assertEqual(command, f"RESET {driver.last_sequence} 2\n")

                    sample += 1
                    command = advance(
                        *acknowledgements(),
                        *multiplayer_batch(
                            sample,
                            players,
                            devices=reconnected_devices,
                            positions=positions,
                            yaws=yaws,
                            ammo=ammo,
                        ),
                    )
                    self.assertEqual(command, f"RESET {driver.last_sequence} ALL\n")

                    sample += 1
                    command = advance(
                        *acknowledgements(),
                        *multiplayer_batch(
                            sample,
                            players,
                            devices=reconnected_devices,
                            positions=positions,
                            yaws=yaws,
                            ammo=ammo,
                        ),
                    )
                    self.assertEqual(command, "")
                    self.assertTrue(driver.completed, driver.result())
                    self.assertEqual(driver.phase, "complete")
                    self.assertTrue(
                        driver.action_evidence["hotplug"][
                            "unaffected_devices_preserved"
                        ]
                    )
                    self.assertEqual(
                        driver.action_evidence["hotplug"]["old_device"], 102
                    )
                    self.assertEqual(
                        driver.action_evidence["hotplug"]["new_device"], 902
                    )
                    for player in range(1, players + 1):
                        evidence = driver.action_evidence["players"][str(player)]
                        self.assertIsNotNone(evidence["movement_sample"])
                        self.assertIsNotNone(evidence["world_effect_sample"])
                        self.assertGreaterEqual(evidence["movement_distance"], 1.5)
                        self.assertGreaterEqual(evidence["camera_delta"], 0.02)
                        self.assertIsNotNone(evidence["fire_sample"])
                        self.assertTrue(evidence["fire_effect_required"])
                        self.assertTrue(evidence["fire_effect_observed"])
                        self.assertEqual(evidence["ammo_decrement"], 1)
                        self.assertIsNotNone(evidence["neutral_sample"])
                    hotplug = driver.action_evidence["hotplug"]
                    self.assertGreaterEqual(
                        hotplug["post_reconnect_movement_distance"], 1.5
                    )
                    self.assertGreaterEqual(
                        hotplug["post_reconnect_camera_delta"], 0.02
                    )
                    self.assertTrue(hotplug["post_reconnect_fire_effect_required"])
                    self.assertTrue(hotplug["post_reconnect_fire_effect_observed"])
                    self.assertEqual(hotplug["post_reconnect_ammo_decrement"], 1)
                finally:
                    driver.close()
                    os.close(read_fd)

    def test_stale_or_incomplete_pad_batch_cannot_advance(self):
        with tempfile.TemporaryDirectory() as temporary:
            log_path = Path(temporary) / "raw.log"
            read_fd, write_fd = os.pipe()
            os.set_blocking(read_fd, False)
            driver = stability.LocalMultiplayerInputDriver(2, write_fd)
            driver.ready_elapsed = 0.0
            driver.phase = "player-move-active"
            driver.action_phase_started_at = 1.0
            driver.sequence = 1
            driver.last_sequence = 1
            driver.minimum_pad_sample = 5
            try:
                log_path.write_text(
                    "\n".join(
                        (
                            "[vpad] ACK seq=1",
                            *pad_batch(5, 2, active_slot=1, left_y=23999),
                            pad_batch(6, 2, active_slot=1, left_y=23999)[0],
                        )
                    )
                    + "\n",
                    encoding="utf-8",
                )
                driver.advance(log_path, 2.0)
                self.assertEqual(driver.phase, "player-move-active")
                with self.assertRaises(BlockingIOError):
                    os.read(read_fd, 4096)
            finally:
                driver.close()
                os.close(read_fd)

    def test_match_readiness_requires_complete_readable_player_telemetry(self):
        with tempfile.TemporaryDirectory() as temporary:
            log_path = Path(temporary) / "raw.log"
            read_fd, write_fd = os.pipe()
            os.set_blocking(read_fd, False)
            driver = stability.LocalMultiplayerInputDriver(2, write_fd)
            driver.ready_elapsed = 0.0
            driver.phase = "waiting-for-match-readiness"
            driver.action_phase_started_at = 0.0
            try:
                lines = [
                    "[vpad] READY pads=2",
                    "[ge-test] mission level=7 players=2 network=0",
                    "[ge] local multiplayer ready level=7 players=2 "
                    "network=0 stable_polls=120",
                    *multiplayer_batch(1, 2),
                    *multiplayer_batch(2, 2, invalid_slot=2),
                ]
                log_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
                driver.advance(log_path, 1.0)
                self.assertEqual(driver.phase, "waiting-for-match-readiness")
                with self.assertRaises(BlockingIOError):
                    os.read(read_fd, 4096)
            finally:
                driver.close()
                os.close(read_fd)

    def test_target_world_effect_and_non_target_stability_are_required(self):
        with tempfile.TemporaryDirectory() as temporary:
            log_path = Path(temporary) / "raw.log"
            read_fd, write_fd = os.pipe()
            os.set_blocking(read_fd, False)
            driver = stability.LocalMultiplayerInputDriver(2, write_fd)
            baseline_positions = {1: 10.0, 2: 20.0}
            baseline_yaws = {1: 0.0, 2: 0.0}
            baseline_text = "\n".join(
                multiplayer_batch(
                    5,
                    2,
                    positions=baseline_positions,
                    yaws=baseline_yaws,
                )
            )
            observations = gameplay.parse_observations(baseline_text)
            baseline_world = gameplay.complete_local_player_batches(
                observations, 2
            )[0]
            driver.ready_elapsed = 0.0
            driver.phase = "player-move-active"
            driver.action_phase_started_at = 0.0
            driver.sequence = 2
            driver.last_sequence = 2
            driver.minimum_pad_sample = 5
            driver.action_player = 1
            driver.action_world_baseline = dict(baseline_world)
            try:
                # Slot 1 receives both axes, but only slot 2 moves: pad routing
                # cannot masquerade as a player-1 gameplay effect.
                log_path.write_text(
                    "\n".join(
                        (
                            "[vpad] ACK seq=1",
                            "[vpad] ACK seq=2",
                            *multiplayer_batch(
                                6,
                                2,
                                active_slot=1,
                                left_y=24000,
                                right_x=16000,
                                positions={1: 10.0, 2: 21.5},
                                yaws={1: 0.0, 2: 0.02},
                            ),
                        )
                    )
                    + "\n",
                    encoding="utf-8",
                )
                driver.advance(log_path, 1.0)
                self.assertEqual(driver.phase, "player-move-active")
                with self.assertRaises(BlockingIOError):
                    os.read(read_fd, 4096)

                # Even a real target effect is rejected while a non-target
                # player drifts beyond the isolation tolerance.
                with log_path.open("a", encoding="utf-8") as stream:
                    stream.write(
                        "\n".join(
                            multiplayer_batch(
                                7,
                                2,
                                active_slot=1,
                                left_y=24000,
                                right_x=16000,
                                positions={1: 11.5, 2: 21.0},
                                yaws={1: 0.02, 2: 0.02},
                            )
                        )
                        + "\n"
                    )
                driver.advance(log_path, 1.5)
                self.assertEqual(driver.phase, "player-move-active")
                with self.assertRaises(BlockingIOError):
                    os.read(read_fd, 4096)

                with log_path.open("a", encoding="utf-8") as stream:
                    stream.write(
                        "\n".join(
                            multiplayer_batch(
                                8,
                                2,
                                active_slot=1,
                                left_y=24000,
                                right_x=16000,
                                positions={1: 11.5, 2: 20.0},
                                yaws={1: 0.02, 2: 0.0},
                            )
                        )
                        + "\n"
                    )
                driver.advance(log_path, 2.0)
                self.assertEqual(
                    os.read(read_fd, 4096).decode("ascii"), "RESET 3 1\n"
                )
                self.assertEqual(driver.phase, "player-move-neutral")
            finally:
                driver.close()
                os.close(read_fd)

    def test_fire_effect_is_skipped_only_when_ammo_is_not_usable(self):
        with tempfile.TemporaryDirectory() as temporary:
            log_path = Path(temporary) / "raw.log"
            read_fd, write_fd = os.pipe()
            os.set_blocking(read_fd, False)
            driver = stability.LocalMultiplayerInputDriver(2, write_fd)
            baseline_text = "\n".join(
                multiplayer_batch(5, 2, ammo={1: 0, 2: 7})
            )
            baseline_world = gameplay.complete_local_player_batches(
                gameplay.parse_observations(baseline_text), 2
            )[0]
            driver.ready_elapsed = 0.0
            driver.phase = "player-fire-active"
            driver.action_phase_started_at = 0.0
            driver.sequence = 1
            driver.last_sequence = 1
            driver.minimum_pad_sample = 5
            driver.action_player = 1
            driver.action_world_baseline = dict(baseline_world)
            driver.action_evidence["players"]["1"][
                "fire_effect_required"
            ] = False
            try:
                log_path.write_text(
                    "\n".join(
                        (
                            "[vpad] ACK seq=1",
                            *multiplayer_batch(
                                6,
                                2,
                                active_slot=1,
                                right_trigger=255,
                                ammo={1: 0, 2: 7},
                            ),
                        )
                    )
                    + "\n",
                    encoding="utf-8",
                )
                driver.advance(log_path, 1.0)
                self.assertEqual(
                    os.read(read_fd, 4096).decode("ascii"), "RESET 2 1\n"
                )
                evidence = driver.action_evidence["players"]["1"]
                self.assertIsNotNone(evidence["fire_sample"])
                self.assertFalse(evidence["fire_effect_required"])
                self.assertFalse(evidence["fire_effect_observed"])
            finally:
                driver.close()
                os.close(read_fd)

    def test_two_and_three_player_batches_reject_unrequested_connected_ports(self):
        for players in (2, 3):
            with self.subTest(players=players):
                unexpected_slot = players + 1
                observations = gameplay.parse_observations(
                    "\n".join(
                        pad_batch(
                            1,
                            players,
                            unexpected_connected_slot=unexpected_slot,
                        )
                    )
                )
                self.assertEqual(
                    gameplay.complete_local_pad_batches(observations, players), ()
                )

    def test_unrequested_guest_activity_is_rejected_even_if_port_is_disconnected(self):
        observations = gameplay.parse_observations(
            "\n".join(
                pad_batch(
                    1,
                    2,
                    left_y=24000,
                    unexpected_active_slot=3,
                )
            )
        )
        self.assertEqual(gameplay.complete_local_pad_batches(observations, 2), ())

    def test_requested_batches_require_proof_for_all_four_native_ports(self):
        observations = gameplay.parse_observations(
            "\n".join(pad_batch(1, 2)[:2])
        )
        self.assertEqual(gameplay.complete_local_pad_batches(observations, 2), ())


if __name__ == "__main__":
    unittest.main()
