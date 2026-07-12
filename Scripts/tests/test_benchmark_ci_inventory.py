#!/usr/bin/env python3
"""Fail hosted static CI when a benchmark corpus or Python contract test is untracked.

The static-check configuration is the execution manifest for benchmark contract
coverage.  A newly added benchmark or lightweight Python test must not silently
exist outside that manifest, because it would pass locally while never running
in hosted CI.

Run from any directory:
    python Scripts/tests/test_benchmark_ci_inventory.py
"""

from __future__ import annotations

import json
import pathlib
import unittest


MONOLITH_ROOT = pathlib.Path(__file__).resolve().parents[2]
CONFIG_PATH = MONOLITH_ROOT / ".github" / "monolith-static-ci.json"


def _relative(path: pathlib.Path) -> str:
    return path.relative_to(MONOLITH_ROOT).as_posix()


class BenchmarkCIInventoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.config = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))

    def test_every_benchmark_manifest_has_one_definition(self) -> None:
        benchmark_config = self.config.get("benchmark_definitions", {})
        self.assertTrue(benchmark_config.get("enabled"), "benchmark_definitions must stay enabled")

        definitions = benchmark_config.get("definitions", [])
        configured_manifests = [str(row.get("manifest", "")) for row in definitions]
        discovered_manifests = sorted(
            _relative(path)
            for path in (MONOLITH_ROOT / "Benchmarks").glob("*/manifest.json")
        )

        self.assertEqual(
            len(configured_manifests),
            len(set(configured_manifests)),
            f"duplicate benchmark manifest definitions: {configured_manifests}",
        )
        self.assertEqual(
            set(configured_manifests),
            set(discovered_manifests),
            "benchmark_definitions must cover every Benchmarks/*/manifest.json exactly once",
        )

    def test_definition_names_are_unique_and_match_manifest_directories(self) -> None:
        definitions = self.config["benchmark_definitions"]["definitions"]
        names = [str(row.get("name", "")) for row in definitions]
        expected_names = [pathlib.PurePosixPath(str(row["manifest"])).parent.name for row in definitions]

        self.assertEqual(len(names), len(set(names)), f"duplicate benchmark definition names: {names}")
        self.assertEqual(
            names,
            expected_names,
            "each benchmark definition name must equal its manifest directory name",
        )

    def test_every_lightweight_python_test_is_in_the_ci_contract_list(self) -> None:
        test_config = self.config.get("benchmark_contract_tests", {})
        self.assertTrue(test_config.get("enabled"), "benchmark_contract_tests must stay enabled")

        configured_tests = [str(row.get("script", "")) for row in test_config.get("tests", [])]
        discovered_paths = sorted((MONOLITH_ROOT / "Scripts").glob("test_*.py"))
        discovered_paths.extend(sorted((MONOLITH_ROOT / "Scripts" / "tests").glob("test_*.py")))
        discovered_tests = [_relative(path) for path in discovered_paths]

        self.assertEqual(
            len(configured_tests),
            len(set(configured_tests)),
            f"duplicate benchmark contract test scripts: {configured_tests}",
        )
        self.assertEqual(
            set(configured_tests),
            set(discovered_tests),
            "benchmark_contract_tests must execute every Scripts/test_*.py and Scripts/tests/test_*.py",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
