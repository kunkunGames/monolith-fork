#!/usr/bin/env python3
"""Regression coverage for executable-relative Monolith DB resolution."""

from __future__ import annotations

import json
import os
import shutil
import sqlite3
import subprocess
import tempfile
import unittest
from pathlib import Path


PLUGIN_ROOT = Path(__file__).resolve().parents[2]


class DefaultDatabaseResolutionTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "nt", "monolith_query.exe is a Windows artifact")
    def test_binaries_entry_points_prefer_monolith_saved_over_sibling_plugins_saved(self) -> None:
        manifest_path = PLUGIN_ROOT / "Binaries" / "monolith_query.current.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        source_exes = [
            manifest_path.parent / manifest["file"],
            manifest_path.parent / "monolith_query.exe",
        ]
        bundle_files = [
            manifest_path,
            manifest_path.parent / manifest["file"],
            manifest_path.parent / manifest["catalog_file"],
        ]

        for source_exe in source_exes:
            with self.subTest(executable=source_exe.name):
                self.assertTrue(source_exe.is_file(), source_exe)
                self._assert_prefers_monolith_saved(source_exe, bundle_files)

    def _assert_prefers_monolith_saved(
        self,
        source_exe: Path,
        bundle_files: list[Path],
    ) -> None:
        with tempfile.TemporaryDirectory(prefix="monolith-query-db-root-") as temp:
            temp_root = Path(temp)
            plugin_root = temp_root / "Plugins" / "Monolith"
            binaries_dir = plugin_root / "Binaries"
            plugin_saved = plugin_root / "Saved"
            sibling_saved = temp_root / "Plugins" / "Saved"
            binaries_dir.mkdir(parents=True)
            plugin_saved.mkdir()
            sibling_saved.mkdir()

            # Perforce keeps tracked binaries read-only. copy2 would preserve
            # that attribute and then fail when the current hashed executable
            # is also source_exe and the same destination is overwritten.
            # These fixtures need file contents only, not depot metadata.
            files_to_copy = dict.fromkeys([*bundle_files, source_exe])
            for bundle_file in files_to_copy:
                shutil.copyfile(bundle_file, binaries_dir / bundle_file.name)
            shutil.copyfile(PLUGIN_ROOT / "Monolith.uplugin", plugin_root / "Monolith.uplugin")

            correct_db = plugin_saved / "ProjectIndex.db"
            connection = sqlite3.connect(correct_db)
            connection.close()

            # The historical relative-probe order selected this broader path
            # first whenever Plugins/Saved happened to exist.
            (sibling_saved / "ProjectIndex.db").write_bytes(b"not a sqlite database")

            env = os.environ.copy()
            env["MONOLITH_TOOL_LOG_ENABLED"] = "0"
            result = subprocess.run(
                [
                    str(binaries_dir / source_exe.name),
                    "project",
                    "health",
                    "--include-counts=false",
                ],
                cwd=temp_root,
                env=env,
                capture_output=True,
                text=True,
                timeout=60,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            payload = json.loads(result.stdout)
            self.assertIn(payload["status"], {"ok", "warning"})
            self.assertNotIn(str(sibling_saved), result.stderr)


if __name__ == "__main__":
    unittest.main()
