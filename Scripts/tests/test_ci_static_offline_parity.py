#!/usr/bin/env python3
"""Regression tests for authoritative Query selection in hosted static CI."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
CI_SCRIPT = ROOT / "Scripts" / "ci_static_checks.py"
FRESHNESS_SCRIPT = ROOT / "Scripts" / "check_offline_exe_fresh.py"
SOURCE_HASH_SCRIPT = ROOT / "Scripts" / "source_generation_hash.py"


def load_ci_module():
    spec = importlib.util.spec_from_file_location("monolith_ci_static_checks_test", CI_SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load ci_static_checks.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class OfflineParityExecutableSelectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.ci = load_ci_module()

    def test_stale_fixed_alias_does_not_override_manifest_selected_image(self) -> None:
        with tempfile.TemporaryDirectory(prefix="monolith-static-parity-") as temp:
            root = Path(temp)
            scripts = root / "Scripts"
            binaries = root / "Binaries"
            scripts.mkdir()
            binaries.mkdir()

            shutil.copy2(FRESHNESS_SCRIPT, scripts / FRESHNESS_SCRIPT.name)
            shutil.copy2(SOURCE_HASH_SCRIPT, scripts / SOURCE_HASH_SCRIPT.name)
            benchmark = scripts / "offline_parity_benchmark.py"
            benchmark.write_text("# subprocess is mocked by this fixture\n", encoding="utf-8")

            fixed_alias = binaries / "monolith_query.exe"
            fixed_alias.write_text("stale fixed compatibility image\n", encoding="utf-8")
            source_hash = "0123456789abcdef"
            immutable = binaries / f"monolith_query-{source_hash}.exe"
            immutable.write_text("authoritative immutable image\n", encoding="utf-8")
            (binaries / "monolith_query.current.json").write_text(
                json.dumps({
                    "tool": "monolith_query",
                    "runtime": "native-cpp",
                    "source_hash": source_hash,
                    "file": immutable.name,
                }),
                encoding="utf-8",
            )

            config = {
                "offline_exe_freshness": {
                    "script": "Scripts/check_offline_exe_fresh.py",
                },
                "offline_parity_smoke": {
                    "enabled": True,
                    "script": "Scripts/offline_parity_benchmark.py",
                    "min_score": 0.8,
                    "max_error_rate": 0.1,
                    "timeout_seconds": 5,
                },
            }
            config_path = root / "monolith-static-ci.json"
            config_path.write_text(json.dumps(config), encoding="utf-8")
            ctx = self.ci.CheckContext(root, config, config_path)
            selected_paths: list[Path] = []

            def fake_run(command, **kwargs):
                self.assertIn("--exe-path", command)
                selected = Path(command[command.index("--exe-path") + 1]).resolve()
                selected_paths.append(selected)
                output_dir = Path(command[command.index("--output-dir") + 1])
                (output_dir / "summary.json").write_text(
                    json.dumps({
                        "metrics": {
                            "offline_parity_score": 1.0,
                            "error_rate": 0.0,
                        },
                    }),
                    encoding="utf-8",
                )
                return subprocess.CompletedProcess(command, 0, "", "")

            with mock.patch.object(self.ci.subprocess, "run", side_effect=fake_run):
                self.ci.check_offline_parity_smoke(ctx)

            self.assertEqual([immutable.resolve()], selected_paths)
            self.assertNotEqual(fixed_alias.resolve(), selected_paths[0])
            self.assertFalse(
                any(finding.severity == "blocker" for finding in ctx.findings),
                ctx.findings,
            )


if __name__ == "__main__":
    unittest.main()
