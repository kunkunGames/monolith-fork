#!/usr/bin/env python3
"""Lock release output and updater parsing to the platform-safe v2 SHA contract."""

from __future__ import annotations

import pathlib
import unittest


MONOLITH_ROOT = pathlib.Path(__file__).resolve().parents[2]


class ReleaseSha256MarkerContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.release_script = (MONOLITH_ROOT / "Scripts" / "make_release.ps1").read_text(
            encoding="utf-8"
        )
        cls.updater_source = (
            MONOLITH_ROOT / "Source" / "MonolithCore" / "Private" / "MonolithUpdateSubsystem.cpp"
        ).read_text(encoding="utf-8")
        cls.hash_source = (
            MONOLITH_ROOT / "Source" / "MonolithCore" / "Private" / "MonolithHashUtils.cpp"
        ).read_text(encoding="utf-8")
        cls.build_rules = (
            MONOLITH_ROOT / "Source" / "MonolithCore" / "MonolithCore.Build.cs"
        ).read_text(encoding="utf-8")
        cls.macos_workflow = (
            MONOLITH_ROOT / ".github" / "workflows" / "macos-build.yml"
        ).read_text(encoding="utf-8")

    def test_windows_release_emits_only_v2_markers(self) -> None:
        self.assertIn(
            'Write-Host "  Monolith-SHA256-v2-$($eng.Tag): $h"',
            self.release_script,
        )
        self.assertIn(
            'Write-Host "  Monolith-SHA256-v2: $LegacyHash"',
            self.release_script,
        )
        self.assertNotIn(
            'Write-Host "  Monolith-SHA256-$($eng.Tag): $h"',
            self.release_script,
        )
        self.assertNotIn(
            'Write-Host "  Monolith-SHA256: $LegacyHash"',
            self.release_script,
        )

    def test_runtime_has_distinct_platform_v2_marker_families(self) -> None:
        for marker in (
            "Monolith-SHA256-v2",
            "Monolith-macOS-SHA256-v2",
            "Monolith-Linux-SHA256-v2",
        ):
            with self.subTest(marker=marker):
                self.assertIn(f'TEXT("{marker}")', self.updater_source)

        self.assertIn("BuildSha256MarkerName(EngineTag)", self.updater_source)
        self.assertIn("ParseSha256FromReleaseNotes(", self.updater_source)
        self.assertIn("ReleaseNotes, EngineTag);", self.updater_source)

    def test_macos_workflow_emits_only_the_v2_marker(self) -> None:
        self.assertIn('echo "Monolith-macOS-SHA256-v2: $SHA"', self.macos_workflow)
        self.assertNotIn("**Monolith-macOS-SHA256-v2:** $SHA", self.macos_workflow)
        self.assertNotIn("**Monolith-macOS-SHA256:** $SHA", self.macos_workflow)

    def test_updater_uses_portable_shared_sha_backend(self) -> None:
        self.assertIn("FMonolithHashUtils::TrySha256Bytes", self.updater_source)
        self.assertNotIn("FPlatformMisc::GetSHA256Signature", self.updater_source)
        self.assertIn("Sha256RoundConstants", self.hash_source)
        self.assertNotIn("BCrypt", self.hash_source)
        self.assertNotIn("bcrypt.lib", self.build_rules)


if __name__ == "__main__":
    unittest.main()
