#!/usr/bin/env python3

from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
LAUNCHER = ROOT / "vendor/GoldenEye-Recomp/src/ge_launcher_macos.mm"
GPU_CAPTURE_HEADER = ROOT / "vendor/GoldenEye-Recomp/src/ge_gpu_capture.h"


def braced_body(source: str, signature: str) -> str:
    search_offset = 0
    while True:
        signature_offset = source.index(signature, search_offset)
        body_search_start = signature_offset + len(signature)
        opening = source.index("{", body_search_start)
        declaration_end = source.find(";", body_search_start)
        if declaration_end == -1 or opening < declaration_end:
            break
        search_offset = body_search_start
    depth = 0
    for offset in range(opening, len(source)):
        character = source[offset]
        if character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : offset]
    raise AssertionError(f"unterminated body for {signature}")


class DiagnosticPrivacyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.launcher = LAUNCHER.read_text(encoding="utf-8")
        cls.capture_header = GPU_CAPTURE_HEADER.read_text(encoding="utf-8")

    def test_gpu_capture_is_excluded_by_default(self) -> None:
        self.assertIn(
            "constexpr bool kIncludeInDiagnosticsByDefault = false;",
            self.capture_header,
        )
        show_body = braced_body(self.launcher, "- (void)show")
        normalized_show = " ".join(show_body.split())
        self.assertIn(
            "setState:ge::gpu_capture::kIncludeInDiagnosticsByDefault "
            "? NSControlStateValueOn : NSControlStateValueOff",
            normalized_show,
        )

    def test_export_wires_checkbox_to_the_opt_in_copy_branch(self) -> None:
        export_action = braced_body(self.launcher, "- (void)exportDiagnostics:")
        self.assertIn("gpu_capture_available_ &&", export_action)
        self.assertIn(
            "[include_gpu_capture_ state] == NSControlStateValueOn",
            export_action,
        )
        self.assertIn("includeGPUCapture:include_gpu_capture", export_action)

        begin_export = braced_body(self.launcher, "- (void)beginDiagnosticExport:")
        self.assertIn("includeGPUCapture == YES", begin_export)
        self.assertIn("ExportDiagnosticBundle(paths, destination,", begin_export)
        self.assertIn("include_gpu_capture, &error", begin_export)

        bundle_export = braced_body(self.launcher, "bool ExportDiagnosticBundle(")
        opt_in = braced_body(bundle_export, "if (include_gpu_capture)")
        copy_call = "CopyCompletedCaptureForDiagnostics("
        self.assertEqual(bundle_export.count(copy_call), 1)
        self.assertIn(copy_call, opt_in)


if __name__ == "__main__":
    unittest.main()
