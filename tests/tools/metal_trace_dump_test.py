#!/usr/bin/env python3

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/metal-trace-dump.sh"


class MetalTraceDumpScriptTest(unittest.TestCase):
    def _fixture(self, root: Path) -> tuple[Path, Path, Path, dict[str, str]]:
        trace = root / "capture.xtr"
        trace.write_bytes(b"trace fixture")

        build = root / "build"
        build.mkdir()
        capture = root / "argv.json"
        executable = build / "trace_dump_metal"
        executable.write_text(
            """#!/usr/bin/env python3
import json, os, sys
with open(os.environ["TRACE_ARGS_OUT"], "w", encoding="utf-8") as stream:
    json.dump(sys.argv[1:], stream)
""",
            encoding="utf-8",
        )
        executable.chmod(0o755)

        bin_dir = root / "bin"
        bin_dir.mkdir()
        cmake = bin_dir / "cmake"
        cmake.write_text(
            """#!/usr/bin/env python3
import json, os, sys
with open(os.environ["CMAKE_ARGS_OUT"], "a", encoding="utf-8") as stream:
    stream.write(json.dumps(sys.argv[1:]) + "\\n")
""",
            encoding="utf-8",
        )
        cmake.chmod(0o755)
        cmake_capture = root / "cmake-argv.jsonl"

        environment = {
            **os.environ,
            "GOLDENEYE_TRACE_BUILD_DIR": str(build),
            "GOLDENEYE_TRACE_JOBS": "1",
            "PATH": f"{bin_dir}:{os.environ.get('PATH', '')}",
            "TRACE_ARGS_OUT": str(capture),
            "CMAKE_ARGS_OUT": str(cmake_capture),
        }
        return trace, capture, cmake_capture, environment

    def test_default_build_is_dedicated_to_the_trace_tool(self):
        script = SCRIPT.read_text(encoding="utf-8")
        self.assertIn(
            'BUILD="${GOLDENEYE_TRACE_BUILD_DIR:-$ROOT/out/tools/metal-trace-dump}"',
            script,
        )

    def test_best_effort_is_order_independent_and_forwarded(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            trace, capture, _, environment = self._fixture(root)
            output = root / "frame"
            result = subprocess.run(
                [
                    "/bin/bash",
                    str(SCRIPT),
                    "--best-effort",
                    str(trace),
                    str(output),
                    "7",
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=environment,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(
                json.loads(capture.read_text(encoding="utf-8")),
                [str(trace), str(output), "7", "--best-effort"],
            )

    def test_default_does_not_enable_best_effort(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            trace, capture, cmake_capture, environment = self._fixture(root)
            result = subprocess.run(
                ["/bin/bash", str(SCRIPT), str(trace)],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=environment,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            arguments = json.loads(capture.read_text(encoding="utf-8"))
            self.assertEqual(arguments[0], str(trace))
            self.assertEqual(arguments[2], "0")
            self.assertNotIn("--best-effort", arguments)
            cmake_invocations = [
                json.loads(line)
                for line in cmake_capture.read_text(encoding="utf-8").splitlines()
            ]
            self.assertEqual(len(cmake_invocations), 2)
            self.assertIn("-DCMAKE_BUILD_TYPE=Release", cmake_invocations[0])
            self.assertIn("-DCMAKE_OSX_ARCHITECTURES=arm64", cmake_invocations[0])
            self.assertIn("-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0", cmake_invocations[0])
            self.assertIn(
                "-DGOLDENEYE_BUILD_METAL_TRACE_DUMP=ON", cmake_invocations[0]
            )
            self.assertIn("--config", cmake_invocations[1])
            self.assertEqual(
                cmake_invocations[1][cmake_invocations[1].index("--config") + 1],
                "Release",
            )

    def test_unknown_option_is_rejected_before_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            trace, capture, _, environment = self._fixture(root)
            result = subprocess.run(
                ["/bin/bash", str(SCRIPT), str(trace), "--unsafe"],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=environment,
            )
            self.assertEqual(result.returncode, 2)
            self.assertIn("unknown option", result.stderr)
            self.assertFalse(capture.exists())


if __name__ == "__main__":
    unittest.main()
