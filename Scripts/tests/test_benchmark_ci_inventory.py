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
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


MONOLITH_ROOT = pathlib.Path(__file__).resolve().parents[2]
CONFIG_PATH = MONOLITH_ROOT / ".github" / "monolith-static-ci.json"
SCRIPTS_DIR = MONOLITH_ROOT / "Scripts"
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

import benchmark_common as benchmark_common  # noqa: E402
import benchmark_inventory as benchmark_inventory  # noqa: E402
import schema_completeness_benchmark as schema_benchmark  # noqa: E402


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

    def test_completion_inventory_portable_check_matches_manifests_and_corpora(self) -> None:
        result = subprocess.run(
            [
                sys.executable,
                str(MONOLITH_ROOT / "Scripts" / "benchmark_inventory.py"),
                "--portable-check",
            ],
            cwd=MONOLITH_ROOT,
            text=True,
            capture_output=True,
            timeout=30,
            check=False,
        )
        self.assertEqual(
            result.returncode,
            0,
            "portable benchmark completion inventory drifted:\n"
            + result.stdout
            + result.stderr,
        )

    def test_portable_inventory_survives_a_clean_checkout_without_saved_artifacts(self) -> None:
        saved_root = (MONOLITH_ROOT / "Saved").resolve()
        original_is_file = pathlib.Path.is_file

        def clean_checkout_is_file(path: pathlib.Path) -> bool:
            resolved = path.resolve()
            if resolved == saved_root or saved_root in resolved.parents:
                return False
            return original_is_file(path)

        report: dict[str, list[str]] = {
            "attested_databases": [],
            "omitted_pending_evidence": [],
        }
        with mock.patch.object(pathlib.Path, "is_file", clean_checkout_is_file):
            rendered = benchmark_inventory.build(
                portable=True,
                validation_report=report,
            )
            with self.assertRaisesRegex(
                benchmark_inventory.InventoryError,
                "database input drifted",
            ):
                benchmark_inventory.build(portable=False)

        self.assertEqual(
            rendered,
            (MONOLITH_ROOT / "Benchmarks" / "INVENTORY.md").read_text(encoding="utf-8"),
        )
        self.assertEqual(report["attested_databases"], ["Saved/EngineSource.db"])
        self.assertTrue(report["omitted_pending_evidence"])
        self.assertTrue(
            all(path.startswith("Saved/") for path in report["omitted_pending_evidence"])
        )


class AcceptedEvidenceValidationTests(unittest.TestCase):
    """Adversarial checks for the accepted-evidence trust boundary."""

    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp_dir.cleanup)
        self.root = pathlib.Path(self.temp_dir.name)
        self.patch_root = mock.patch.object(
            benchmark_inventory,
            "MONOLITH_ROOT",
            self.root,
        )
        self.patch_root.start()
        self.addCleanup(self.patch_root.stop)
        self.patch_benchmark_root = mock.patch.object(
            benchmark_inventory,
            "BENCHMARK_ROOT",
            self.root / "Benchmarks",
        )
        self.patch_benchmark_root.start()
        self.addCleanup(self.patch_benchmark_root.stop)

    @staticmethod
    def _write_json(path: pathlib.Path, payload: dict) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    @staticmethod
    def _write_jsonl(path: pathlib.Path, rows: list[dict]) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            "".join(json.dumps(row, sort_keys=True) + "\n" for row in rows),
            encoding="utf-8",
        )

    def _fixture(self, task_count: int = 1):
        tasks_path = self.root / "Benchmarks" / "ActionGuidance" / "tasks.jsonl"
        manifest_path = tasks_path.parent / "manifest.json"
        runner_path = self.root / "Scripts" / "action_guidance_benchmark.py"
        common_path = self.root / "Scripts" / "benchmark_common.py"
        evidence_dir = (
            self.root
            / "Saved"
            / "Monolith"
            / "Benchmarks"
            / "ActionGuidance"
            / "unit-valid"
        )
        summary_path = evidence_dir / "summary.json"
        result_path = evidence_dir / "per_task.jsonl"

        tasks = [
            {
                "id": f"AG-{index:03d}",
                "namespace": "blueprint",
                "action": f"action_{index}",
            }
            for index in range(1, task_count + 1)
        ]
        results = [
            {
                "task_id": task["id"],
                "namespace": task["namespace"],
                "action": task["action"],
                "task_success": True,
                "transport_error": False,
            }
            for task in tasks
        ]
        self._write_jsonl(tasks_path, tasks)
        self._write_json(manifest_path, {
            "task_count": task_count,
            "catalog_version": "sha256:unit",
            "catalog_action_count": 1,
            "catalog_namespace_count": 1,
            "run_gates": {
                "default_max_recovery_calls": 3,
                "max_transport_failed_fraction": 0.05,
                "max_consecutive_transport_failures": 3,
                "min_transport_fraction_sample": 20,
            },
        })
        runner_path.parent.mkdir(parents=True, exist_ok=True)
        runner_path.write_text("# unit runner\n", encoding="utf-8")
        common_path.write_text("# unit benchmark common\n", encoding="utf-8")
        self._write_jsonl(result_path, results)

        inputs = benchmark_common.build_benchmark_inputs(
            "ActionGuidance",
            tasks_path=tasks_path,
            mcp_status={
                "catalog_version": "sha256:unit",
                "project_name": "Speed",
            },
            extra_files={"runner": runner_path},
            plugin_root=self.root,
        )
        summary = {
            "run_valid": True,
            "completion_status": "completed",
            "metrics_valid": True,
            "metrics_scope": "complete_run",
            "metrics": {"task_success_rate": 1.0},
            "comparison_valid": True,
            "task_count": task_count,
            "task_corpus": {
                "mode": "canonical",
                "canonical": True,
                "comparable": True,
                "validated_task_count": task_count,
            },
            "transport_failure_count": 0,
            "max_recovery_calls": 3,
            "max_transport_failed_fraction": 0.05,
            "max_consecutive_transport_failures": 3,
            "min_transport_fraction_sample": 20,
            "status_identity_start": {
                "endpoint": "http://unit.test/mcp",
                "server_version": "unit",
                "catalog_version": "sha256:unit",
                "project": "Speed",
                "engine_version": "unit",
            },
            "status_identity_end": {
                "endpoint": "http://unit.test/mcp",
                "server_version": "unit",
                "catalog_version": "sha256:unit",
                "project": "Speed",
                "engine_version": "unit",
            },
            "benchmark_inputs": inputs,
            "input_fingerprint": inputs["fingerprint_sha256"],
        }
        status = {
            "evidence": "Saved/Monolith/Benchmarks/ActionGuidance/unit-valid/summary.json",
            "input_fingerprint": inputs["fingerprint_sha256"],
        }
        namespace_results = {
            "blueprint": {
                "pass": task_count,
                "fail": 0,
                "expected_skip": 0,
            }
        }
        self._write_json(summary_path, summary)
        return summary_path, result_path, summary, status, namespace_results, results

    def _publish(self, summary_path: pathlib.Path, summary: dict, status: dict) -> None:
        self._write_json(summary_path, summary)
        status["input_fingerprint"] = summary["input_fingerprint"]

    def _repin_inputs(self, summary: dict) -> None:
        benchmark_common.refresh_benchmark_input_fingerprint(
            summary["benchmark_inputs"]
        )
        summary["input_fingerprint"] = summary["benchmark_inputs"][
            "fingerprint_sha256"
        ]

    def _validate(self, status: dict, namespace_results: dict) -> None:
        benchmark_inventory.validate_accepted_evidence(
            "ActionGuidance",
            status,
            namespace_results,
        )

    def test_valid_full_run_is_accepted(self) -> None:
        _, _, _, status, namespace_results, _ = self._fixture()
        self._validate(status, namespace_results)

    def test_interrupted_or_invalid_run_is_rejected(self) -> None:
        for field, invalid_value in (
            ("run_valid", False),
            ("completion_status", "interrupted"),
            ("metrics_valid", False),
        ):
            with self.subTest(field=field):
                summary_path, _, summary, status, namespace_results, _ = self._fixture()
                summary[field] = invalid_value
                self._publish(summary_path, summary, status)
                with self.assertRaises(benchmark_inventory.InventoryError):
                    self._validate(status, namespace_results)

    def test_non_comparable_subset_is_rejected(self) -> None:
        summary_path, _, summary, status, namespace_results, _ = self._fixture()
        summary["comparison_valid"] = False
        self._publish(summary_path, summary, status)
        with self.assertRaisesRegex(
            benchmark_inventory.InventoryError,
            "canonical full-corpus",
        ):
            self._validate(status, namespace_results)

    def test_empty_input_contract_is_rejected_even_with_recomputed_fingerprint(self) -> None:
        summary_path, _, summary, status, namespace_results, _ = self._fixture()
        summary["benchmark_inputs"]["files"] = {}
        summary["benchmark_inputs"]["database_files"] = []
        self._repin_inputs(summary)
        self._publish(summary_path, summary, status)
        with self.assertRaisesRegex(
            benchmark_inventory.InventoryError,
            "input files differ",
        ):
            self._validate(status, namespace_results)

    def test_database_scope_marker_cannot_be_forged(self) -> None:
        summary_path, _, summary, status, namespace_results, _ = self._fixture()
        summary["benchmark_inputs"]["database_files_scope"] = "all_local_databases"
        self._repin_inputs(summary)
        self._publish(summary_path, summary, status)
        with self.assertRaisesRegex(
            benchmark_inventory.InventoryError,
            "exact DB-scope marker",
        ):
            self._validate(status, namespace_results)

    def test_forged_self_reported_fingerprint_is_rejected(self) -> None:
        summary_path, _, summary, status, namespace_results, _ = self._fixture()
        forged = "f" * 64
        summary["benchmark_inputs"]["fingerprint_sha256"] = forged
        summary["input_fingerprint"] = forged
        status["input_fingerprint"] = forged
        self._write_json(summary_path, summary)
        with self.assertRaisesRegex(
            benchmark_inventory.InventoryError,
            "recomputed=",
        ):
            self._validate(status, namespace_results)

    def test_missing_or_mismatched_result_ids_are_rejected(self) -> None:
        for rows in (
            [],
            [{"task_id": "AG-999", "namespace": "blueprint", "task_success": True}],
        ):
            with self.subTest(rows=rows):
                _, result_path, _, status, namespace_results, _ = self._fixture()
                self._write_jsonl(result_path, rows)
                with self.assertRaises(benchmark_inventory.InventoryError):
                    self._validate(status, namespace_results)

    def test_duplicate_result_ids_are_rejected(self) -> None:
        _, result_path, _, status, namespace_results, results = self._fixture(task_count=2)
        self._write_jsonl(result_path, [results[0], results[0]])
        with self.assertRaisesRegex(
            benchmark_inventory.InventoryError,
            "duplicate task id",
        ):
            self._validate(status, namespace_results)

    def test_result_order_must_match_the_canonical_corpus(self) -> None:
        _, result_path, _, status, namespace_results, results = self._fixture(task_count=2)
        self._write_jsonl(result_path, list(reversed(results)))
        with self.assertRaisesRegex(
            benchmark_inventory.InventoryError,
            "result order does not match",
        ):
            self._validate(status, namespace_results)

    def test_non_boolean_result_and_declared_namespace_drift_are_rejected(self) -> None:
        _, result_path, _, status, namespace_results, results = self._fixture()
        invalid_result = dict(results[0])
        invalid_result["task_success"] = 1
        self._write_jsonl(result_path, [invalid_result])
        with self.assertRaisesRegex(benchmark_inventory.InventoryError, "must be boolean"):
            self._validate(status, namespace_results)

        _, _, _, status, _, _ = self._fixture()
        with self.assertRaisesRegex(
            benchmark_inventory.InventoryError,
            "namespace results do not match derived evidence",
        ):
            self._validate(
                status,
                {"rogue": {"pass": 1, "fail": 0, "expected_skip": 0}},
            )

    def test_result_action_must_match_the_canonical_task(self) -> None:
        _, result_path, _, status, namespace_results, results = self._fixture()
        forged_result = dict(results[0])
        forged_result["action"] = "different_action"
        self._write_jsonl(result_path, [forged_result])
        with self.assertRaisesRegex(
            benchmark_inventory.InventoryError,
            "result action mismatch",
        ):
            self._validate(status, namespace_results)

    def test_transport_recovery_requires_explicit_provenance(self) -> None:
        summary_path, result_path, summary, status, namespace_results, results = self._fixture()
        forged_result = dict(results[0])
        forged_result.update({
            "task_success": True,
            "transport_error": True,
            "direct_success": True,
            "transport_failure_call_count": 1,
        })
        summary["transport_failure_count"] = 1
        self._write_jsonl(result_path, [forged_result])
        self._publish(summary_path, summary, status)
        with self.assertRaisesRegex(
            benchmark_inventory.InventoryError,
            "lacks explicit recovery provenance",
        ):
            self._validate(status, namespace_results)

    def test_status_identity_requires_complete_current_catalog_binding(self) -> None:
        summary_path, _, summary, status, namespace_results, _ = self._fixture()
        del summary["status_identity_start"]["engine_version"]
        del summary["status_identity_end"]["engine_version"]
        self._publish(summary_path, summary, status)
        with self.assertRaisesRegex(
            benchmark_inventory.InventoryError,
            "complete identical status identities",
        ):
            self._validate(status, namespace_results)

        summary_path, _, summary, status, namespace_results, _ = self._fixture()
        summary["benchmark_inputs"]["mcp_catalog"]["status"]["catalog_version"] = "sha256:old"
        summary["status_identity_start"]["catalog_version"] = "sha256:old"
        summary["status_identity_end"]["catalog_version"] = "sha256:old"
        self._repin_inputs(summary)
        self._publish(summary_path, summary, status)
        with self.assertRaisesRegex(
            benchmark_inventory.InventoryError,
            "catalog identity is not current",
        ):
            self._validate(status, namespace_results)

    def test_result_affecting_execution_settings_match_the_manifest(self) -> None:
        summary_path, _, summary, status, namespace_results, _ = self._fixture()
        summary["max_recovery_calls"] = 100
        self._publish(summary_path, summary, status)
        with self.assertRaisesRegex(
            benchmark_inventory.InventoryError,
            "max_recovery_calls differs",
        ):
            self._validate(status, namespace_results)

    def test_current_live_identity_cannot_rescue_stale_source_or_ai_manifest(self) -> None:
        current_catalog_version = "sha256:current"
        run_gates = {
            "max_transport_failed_fraction": 0.05,
            "max_consecutive_transport_failures": 3,
            "min_transport_fraction_sample": 20,
        }
        summary = {
            **run_gates,
            "status_identity_start": {"catalog_version": current_catalog_version},
            "status_identity_end": {"catalog_version": current_catalog_version},
            "benchmark_inputs": {
                "mcp_catalog": {
                    "status": {"catalog_version": current_catalog_version},
                },
            },
        }
        with mock.patch.object(
            benchmark_inventory,
            "current_catalog_identity",
            return_value={"catalog_version": current_catalog_version},
        ):
            for suite_key in ("SourceIndex", "AICapability"):
                with self.subTest(suite=suite_key):
                    manifest_path = self.root / "Benchmarks" / suite_key / "manifest.json"
                    self._write_json(manifest_path, {
                        "catalog_version": "sha256:stale",
                        "run_gates": run_gates,
                    })
                    with self.assertRaisesRegex(
                        benchmark_inventory.InventoryError,
                        f"accepted {suite_key} canonical manifest catalog_version is not current",
                    ):
                        benchmark_inventory.validate_task_execution_contract(
                            suite_key,
                            summary,
                            manifest_path,
                        )

                    self._write_json(manifest_path, {
                        "catalog_version": current_catalog_version,
                        "run_gates": run_gates,
                    })
                    benchmark_inventory.validate_task_execution_contract(
                        suite_key,
                        summary,
                        manifest_path,
                    )

    def test_invalid_artifacts_cannot_coexist_with_accepted_summary(self) -> None:
        summary_path, _, _, status, namespace_results, _ = self._fixture()
        self._write_json(summary_path.parent / "run_failure.json", {"run_valid": False})
        with self.assertRaisesRegex(
            benchmark_inventory.InventoryError,
            "coexists with invalid-run artifacts",
        ):
            self._validate(status, namespace_results)


class PortableAcceptedBundleValidationTests(unittest.TestCase):
    """Exercise the clean-checkout DB-attestation boundary adversarially."""

    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp_dir.cleanup)
        self.root = pathlib.Path(self.temp_dir.name)
        self.patch_root = mock.patch.object(
            benchmark_inventory,
            "MONOLITH_ROOT",
            self.root,
        )
        self.patch_root.start()
        self.addCleanup(self.patch_root.stop)

    @staticmethod
    def _write_json(path: pathlib.Path, payload: dict) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    def _fixture(self) -> tuple[dict, dict, dict, dict[str, pathlib.Path]]:
        paths = {
            "tasks": self.root / "Benchmarks" / "OfflineParity" / "actions.jsonl",
            "manifest": self.root / "Benchmarks" / "OfflineParity" / "manifest.json",
            "runner": self.root / "Scripts" / "offline_parity_benchmark.py",
            "benchmark_common": self.root / "Scripts" / "benchmark_common.py",
            "offline_python": self.root / "Scripts" / "monolith_offline.py",
            "offline_exe": self.root / "Binaries" / "monolith_query.exe",
            "database": self.root / "Saved" / "EngineSource.db",
        }
        for path in paths.values():
            path.parent.mkdir(parents=True, exist_ok=True)
        paths["tasks"].write_text(
            json.dumps({
                "action": "list_uclasses",
                "args": [],
                "label": "cppreflect.list_uclasses",
                "namespace": "cppreflect",
            }) + "\n",
            encoding="utf-8",
        )
        self._write_json(paths["manifest"], {"action_count": 1})
        paths["runner"].write_text("# unit runner\n", encoding="utf-8")
        paths["benchmark_common"].write_text("# unit common\n", encoding="utf-8")
        paths["offline_python"].write_text("# unit offline reader\n", encoding="utf-8")
        paths["offline_exe"].write_bytes(b"MZ-unit-query")
        paths["database"].write_bytes(b"database-v1")

        inputs = benchmark_common.build_benchmark_inputs(
            "OfflineParity",
            tasks_path=paths["tasks"],
            extra_files={
                "runner": paths["runner"],
                "offline_exe": paths["offline_exe"],
                "offline_python": paths["offline_python"],
            },
            database_paths=("Saved/EngineSource.db",),
            plugin_root=self.root,
        )
        summary = {
            "benchmark_inputs": inputs,
            "input_fingerprint": inputs["fingerprint_sha256"],
        }
        status = {"input_fingerprint": inputs["fingerprint_sha256"]}
        bundle_manifest = {
            "input_fingerprint": inputs["fingerprint_sha256"],
            "database_inputs": json.loads(json.dumps(inputs["database_files"])),
        }
        return summary, status, bundle_manifest, paths

    def test_portable_accepts_clean_checkout_mtime_drift_and_absent_database(self) -> None:
        summary, status, bundle_manifest, paths = self._fixture()
        recorded_runner_mtime = summary["benchmark_inputs"]["files"]["runner"]["mtime_ns"]
        os.utime(
            paths["runner"],
            ns=(recorded_runner_mtime + 10_000_000, recorded_runner_mtime + 10_000_000),
        )
        paths["database"].unlink()
        report: dict[str, list[str]] = {
            "attested_databases": [],
            "omitted_pending_evidence": [],
        }

        benchmark_inventory.validate_input_evidence(
            "OfflineParity",
            status,
            summary,
            portable=True,
            bundle_manifest=bundle_manifest,
            validation_report=report,
        )

        self.assertEqual(report["attested_databases"], ["Saved/EngineSource.db"])
        with self.assertRaisesRegex(
            benchmark_inventory.InventoryError,
            "input mtime drifted|database input drifted",
        ):
            benchmark_inventory.validate_input_evidence(
                "OfflineParity",
                status,
                summary,
                portable=False,
                bundle_manifest=bundle_manifest,
            )

    def test_portable_rejects_present_database_content_drift(self) -> None:
        summary, status, bundle_manifest, paths = self._fixture()
        recorded_database_mtime = summary["benchmark_inputs"]["database_files"][0]["mtime_ns"]
        paths["database"].write_bytes(b"database-v2")
        os.utime(
            paths["database"],
            ns=(recorded_database_mtime, recorded_database_mtime),
        )

        with self.assertRaisesRegex(
            benchmark_inventory.InventoryError,
            "database content drifted",
        ):
            benchmark_inventory.validate_input_evidence(
                "OfflineParity",
                status,
                summary,
                portable=True,
                bundle_manifest=bundle_manifest,
            )

    def test_portable_rejects_bundle_database_attestation_drift(self) -> None:
        summary, status, bundle_manifest, _ = self._fixture()
        bundle_manifest["database_inputs"][0]["sha256"] = "f" * 64

        with self.assertRaisesRegex(
            benchmark_inventory.InventoryError,
            "bundle database attestation drifted",
        ):
            benchmark_inventory.validate_input_evidence(
                "OfflineParity",
                status,
                summary,
                portable=True,
                bundle_manifest=bundle_manifest,
            )

    def test_portable_rejects_extra_database_attestation_fields_even_when_rehashed(self) -> None:
        summary, status, bundle_manifest, _ = self._fixture()
        summary["benchmark_inputs"]["database_files"][0]["untrusted_note"] = "ignored?"
        benchmark_common.refresh_benchmark_input_fingerprint(summary["benchmark_inputs"])
        summary["input_fingerprint"] = summary["benchmark_inputs"]["fingerprint_sha256"]
        status["input_fingerprint"] = summary["input_fingerprint"]
        bundle_manifest["input_fingerprint"] = summary["input_fingerprint"]
        bundle_manifest["database_inputs"] = json.loads(json.dumps(
            summary["benchmark_inputs"]["database_files"]
        ))

        with self.assertRaisesRegex(
            benchmark_inventory.InventoryError,
            "database input 0 structure is invalid",
        ):
            benchmark_inventory.validate_input_evidence(
                "OfflineParity",
                status,
                summary,
                portable=True,
                bundle_manifest=bundle_manifest,
            )

    def test_bundle_manifest_pins_summary_and_raw_result_content(self) -> None:
        summary, status, bundle_inputs, _ = self._fixture()
        snapshot_id = "unit-snapshot"
        bundle_dir = (
            self.root
            / "Benchmarks"
            / "OfflineParity"
            / "accepted"
            / snapshot_id
        )
        summary_path = bundle_dir / "summary.json"
        result_path = bundle_dir / "per_action.jsonl"
        manifest_path = bundle_dir / benchmark_inventory.ACCEPTED_BUNDLE_FILENAME
        self._write_json(summary_path, summary)
        result_path.write_text(
            json.dumps({"label": "cppreflect.list_uclasses", "status": "MATCH"}) + "\n",
            encoding="utf-8",
        )
        manifest = {
            "schema_version": benchmark_inventory.ACCEPTED_BUNDLE_SCHEMA_VERSION,
            "benchmark": "OfflineParity",
            "suite": "OfflineParity",
            "snapshot_id": snapshot_id,
            "summary_file": "summary.json",
            "summary_sha256": benchmark_inventory.file_sha256(summary_path),
            "artifact_files": {
                "per_action.jsonl": {
                    "sha256": benchmark_inventory.file_sha256(result_path),
                    "non_empty_line_count": 1,
                },
            },
            "input_fingerprint": bundle_inputs["input_fingerprint"],
            "database_inputs": bundle_inputs["database_inputs"],
            "portable_database_policy": benchmark_inventory.PORTABLE_DATABASE_POLICY,
        }
        self._write_json(manifest_path, manifest)
        status.update({
            "evidence": (
                f"Benchmarks/OfflineParity/accepted/{snapshot_id}/summary.json"
            ),
            "evidence_bundle_sha256": benchmark_inventory.file_sha256(manifest_path),
        })

        benchmark_inventory.load_accepted_bundle(
            "OfflineParity",
            status,
            summary_path,
            portable=True,
        )
        result_path.write_text("{}\n", encoding="utf-8")
        with self.assertRaisesRegex(
            benchmark_inventory.InventoryError,
            "artifact content drifted: per_action.jsonl",
        ):
            benchmark_inventory.load_accepted_bundle(
                "OfflineParity",
                status,
                summary_path,
                portable=True,
            )

    def test_artifact_contract_covers_every_validator_consumed_sidecar(self) -> None:
        self.assertEqual(
            benchmark_inventory.ACCEPTED_ARTIFACT_FILES,
            {
                "OfflineParity": ("per_action.jsonl",),
                "ActionGuidance": ("per_task.jsonl",),
                "SourceIndex": ("per_task.jsonl",),
                "SchemaCompletenessProbe": (
                    "probe_results.jsonl",
                    "per_action.jsonl",
                    "probe_preflight.json",
                ),
                benchmark_inventory.FULL_CATALOG_KEY: (
                    "per_action.jsonl",
                    "namespace_breakdown.json",
                    "scan_checkpoint.json",
                ),
                "ProjectIndex": ("per_task.jsonl",),
                "AICapability": ("per_task.jsonl",),
                "AssetEditing": ("per_task.jsonl",),
            },
        )

    def test_bundle_manifest_pins_schema_full_json_sidecars(self) -> None:
        snapshot_id = "unit-full"
        bundle_dir = (
            self.root
            / "Benchmarks"
            / "SchemaCompleteness"
            / "accepted"
            / snapshot_id
        )
        summary_path = bundle_dir / "summary.json"
        action_path = bundle_dir / "per_action.jsonl"
        namespace_path = bundle_dir / "namespace_breakdown.json"
        checkpoint_path = bundle_dir / "scan_checkpoint.json"
        manifest_path = bundle_dir / benchmark_inventory.ACCEPTED_BUNDLE_FILENAME
        self._write_json(summary_path, {})
        action_path.write_text(
            json.dumps({"namespace": "unit", "action": "action"}) + "\n",
            encoding="utf-8",
        )
        self._write_json(namespace_path, {})
        self._write_json(checkpoint_path, {})
        manifest = {
            "schema_version": benchmark_inventory.ACCEPTED_BUNDLE_SCHEMA_VERSION,
            "benchmark": "SchemaCompleteness",
            "suite": benchmark_inventory.FULL_CATALOG_KEY,
            "snapshot_id": snapshot_id,
            "summary_file": "summary.json",
            "summary_sha256": benchmark_inventory.file_sha256(summary_path),
            "artifact_files": {
                "per_action.jsonl": {
                    "sha256": benchmark_inventory.file_sha256(action_path),
                    "non_empty_line_count": 1,
                },
                "namespace_breakdown.json": {
                    "sha256": benchmark_inventory.file_sha256(namespace_path),
                },
                "scan_checkpoint.json": {
                    "sha256": benchmark_inventory.file_sha256(checkpoint_path),
                },
            },
            "input_fingerprint": "0" * 64,
            "database_inputs": [],
            "portable_database_policy": benchmark_inventory.PORTABLE_DATABASE_POLICY,
        }
        self._write_json(manifest_path, manifest)
        status = {
            "evidence": (
                f"Benchmarks/SchemaCompleteness/accepted/{snapshot_id}/summary.json"
            ),
            "evidence_bundle_sha256": benchmark_inventory.file_sha256(manifest_path),
        }

        benchmark_inventory.load_accepted_bundle(
            benchmark_inventory.FULL_CATALOG_KEY,
            status,
            summary_path,
            portable=True,
        )
        self._write_json(namespace_path, {"tampered": True})
        with self.assertRaisesRegex(
            benchmark_inventory.InventoryError,
            "artifact content drifted: namespace_breakdown.json",
        ):
            benchmark_inventory.load_accepted_bundle(
                benchmark_inventory.FULL_CATALOG_KEY,
                status,
                summary_path,
                portable=True,
            )

        self._write_json(namespace_path, {})
        del manifest["artifact_files"]["scan_checkpoint.json"]
        self._write_json(manifest_path, manifest)
        status["evidence_bundle_sha256"] = benchmark_inventory.file_sha256(manifest_path)
        with self.assertRaisesRegex(
            benchmark_inventory.InventoryError,
            "bundle artifact set is invalid",
        ):
            benchmark_inventory.load_accepted_bundle(
                benchmark_inventory.FULL_CATALOG_KEY,
                status,
                summary_path,
                portable=True,
            )


class OfflineParityEvidenceDerivationTests(unittest.TestCase):
    @staticmethod
    def _summary(*, decision_id: str | None = None) -> dict:
        return {
            "run_environment": {"valid": True},
            "version": {
                "exe_parity_spec_rev": "unit-revision",
                "py_parity_spec_rev": "unit-revision",
                "version_parity_ok": True,
            },
            "chain_inputs": {
                "decision_id": decision_id,
            },
        }

    def test_version_parity_requires_strict_true_and_nonempty_exact_revisions(self) -> None:
        invalid_versions = (
            {
                "exe_parity_spec_rev": "unit-revision",
                "py_parity_spec_rev": "unit-revision",
                "version_parity_ok": 1,
            },
            {
                "exe_parity_spec_rev": "",
                "py_parity_spec_rev": "",
                "version_parity_ok": True,
            },
            {
                "exe_parity_spec_rev": "unit-revision-a",
                "py_parity_spec_rev": "unit-revision-b",
                "version_parity_ok": True,
            },
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            for version in invalid_versions:
                with self.subTest(version=version):
                    summary = self._summary()
                    summary["version"] = version
                    with self.assertRaisesRegex(
                        benchmark_inventory.InventoryError,
                        "versions do not match|revisions must be non-empty and equal",
                    ):
                        benchmark_inventory.derive_offline_parity_results(
                            root / "summary.json",
                            summary,
                            root / "actions.jsonl",
                        )

    def test_decision_task_cannot_forge_skip_when_decision_id_is_available(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            result_dir = root / "results"
            result_dir.mkdir(parents=True)
            task_path = root / "actions.jsonl"
            task_path.write_text(
                json.dumps({
                    "label": "decision.get_decision",
                    "namespace": "decision",
                    "requires": "decision_id",
                }) + "\n",
                encoding="utf-8",
            )
            (result_dir / "per_action.jsonl").write_text(
                json.dumps({
                    "label": "decision.get_decision",
                    "status": "SKIP",
                    "diff_count": 0,
                    "expected_error": False,
                    "offline_unsupported": False,
                    "error_kind": "skip",
                    "error": "current DB corpus has no decision_id input",
                    "exe_exit_code": None,
                    "py_exit_code": None,
                }) + "\n",
                encoding="utf-8",
            )

            with self.assertRaisesRegex(
                benchmark_inventory.InventoryError,
                "skipped despite an available decision_id",
            ):
                benchmark_inventory.derive_offline_parity_results(
                    result_dir / "summary.json",
                    self._summary(decision_id="decision-unit-001"),
                    task_path,
                )


class SchemaProbeEvidenceDerivationTests(unittest.TestCase):
    def test_forged_probe_pass_cannot_override_failed_raw_dimension(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            probe_dir = root / "Benchmarks" / "SchemaCompleteness"
            result_dir = root / "Saved" / "SchemaProbe"
            probe_dir.mkdir(parents=True)
            result_dir.mkdir(parents=True)
            probe_path = probe_dir / "probe_set.jsonl"
            summary_path = result_dir / "summary.json"
            probe = {
                "namespace": "blueprint",
                "action": "create_blueprint",
                "expected_dimensions": ["param_types_declared"],
                "availability": {"mode": "required"},
            }
            result = {
                "probe_index": 1,
                "namespace": "blueprint",
                "action": "create_blueprint",
                "availability": {"mode": "required"},
                "expected_dimensions": ["param_types_declared"],
                "result_status": "scored",
                "catalog_presence": "present",
                "dimension_results": {
                    "param_types_declared": False,
                    "required_params_marked": True,
                    "value_domain": True,
                    "planning_signals_present": True,
                    "skill_routing_present": True,
                    "output_contract_declared": True,
                },
                "expected_passed": [],
                "expected_failed": ["param_types_declared"],
                "expected_na": [],
                "probe_pass": True,
            }
            action_row = {
                "namespace": "blueprint",
                "action": "create_blueprint",
                "param_types_declared": False,
                "required_params_marked": True,
                "value_domain": True,
                "planning_signals_present": True,
                "skill_routing_present": True,
                "output_contract_declared": True,
            }
            probe_path.write_text(json.dumps(probe) + "\n", encoding="utf-8")
            (probe_dir / "manifest.json").write_text(
                json.dumps({
                    "scoring": {
                        "dimensions": [
                            {"name": name}
                            for name in benchmark_inventory.CHECKPOINT_DIMENSION_FIELDS
                        ],
                    },
                }) + "\n",
                encoding="utf-8",
            )
            (result_dir / "probe_results.jsonl").write_text(
                json.dumps(result) + "\n", encoding="utf-8",
            )
            (result_dir / "per_action.jsonl").write_text(
                json.dumps(action_row) + "\n", encoding="utf-8",
            )
            summary = {
                "run_valid": True,
                "completion_status": "completed",
                "metrics_valid": True,
                "metrics_scope": "complete_run",
                "metrics": {},
            }
            summary_path.write_text(json.dumps(summary) + "\n", encoding="utf-8")

            with self.assertRaisesRegex(
                benchmark_inventory.InventoryError,
                "probe_pass is not derived",
            ):
                benchmark_inventory.derive_schema_probe_results(
                    summary_path,
                    summary,
                    probe_path,
                )

    def test_scored_probe_rejects_missing_raw_schema_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            probe_dir = root / "Benchmarks" / "SchemaCompleteness"
            result_dir = root / "Saved" / "SchemaProbe"
            probe_dir.mkdir(parents=True)
            result_dir.mkdir(parents=True)
            probe_path = probe_dir / "probe_set.jsonl"
            summary_path = result_dir / "summary.json"
            raw_schema = {"params": {"Broken": {}}}
            quality = schema_benchmark.score_schema_quality(raw_schema)
            probe = {
                "namespace": "blueprint",
                "action": "create_blueprint",
                "expected_dimensions": ["param_types_declared"],
                "availability": {"mode": "required"},
            }
            result = {
                "probe_index": 1,
                "namespace": "blueprint",
                "action": "create_blueprint",
                "availability": {"mode": "required"},
                "expected_dimensions": ["param_types_declared"],
                "result_status": "scored",
                "catalog_presence": "present",
                "dimension_results": {
                    field: quality[field]
                    for field in schema_benchmark.CHECKPOINT_DIMENSION_FIELDS
                },
                "value_domain_diagnostics": quality["value_domain_diagnostics"],
                "expected_passed": [],
                "expected_failed": ["param_types_declared"],
                "expected_na": [],
                "probe_pass": False,
            }
            action_row = {
                "namespace": "blueprint",
                "action": "create_blueprint",
                "failure_kind": "ok",
                "error": "",
                "transport_error": False,
                "transport_status": None,
                "transport_error_raw": "",
                "raw_schema_hash_kind": schema_benchmark.RAW_SCHEMA_HASH_KIND,
                "raw_schema_sha256": schema_benchmark._sha256_json(raw_schema),
                **quality,
            }
            probe_path.write_text(json.dumps(probe) + "\n", encoding="utf-8")
            (probe_dir / "manifest.json").write_text(
                json.dumps({
                    "scoring": {
                        "dimensions": [
                            {"name": name}
                            for name in benchmark_inventory.CHECKPOINT_DIMENSION_FIELDS
                        ],
                    },
                }) + "\n",
                encoding="utf-8",
            )
            (result_dir / "probe_results.jsonl").write_text(
                json.dumps(result) + "\n",
                encoding="utf-8",
            )
            (result_dir / "per_action.jsonl").write_text(
                json.dumps(action_row) + "\n",
                encoding="utf-8",
            )
            summary = {
                "run_valid": True,
                "completion_status": "completed",
                "metrics_valid": True,
                "metrics_scope": "complete_run",
                "metrics": {},
            }
            with self.assertRaisesRegex(
                benchmark_inventory.InventoryError,
                "missing canonical raw_schema",
            ):
                benchmark_inventory.derive_schema_probe_results(
                    summary_path,
                    summary,
                    probe_path,
                )


class TaskResultTerminalHealthTests(unittest.TestCase):
    def test_source_index_cannot_hide_protocol_failure_by_deleting_the_boolean(self) -> None:
        catalog_version = benchmark_inventory.current_catalog_identity()["catalog_version"]
        identity = {
            "endpoint": "http://unit.test/mcp",
            "server_version": "unit",
            "catalog_version": catalog_version,
            "project": "Speed",
            "engine_version": "unit",
        }
        summary = {
            "run_valid": True,
            "completion_status": "completed",
            "metrics_valid": True,
            "metrics_scope": "complete_run",
            "metrics": {},
            "comparison_valid": True,
            "status_identity_start": identity,
            "status_identity_end": dict(identity),
            "benchmark_inputs": {
                "mcp_catalog": {
                    "status": {"catalog_version": catalog_version},
                },
            },
            "task_count": 1,
            "task_corpus": {
                "mode": "canonical",
                "canonical": True,
                "comparable": True,
                "validated_task_count": 1,
            },
            "transport_failure_count": 0,
        }
        task = {
            "id": "SIB-UNIT-001",
            "category": "symbol_lookup",
            "namespace": "source",
            "action": "search_source",
        }
        forged_result = {
            "task_id": task["id"],
            "category": task["category"],
            "namespace": task["namespace"],
            "action": task["action"],
            "direct_success": True,
            "transport_error": False,
            "failure_kind": "protocol_error",
            "response_is_error": True,
        }
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            task_path = root / "tasks.jsonl"
            summary_path = root / "summary.json"
            task_path.write_text(json.dumps(task) + "\n", encoding="utf-8")
            (root / "per_task.jsonl").write_text(
                json.dumps(forged_result) + "\n", encoding="utf-8",
            )
            with self.assertRaisesRegex(
                benchmark_inventory.InventoryError,
                "protocol_error must be a required boolean",
            ):
                benchmark_inventory.derive_task_suite_results(
                    "SourceIndex", summary_path, summary, task_path,
                )


class SchemaFullEvidenceDerivationTests(unittest.TestCase):
    @staticmethod
    def _row() -> dict:
        raw_schema = schema_benchmark.canonical_raw_schema({
            "params": {"Broken": {}},
        })
        return {
            "namespace": "blueprint",
            "action": "create_blueprint",
            "failure_kind": "ok",
            "error": "",
            "transport_error": False,
            "transport_status": None,
            "transport_error_raw": "",
            "raw_schema_hash_kind": schema_benchmark.RAW_SCHEMA_HASH_KIND,
            "raw_schema": raw_schema,
            "raw_schema_sha256": schema_benchmark._sha256_json(raw_schema),
            **schema_benchmark.score_schema_quality(raw_schema),
        }

    @staticmethod
    def _summary(row: dict) -> dict:
        rows = [row]
        namespace_breakdown = benchmark_inventory.build_schema_namespace_breakdown(rows)
        aggregate = benchmark_inventory.aggregate_schema_metrics(
            "unit", rows, 1, namespace_breakdown,
        )
        action_ids = ["blueprint.create_blueprint"]
        action_hash = benchmark_inventory.hashlib.sha256(
            json.dumps(
                action_ids,
                sort_keys=True,
                separators=(",", ":"),
                ensure_ascii=True,
            ).encode("utf-8")
        ).hexdigest()
        return {
            "label": "unit",
            "run_valid": True,
            "completion_status": "completed",
            "metrics_valid": True,
            "metrics_scope": "complete_run",
            "comparable": True,
            "scanned_action_count": 1,
            "total_expected_action_count": 1,
            "failed_action_count": 0,
            "namespace_count": 1,
            "metrics": aggregate["metrics"],
            "namespace_breakdown": namespace_breakdown,
            "completed_action_count": 1,
            "completed_valid_action_count": 1,
            "remaining_action_count": 0,
            "total_action_count": 1,
            "full_catalog_action_count": 1,
            "benchmark_inputs": {
                "fingerprint_sha256": "unit-input",
                "schema_registry": {
                    "catalog_action_count": 1,
                    "catalog_action_ids_hash_kind": benchmark_inventory.CATALOG_ACTION_IDS_HASH_KIND,
                    "catalog_action_ids_sha256": action_hash,
                },
                "mcp_catalog": {
                    "status": {"catalog_version": "sha256:unit"},
                },
            },
            "input_fingerprint": "unit-input",
            "checkpoint_provenance": {
                "checkpoint_file": "scan_checkpoint.json",
                "result_file": "per_action.jsonl",
                "segment_count": 1,
                "resumed": False,
                "outage_count": 0,
                "invalid_attempt_count": 0,
                "recovery_event_count": 0,
                "catalog_version": "sha256:unit",
                "catalog_action_ids_hash_kind": benchmark_inventory.CATALOG_ACTION_IDS_HASH_KIND,
                "catalog_action_ids_sha256": action_hash,
            },
        }

    def test_full_catalog_classifies_incomplete_schema_as_quality_failure(self) -> None:
        row = self._row()
        summary = self._summary(row)
        with tempfile.TemporaryDirectory() as temp_dir:
            result_dir = pathlib.Path(temp_dir)
            summary_path = result_dir / "summary.json"
            (result_dir / "per_action.jsonl").write_text(
                json.dumps(row) + "\n", encoding="utf-8",
            )
            (result_dir / "namespace_breakdown.json").write_text(
                json.dumps(summary["namespace_breakdown"]) + "\n",
                encoding="utf-8",
            )
            (result_dir / "scan_checkpoint.json").write_text(
                json.dumps({
                    "schema_version": schema_benchmark.SCAN_CHECKPOINT_SCHEMA_VERSION,
                    "benchmark": "SchemaCompleteness",
                    "scan_scope": "full_catalog",
                    "state": "completed",
                    "results_file": "per_action.jsonl",
                    "completed_valid_action_count": 1,
                    "benchmark_inputs": summary["benchmark_inputs"],
                    "resume_identity": {
                        "catalog_version": "sha256:unit",
                        "catalog_action_count": 1,
                        "catalog_action_ids_hash_kind": benchmark_inventory.CATALOG_ACTION_IDS_HASH_KIND,
                        "catalog_action_ids_sha256": summary["checkpoint_provenance"]["catalog_action_ids_sha256"],
                        "benchmark_input_fingerprint": "unit-input",
                    },
                    "segments": [{"completion_status": "completed"}],
                    "outages": [],
                    "invalid_attempts": [],
                    "recovery_events": [],
                }) + "\n",
                encoding="utf-8",
            )
            derived = benchmark_inventory.derive_schema_full_results(summary_path, summary)
        self.assertEqual(
            derived,
            {"blueprint": {"pass": 0, "fail": 1, "expected_skip": 0}},
        )

    def test_full_catalog_summary_metrics_are_recomputed_from_raw_rows(self) -> None:
        row = self._row()
        summary = self._summary(row)
        summary["metrics"]["schema_completeness_score"] = 1.0
        with tempfile.TemporaryDirectory() as temp_dir:
            result_dir = pathlib.Path(temp_dir)
            summary_path = result_dir / "summary.json"
            (result_dir / "per_action.jsonl").write_text(
                json.dumps(row) + "\n", encoding="utf-8",
            )
            (result_dir / "namespace_breakdown.json").write_text(
                json.dumps(summary["namespace_breakdown"]) + "\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                benchmark_inventory.InventoryError,
                "metrics are not derived",
            ):
                benchmark_inventory.derive_schema_full_results(summary_path, summary)

    def _assert_full_row_rejected(self, row: dict, message: str) -> None:
        summary = self._summary(row)
        with tempfile.TemporaryDirectory() as temp_dir:
            result_dir = pathlib.Path(temp_dir)
            summary_path = result_dir / "summary.json"
            (result_dir / "per_action.jsonl").write_text(
                json.dumps(row) + "\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                benchmark_inventory.InventoryError,
                message,
            ):
                benchmark_inventory.derive_schema_full_results(summary_path, summary)

    def test_full_catalog_rejects_missing_raw_schema(self) -> None:
        row = self._row()
        del row["raw_schema"]
        self._assert_full_row_rejected(row, "missing canonical raw_schema")

    def test_full_catalog_rejects_tampered_raw_schema_with_stale_sha(self) -> None:
        row = self._row()
        row["raw_schema"]["planning_signals"] = ["forged"]
        self._assert_full_row_rejected(row, "does not match raw_schema")

    def test_full_catalog_rejects_forged_all_green_derived_fields(self) -> None:
        row = self._row()
        for field in schema_benchmark.CHECKPOINT_DIMENSION_FIELDS:
            row[field] = True
        row["schema_score"] = 1.0
        row["value_domain_diagnostics"] = [{
            "param": "Broken",
            "ok": True,
            "reason": "forged_ok",
        }]
        self._assert_full_row_rejected(
            row,
            "param_types_declared is not derived from raw_schema",
        )


class InventoryGapContractTests(unittest.TestCase):
    @staticmethod
    def _execution_gates() -> list[dict]:
        return [
            {"id": gate_id, "status": "pending", "contract": "unit contract"}
            for gate_id in benchmark_inventory.REQUIRED_EXECUTION_GATE_IDS
        ]

    def test_suite_gap_id_must_match_the_declared_gap_membership(self) -> None:
        rows = [
            {
                "suite": "ActionGuidance",
                "namespace": "blueprint",
                "items": 1,
                "unwritten": 0,
            },
            {
                "suite": "SourceIndex",
                "namespace": "source",
                "items": 1,
                "unwritten": 0,
            },
        ]
        status = {
            "suites": {
                "ActionGuidance": {
                    "state": "pending",
                    "namespace_results": {},
                    "gap_id": "GAP-STATUS",
                },
                "SourceIndex": {
                    "state": "accepted",
                    "namespace_results": {
                        "source": {"pass": 1, "fail": 0, "expected_skip": 0},
                    },
                    "gap_id": "",
                },
            },
            "gaps": [
                {"id": "GAP-STATUS", "suites": ["SourceIndex"]},
                {"id": "GAP-DECLARED", "suites": ["ActionGuidance"]},
            ],
            "execution_gates": self._execution_gates(),
        }
        with self.assertRaisesRegex(
            benchmark_inventory.InventoryError,
            "gap_id disagrees with gaps",
        ):
            benchmark_inventory.apply_status(rows, status)

    def test_all_resolved_suites_allow_an_empty_gap_list(self) -> None:
        rows = [{
            "suite": "ActionGuidance",
            "namespace": "blueprint",
            "items": 1,
            "unwritten": 0,
        }]
        status = {
            "suites": {
                "ActionGuidance": {
                    "state": "accepted",
                    "namespace_results": {
                        "blueprint": {"pass": 1, "fail": 0, "expected_skip": 0},
                    },
                    "gap_id": "",
                },
            },
            "gaps": [],
            "execution_gates": self._execution_gates(),
        }
        with mock.patch.object(benchmark_inventory, "validate_accepted_evidence"):
            benchmark_inventory.apply_status(rows, status)
        self.assertEqual(rows[0]["unverified"], 0)

    def test_pending_suite_cannot_claim_forged_full_pass_credit(self) -> None:
        rows = [{
            "suite": "ActionGuidance",
            "namespace": "blueprint",
            "items": 1,
            "unwritten": 0,
        }]
        status = {
            "suites": {
                "ActionGuidance": {
                    "state": "pending",
                    "namespace_results": {
                        "blueprint": {"pass": 1, "fail": 0, "expected_skip": 0},
                    },
                    "gap_id": "",
                },
            },
            "gaps": [],
            "execution_gates": self._execution_gates(),
        }
        with self.assertRaisesRegex(
            benchmark_inventory.InventoryError,
            "pending suite ActionGuidance cannot claim",
        ):
            benchmark_inventory.apply_status(rows, status)

    def test_pending_evidence_path_must_resolve_to_an_existing_file(self) -> None:
        rows = [{
            "suite": "ActionGuidance",
            "namespace": "blueprint",
            "items": 1,
            "unwritten": 0,
        }]
        status = {
            "suites": {
                "ActionGuidance": {
                    "state": "pending",
                    "evidence": "Saved/Monolith/Benchmarks/ActionGuidance/unit/missing.json",
                    "namespace_results": {},
                    "gap_id": "GAP-ACTION",
                },
            },
            "gaps": [{"id": "GAP-ACTION", "suites": ["ActionGuidance"]}],
            "execution_gates": self._execution_gates(),
        }
        with tempfile.TemporaryDirectory() as temp_dir, mock.patch.object(
            benchmark_inventory,
            "MONOLITH_ROOT",
            pathlib.Path(temp_dir),
        ):
            with self.assertRaisesRegex(
                benchmark_inventory.InventoryError,
                "pending evidence is missing for ActionGuidance",
            ):
                benchmark_inventory.apply_status(rows, status)

            portable_report: dict[str, list[str]] = {
                "attested_databases": [],
                "omitted_pending_evidence": [],
            }
            benchmark_inventory.apply_status(
                rows,
                status,
                portable=True,
                validation_report=portable_report,
            )
            self.assertEqual(
                portable_report["omitted_pending_evidence"],
                ["Saved/Monolith/Benchmarks/ActionGuidance/unit/missing.json"],
            )
            self.assertEqual(rows[0]["unverified"], 1)

            evidence_path = (
                pathlib.Path(temp_dir)
                / "Saved/Monolith/Benchmarks/ActionGuidance/unit/missing.json"
            )
            evidence_path.parent.mkdir(parents=True, exist_ok=True)
            evidence_path.write_text("diagnostic evidence\n", encoding="utf-8")
            benchmark_inventory.apply_status(rows, status)
            self.assertEqual(rows[0]["unverified"], 1)

    def test_overall_done_requires_every_suite_state_to_be_accepted(self) -> None:
        suite_statuses = {
            spec.key: {"state": "accepted"}
            for spec in benchmark_inventory.CORPORA
        }
        suite_statuses[benchmark_inventory.FULL_CATALOG_KEY] = {"state": "accepted"}
        suite_statuses["ActionGuidance"] = {"state": "pending"}
        status = {
            "snapshot_id": "unit",
            "suites": suite_statuses,
            "gaps": [],
            "execution_gates": [
                {
                    "id": gate_id,
                    "status": "passed",
                    "contract": "unit contract",
                    "evidence": "unit evidence",
                }
                for gate_id in benchmark_inventory.REQUIRED_EXECUTION_GATE_IDS
            ],
        }
        rendered = benchmark_inventory.render_inventory(
            [],
            status,
            {"version": "unit", "namespace_count": 0, "action_count": 0},
        )
        self.assertIn("| YES | 7/8 | NO | 5/5 | NO |", rendered)

    def test_unresolved_suite_still_requires_a_declared_gap(self) -> None:
        rows = [{
            "suite": "ActionGuidance",
            "namespace": "blueprint",
            "items": 1,
            "unwritten": 0,
        }]
        status = {
            "suites": {
                "ActionGuidance": {
                    "state": "pending",
                    "namespace_results": {},
                    "gap_id": "",
                },
            },
            "gaps": [],
            "execution_gates": self._execution_gates(),
        }
        with self.assertRaisesRegex(
            benchmark_inventory.InventoryError,
            "requires a declared gap_id",
        ):
            benchmark_inventory.apply_status(rows, status)


class StrictEvidenceJsonTests(unittest.TestCase):
    def test_duplicate_members_and_non_finite_numbers_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            path = pathlib.Path(temp_dir) / "evidence.json"
            path.write_text('{"value": 1, "value": 2}\n', encoding="utf-8")
            with self.assertRaisesRegex(
                benchmark_inventory.InventoryError,
                "duplicate JSON object member",
            ):
                benchmark_inventory.load_json(path)
            path.write_text('{"value": NaN}\n', encoding="utf-8")
            with self.assertRaisesRegex(
                benchmark_inventory.InventoryError,
                "non-finite JSON number",
            ):
                benchmark_inventory.load_json(path)

    def test_exact_json_comparison_does_not_coerce_booleans_to_numbers(self) -> None:
        self.assertFalse(benchmark_inventory.exact_json_equal({"value": True}, {"value": 1}))
        self.assertFalse(benchmark_inventory.exact_json_equal({"value": False}, {"value": 0.0}))


if __name__ == "__main__":
    unittest.main(verbosity=2)
