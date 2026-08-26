#!/usr/bin/env python3

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "config/goldeneye-release.json"
BUILD_SCRIPT = ROOT / "launcher/build-app.sh"
RELEASE_SCRIPT = ROOT / "tools/sign-notarize.sh"


def manifest_metadata() -> dict[str, str]:
    parsed = json.loads(MANIFEST.read_text(encoding="utf-8"))
    return {key: str(value) for key, value in parsed.items()}


def script_metadata(script: Path, overrides: dict[str, str] | None = None) -> dict[str, str]:
    environment = os.environ.copy()
    for key in ("VERSION", "APP_VERSION", "BUILD_NUMBER"):
        environment.pop(key, None)
    environment.update(overrides or {})
    result = subprocess.run(
        ["/bin/bash", str(script), "--print-version"],
        cwd=ROOT,
        env=environment,
        check=True,
        capture_output=True,
        text=True,
    )
    return dict(line.split("=", 1) for line in result.stdout.splitlines())


class ReleaseVersionTest(unittest.TestCase):
    def test_manifest_has_valid_schema(self) -> None:
        raw = json.loads(MANIFEST.read_text(encoding="utf-8"))
        self.assertEqual(set(raw), {"version", "build"})
        self.assertIsInstance(raw["version"], str)
        self.assertIsInstance(raw["build"], str)
        self.assertRegex(raw["version"], r"^[0-9]+\.[0-9]+(?:\.[0-9]+)?$")
        self.assertRegex(raw["build"], r"^[0-9]+(?:\.[0-9]+){0,2}$")

    @unittest.skipUnless(sys.platform == "darwin", "release scripts require macOS")
    def test_build_and_notarization_scripts_read_manifest_defaults(self) -> None:
        expected = manifest_metadata()
        expected_output = {
            "VERSION": expected["version"],
            "BUILD_NUMBER": expected["build"],
        }
        self.assertEqual(script_metadata(BUILD_SCRIPT), expected_output)
        self.assertEqual(script_metadata(RELEASE_SCRIPT), expected_output)

    @unittest.skipUnless(sys.platform == "darwin", "release scripts require macOS")
    def test_intentional_overrides_are_consistent_and_nonpersistent(self) -> None:
        override = {"VERSION": "9.8.7", "BUILD_NUMBER": "42"}
        expected = {"VERSION": "9.8.7", "BUILD_NUMBER": "42"}
        self.assertEqual(script_metadata(BUILD_SCRIPT, override), expected)
        self.assertEqual(script_metadata(RELEASE_SCRIPT, override), expected)
        self.assertEqual(
            script_metadata(BUILD_SCRIPT)["VERSION"],
            manifest_metadata()["version"],
        )

    def test_bundle_generation_has_no_hardcoded_release_number(self) -> None:
        manifest_version = manifest_metadata()["version"]
        for relative in (
            "launcher/build-app.sh",
            "tools/sign-notarize.sh",
            "vendor/GoldenEye-Recomp/CMakeLists.txt",
        ):
            source = (ROOT / relative).read_text(encoding="utf-8")
            self.assertNotIn(manifest_version, source, relative)
            self.assertIn("goldeneye-release.json", source, relative)

        cmake = (ROOT / "vendor/GoldenEye-Recomp/CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        self.assertIn("unset(GOLDENEYE_VERSION CACHE)", cmake)
        self.assertIn("unset(GOLDENEYE_BUILD_NUMBER CACHE)", cmake)

        verifier = (
            ROOT / "vendor/GoldenEye-Recomp/packaging/macos/verify_app.sh"
        ).read_text(encoding="utf-8")
        self.assertIn('ACTUAL_VERSION" = "$EXPECTED_VERSION', verifier)
        self.assertIn('ACTUAL_BUILD_NUMBER" = "$EXPECTED_BUILD_NUMBER', verifier)
        self.assertIn("GOLDENEYE_TEST_CAPTURE_DAM_FRAME", verifier)
        self.assertIn("REX_TEST_VIRTUAL_GAMEPADS", verifier)

        cmake = (ROOT / "vendor/GoldenEye-Recomp/CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        self.assertIn("REXGLUE_ENABLE_INPUT_TEST_HARNESS}>,on,off", cmake)
        self.assertIn("${CMAKE_BINARY_DIR}/rexglue-output", cmake)

        build_script = BUILD_SCRIPT.read_text(encoding="utf-8")
        self.assertIn(
            'RELEASE_BUILD_DIR="$REPO_ROOT/vendor/GoldenEye-Recomp/out/build/'
            'macos-arm64-release"',
            build_script,
        )
        self.assertIn(
            'RELEASE_REXGLUE_OUTPUT="$RELEASE_BUILD_DIR/rexglue-output"',
            build_script,
        )
        self.assertIn(
            '"-DREXGLUE_OUTPUT_DIRECTORY=$RELEASE_REXGLUE_OUTPUT"',
            build_script,
        )
        self.assertIn('cmake --build "$RELEASE_BUILD_DIR"', build_script)

    def test_release_links_follow_manifest(self) -> None:
        version = manifest_metadata()["version"]
        artifact = f"GoldenEye-Metal-{version}-macos-arm64"
        readme = (ROOT / "README.md").read_text(encoding="utf-8")
        distribution = (ROOT / "docs/MACOS_DISTRIBUTION.md").read_text(
            encoding="utf-8"
        )
        self.assertIn(f"releases/download/v{version}/{artifact}.dmg", readme)
        self.assertIn(f"releases/tag/v{version}", readme)
        self.assertIn(f"release/{artifact}.zip", distribution)
        self.assertIn(f"release/{artifact}.dmg", distribution)


if __name__ == "__main__":
    unittest.main()
