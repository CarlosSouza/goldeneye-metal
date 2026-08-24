#!/usr/bin/env python3
"""State-driven gameplay actions for GoldenEye's private integration harness."""

from __future__ import annotations

import math
import os
import re
from dataclasses import asdict, dataclass
from typing import Any

_FLOAT = r"[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?"
DAM_LEVEL_ID = 0x21
LOCAL_PAD_SLOT_COUNT = 4
MISSION_STATE = re.compile(
    r"\[ge-test\] mission level=(-?\d+) players=(-?\d+) network=(\d+)"
)
GRAPHICS_STATE = re.compile(r"\[ge-test\] graphics original=(\d+) known=(\d+)")
GAMEPLAY_SAMPLE = re.compile(
    rf"\[ge-test\] gameplay sample=(?P<sample>\d+) poll=(?P<poll>\d+) "
    rf"frame=(?P<frame>\d+) present=(?P<present>\d+) "
    rf"player=0x(?P<player>[0-9a-fA-F]+) coords=0x(?P<coords>[0-9a-fA-F]+) "
    rf"pos_valid=(?P<pos_valid>[01]) "
    rf"pos=\((?P<x>{_FLOAT}),(?P<y>{_FLOAT}),(?P<z>{_FLOAT})\) "
    rf"camera=\((?P<yaw>{_FLOAT}),(?P<pitch>{_FLOAT})\) "
    rf"(?:weapon=(?P<weapon>-?\d+) ammo_valid=(?P<ammo_valid>[01]) "
    rf"ammo=(?P<ammo>-?\d+) )?"
    rf"pause=(?P<pause>\d+) disabled=(?P<disabled>\d+) watch=(?P<watch>\d+) "
    rf"buttons=0x(?P<buttons>[0-9a-fA-F]+) lt=(?P<lt>\d+) rt=(?P<rt>\d+) "
    rf"lx=(?P<lx>-?\d+) ly=(?P<ly>-?\d+) rx=(?P<rx>-?\d+) ry=(?P<ry>-?\d+)"
)
LOCAL_PAD_SAMPLE = re.compile(
    rf"\[ge-test\] local-pad sample=(?P<sample>\d+) poll=(?P<poll>\d+) "
    rf"frame=(?P<frame>\d+) present=(?P<present>\d+) "
    rf"slot=(?P<slot>\d+) connected=(?P<connected>[01]) "
    rf"device=(?P<device>\d+) "
    rf"buttons=0x(?P<buttons>[0-9a-fA-F]+) "
    rf"lt=(?P<lt>\d+) rt=(?P<rt>\d+) "
    rf"lx=(?P<lx>-?\d+) ly=(?P<ly>-?\d+) "
    rf"rx=(?P<rx>-?\d+) ry=(?P<ry>-?\d+)"
)
LOCAL_PLAYER_SAMPLE = re.compile(
    rf"\[ge-test\] local-player sample=(?P<sample>\d+) poll=(?P<poll>\d+) "
    rf"frame=(?P<frame>\d+) present=(?P<present>\d+) "
    rf"slot=(?P<slot>\d+) player=0x(?P<player>[0-9a-fA-F]+) "
    rf"player_valid=(?P<player_valid>[01]) "
    rf"coords=0x(?P<coords>[0-9a-fA-F]+) "
    rf"pos_valid=(?P<pos_valid>[01]) "
    rf"pos=\((?P<x>{_FLOAT}),(?P<y>{_FLOAT}),(?P<z>{_FLOAT})\) "
    rf"camera_valid=(?P<camera_valid>[01]) "
    rf"camera=\((?P<yaw>{_FLOAT}),(?P<pitch>{_FLOAT})\) "
    rf"weapon_valid=(?P<weapon_valid>[01]) weapon=(?P<weapon>-?\d+) "
    rf"ammo_valid=(?P<ammo_valid>[01]) ammo=(?P<ammo>-?\d+) "
    rf"pause=(?P<pause>\d+) disabled=(?P<disabled>\d+) watch=(?P<watch>\d+)"
)
VPAD_READY = re.compile(r"\[vpad\] READY pads=(\d+)")
VPAD_ACK = re.compile(r"\[vpad\] ACK seq=(\d+)")
VPAD_REJECT = re.compile(r"\[vpad\] REJECT reason=.*? command=\S+\s+(\d+)(?:\s|$)")
HOST_PAUSE_EVENT_PREFIX = "[ge-test] host-pause "
HOST_PAUSE_OPEN = re.compile(r"open-request queued=(?P<queued>[01])")
HOST_PAUSE_FROZEN = re.compile(
    r"frozen open_generation=(?P<open_generation>\d+) "
    r"samples=(?P<samples>\d+) duration_ms=(?P<duration_ms>\d+) "
    r"frame_delta=(?P<frame_delta>\d+) present_delta=(?P<present_delta>\d+) "
    r"world_stable=(?P<world_stable>[01]) ui_open=(?P<ui_open>[01]) "
    r"owned=(?P<owned>[01])"
)
HOST_PAUSE_CLOSE = re.compile(
    r"close-request open_generation=(?P<open_generation>\d+) "
    r"queued=(?P<queued>[01])"
)
HOST_PAUSE_RESUMED = re.compile(
    r"resumed open_generation=(?P<open_generation>\d+) "
    r"resume_generation=(?P<resume_generation>\d+) "
    r"samples=(?P<samples>\d+) duration_ms=(?P<duration_ms>\d+) "
    r"frame_delta=(?P<frame_delta>\d+) present_delta=(?P<present_delta>\d+) "
    r"ui_closed=(?P<ui_closed>[01]) pause_released=(?P<pause_released>[01]) "
    r"input_ready=(?P<input_ready>[01])"
)
HOST_PAUSE_FAILED = re.compile(
    r"failed phase=(?P<phase>[a-z-]+) reason=(?P<reason>[a-z0-9-]+)"
)


@dataclass(frozen=True)
class Mission:
    level: int
    players: int
    network: bool


@dataclass(frozen=True)
class Graphics:
    index: int
    original: bool
    known: bool


@dataclass(frozen=True)
class Sample:
    sample: int
    poll: int
    frame: int
    present: int
    player: int
    coordinates: int
    position_valid: bool
    x: float
    y: float
    z: float
    yaw: float
    pitch: float
    weapon: int
    right_magazine_valid: bool
    right_magazine: int
    pause: int
    disabled: int
    watch: int
    buttons: int
    left_trigger: int
    right_trigger: int
    left_x: int
    left_y: int
    right_x: int
    right_y: int

    def eligible(self) -> bool:
        return (
            self.player != 0
            and self.coordinates != 0
            and self.position_valid
            and self.pause == 0
            and self.disabled == 0
            and self.watch == 0
        )


@dataclass(frozen=True)
class LocalPadSample:
    sample: int
    poll: int
    frame: int
    present: int
    slot: int
    connected: bool
    device: int
    buttons: int
    left_trigger: int
    right_trigger: int
    left_x: int
    left_y: int
    right_x: int
    right_y: int

    def neutral(self, threshold: int = 2_000) -> bool:
        return (
            self.buttons == 0
            and self.left_trigger == 0
            and self.right_trigger == 0
            and abs(self.left_x) < threshold
            and abs(self.left_y) < threshold
            and abs(self.right_x) < threshold
            and abs(self.right_y) < threshold
        )


@dataclass(frozen=True)
class LocalPlayerSample:
    sample: int
    poll: int
    frame: int
    present: int
    slot: int
    player: int
    player_valid: bool
    coordinates: int
    position_valid: bool
    x: float
    y: float
    z: float
    camera_valid: bool
    yaw: float
    pitch: float
    weapon_valid: bool
    weapon: int
    right_magazine_valid: bool
    right_magazine: int
    pause: int
    disabled: int
    watch: int

    def eligible(self) -> bool:
        return (
            self.player != 0
            and self.player_valid
            and self.coordinates != 0
            and self.position_valid
            and self.camera_valid
            and self.pause == 0
            and self.disabled == 0
            and self.watch == 0
        )


@dataclass(frozen=True)
class Observations:
    mission: Mission | None
    graphics: tuple[Graphics, ...]
    samples: tuple[Sample, ...]
    local_pads: tuple[LocalPadSample, ...]
    local_players: tuple[LocalPlayerSample, ...]
    acknowledgements: frozenset[int]
    rejected_sequence: int | None
    ready_pads: frozenset[int]


def _counter_advanced(previous: int, current: int) -> bool:
    delta = (current - previous) & 0xFFFFFFFF
    return delta != 0 and delta < 0x80000000


def _sample_from_match(match: re.Match[str]) -> Sample:
    groups = match.groupdict()
    return Sample(
        sample=int(groups["sample"]),
        poll=int(groups["poll"]),
        frame=int(groups["frame"]),
        present=int(groups["present"]),
        player=int(groups["player"], 16),
        coordinates=int(groups["coords"], 16),
        position_valid=groups["pos_valid"] == "1",
        x=float(groups["x"]),
        y=float(groups["y"]),
        z=float(groups["z"]),
        yaw=float(groups["yaw"]),
        pitch=float(groups["pitch"]),
        weapon=int(groups["weapon"]) if groups["weapon"] is not None else 0,
        right_magazine_valid=groups["ammo_valid"] == "1",
        right_magazine=(
            int(groups["ammo"]) if groups["ammo"] is not None else 0
        ),
        pause=int(groups["pause"]),
        disabled=int(groups["disabled"]),
        watch=int(groups["watch"]),
        buttons=int(groups["buttons"], 16),
        left_trigger=int(groups["lt"]),
        right_trigger=int(groups["rt"]),
        left_x=int(groups["lx"]),
        left_y=int(groups["ly"]),
        right_x=int(groups["rx"]),
        right_y=int(groups["ry"]),
    )


def _local_pad_from_match(match: re.Match[str]) -> LocalPadSample:
    groups = match.groupdict()
    return LocalPadSample(
        sample=int(groups["sample"]),
        poll=int(groups["poll"]),
        frame=int(groups["frame"]),
        present=int(groups["present"]),
        slot=int(groups["slot"]),
        connected=groups["connected"] == "1",
        device=int(groups["device"]),
        buttons=int(groups["buttons"], 16),
        left_trigger=int(groups["lt"]),
        right_trigger=int(groups["rt"]),
        left_x=int(groups["lx"]),
        left_y=int(groups["ly"]),
        right_x=int(groups["rx"]),
        right_y=int(groups["ry"]),
    )


def _local_player_from_match(match: re.Match[str]) -> LocalPlayerSample:
    groups = match.groupdict()
    return LocalPlayerSample(
        sample=int(groups["sample"]),
        poll=int(groups["poll"]),
        frame=int(groups["frame"]),
        present=int(groups["present"]),
        slot=int(groups["slot"]),
        player=int(groups["player"], 16),
        player_valid=groups["player_valid"] == "1",
        coordinates=int(groups["coords"], 16),
        position_valid=groups["pos_valid"] == "1",
        x=float(groups["x"]),
        y=float(groups["y"]),
        z=float(groups["z"]),
        camera_valid=groups["camera_valid"] == "1",
        yaw=float(groups["yaw"]),
        pitch=float(groups["pitch"]),
        weapon_valid=groups["weapon_valid"] == "1",
        weapon=int(groups["weapon"]),
        right_magazine_valid=groups["ammo_valid"] == "1",
        right_magazine=int(groups["ammo"]),
        pause=int(groups["pause"]),
        disabled=int(groups["disabled"]),
        watch=int(groups["watch"]),
    )


def parse_observations(text: str) -> Observations:
    mission_matches = list(MISSION_STATE.finditer(text))
    mission = None
    if mission_matches:
        latest = mission_matches[-1]
        mission = Mission(
            level=int(latest.group(1)),
            players=int(latest.group(2)),
            network=latest.group(3) != "0",
        )
    graphics = tuple(
        Graphics(
            index=index, original=match.group(1) != "0", known=match.group(2) != "0"
        )
        for index, match in enumerate(GRAPHICS_STATE.finditer(text), start=1)
    )
    rejections = list(VPAD_REJECT.finditer(text))
    return Observations(
        mission=mission,
        graphics=graphics,
        samples=tuple(
            _sample_from_match(match) for match in GAMEPLAY_SAMPLE.finditer(text)
        ),
        local_pads=tuple(
            _local_pad_from_match(match) for match in LOCAL_PAD_SAMPLE.finditer(text)
        ),
        local_players=tuple(
            _local_player_from_match(match)
            for match in LOCAL_PLAYER_SAMPLE.finditer(text)
        ),
        acknowledgements=frozenset(
            int(match.group(1)) for match in VPAD_ACK.finditer(text)
        ),
        rejected_sequence=int(rejections[-1].group(1)) if rejections else None,
        ready_pads=frozenset(
            int(match.group(1)) for match in VPAD_READY.finditer(text)
        ),
    )


def parse_host_pause_evidence(text: str) -> dict[str, Any]:
    """Validate the exact harness-only Host Settings proof event sequence."""
    matchers = (
        ("open-request", HOST_PAUSE_OPEN),
        ("frozen", HOST_PAUSE_FROZEN),
        ("close-request", HOST_PAUSE_CLOSE),
        ("resumed", HOST_PAUSE_RESUMED),
        ("failed", HOST_PAUSE_FAILED),
    )
    events: list[tuple[str, dict[str, str]]] = []
    malformed: list[str] = []
    for line in text.splitlines():
        if HOST_PAUSE_EVENT_PREFIX not in line:
            continue
        payload = line.split(HOST_PAUSE_EVENT_PREFIX, 1)[1].strip()
        matched = False
        for name, matcher in matchers:
            match = matcher.fullmatch(payload)
            if match is None:
                continue
            events.append((name, match.groupdict()))
            matched = True
            break
        if not matched:
            malformed.append(payload)

    result: dict[str, Any] = {
        "requested": bool(events),
        "validated": False,
        "status": "pending",
        "events": [name for name, _ in events],
        "failure": None,
        "malformed": malformed,
        "open_generation": None,
        "resume_generation": None,
        "frozen_samples": 0,
        "frozen_duration_ms": 0,
        "paused_frame_delta": 0,
        "paused_present_delta": 0,
        "resumed_samples": 0,
        "resumed_duration_ms": 0,
        "resumed_frame_delta": 0,
        "resumed_present_delta": 0,
    }
    if malformed:
        result["status"] = "failed"
        result["failure"] = "malformed host-pause event"
        return result

    failed = next((fields for name, fields in events if name == "failed"), None)
    if failed is not None:
        result["status"] = "failed"
        result["failure"] = (
            f"runtime failed in {failed['phase']}: {failed['reason']}"
        )
        return result

    expected = ("open-request", "frozen", "close-request", "resumed")
    names = tuple(name for name, _ in events)
    if names != expected[: len(names)] or len(names) > len(expected):
        result["status"] = "failed"
        result["failure"] = "unexpected host-pause event sequence"
        return result
    if len(events) < len(expected):
        return result

    open_fields = events[0][1]
    frozen_fields = events[1][1]
    close_fields = events[2][1]
    resumed_fields = events[3][1]
    open_generation = int(frozen_fields["open_generation"])
    resume_generation = int(resumed_fields["resume_generation"])
    numeric = {
        "open_generation": open_generation,
        "resume_generation": resume_generation,
        "frozen_samples": int(frozen_fields["samples"]),
        "frozen_duration_ms": int(frozen_fields["duration_ms"]),
        "paused_frame_delta": int(frozen_fields["frame_delta"]),
        "paused_present_delta": int(frozen_fields["present_delta"]),
        "resumed_samples": int(resumed_fields["samples"]),
        "resumed_duration_ms": int(resumed_fields["duration_ms"]),
        "resumed_frame_delta": int(resumed_fields["frame_delta"]),
        "resumed_present_delta": int(resumed_fields["present_delta"]),
    }
    result.update(numeric)
    valid = (
        open_fields["queued"] == "1"
        and close_fields["queued"] == "1"
        and open_generation > 0
        and int(close_fields["open_generation"]) == open_generation
        and int(resumed_fields["open_generation"]) == open_generation
        and resume_generation == open_generation + 1
        and numeric["frozen_samples"] >= 3
        and numeric["frozen_duration_ms"] >= 1000
        and numeric["paused_present_delta"] > 0
        and frozen_fields["world_stable"] == "1"
        and frozen_fields["ui_open"] == "1"
        and frozen_fields["owned"] == "1"
        and numeric["resumed_samples"] >= 2
        and numeric["resumed_duration_ms"] >= 500
        and numeric["resumed_frame_delta"] > 0
        and numeric["resumed_present_delta"] > 0
        and resumed_fields["ui_closed"] == "1"
        and resumed_fields["pause_released"] == "1"
        and resumed_fields["input_ready"] == "1"
    )
    if not valid:
        result["status"] = "failed"
        result["failure"] = "host-pause proof fields did not meet the gate"
        return result
    result["validated"] = True
    result["status"] = "pass"
    return result


def complete_local_pad_batches(
    observations: Observations, players: int
) -> tuple[dict[int, LocalPadSample], ...]:
    """Return complete requested-slot batches with every other port inactive."""
    if players < 1 or players > LOCAL_PAD_SLOT_COUNT:
        return ()
    requested_slots = set(range(1, players + 1))
    all_slots = set(range(1, LOCAL_PAD_SLOT_COUNT + 1))
    grouped: dict[int, dict[int, LocalPadSample]] = {}
    invalid_samples: set[int] = set()
    for sample in observations.local_pads:
        if sample.slot not in all_slots:
            invalid_samples.add(sample.sample)
            continue
        batch = grouped.setdefault(sample.sample, {})
        if sample.slot in batch:
            invalid_samples.add(sample.sample)
            continue
        batch[sample.slot] = sample

    complete: list[dict[int, LocalPadSample]] = []
    for sample_number in sorted(grouped):
        batch = grouped[sample_number]
        # The native trace publishes all four XInput ports. Requiring the full
        # cohort prevents a 2/3-player run from silently ignoring an extra
        # physical or virtual controller outside the requested match slots.
        if sample_number in invalid_samples or set(batch) != all_slots:
            continue
        first = batch[1]
        if any(
            item.poll != first.poll
            or item.frame != first.frame
            or item.present != first.present
            for item in batch.values()
        ):
            continue
        unexpected = (all_slots - requested_slots)
        if any(
            batch[slot].connected
            or batch[slot].device != 0
            or not batch[slot].neutral()
            for slot in unexpected
        ):
            continue
        complete.append({slot: batch[slot] for slot in sorted(requested_slots)})
    return tuple(complete)


def complete_local_player_batches(
    observations: Observations, players: int
) -> tuple[dict[int, LocalPlayerSample], ...]:
    """Return complete, internally consistent per-slot title-state cohorts."""
    if players < 1 or players > LOCAL_PAD_SLOT_COUNT:
        return ()
    requested_slots = set(range(1, players + 1))
    all_slots = set(range(1, LOCAL_PAD_SLOT_COUNT + 1))
    grouped: dict[int, dict[int, LocalPlayerSample]] = {}
    invalid_samples: set[int] = set()
    for sample in observations.local_players:
        if sample.slot not in all_slots:
            invalid_samples.add(sample.sample)
            continue
        batch = grouped.setdefault(sample.sample, {})
        if sample.slot in batch:
            invalid_samples.add(sample.sample)
            continue
        batch[sample.slot] = sample

    complete: list[dict[int, LocalPlayerSample]] = []
    for sample_number in sorted(grouped):
        batch = grouped[sample_number]
        # The native hook emits all four slots in one trace pass. Requiring all
        # four makes a truncated or interleaved log unusable as gameplay proof.
        if sample_number in invalid_samples or set(batch) != all_slots:
            continue
        first = batch[1]
        if any(
            item.poll != first.poll
            or item.frame != first.frame
            or item.present != first.present
            for item in batch.values()
        ):
            continue
        complete.append({slot: batch[slot] for slot in sorted(requested_slots)})
    return tuple(complete)


def complete_local_multiplayer_batches(
    observations: Observations, players: int
) -> tuple[
    tuple[dict[int, LocalPadSample], dict[int, LocalPlayerSample]], ...
]:
    """Join pad delivery and world-state proof on the exact native sample."""
    pads = {
        batch[1].sample: batch
        for batch in complete_local_pad_batches(observations, players)
    }
    title_players = {
        batch[1].sample: batch
        for batch in complete_local_player_batches(observations, players)
    }
    return tuple(
        (pads[sample], title_players[sample])
        for sample in sorted(pads.keys() & title_players.keys())
    )


def _distance(first: Sample, second: Sample) -> float:
    return math.sqrt(
        (second.x - first.x) ** 2
        + (second.y - first.y) ** 2
        + (second.z - first.z) ** 2
    )


class DamGameplayScenario:
    """Drive and prove one repeatable Dam control scenario through guest input."""

    SOAK_HEARTBEAT_INTERVAL_SECONDS = 1.0
    PHASE_TIMEOUT_SECONDS = 12.0
    MOVEMENT_DISTANCE = 2.0
    CAMERA_DELTA = 0.01
    ACTIVE_AXIS_THRESHOLD = 12_000
    NEUTRAL_AXIS_THRESHOLD = 2_000
    HOST_RESUME_PULSE_MS = 2_000
    HOST_RESUME_BASELINE_POSITION_DRIFT = 0.25
    HOST_RESUME_BASELINE_CAMERA_DRIFT = 0.002

    def __init__(self, command_fd: int, *, host_pause_required: bool = False):
        self.command_fd = command_fd
        self.host_pause_required = host_pause_required
        self.host_pause = parse_host_pause_evidence("")
        self.phase = "waiting-for-gameplay"
        self.phase_started_at = 0.0
        self.sequence = 0
        self.last_sequence = 0
        self.last_ack = 0
        self.acknowledged_sequences: set[int] = set()
        self.sent: list[dict[str, Any]] = []
        self.error: str | None = None
        self.completed = False
        self.observed_state: int | None = None
        self.observed_menu: str | None = None
        self.observed_joined = 0
        self.soak_active = False
        self.soak_heartbeat_batches: list[list[int]] = []
        self.next_soak_heartbeat_at = 0.0
        self.baseline: Sample | None = None
        self.input_baseline: Sample | None = None
        self.minimum_sample = 0
        self.minimum_poll = 0
        self.graphics_baseline: Graphics | None = None
        self.minimum_graphics_index = 0
        self.evidence: dict[str, Any] = {
            "look_input_observed": False,
            "look_input_sample": None,
            "camera_delta": 0.0,
            "camera_effect_sample": None,
            "movement_input_observed": False,
            "movement_input_sample": None,
            "movement_distance": 0.0,
            "movement_effect_sample": None,
            "fire_input_observed": False,
            "fire_input_sample": None,
            "fire_effect_observed": False,
            "fire_effect_sample": None,
            "ammo_before": None,
            "ammo_after": None,
            "ammo_decrement": 0,
            "pause_observed": False,
            "resume_observed": False,
            "graphics_toggled": False,
            "graphics_restored": False,
            "final_neutral_observed": False,
            "host_resume_probe_bounded": False,
            "host_resume_probe_hold_ms": 0,
            "host_resume_probe_sequence": None,
            "host_resume_neutral_baseline_sample": None,
            "host_resume_baseline_position_drift": None,
            "host_resume_baseline_camera_drift": None,
            "host_resume_probe_released": False,
        }

    def _set_phase(self, phase: str, elapsed: float) -> None:
        self.phase = phase
        self.phase_started_at = elapsed

    def _command_finished(self) -> bool:
        return self.last_sequence == 0 or all(
            sequence in self.acknowledged_sequences
            for sequence in range(1, self.last_sequence + 1)
        )

    def _send(self, command: str, elapsed: float, reason: str) -> bool:
        payload = command.encode("ascii")
        try:
            written = os.write(self.command_fd, payload)
            if written != len(payload):
                raise OSError(f"short virtual-gamepad write: {written}/{len(payload)}")
        except OSError as error:
            self.error = str(error)
            return False
        self.last_sequence = self.sequence
        self.sent.append(
            {
                "phase": self.phase,
                "reason": reason,
                "sent_seconds": elapsed,
                "commands": [command.rstrip()],
            }
        )
        return True

    def _set_axis(self, axis: str, value: int, elapsed: float, reason: str) -> bool:
        self.sequence += 1
        return self._send(
            f"SET_AXIS {self.sequence} 1 {axis} {value}\n", elapsed, reason
        )

    def _pulse_button(
        self, button: str, hold_ms: int, elapsed: float, reason: str
    ) -> bool:
        self.sequence += 1
        return self._send(
            f"PULSE_BUTTON {self.sequence} 1 {button} {hold_ms}\n",
            elapsed,
            reason,
        )

    def _pulse_axis(
        self, axis: str, value: int, hold_ms: int, elapsed: float, reason: str
    ) -> bool:
        self.sequence += 1
        return self._send(
            f"PULSE_AXIS {self.sequence} 1 {axis} {value} {hold_ms}\n",
            elapsed,
            reason,
        )

    def _reset(self, elapsed: float, reason: str) -> bool:
        self.sequence += 1
        return self._send(f"RESET {self.sequence} 1\n", elapsed, reason)

    def _fresh_samples(self, observations: Observations) -> tuple[Sample, ...]:
        return tuple(
            sample
            for sample in observations.samples
            if sample.sample > self.minimum_sample and sample.poll > self.minimum_poll
        )

    def _latest_eligible(self, observations: Observations) -> Sample | None:
        return next(
            (sample for sample in reversed(observations.samples) if sample.eligible()),
            None,
        )

    def _mark_observation_boundary(self, observations: Observations) -> None:
        if observations.samples:
            latest = observations.samples[-1]
            self.minimum_sample = latest.sample
            self.minimum_poll = latest.poll

    def _record_baseline(
        self, sample: Sample, observations: Observations | None = None
    ) -> None:
        self.baseline = sample
        self.input_baseline = None
        self.minimum_sample = sample.sample
        self.minimum_poll = sample.poll
        if observations is not None:
            self._mark_observation_boundary(observations)

    @staticmethod
    def _same_player(
        sample: Sample, baseline: Sample, *, require_coordinates: bool = False
    ) -> bool:
        return (
            sample.eligible()
            and sample.player == baseline.player
            and (not require_coordinates or sample.coordinates == baseline.coordinates)
        )

    @staticmethod
    def _is_dam_mission(mission: Mission | None) -> bool:
        return (
            mission is not None
            and mission.level == DAM_LEVEL_ID
            and mission.players == 1
            and not mission.network
        )

    def _fresh_graphics(self, observations: Observations) -> tuple[Graphics, ...]:
        return tuple(
            state
            for state in observations.graphics
            if state.index > self.minimum_graphics_index and state.known
        )

    def _update_protocol(self, observations: Observations) -> None:
        self.acknowledged_sequences = set(observations.acknowledgements)
        self.last_ack = max(self.acknowledged_sequences, default=0)
        if observations.rejected_sequence is not None:
            self.error = (
                f"virtual-gamepad command {observations.rejected_sequence} was rejected"
            )

    def _ready_baseline(self, observations: Observations) -> Sample | None:
        mission = observations.mission
        if (
            1 not in observations.ready_pads
            or not self._is_dam_mission(mission)
        ):
            return None
        eligible = [sample for sample in observations.samples if sample.eligible()]
        if len(eligible) < 2:
            return None
        previous, current = eligible[-2], eligible[-1]
        if (
            previous.player != current.player
            or previous.coordinates != current.coordinates
            or not self._sample_neutral(previous)
            or not self._sample_neutral(current)
            or not _counter_advanced(previous.frame, current.frame)
            or not _counter_advanced(previous.present, current.present)
        ):
            return None
        if self.host_pause_required:
            position_drift = _distance(previous, current)
            camera_drift = max(
                abs(current.yaw - previous.yaw),
                abs(current.pitch - previous.pitch),
            )
            if (
                position_drift > self.HOST_RESUME_BASELINE_POSITION_DRIFT
                or camera_drift > self.HOST_RESUME_BASELINE_CAMERA_DRIFT
            ):
                return None
            self.evidence["host_resume_neutral_baseline_sample"] = current.sample
            self.evidence["host_resume_baseline_position_drift"] = position_drift
            self.evidence["host_resume_baseline_camera_drift"] = camera_drift
        return current

    def _sample_neutral(self, sample: Sample) -> bool:
        return (
            sample.buttons == 0
            and sample.left_trigger == 0
            and sample.right_trigger == 0
            and abs(sample.left_x) < self.NEUTRAL_AXIS_THRESHOLD
            and abs(sample.left_y) < self.NEUTRAL_AXIS_THRESHOLD
            and abs(sample.right_x) < self.NEUTRAL_AXIS_THRESHOLD
            and abs(sample.right_y) < self.NEUTRAL_AXIS_THRESHOLD
        )

    def _advance_active(self, observations: Observations, elapsed: float) -> None:
        fresh = self._fresh_samples(observations)
        baseline = self.baseline
        if baseline is None:
            self.error = f"{self.phase} has no gameplay baseline"
            return

        if self.phase == "look-active":
            for sample in fresh:
                if (
                    self.input_baseline is None
                    and self._same_player(sample, baseline)
                    and abs(sample.right_x) >= self.ACTIVE_AXIS_THRESHOLD
                ):
                    self.input_baseline = sample
                    self.evidence["look_input_observed"] = True
                    self.evidence["look_input_sample"] = sample.sample
                    continue
                input_baseline = self.input_baseline
                if (
                    input_baseline is not None
                    and sample.sample > input_baseline.sample
                    and self._same_player(sample, input_baseline)
                ):
                    camera_delta = abs(sample.yaw - input_baseline.yaw)
                    if camera_delta > self.evidence["camera_delta"]:
                        self.evidence["camera_delta"] = camera_delta
                        self.evidence["camera_effect_sample"] = sample.sample
            if (
                self._command_finished()
                and self.evidence["look_input_observed"]
                and self.evidence["camera_delta"] >= self.CAMERA_DELTA
            ):
                latest = self._latest_eligible(observations)
                if latest and self._reset(elapsed, "release look input"):
                    self._mark_observation_boundary(observations)
                    self._set_phase("look-neutral", elapsed)
            return

        if self.phase == "move-active":
            for sample in fresh:
                if (
                    self.input_baseline is None
                    and self._same_player(
                        sample, baseline, require_coordinates=True
                    )
                    and abs(sample.left_y) >= self.ACTIVE_AXIS_THRESHOLD
                ):
                    self.input_baseline = sample
                    self.evidence["movement_input_observed"] = True
                    self.evidence["movement_input_sample"] = sample.sample
                    continue
                input_baseline = self.input_baseline
                if (
                    input_baseline is not None
                    and sample.sample > input_baseline.sample
                    and self._same_player(
                        sample, input_baseline, require_coordinates=True
                    )
                ):
                    distance = _distance(input_baseline, sample)
                    if distance > self.evidence["movement_distance"]:
                        self.evidence["movement_distance"] = distance
                        self.evidence["movement_effect_sample"] = sample.sample
            if (
                self._command_finished()
                and self.evidence["movement_input_observed"]
                and self.evidence["movement_distance"] >= self.MOVEMENT_DISTANCE
            ):
                latest = self._latest_eligible(observations)
                if latest and self._reset(elapsed, "release movement input"):
                    self._mark_observation_boundary(observations)
                    self._set_phase("move-neutral", elapsed)
            return

        if self.phase == "fire-active":
            for sample in fresh:
                if (
                    self._same_player(sample, baseline)
                    and sample.right_trigger > 0
                ):
                    if not self.evidence["fire_input_observed"]:
                        self.evidence["fire_input_observed"] = True
                        self.evidence["fire_input_sample"] = sample.sample
                    if (
                        baseline.right_magazine_valid
                        and sample.right_magazine_valid
                        and sample.weapon == baseline.weapon
                        and sample.right_magazine < baseline.right_magazine
                    ):
                        self.evidence["fire_effect_observed"] = True
                        self.evidence["fire_effect_sample"] = sample.sample
                        self.evidence["ammo_before"] = baseline.right_magazine
                        self.evidence["ammo_after"] = sample.right_magazine
                        self.evidence["ammo_decrement"] = (
                            baseline.right_magazine - sample.right_magazine
                        )
            if (
                self._command_finished()
                and self.evidence["fire_input_observed"]
                and self.evidence["fire_effect_observed"]
            ):
                latest = self._latest_eligible(observations)
                if latest and self._reset(elapsed, "release fire input"):
                    self._mark_observation_boundary(observations)
                    self._set_phase("fire-neutral", elapsed)
            return

    def _advance_neutral(self, observations: Observations, elapsed: float) -> None:
        if not self._command_finished():
            return
        neutral = next(
            (
                sample
                for sample in reversed(self._fresh_samples(observations))
                if sample.eligible()
                and (self.baseline is None or sample.player == self.baseline.player)
                and abs(sample.left_x) < self.NEUTRAL_AXIS_THRESHOLD
                and abs(sample.left_y) < self.NEUTRAL_AXIS_THRESHOLD
                and abs(sample.right_x) < self.NEUTRAL_AXIS_THRESHOLD
                and abs(sample.right_y) < self.NEUTRAL_AXIS_THRESHOLD
                and sample.left_trigger == 0
                and sample.right_trigger == 0
            ),
            None,
        )
        if neutral is None:
            return
        if self.phase == "move-neutral" and (
            not neutral.right_magazine_valid or neutral.right_magazine <= 0
        ):
            return
        self._record_baseline(neutral, observations)
        if self.phase == "look-neutral":
            if self.host_pause_required:
                self.evidence["host_resume_probe_released"] = True
            if self._set_axis("LY", -24_000, elapsed, "move Bond forward"):
                self._set_phase("move-active", elapsed)
        elif self.phase == "move-neutral":
            if self._set_axis("RT", 32_767, elapsed, "fire the current weapon"):
                self._set_phase("fire-active", elapsed)
        elif self.phase == "fire-neutral":
            if self._pulse_button("START", 300, elapsed, "open the retail pause menu"):
                self._set_phase("pause-opening", elapsed)
        elif self.phase == "final-neutral":
            self.evidence["final_neutral_observed"] = True
            self.completed = True
            self._set_phase("complete", elapsed)

    def advance(
        self,
        log_text: str,
        elapsed: float,
        *,
        host_pause_text: str | None = None,
    ) -> None:
        if self.error or self.command_fd < 0:
            return
        observations = parse_observations(log_text)
        self._update_protocol(observations)
        if self.error:
            return

        if self.host_pause_required:
            self.host_pause = parse_host_pause_evidence(
                log_text if host_pause_text is None else host_pause_text
            )
            if self.host_pause["status"] == "failed":
                self.error = (
                    "host pause integration proof failed: "
                    + str(self.host_pause["failure"])
                )
                return
            if not self.host_pause["validated"]:
                return

        if self.soak_active:
            self._advance_soak(observations, elapsed)
            return

        if self.completed:
            return

        if self.phase != "waiting-for-gameplay" and not self._is_dam_mission(
            observations.mission
        ):
            mission = observations.mission
            identity = "unknown" if mission is None else str(mission.level)
            self.error = f"gameplay scenario left Dam mission (level={identity})"
            return

        if self.phase != "waiting-for-gameplay" and (
            elapsed - self.phase_started_at > self.PHASE_TIMEOUT_SECONDS
        ):
            self.error = f"gameplay scenario timed out in phase {self.phase}"
            return

        if self.phase == "waiting-for-gameplay":
            baseline = self._ready_baseline(observations)
            if baseline is None:
                return
            self._record_baseline(baseline, observations)
            if self.host_pause_required:
                sent = self._pulse_axis(
                    "RX",
                    16_000,
                    self.HOST_RESUME_PULSE_MS,
                    elapsed,
                    "prove bounded camera input after Host Settings resume",
                )
                if sent:
                    self.evidence["host_resume_probe_bounded"] = True
                    self.evidence["host_resume_probe_hold_ms"] = (
                        self.HOST_RESUME_PULSE_MS
                    )
                    self.evidence["host_resume_probe_sequence"] = self.sequence
            else:
                sent = self._set_axis(
                    "RX", 16_000, elapsed, "turn the camera right"
                )
            if sent:
                self._set_phase("look-active", elapsed)
            return

        if self.phase in ("look-active", "move-active", "fire-active"):
            self._advance_active(observations, elapsed)
            return

        if self.phase in (
            "look-neutral",
            "move-neutral",
            "fire-neutral",
            "final-neutral",
        ):
            self._advance_neutral(observations, elapsed)
            return

        fresh = self._fresh_samples(observations)
        if self.phase == "pause-opening":
            if self._command_finished() and any(sample.pause != 0 for sample in fresh):
                self.evidence["pause_observed"] = True
                self._mark_observation_boundary(observations)
                if self._pulse_button(
                    "START", 300, elapsed, "resume from retail pause"
                ):
                    self._set_phase("pause-closing", elapsed)
            return

        if self.phase == "pause-closing":
            resumed = next(
                (sample for sample in reversed(fresh) if sample.pause == 0), None
            )
            if self._command_finished() and resumed is not None:
                self.evidence["resume_observed"] = True
                known_graphics = next(
                    (state for state in reversed(observations.graphics) if state.known),
                    None,
                )
                if known_graphics is None:
                    return
                self.graphics_baseline = known_graphics
                self.minimum_graphics_index = known_graphics.index
                if self._pulse_button(
                    "RIGHT_SHOULDER",
                    400,
                    elapsed,
                    "toggle original/remastered graphics",
                ):
                    self._mark_observation_boundary(observations)
                    self._set_phase("graphics-toggle", elapsed)
            return

        if self.phase == "graphics-toggle":
            baseline_graphics = self.graphics_baseline
            changed = next(
                (
                    state
                    for state in self._fresh_graphics(observations)
                    if baseline_graphics is not None
                    and state.original != baseline_graphics.original
                ),
                None,
            )
            if self._command_finished() and changed is not None:
                self.evidence["graphics_toggled"] = True
                self.minimum_graphics_index = changed.index
                if self._pulse_button(
                    "RIGHT_SHOULDER", 400, elapsed, "restore starting graphics mode"
                ):
                    self._mark_observation_boundary(observations)
                    self._set_phase("graphics-restore", elapsed)
            return

        if self.phase == "graphics-restore":
            restored = next(
                (
                    state
                    for state in self._fresh_graphics(observations)
                    if self.graphics_baseline is not None
                    and state.original == self.graphics_baseline.original
                ),
                None,
            )
            if self._command_finished() and restored is not None:
                self.evidence["graphics_restored"] = True
                latest = (
                    observations.samples[-1] if observations.samples else self.baseline
                )
                if latest is not None:
                    self.minimum_sample = latest.sample
                    self.minimum_poll = latest.poll
                if self._reset(elapsed, "finish with neutral controller state"):
                    self._set_phase("final-neutral", elapsed)

    def begin_post_ready_soak(self, elapsed: float, *, heartbeat_enabled: bool) -> None:
        self.soak_active = heartbeat_enabled
        self._set_phase("post-ready-soak", elapsed)
        if heartbeat_enabled:
            self._send_soak_heartbeat(elapsed)

    def _send_soak_heartbeat(self, elapsed: float) -> None:
        first_sequence = self.sequence + 1
        if self._reset(elapsed, "verify post-ready virtual-gamepad input continuity"):
            self.soak_heartbeat_batches.append([first_sequence])
            self.next_soak_heartbeat_at = elapsed + self.SOAK_HEARTBEAT_INTERVAL_SECONDS

    def _advance_soak(self, observations: Observations, elapsed: float) -> None:
        pending = [
            sequence
            for batch in self.soak_heartbeat_batches
            for sequence in batch
            if sequence not in observations.acknowledgements
        ]
        if not pending and elapsed >= self.next_soak_heartbeat_at:
            self._send_soak_heartbeat(elapsed)

    def finish_post_ready_soak(self, log_text: str, elapsed: float) -> None:
        if not self.soak_active or self.error:
            return
        observations = parse_observations(log_text)
        self._update_protocol(observations)
        missing = [
            sequence
            for batch in self.soak_heartbeat_batches
            for sequence in batch
            if sequence not in self.acknowledged_sequences
        ]
        if missing:
            self.error = (
                "post-ready virtual-gamepad heartbeat was not acknowledged: "
                + ", ".join(str(sequence) for sequence in missing)
            )

    def result(self) -> dict[str, Any]:
        return {
            "phase": self.phase,
            "completed": self.completed,
            "error": self.error,
            "baseline": asdict(self.baseline) if self.baseline else None,
            "graphics_baseline": (
                asdict(self.graphics_baseline) if self.graphics_baseline else None
            ),
            "host_pause_required": self.host_pause_required,
            "host_pause": dict(self.host_pause),
            "evidence": dict(self.evidence),
        }

    def close(self) -> None:
        if self.command_fd >= 0:
            os.close(self.command_fd)
            self.command_fd = -1
