#!/usr/bin/env python3
"""Offline unit tests for SchemaCompleteness catalog enumeration + fail-fast.

Covers the N5 (2026-07-10) benchmark-contract drift: the compact discover
contract stopped inlining action lists in mode="summary" rows, which made the
old scan enumerate 0 actions and silently record schema_completeness_score 0.0
with exit code 0. These tests fabricate the documented compact discover
envelopes and assert that:

  * discover_namespace_actions pages through mode="actions" via next_offset,
  * enumerate_catalog_actions honors legacy inline lists, skips
    action_count == 0 optional namespaces, and reports count mismatches,
  * cmd_scan exits non-zero on a zero-action enumeration instead of writing a
    0.0 baseline, and exits non-zero when every schema fetch fails mid-scan,
  * full-scan checkpoints resume only successful rows under an identical input
    and catalog identity, preserving segment/outage provenance,
  * duplicate result ids and catalog/input drift are rejected before scoring,
  * the happy path still scans and writes summary.json with exit 0.

Run:
    python Plugins/Monolith/Scripts/tests/test_schema_completeness_enumeration.py
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

SCRIPTS_DIR = pathlib.Path(__file__).resolve().parent.parent
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

import benchmark_common as benchmark_common  # noqa: E402
import schema_completeness_benchmark as scb  # noqa: E402


def mcp_envelope(payload: dict) -> dict:
    return {
        "result": {
            "content": [{"type": "text", "text": json.dumps(payload)}],
            "isError": False,
        }
    }


HEALTHY_SCHEMA = {
    "skill": "unreal-blueprints",
    "planning_signals": ["read_first"],
    "output_contract_status": "declared",
    "params": {
        "asset_path": {
            "type": "string",
            "description": "Blueprint asset path",
            "required": True,
        },
    },
}


class FakeCompactDiscoverServer:
    """Serves the compact discover contract: summary rows carry action_count
    only; action names come from paginated mode="actions"; schemas come from
    mode="schema"."""

    def __init__(
        self,
        catalog: dict,
        page_size: int = 2,
        schema=HEALTHY_SCHEMA,
        catalog_version: str = "sha256:fake",
    ):
        # catalog: {namespace: [action, ...]}
        self.catalog = catalog
        self.page_size = page_size
        self.schema = schema
        self.catalog_version = catalog_version
        self.calls = []

    def __call__(self, url, tool, arguments, timeout_s=8.0):
        self.calls.append(arguments)
        if tool == "monolith_status":
            return mcp_envelope({
                "server_running": True,
                "project_name": "Speed",
                "catalog_version": self.catalog_version,
            })
        namespace = arguments.get("namespace")
        mode = arguments.get("mode")
        if not namespace:
            rows = [
                {"namespace": ns, "action_count": len(actions), "projection": "summary"}
                for ns, actions in self.catalog.items()
            ]
            return mcp_envelope({
                "mode": "summary",
                "catalog_version": self.catalog_version,
                "namespaces": rows,
            })
        if mode == "actions":
            actions = self.catalog.get(namespace, [])
            offset = int(arguments.get("offset", 0))
            page = actions[offset:offset + self.page_size]
            payload = {
                "namespace": namespace,
                "mode": "actions",
                "actions": [{"action": name} for name in page],
                "total": len(actions),
                "truncated": offset + self.page_size < len(actions),
            }
            if payload["truncated"]:
                payload["next_offset"] = offset + self.page_size
            return mcp_envelope(payload)
        if mode == "schema":
            return mcp_envelope({"namespace": namespace, "action": arguments.get("action"), "schema": self.schema})
        raise AssertionError(f"unexpected fake discover call: {arguments}")


class PaginationTests(unittest.TestCase):
    def _with_fake(self, fake):
        original = scb.mcp_call
        scb.mcp_call = fake
        self.addCleanup(lambda: setattr(scb, "mcp_call", original))

    def test_pages_are_joined_via_next_offset(self):
        fake = FakeCompactDiscoverServer({"blueprint": ["a1", "a2", "a3"]}, page_size=2)
        self._with_fake(fake)
        actions = scb.discover_namespace_actions("http://x", "blueprint", 1.0)
        self.assertEqual(actions, ["a1", "a2", "a3"])
        action_pages = [c for c in fake.calls if c.get("mode") == "actions"]
        self.assertEqual(len(action_pages), 2)

    def test_missing_actions_list_raises(self):
        def fake(url, tool, arguments, timeout_s=8.0):
            return mcp_envelope({"namespace": "blueprint", "mode": "actions"})

        self._with_fake(fake)
        with self.assertRaises(RuntimeError):
            scb.discover_namespace_actions("http://x", "blueprint", 1.0)

    def test_transport_error_raises(self):
        def fake(url, tool, arguments, timeout_s=8.0):
            return {"transport_error": True, "status": None, "raw": "connection refused"}

        self._with_fake(fake)
        with self.assertRaises(RuntimeError):
            scb.discover_namespace_actions("http://x", "blueprint", 1.0)

    def test_non_advancing_pagination_raises(self):
        def fake(url, tool, arguments, timeout_s=8.0):
            return mcp_envelope({
                "namespace": "blueprint",
                "mode": "actions",
                "actions": [{"action": "a1"}],
                "truncated": True,
                "next_offset": 0,
            })

        self._with_fake(fake)
        with self.assertRaises(RuntimeError):
            scb.discover_namespace_actions("http://x", "blueprint", 1.0)


class SchemaFetchOutcomeTests(unittest.TestCase):
    def test_non_object_top_level_json_is_structured_protocol_error(self):
        with mock.patch.object(scb, "mcp_call", return_value=[]):
            outcome = scb.fetch_schema_for_action(
                "http://x", "blueprint", "a1", timeout_s=1.0
            )
        self.assertEqual(outcome.failure_kind, "protocol_error")
        self.assertFalse(outcome.transport_error)
        self.assertIn("top-level JSON", outcome.error)


class EnumerateCatalogTests(unittest.TestCase):
    def _with_fake(self, fake):
        original = scb.mcp_call
        scb.mcp_call = fake
        self.addCleanup(lambda: setattr(scb, "mcp_call", original))

    def test_compact_rows_enumerate_and_zero_count_namespaces_skip(self):
        fake = FakeCompactDiscoverServer({"blueprint": ["a1", "a2", "a3"]}, page_size=2)
        self._with_fake(fake)
        namespaces = [
            {"namespace": "blueprint", "action_count": 3, "projection": "summary"},
            # optional module that is disabled/not installed: never enumerated
            {"namespace": "metahuman", "action_count": 0, "projection": "summary"},
        ]
        pairs, errors = scb.enumerate_catalog_actions("http://x", namespaces, 1.0)
        self.assertEqual(errors, [])
        self.assertEqual(pairs, [("blueprint", "a1"), ("blueprint", "a2"), ("blueprint", "a3")])
        self.assertTrue(all(c.get("namespace") != "metahuman" for c in fake.calls))

    def test_legacy_inline_actions_are_honored_without_extra_calls(self):
        def fake(url, tool, arguments, timeout_s=8.0):
            raise AssertionError("legacy inline rows must not trigger mode=actions calls")

        self._with_fake(fake)
        namespaces = [{"namespace": "source", "actions": [{"action": "read_file"}, "search_source"]}]
        pairs, errors = scb.enumerate_catalog_actions("http://x", namespaces, 1.0)
        self.assertEqual(errors, [])
        self.assertEqual(pairs, [("source", "read_file"), ("source", "search_source")])

    def test_count_mismatch_is_reported_as_error(self):
        fake = FakeCompactDiscoverServer({"blueprint": ["a1", "a2"]}, page_size=2)
        self._with_fake(fake)
        namespaces = [{"namespace": "blueprint", "action_count": 5, "projection": "summary"}]
        pairs, errors = scb.enumerate_catalog_actions("http://x", namespaces, 1.0)
        self.assertEqual(len(pairs), 2)
        self.assertEqual(len(errors), 1)
        self.assertIn("action_count=5", errors[0])

    def test_namespace_enumeration_failure_is_reported_as_error(self):
        def fake(url, tool, arguments, timeout_s=8.0):
            if not arguments.get("namespace"):
                return mcp_envelope({"mode": "summary", "namespaces": []})
            return {"transport_error": True, "status": None, "raw": "boom"}

        self._with_fake(fake)
        namespaces = [{"namespace": "blueprint", "action_count": 3, "projection": "summary"}]
        pairs, errors = scb.enumerate_catalog_actions("http://x", namespaces, 1.0)
        self.assertEqual(pairs, [])
        self.assertEqual(len(errors), 1)

    def test_transport_failure_aborts_remaining_namespace_requests(self):
        requested_namespaces = []

        def fake(url, tool, arguments, timeout_s=8.0):
            namespace = arguments.get("namespace")
            requested_namespaces.append(namespace)
            if namespace == "first":
                return {
                    "transport_error": True,
                    "status": None,
                    "raw": "timed out behind a blocked editor dispatch",
                }
            raise AssertionError("transport failure must stop catalog enumeration")

        self._with_fake(fake)
        namespaces = [
            {"namespace": "first", "action_count": 1, "projection": "summary"},
            {"namespace": "second", "action_count": 1, "projection": "summary"},
        ]
        pairs, errors = scb.enumerate_catalog_actions("http://x", namespaces, 1.0)
        self.assertEqual(pairs, [])
        self.assertEqual(len(errors), 1)
        self.assertIn("transport error", errors[0])
        self.assertEqual(requested_namespaces, ["first"])

    def test_catalog_version_change_during_enumeration_is_rejected(self):
        def fake(url, tool, arguments, timeout_s=8.0):
            if not arguments.get("namespace"):
                if arguments.get("if_version"):
                    return mcp_envelope({
                        "status": "changed",
                        "catalog_version": "sha256:v2",
                        "namespaces": [],
                    })
                return mcp_envelope({
                    "mode": "summary",
                    "catalog_version": "sha256:v1",
                    "namespaces": [{"namespace": "blueprint", "action_count": 1}],
                })
            return mcp_envelope({
                "namespace": "blueprint",
                "mode": "actions",
                "actions": [{"action": "a1"}],
                "total": 1,
                "truncated": False,
            })

        self._with_fake(fake)
        with self.assertRaisesRegex(RuntimeError, "catalog version changed"):
            scb.discover_complete_catalog("http://x", 1.0)


class ProbeSetContractTests(unittest.TestCase):
    @staticmethod
    def _write_contract(root: pathlib.Path, rows: list, manifest_count=None):
        probe_path = root / "probe_set.jsonl"
        probe_path.write_text(
            "".join(json.dumps(row) + "\n" for row in rows),
            encoding="utf-8",
        )
        manifest = {"probe_set_task_count": len(rows) if manifest_count is None else manifest_count}
        (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        return probe_path

    @staticmethod
    def _row(action="a1", availability=None):
        row = {
            "namespace": "blueprint",
            "action": action,
            "priority": "high",
            "expected_dimensions": ["param_types_declared", "planning_signals_present"],
            "rationale": "offline probe contract",
        }
        if availability is not None:
            row["availability"] = availability
        return row

    def test_source_control_probes_track_live_actions_not_stale_names(self):
        probe_path = SCRIPTS_DIR.parent / "Benchmarks" / "SchemaCompleteness" / "probe_set.jsonl"
        probes = scb.load_probe_set(probe_path)
        pairs = {(str(row.get("namespace")), str(row.get("action"))) for row in probes}
        self.assertIn(("source_control", "list_opened"), pairs)
        self.assertIn(("source_control", "map_depot_paths"), pairs)
        self.assertNotIn(("source_control", "list_changelists"), pairs)
        self.assertNotIn(("source_control", "get_diff"), pairs)

        source_control_rows = [row for row in probes if row.get("namespace") == "source_control"]
        target_rows = {
            str(row.get("action")): row
            for row in source_control_rows
            if row.get("action") in {"list_opened", "map_depot_paths"}
        }
        self.assertEqual(set(target_rows), {"list_opened", "map_depot_paths"})
        for row in target_rows.values():
            self.assertEqual(row.get("priority"), "critical")
            self.assertIn("value_domain", row.get("expected_dimensions", []))

    def test_checked_in_contract_matches_manifest_without_hidden_supplements(self):
        probe_path = SCRIPTS_DIR.parent / "Benchmarks" / "SchemaCompleteness" / "probe_set.jsonl"
        physical_count = sum(1 for line in probe_path.read_text(encoding="utf-8").splitlines() if line.strip())
        probes = scb.load_probe_set(probe_path)
        self.assertEqual(len(probes), physical_count)
        self.assertFalse(hasattr(scb, "STATIC_PROBE_SUPPLEMENTS_20260617"))

    def test_checked_in_migration_map_is_fully_applied(self):
        benchmark_dir = SCRIPTS_DIR.parent / "Benchmarks" / "SchemaCompleteness"
        probes = scb.load_probe_set(benchmark_dir / "probe_set.jsonl")
        probe_by_id = {
            f"{row['namespace']}.{row['action']}": row
            for row in probes
        }
        migration = json.loads(
            (benchmark_dir / "probe_migration_20260711.json").read_text(encoding="utf-8")
        )
        mappings = migration["mappings"]
        self.assertEqual(len(mappings), 122)
        stale_ids = {row["source_action_id"] for row in mappings}
        self.assertTrue(stale_ids.isdisjoint(probe_by_id))
        replacement_ids = {
            replacement
            for row in mappings
            for replacement in row["replacements"]
        }
        self.assertTrue(replacement_ids.issubset(probe_by_id))
        manifest = json.loads((benchmark_dir / "manifest.json").read_text(encoding="utf-8"))
        provenance = manifest["probe_migration"]
        classification_counts = {
            classification: sum(
                1 for row in mappings if row["classification"] == classification
            )
            for classification in ("direct", "partial", "composite", "no-equivalent")
        }
        self.assertEqual(provenance["migration_mapping_count"], len(mappings))
        self.assertEqual(provenance["classification_counts"], classification_counts)
        self.assertEqual(
            provenance["replacement_occurrence_count"],
            sum(len(row["replacements"]) for row in mappings),
        )
        self.assertEqual(
            provenance["unique_replacement_action_count"],
            len(replacement_ids),
        )
        self.assertEqual(provenance["gated_action_count"], len(migration["gated"]))
        self.assertEqual(provenance["replacement_actions_added"], 64)
        self.assertEqual(provenance["newly_added_probe_action_count"], 64)
        for action_id, gate in migration["gated"].items():
            self.assertEqual(
                probe_by_id[action_id]["availability"],
                {
                    "mode": gate["mode"],
                    "gate": {"kind": gate["kind"], "id": gate["id"]},
                },
            )

    def test_checked_in_runtime_config_gates_are_explicit(self):
        benchmark_dir = SCRIPTS_DIR.parent / "Benchmarks" / "SchemaCompleteness"
        probes = scb.load_probe_set(benchmark_dir / "probe_set.jsonl")
        probe_by_id = {
            f"{row['namespace']}.{row['action']}": row
            for row in probes
        }
        expected_gates = {
            "gamefeatures.list_plugins": "bEnableGameFeatureActions",
            "slate.describe_widget": "bEnableSlateInspectorActions",
            "worldgen.get_building_archetype": "bEnableProceduralTownGen",
        }
        for action_id, gate_id in expected_gates.items():
            self.assertEqual(
                probe_by_id[action_id]["availability"],
                {
                    "mode": "feature_gated",
                    "gate": {"kind": "config", "id": gate_id},
                },
            )

    def test_checked_in_run_gates_match_runner_defaults(self):
        manifest_path = (
            SCRIPTS_DIR.parent / "Benchmarks" / "SchemaCompleteness" / "manifest.json"
        )
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        self.assertEqual(
            manifest["run_gates"],
            {
                "max_failed_fraction": scb.DEFAULT_MAX_FAILED_FRACTION,
                "max_transport_failed_fraction": scb.DEFAULT_MAX_TRANSPORT_FAILED_FRACTION,
                "max_consecutive_transport_failures": scb.DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES,
                "min_transport_fraction_sample": scb.MIN_TRANSPORT_FRACTION_SAMPLE,
                "invalid_status_response_aborts_before_schema_fetch": True,
                "status_catalog_version_mismatch_aborts_before_schema_fetch": True,
                "catalog_transport_failure_aborts_remaining_namespaces": True,
                "invalid_run_writes_summary": False,
                "full_scan_requires_all_valid_results": True,
                "full_scan_resume_checkpoint": True,
                "successful_rows_require_canonical_raw_schema": True,
                "successful_rows_require_exact_raw_rescoring": True,
                "resume_requires_identical_input_catalog_identity": True,
                "normal_summary_requires_exact_catalog_result_set": True,
            },
        )
        self.assertEqual(
            manifest["full_scan_resume"]["contract_version"],
            scb.SCAN_CONTRACT_VERSION,
        )
        self.assertEqual(
            manifest["full_scan_resume"]["checkpoint_schema_version"],
            scb.SCAN_CHECKPOINT_SCHEMA_VERSION,
        )
        self.assertEqual(
            manifest["full_scan_resume"]["raw_schema_hash_kind"],
            scb.RAW_SCHEMA_HASH_KIND,
        )

    def test_duplicate_probe_pair_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            probe_path = self._write_contract(root, [self._row(), self._row()])
            with self.assertRaisesRegex(scb.ProbeSetContractError, "duplicate probe"):
                scb.load_probe_set(probe_path)

    def test_manifest_count_mismatch_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            probe_path = self._write_contract(root, [self._row()], manifest_count=2)
            with self.assertRaisesRegex(scb.ProbeSetContractError, "parsed_probe_count=1"):
                scb.load_probe_set(probe_path)

    def test_unknown_or_unproven_availability_is_rejected(self):
        bad_rows = [
            self._row(availability={"mode": "conditional"}),
            self._row(availability={"mode": "feature_gated"}),
        ]
        for row in bad_rows:
            with self.subTest(row=row):
                with tempfile.TemporaryDirectory() as tmp:
                    root = pathlib.Path(tmp)
                    probe_path = self._write_contract(root, [row])
                    with self.assertRaises(scb.ProbeSetContractError):
                        scb.load_probe_set(probe_path)

    def test_duplicate_json_member_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            probe_path = root / "probe_set.jsonl"
            probe_path.write_text(
                '{"namespace":"blueprint","action":"a1","action":"a2",'
                '"priority":"high","expected_dimensions":["planning_signals_present"],'
                '"rationale":"duplicate action key"}\n',
                encoding="utf-8",
            )
            (root / "manifest.json").write_text(
                json.dumps({"probe_set_task_count": 1}), encoding="utf-8"
            )
            with self.assertRaisesRegex(scb.ProbeSetContractError, "duplicate JSON member"):
                scb.load_probe_set(probe_path)

    def test_noncanonical_action_id_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            probe_path = self._write_contract(root, [self._row(action="Bad-Action")])
            with self.assertRaisesRegex(scb.ProbeSetContractError, "canonical lower_snake_case"):
                scb.load_probe_set(probe_path)

    def test_duplicate_expected_dimension_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            row = self._row()
            row["expected_dimensions"] = [
                "planning_signals_present",
                "planning_signals_present",
            ]
            probe_path = self._write_contract(root, [row])
            with self.assertRaisesRegex(scb.ProbeSetContractError, "duplicate expected_dimensions"):
                scb.load_probe_set(probe_path)

    def test_missing_availability_defaults_to_required(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            probe_path = self._write_contract(root, [self._row()])
            probes = scb.load_probe_set(probe_path)
            self.assertEqual(probes[0]["availability"], {"mode": "required"})


class SchemaBenchmarkInputTests(unittest.TestCase):
    @staticmethod
    def _build_inputs(
        probe_path: pathlib.Path,
        runner_path: pathlib.Path,
        *,
        catalog_version: str = "sha256:v1",
    ) -> dict:
        with mock.patch.object(scb, "__file__", str(runner_path)):
            return scb.build_schema_registry_inputs(
                status={
                    "catalog_version": catalog_version,
                    "project": "UnitTest",
                },
                namespaces=[
                    {
                        "namespace": "blueprint",
                        "actions": [{"action": "a1"}],
                    }
                ],
                catalog_pairs=[("blueprint", "a1")],
                probe_set_path=probe_path,
            )

    def test_probe_inputs_hash_contract_runner_and_exclude_databases(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            probe_path = ProbeSetContractTests._write_contract(
                root,
                [ProbeSetContractTests._row()],
            )
            runner_path = root / "schema_runner.py"
            runner_path.write_text("# synthetic runner\n", encoding="utf-8")

            inputs = self._build_inputs(probe_path, runner_path)

            self.assertEqual(
                set(inputs["files"]),
                {"benchmark_common", "manifest", "probe_set", "runner"},
            )
            self.assertEqual(inputs["database_files"], [])
            self.assertEqual(
                inputs["database_files_scope"],
                "not_applicable_to_live_schema_registry_scan",
            )
            self.assertEqual(inputs["schema_registry"]["catalog_action_count"], 1)
            self.assertEqual(
                inputs["schema_registry"]["catalog_action_ids_sha256"],
                scb._sha256_json(["blueprint.a1"]),
            )
            payload = dict(inputs)
            fingerprint = payload.pop("fingerprint_sha256")
            self.assertEqual(fingerprint, scb._sha256_json(payload))
            self.assertEqual(
                fingerprint,
                benchmark_common.benchmark_input_fingerprint(inputs),
            )

    def test_probe_fingerprint_tracks_contract_runner_and_catalog_not_databases(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            probe_path = ProbeSetContractTests._write_contract(
                root,
                [ProbeSetContractTests._row()],
            )
            runner_path = root / "schema_runner.py"
            runner_path.write_text("# runner v1\n", encoding="utf-8")
            unrelated_db = root / "Unrelated.db"
            unrelated_db.write_bytes(b"database-v1")

            with mock.patch.object(
                benchmark_common,
                "_DB_CANDIDATES",
                (unrelated_db,),
            ):
                baseline = self._build_inputs(probe_path, runner_path)

                probe_path.write_text(
                    json.dumps(ProbeSetContractTests._row(action="a2")) + "\n",
                    encoding="utf-8",
                )
                probe_changed = self._build_inputs(probe_path, runner_path)
                self.assertNotEqual(
                    baseline["fingerprint_sha256"],
                    probe_changed["fingerprint_sha256"],
                )

                runner_path.write_text("# runner v2\n", encoding="utf-8")
                runner_changed = self._build_inputs(probe_path, runner_path)
                self.assertNotEqual(
                    probe_changed["fingerprint_sha256"],
                    runner_changed["fingerprint_sha256"],
                )

                catalog_changed = self._build_inputs(
                    probe_path,
                    runner_path,
                    catalog_version="sha256:v2",
                )
                self.assertNotEqual(
                    runner_changed["fingerprint_sha256"],
                    catalog_changed["fingerprint_sha256"],
                )

                unrelated_db.write_bytes(b"database-v2-with-different-size")
                after_db_change = self._build_inputs(
                    probe_path,
                    runner_path,
                    catalog_version="sha256:v2",
                )

            self.assertEqual(catalog_changed["database_files"], [])
            self.assertEqual(after_db_change["database_files"], [])
            self.assertEqual(
                catalog_changed["fingerprint_sha256"],
                after_db_change["fingerprint_sha256"],
            )


class ProbeCatalogPreflightTests(unittest.TestCase):
    def _with_fake(self, fake):
        original = scb.mcp_call
        scb.mcp_call = fake
        self.addCleanup(lambda: setattr(scb, "mcp_call", original))

    @staticmethod
    def _probe_args(probe_path: pathlib.Path, output_dir: pathlib.Path):
        return argparse.Namespace(
            probe_set=probe_path,
            mcp_url="http://x",
            output_dir=output_dir,
            label="probe-unit-test",
            request_timeout_s=1.0,
            max_failed_fraction=0.05,
            max_transport_failed_fraction=scb.DEFAULT_MAX_TRANSPORT_FAILED_FRACTION,
            max_consecutive_transport_failures=scb.DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES,
            min_transport_fraction_sample=scb.MIN_TRANSPORT_FRACTION_SAMPLE,
            resume=False,
        )

    def test_required_absence_aborts_before_schema_fetch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            row = ProbeSetContractTests._row(action="missing")
            probe_path = ProbeSetContractTests._write_contract(root, [row])
            fake = FakeCompactDiscoverServer({"blueprint": ["present"]})
            self._with_fake(fake)
            output_dir = root / "run"
            rc = scb.cmd_probe(self._probe_args(probe_path, output_dir))
            self.assertEqual(rc, 1)
            self.assertFalse(any(call.get("mode") == "schema" for call in fake.calls))
            self.assertFalse((output_dir / "summary.json").exists())
            result = json.loads((output_dir / "probe_results.jsonl").read_text(encoding="utf-8"))
            self.assertEqual(result["result_status"], "stale")

    def test_feature_gated_absence_is_skipped_without_score_pollution(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            rows = [
                ProbeSetContractTests._row(action="present"),
                ProbeSetContractTests._row(
                    action="gated",
                    availability={
                        "mode": "feature_gated",
                        "gate": {"kind": "compile_flag", "id": "WITH_TEST"},
                    },
                ),
            ]
            probe_path = ProbeSetContractTests._write_contract(root, rows)
            fake = FakeCompactDiscoverServer({"blueprint": ["present"]})
            self._with_fake(fake)
            output_dir = root / "run"
            rc = scb.cmd_probe(self._probe_args(probe_path, output_dir))
            self.assertEqual(rc, 0)
            schema_calls = [call for call in fake.calls if call.get("mode") == "schema"]
            self.assertEqual(len(schema_calls), 1)
            summary = json.loads((output_dir / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["scanned_action_count"], 1)
            self.assertEqual(summary["probe_metrics"]["declared_probe_count"], 2)
            self.assertEqual(summary["probe_metrics"]["skipped_feature_gated_count"], 1)
            results = [json.loads(line) for line in (output_dir / "probe_results.jsonl").read_text(encoding="utf-8").splitlines()]
            self.assertEqual([row["result_status"] for row in results], ["scored", "skipped"])

    def test_feature_gated_probe_is_scored_when_present(self):
        probe = {
            "namespace": "blueprint",
            "action": "gated",
            "availability": {
                "mode": "feature_gated",
                "gate": {"kind": "compile_flag", "id": "WITH_TEST"},
            },
        }
        runnable, skipped, stale = scb.classify_probes_against_catalog(
            [probe], {"blueprint": {"gated"}}
        )
        self.assertEqual(runnable, [probe])
        self.assertEqual(skipped, [])
        self.assertEqual(stale, [])

    def test_three_consecutive_probe_transport_failures_abort_without_summary(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            rows = [ProbeSetContractTests._row(action=f"a{index}") for index in range(1, 6)]
            probe_path = ProbeSetContractTests._write_contract(root, rows)

            class SchemaDeadServer(FakeCompactDiscoverServer):
                def __call__(self, url, tool, arguments, timeout_s=8.0):
                    if arguments.get("mode") == "schema":
                        self.calls.append(arguments)
                        return {"transport_error": True, "status": None, "raw": "editor down"}
                    return super().__call__(url, tool, arguments, timeout_s=timeout_s)

            fake = SchemaDeadServer({"blueprint": [f"a{index}" for index in range(1, 6)]})
            self._with_fake(fake)
            output_dir = root / "run"
            rc = scb.cmd_probe(self._probe_args(probe_path, output_dir))
            self.assertEqual(rc, 1)
            self.assertEqual(
                len([call for call in fake.calls if call.get("mode") == "schema"]),
                3,
            )
            self.assertFalse((output_dir / "summary.json").exists())
            failure = json.loads((output_dir / "run_failure.json").read_text(encoding="utf-8"))
            self.assertEqual(failure["transport_gate_reason"], "consecutive_transport_failures")
            self.assertEqual(failure["completed_action_count"], 3)
            self.assertTrue((output_dir / "partial_summary.json").exists())

    def test_probe_scoring_exception_preserves_triggering_rows_and_invalid_artifacts(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            probe_path = ProbeSetContractTests._write_contract(
                root,
                [ProbeSetContractTests._row(action="a1")],
            )
            fake = FakeCompactDiscoverServer({"blueprint": ["a1"]})
            self._with_fake(fake)
            output_dir = root / "run"
            with mock.patch.object(
                scb,
                "score_schema_quality",
                side_effect=RuntimeError("score exploded"),
            ):
                rc = scb.cmd_probe(self._probe_args(probe_path, output_dir))

            self.assertEqual(rc, 1)
            self.assertFalse((output_dir / "summary.json").exists())
            failure = json.loads((output_dir / "run_failure.json").read_text(encoding="utf-8"))
            partial = json.loads((output_dir / "partial_summary.json").read_text(encoding="utf-8"))
            per_action = [
                json.loads(line)
                for line in (output_dir / "per_action.jsonl").read_text(encoding="utf-8").splitlines()
            ]
            probe_results = [
                json.loads(line)
                for line in (output_dir / "probe_results.jsonl").read_text(encoding="utf-8").splitlines()
            ]
            self.assertEqual(failure["completion_status"], "aborted_runner_exception")
            self.assertFalse(failure["metrics_valid"])
            self.assertFalse(partial["metrics_valid"])
            self.assertEqual(per_action[0]["failure_kind"], "runner_exception")
            self.assertEqual(probe_results[0]["failure_kind"], "runner_exception")


class ScanFailFastTests(unittest.TestCase):
    def _with_fake(self, fake):
        original = scb.mcp_call
        scb.mcp_call = fake
        self.addCleanup(lambda: setattr(scb, "mcp_call", original))

    def _scan_args(
        self,
        output_dir: pathlib.Path,
        max_actions=None,
        max_failed_fraction=scb.DEFAULT_MAX_FAILED_FRACTION,
    ) -> argparse.Namespace:
        return argparse.Namespace(
            mcp_url="http://x",
            output_dir=output_dir,
            label="unit-test",
            request_timeout_s=1.0,
            max_actions=max_actions,
            max_failed_fraction=max_failed_fraction,
            max_transport_failed_fraction=scb.DEFAULT_MAX_TRANSPORT_FAILED_FRACTION,
            max_consecutive_transport_failures=scb.DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES,
            min_transport_fraction_sample=scb.MIN_TRANSPORT_FRACTION_SAMPLE,
        )

    def test_zero_action_enumeration_exits_nonzero_and_writes_no_summary(self):
        # Compact summary with only zero-count namespaces enumerates to zero
        # actions — the historic N5 silent-0.0 shape must now exit 1.
        fake = FakeCompactDiscoverServer({"metahuman": []})
        self._with_fake(fake)
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "run"
            rc = scb.cmd_scan(self._scan_args(out))
            self.assertEqual(rc, 1)
            self.assertFalse((out / "summary.json").exists())

    def test_status_transport_failure_aborts_before_schema_calls(self):
        class StatusDownServer(FakeCompactDiscoverServer):
            def __call__(self, url, tool, arguments, timeout_s=8.0):
                if tool == "monolith_status":
                    self.calls.append(arguments)
                    return {"transport_error": True, "status": None, "raw": "status down"}
                return super().__call__(url, tool, arguments, timeout_s=timeout_s)

        fake = StatusDownServer({"blueprint": ["a1", "a2"]})
        self._with_fake(fake)
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "run"
            rc = scb.cmd_scan(self._scan_args(out))
            self.assertEqual(rc, 1)
            self.assertFalse(any(call.get("mode") == "schema" for call in fake.calls))
            failure = json.loads((out / "run_failure.json").read_text(encoding="utf-8"))
            self.assertEqual(failure["failure_stage"], "status_preflight")
            self.assertFalse((out / "summary.json").exists())

    def test_status_protocol_error_aborts_before_schema_calls(self):
        class StatusProtocolServer(FakeCompactDiscoverServer):
            def __call__(self, url, tool, arguments, timeout_s=8.0):
                if tool == "monolith_status":
                    self.calls.append(arguments)
                    return {"parse_error": True, "raw": "not-json"}
                return super().__call__(url, tool, arguments, timeout_s=timeout_s)

        fake = StatusProtocolServer({"blueprint": ["a1"]})
        self._with_fake(fake)
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "run"
            rc = scb.cmd_scan(self._scan_args(out))
            self.assertEqual(rc, 1)
            self.assertFalse(any(call.get("mode") == "schema" for call in fake.calls))
            failure = json.loads((out / "run_failure.json").read_text(encoding="utf-8"))
            self.assertEqual(failure["failure_kind"], "protocol_error")
            self.assertFalse(failure["metrics_valid"])
            self.assertFalse((out / "summary.json").exists())

    def test_status_catalog_version_mismatch_aborts_before_schema_calls(self):
        class CatalogMismatchServer(FakeCompactDiscoverServer):
            def __call__(self, url, tool, arguments, timeout_s=8.0):
                if tool == "monolith_status":
                    self.calls.append(arguments)
                    return mcp_envelope({
                        "server_running": True,
                        "project_name": "Speed",
                        "catalog_version": "sha256:v2",
                    })
                if not arguments.get("namespace"):
                    self.calls.append(arguments)
                    return mcp_envelope({
                        "mode": "summary",
                        "catalog_version": "sha256:v1",
                        "namespaces": [
                            {"namespace": "blueprint", "action_count": 1}
                        ],
                    })
                return super().__call__(url, tool, arguments, timeout_s=timeout_s)

        fake = CatalogMismatchServer({"blueprint": ["a1"]})
        self._with_fake(fake)
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "run"
            rc = scb.cmd_scan(self._scan_args(out))
            self.assertEqual(rc, 1)
            self.assertFalse(any(call.get("mode") == "schema" for call in fake.calls))
            failure = json.loads((out / "run_failure.json").read_text(encoding="utf-8"))
            self.assertEqual(failure["failure_stage"], "catalog_status_recheck")
            self.assertEqual(failure["enumerated_catalog_version"], "sha256:v1")
            self.assertEqual(failure["status_catalog_version"], "sha256:v2")
            self.assertFalse((out / "summary.json").exists())

    def test_invalid_transport_configuration_writes_failure_without_scoring(self):
        fake = FakeCompactDiscoverServer({"blueprint": ["a1"]})
        self._with_fake(fake)
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "run"
            args = self._scan_args(out)
            args.min_transport_fraction_sample = 0
            rc = scb.cmd_scan(args)
            self.assertEqual(rc, 1)
            self.assertFalse(any(call.get("mode") == "schema" for call in fake.calls))
            failure = json.loads((out / "run_failure.json").read_text(encoding="utf-8"))
            self.assertEqual(failure["completion_status"], "aborted_invalid_configuration")
            self.assertFalse(failure["metrics_valid"])

    def test_enumeration_error_aborts_before_scoring(self):
        def fake(url, tool, arguments, timeout_s=8.0):
            if not arguments.get("namespace"):
                return mcp_envelope({
                    "mode": "summary",
                    "namespaces": [
                        {"namespace": "blueprint", "action_count": 2},
                        {"namespace": "broken", "action_count": 4},
                    ],
                })
            if arguments.get("namespace") == "blueprint":
                return mcp_envelope({
                    "namespace": "blueprint",
                    "mode": "actions",
                    "actions": [{"action": "a1"}, {"action": "a2"}],
                    "total": 2,
                    "truncated": False,
                })
            return {"transport_error": True, "status": None, "raw": "boom"}

        self._with_fake(fake)
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "run"
            rc = scb.cmd_scan(self._scan_args(out))
            self.assertEqual(rc, 1)
            self.assertFalse((out / "summary.json").exists())

    def test_all_schema_fetches_failing_exits_nonzero(self):
        class SchemaDeadServer(FakeCompactDiscoverServer):
            def __call__(self, url, tool, arguments, timeout_s=8.0):
                if arguments.get("mode") == "schema":
                    return {"transport_error": True, "status": None, "raw": "died mid-scan"}
                return super().__call__(url, tool, arguments, timeout_s=timeout_s)

        fake = SchemaDeadServer({"blueprint": ["a1", "a2"]})
        self._with_fake(fake)
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "run"
            rc = scb.cmd_scan(self._scan_args(out))
            self.assertEqual(rc, 1)
            self.assertFalse((out / "summary.json").exists())
            failure = json.loads((out / "run_failure.json").read_text(encoding="utf-8"))
            self.assertEqual(failure["attempted_invalid_action_count"], 2)
            self.assertEqual(failure["completed_valid_action_count"], 0)
            self.assertFalse(failure["run_valid"])

    def test_partial_failure_over_budget_exits_nonzero(self):
        # One namespace's schema fetches fail (editor died mid-scan burst);
        # 2/4 failed > 5% budget must exit 1 without a normal summary.
        class BlueprintDeadServer(FakeCompactDiscoverServer):
            def __call__(self, url, tool, arguments, timeout_s=8.0):
                if arguments.get("mode") == "schema" and arguments.get("namespace") == "blueprint":
                    return {"transport_error": True, "status": None, "raw": "editor restarting"}
                return super().__call__(url, tool, arguments, timeout_s=timeout_s)

        fake = BlueprintDeadServer({"blueprint": ["a1", "a2"], "source": ["s1", "s2"]})
        self._with_fake(fake)
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "run"
            rc = scb.cmd_scan(self._scan_args(out))
            self.assertEqual(rc, 1)
            self.assertFalse((out / "summary.json").exists())
            failure = json.loads((out / "run_failure.json").read_text(encoding="utf-8"))
            self.assertEqual(failure["attempted_invalid_action_count"], 2)
            self.assertEqual(failure["completed_valid_action_count"], 2)

    def test_partial_failure_within_budget_still_requires_exact_valid_set(self):
        class BlueprintSchemaMissingServer(FakeCompactDiscoverServer):
            def __call__(self, url, tool, arguments, timeout_s=8.0):
                if arguments.get("mode") == "schema" and arguments.get("namespace") == "blueprint":
                    return mcp_envelope({
                        "namespace": "blueprint",
                        "action": arguments.get("action"),
                    })
                return super().__call__(url, tool, arguments, timeout_s=timeout_s)

        fake = BlueprintSchemaMissingServer({"blueprint": ["a1", "a2"], "source": ["s1", "s2"]})
        self._with_fake(fake)
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "run"
            rc = scb.cmd_scan(self._scan_args(out, max_failed_fraction=0.5))
            self.assertEqual(rc, 1)
            self.assertFalse((out / "summary.json").exists())
            failure = json.loads((out / "run_failure.json").read_text(encoding="utf-8"))
            self.assertEqual(failure["completion_status"], "incomplete_valid_results")
            self.assertEqual(failure["missing_valid_action_count"], 2)

    def test_three_consecutive_transport_failures_abort_early(self):
        class SchemaDeadServer(FakeCompactDiscoverServer):
            def __call__(self, url, tool, arguments, timeout_s=8.0):
                if arguments.get("mode") == "schema":
                    self.calls.append(arguments)
                    # Empty raw diagnostics must still count as transport failures.
                    return {"transport_error": True, "status": None, "raw": ""}
                return super().__call__(url, tool, arguments, timeout_s=timeout_s)

        fake = SchemaDeadServer({"blueprint": [f"a{index}" for index in range(1, 7)]})
        self._with_fake(fake)
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "run"
            rc = scb.cmd_scan(self._scan_args(out))
            self.assertEqual(rc, 1)
            self.assertEqual(
                len([call for call in fake.calls if call.get("mode") == "schema"]),
                3,
            )
            self.assertFalse((out / "summary.json").exists())
            failure = json.loads((out / "run_failure.json").read_text(encoding="utf-8"))
            self.assertEqual(failure["transport_gate_reason"], "consecutive_transport_failures")
            self.assertEqual(failure["completed_valid_action_count"], 0)
            self.assertEqual(failure["segment_attempted_action_count"], 3)

    def test_transport_fraction_gate_aborts_at_minimum_sample(self):
        class IntermittentServer(FakeCompactDiscoverServer):
            def __call__(self, url, tool, arguments, timeout_s=8.0):
                if arguments.get("mode") == "schema" and arguments.get("action") in {
                    "a1", "a6", "a11", "a16"
                }:
                    self.calls.append(arguments)
                    return {"transport_error": True, "status": None, "raw": "intermittent"}
                return super().__call__(url, tool, arguments, timeout_s=timeout_s)

        fake = IntermittentServer({"blueprint": [f"a{index}" for index in range(1, 22)]})
        self._with_fake(fake)
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "run"
            rc = scb.cmd_scan(self._scan_args(out, max_failed_fraction=1.0))
            self.assertEqual(rc, 1)
            failure = json.loads((out / "run_failure.json").read_text(encoding="utf-8"))
            self.assertEqual(failure["transport_gate_reason"], "transport_failed_fraction")
            self.assertEqual(failure["completed_valid_action_count"], 16)
            self.assertEqual(failure["segment_attempted_action_count"], 20)
            self.assertEqual(failure["transport_failure_count"], 4)

    def test_unexpected_runner_exception_aborts_instead_of_scoring_zero(self):
        class BrokenRunnerServer(FakeCompactDiscoverServer):
            def __call__(self, url, tool, arguments, timeout_s=8.0):
                if arguments.get("mode") == "schema":
                    self.calls.append(arguments)
                    raise ValueError("programming defect")
                return super().__call__(url, tool, arguments, timeout_s=timeout_s)

        fake = BrokenRunnerServer({"blueprint": ["a1", "a2"]})
        self._with_fake(fake)
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "run"
            rc = scb.cmd_scan(self._scan_args(out))
            self.assertEqual(rc, 1)
            self.assertFalse((out / "summary.json").exists())
            failure = json.loads((out / "run_failure.json").read_text(encoding="utf-8"))
            self.assertEqual(failure["completion_status"], "aborted_runner_exception")
            self.assertEqual(failure["completed_valid_action_count"], 0)
            self.assertEqual(failure["segment_attempted_action_count"], 1)
            self.assertIn("ValueError", failure["exception"])
            self.assertFalse(failure["metrics_valid"])
            partial = json.loads((out / "partial_summary.json").read_text(encoding="utf-8"))
            self.assertFalse(partial["metrics_valid"])

    def test_scoring_exception_aborts_and_preserves_triggering_action(self):
        fake = FakeCompactDiscoverServer({"blueprint": ["a1", "a2"]})
        self._with_fake(fake)
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "run"
            with mock.patch.object(
                scb,
                "score_schema_quality",
                side_effect=RuntimeError("score exploded"),
            ):
                rc = scb.cmd_scan(self._scan_args(out))
            self.assertEqual(rc, 1)
            failure = json.loads((out / "run_failure.json").read_text(encoding="utf-8"))
            rows = [
                json.loads(line)
                for line in (out / "per_action.jsonl").read_text(encoding="utf-8").splitlines()
            ]
            self.assertEqual(failure["completion_status"], "aborted_runner_exception")
            self.assertFalse(failure["metrics_valid"])
            self.assertEqual(rows, [])
            checkpoint = json.loads(
                (out / "scan_checkpoint.json").read_text(encoding="utf-8")
            )
            self.assertEqual(len(checkpoint["invalid_attempts"]), 1)
            self.assertEqual(
                checkpoint["invalid_attempts"][0]["failure_kind"],
                "runner_exception",
            )
            self.assertIn(
                "score exploded",
                checkpoint["invalid_attempts"][0]["error"],
            )
            self.assertFalse((out / "summary.json").exists())

    def test_malformed_success_row_aborts_checkpoint_contract(self):
        fake = FakeCompactDiscoverServer({"blueprint": ["a1"]})
        self._with_fake(fake)
        malformed_row = {
            "namespace": "blueprint",
            "action": "a1",
            # JSON numbers must not pass as booleans in reusable score rows.
            "param_types_declared": 1,
            "required_params_marked": True,
            "value_domain": True,
            "planning_signals_present": True,
            "skill_routing_present": True,
            "output_contract_declared": True,
            "schema_score": 1.0,
            "value_domain_diagnostics": [],
            "error": "",
            "failure_kind": "ok",
            "transport_error": False,
            "transport_status": None,
            "transport_error_raw": "",
        }
        outcome = scb.SchemaFetchOutcome(HEALTHY_SCHEMA, "ok")
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "run"
            with mock.patch.object(
                scb,
                "fetch_and_score_schema_target",
                return_value=(outcome, malformed_row),
            ):
                rc = scb.cmd_scan(self._scan_args(out))
            self.assertEqual(rc, 1)
            self.assertFalse((out / "summary.json").exists())
            failure = json.loads((out / "run_failure.json").read_text(encoding="utf-8"))
            self.assertEqual(
                failure["completion_status"],
                "aborted_invalid_result_contract",
            )
            checkpoint = json.loads(
                (out / "scan_checkpoint.json").read_text(encoding="utf-8")
            )
            self.assertEqual(
                checkpoint["invalid_attempts"][0]["failure_kind"],
                "invalid_result_contract",
            )

    def test_short_population_transport_fraction_is_rejected_at_finalize(self):
        class OneTransportServer(FakeCompactDiscoverServer):
            def __call__(self, url, tool, arguments, timeout_s=8.0):
                if arguments.get("mode") == "schema" and arguments.get("action") == "a1":
                    self.calls.append(arguments)
                    return {"transport_error": True, "status": 503, "raw": "down"}
                return super().__call__(url, tool, arguments, timeout_s=timeout_s)

        fake = OneTransportServer({"blueprint": [f"a{index}" for index in range(1, 11)]})
        self._with_fake(fake)
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "run"
            rc = scb.cmd_scan(self._scan_args(out, max_failed_fraction=1.0))
            self.assertEqual(rc, 1)
            failure = json.loads((out / "run_failure.json").read_text(encoding="utf-8"))
            self.assertEqual(
                failure["completion_status"],
                "completed_transport_failure_budget_exceeded",
            )
            self.assertEqual(
                failure["transport_gate_reason"],
                "final_transport_failed_fraction",
            )
            self.assertEqual(failure["last_action_id"], "blueprint.a1")
            self.assertFalse((out / "summary.json").exists())

    def test_transport_abort_resumes_only_missing_actions_and_completes(self):
        class FirstSegmentServer(FakeCompactDiscoverServer):
            def __call__(self, url, tool, arguments, timeout_s=8.0):
                if (
                    arguments.get("mode") == "schema"
                    and arguments.get("action") in {"a3", "a4", "a5"}
                ):
                    self.calls.append(arguments)
                    return {
                        "transport_error": True,
                        "status": 503,
                        "raw": "editor stopped",
                    }
                return super().__call__(url, tool, arguments, timeout_s=timeout_s)

        catalog = {"blueprint": ["a1", "a2", "a3", "a4", "a5"]}
        first = FirstSegmentServer(catalog)
        second = FakeCompactDiscoverServer(catalog)
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "run"
            with mock.patch.object(scb, "mcp_call", first):
                rc = scb.cmd_scan(self._scan_args(out))
            self.assertEqual(rc, 1)
            self.assertFalse((out / "summary.json").exists())

            checkpoint = json.loads(
                (out / "scan_checkpoint.json").read_text(encoding="utf-8")
            )
            self.assertEqual(checkpoint["completed_valid_action_count"], 2)
            self.assertEqual(len(checkpoint["segments"]), 1)
            self.assertEqual(len(checkpoint["outages"]), 3)

            resume_args = self._scan_args(out)
            resume_args.resume = True
            with mock.patch.object(scb, "mcp_call", second):
                rc = scb.cmd_scan(resume_args)
            self.assertEqual(rc, 0)

            resumed_schema_actions = [
                call.get("action")
                for call in second.calls
                if call.get("mode") == "schema"
            ]
            self.assertEqual(resumed_schema_actions, ["a3", "a4", "a5"])
            rows = [
                json.loads(line)
                for line in (out / "per_action.jsonl").read_text(encoding="utf-8").splitlines()
            ]
            self.assertEqual(
                [f"{row['namespace']}.{row['action']}" for row in rows],
                [f"blueprint.a{index}" for index in range(1, 6)],
            )
            summary = json.loads((out / "summary.json").read_text(encoding="utf-8"))
            self.assertTrue(summary["run_valid"])
            self.assertTrue(summary["comparable"])
            self.assertEqual(summary["completed_valid_action_count"], 5)
            self.assertEqual(summary["checkpoint_provenance"]["segment_count"], 2)
            self.assertEqual(summary["checkpoint_provenance"]["outage_count"], 3)
            self.assertFalse((out / "run_failure.json").exists())

    def test_resume_rejects_catalog_identity_change_before_schema_fetch(self):
        class FirstSegmentServer(FakeCompactDiscoverServer):
            def __call__(self, url, tool, arguments, timeout_s=8.0):
                if arguments.get("mode") == "schema":
                    self.calls.append(arguments)
                    return {
                        "transport_error": True,
                        "status": None,
                        "raw": "editor stopped",
                    }
                return super().__call__(url, tool, arguments, timeout_s=timeout_s)

        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "run"
            first = FirstSegmentServer({"blueprint": ["a1", "a2", "a3"]})
            with mock.patch.object(scb, "mcp_call", first):
                self.assertEqual(scb.cmd_scan(self._scan_args(out)), 1)

            changed = FakeCompactDiscoverServer(
                {"blueprint": ["a1", "a2", "a3", "a4"]},
                catalog_version="sha256:changed",
            )
            resume_args = self._scan_args(out)
            resume_args.resume = True
            with mock.patch.object(scb, "mcp_call", changed):
                rc = scb.cmd_scan(resume_args)
            self.assertEqual(rc, 1)
            self.assertFalse(
                any(call.get("mode") == "schema" for call in changed.calls)
            )
            failure = json.loads((out / "run_failure.json").read_text(encoding="utf-8"))
            self.assertEqual(failure["failure_stage"], "checkpoint_preflight")
            self.assertIn("identity mismatch", failure["error"])
            self.assertFalse((out / "summary.json").exists())

    def test_resume_rejects_duplicate_checkpoint_action_ids(self):
        class LastActionsDownServer(FakeCompactDiscoverServer):
            def __call__(self, url, tool, arguments, timeout_s=8.0):
                if (
                    arguments.get("mode") == "schema"
                    and arguments.get("action") != "a1"
                ):
                    self.calls.append(arguments)
                    return {
                        "transport_error": True,
                        "status": None,
                        "raw": "editor stopped",
                    }
                return super().__call__(url, tool, arguments, timeout_s=timeout_s)

        catalog = {"blueprint": ["a1", "a2", "a3", "a4"]}
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "run"
            with mock.patch.object(scb, "mcp_call", LastActionsDownServer(catalog)):
                self.assertEqual(scb.cmd_scan(self._scan_args(out)), 1)

            results_path = out / "per_action.jsonl"
            first_row = results_path.read_text(encoding="utf-8").splitlines()[0]
            with results_path.open("a", encoding="utf-8", newline="\n") as handle:
                handle.write(first_row + "\n")

            healthy = FakeCompactDiscoverServer(catalog)
            resume_args = self._scan_args(out)
            resume_args.resume = True
            with mock.patch.object(scb, "mcp_call", healthy):
                rc = scb.cmd_scan(resume_args)
            self.assertEqual(rc, 1)
            self.assertFalse(
                any(call.get("mode") == "schema" for call in healthy.calls)
            )
            failure = json.loads((out / "run_failure.json").read_text(encoding="utf-8"))
            self.assertIn("duplicate action id", failure["error"])

    def test_resume_rejects_runner_configuration_change(self):
        class FirstSegmentServer(FakeCompactDiscoverServer):
            def __call__(self, url, tool, arguments, timeout_s=8.0):
                if arguments.get("mode") == "schema":
                    self.calls.append(arguments)
                    return {
                        "transport_error": True,
                        "status": None,
                        "raw": "editor stopped",
                    }
                return super().__call__(url, tool, arguments, timeout_s=timeout_s)

        catalog = {"blueprint": ["a1", "a2", "a3"]}
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "run"
            with mock.patch.object(scb, "mcp_call", FirstSegmentServer(catalog)):
                self.assertEqual(scb.cmd_scan(self._scan_args(out)), 1)

            healthy = FakeCompactDiscoverServer(catalog)
            resume_args = self._scan_args(out)
            resume_args.resume = True
            resume_args.request_timeout_s = 2.0
            with mock.patch.object(scb, "mcp_call", healthy):
                rc = scb.cmd_scan(resume_args)
            self.assertEqual(rc, 1)
            self.assertFalse(
                any(call.get("mode") == "schema" for call in healthy.calls)
            )
            failure = json.loads((out / "run_failure.json").read_text(encoding="utf-8"))
            self.assertIn("request_timeout_s", failure["error"])

    def test_resume_records_result_append_that_preceded_checkpoint_count_flush(self):
        class LastActionsDownServer(FakeCompactDiscoverServer):
            def __call__(self, url, tool, arguments, timeout_s=8.0):
                if (
                    arguments.get("mode") == "schema"
                    and arguments.get("action") != "a1"
                ):
                    self.calls.append(arguments)
                    return {
                        "transport_error": True,
                        "status": None,
                        "raw": "editor stopped",
                    }
                return super().__call__(url, tool, arguments, timeout_s=timeout_s)

        catalog = {"blueprint": ["a1", "a2", "a3", "a4"]}
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "run"
            with mock.patch.object(scb, "mcp_call", LastActionsDownServer(catalog)):
                self.assertEqual(scb.cmd_scan(self._scan_args(out)), 1)

            checkpoint_path = out / "scan_checkpoint.json"
            checkpoint = json.loads(checkpoint_path.read_text(encoding="utf-8"))
            self.assertEqual(checkpoint["completed_valid_action_count"], 1)
            checkpoint["completed_valid_action_count"] = 0
            checkpoint_path.write_text(
                json.dumps(checkpoint, indent=2) + "\n",
                encoding="utf-8",
            )

            healthy = FakeCompactDiscoverServer(catalog)
            resume_args = self._scan_args(out)
            resume_args.resume = True
            with mock.patch.object(scb, "mcp_call", healthy):
                rc = scb.cmd_scan(resume_args)
            self.assertEqual(rc, 0)
            self.assertNotIn(
                "a1",
                [
                    call.get("action")
                    for call in healthy.calls
                    if call.get("mode") == "schema"
                ],
            )
            summary = json.loads((out / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(
                summary["checkpoint_provenance"]["recovery_event_count"],
                1,
            )

    def test_completion_identity_outage_resumes_without_refetching_valid_rows(self):
        class CompletionRecheckDownServer(FakeCompactDiscoverServer):
            def __init__(self, catalog):
                super().__init__(catalog)
                self.version_recheck_count = 0

            def __call__(self, url, tool, arguments, timeout_s=8.0):
                if not arguments.get("namespace") and arguments.get("if_version"):
                    self.version_recheck_count += 1
                    if self.version_recheck_count == 2:
                        self.calls.append(arguments)
                        return {
                            "transport_error": True,
                            "status": None,
                            "raw": "editor stopped after final schema",
                        }
                return super().__call__(url, tool, arguments, timeout_s=timeout_s)

        catalog = {"blueprint": ["a1", "a2"]}
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "run"
            first = CompletionRecheckDownServer(catalog)
            with mock.patch.object(scb, "mcp_call", first):
                rc = scb.cmd_scan(self._scan_args(out))
            self.assertEqual(rc, 1)
            self.assertFalse((out / "summary.json").exists())
            failure = json.loads((out / "run_failure.json").read_text(encoding="utf-8"))
            self.assertEqual(
                failure["completion_status"],
                "aborted_completion_identity_recheck",
            )
            self.assertEqual(failure["completed_valid_action_count"], 2)

            second = FakeCompactDiscoverServer(catalog)
            resume_args = self._scan_args(out)
            resume_args.resume = True
            with mock.patch.object(scb, "mcp_call", second):
                rc = scb.cmd_scan(resume_args)
            self.assertEqual(rc, 0)
            self.assertFalse(
                any(call.get("mode") == "schema" for call in second.calls)
            )
            summary = json.loads((out / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["checkpoint_provenance"]["segment_count"], 2)
            self.assertEqual(summary["checkpoint_provenance"]["outage_count"], 1)

    def test_publication_crash_window_is_republished_on_resume(self):
        catalog = {"blueprint": ["a1", "a2"]}
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "run"
            first = FakeCompactDiscoverServer(catalog)
            real_persist = scb.persist_scan_checkpoint

            def fail_final_state(output_dir, checkpoint, completed_count):
                if checkpoint.get("state") == "completed":
                    raise OSError("simulated crash before final checkpoint flip")
                return real_persist(output_dir, checkpoint, completed_count)

            with mock.patch.object(scb, "mcp_call", first), mock.patch.object(
                scb,
                "persist_scan_checkpoint",
                side_effect=fail_final_state,
            ):
                with self.assertRaisesRegex(OSError, "simulated crash"):
                    scb.cmd_scan(self._scan_args(out))

            checkpoint = json.loads(
                (out / "scan_checkpoint.json").read_text(encoding="utf-8")
            )
            self.assertEqual(checkpoint["state"], "publishing")
            self.assertTrue((out / "summary.json").is_file())

            second = FakeCompactDiscoverServer(catalog)
            resume_args = self._scan_args(out)
            resume_args.resume = True
            with mock.patch.object(scb, "mcp_call", second):
                rc = scb.cmd_scan(resume_args)
            self.assertEqual(rc, 0)
            self.assertFalse(
                any(call.get("mode") == "schema" for call in second.calls)
            )
            checkpoint = json.loads(
                (out / "scan_checkpoint.json").read_text(encoding="utf-8")
            )
            self.assertEqual(checkpoint["state"], "completed")
            self.assertTrue((out / "summary.json").is_file())

    def test_happy_path_scans_compact_catalog_with_exit_zero(self):
        fake = FakeCompactDiscoverServer({"blueprint": ["a1", "a2", "a3"]}, page_size=2)
        self._with_fake(fake)
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "run"
            rc = scb.cmd_scan(self._scan_args(out))
            self.assertEqual(rc, 0)
            summary = json.loads((out / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["scanned_action_count"], 3)
            self.assertEqual(summary["failed_action_count"], 0)
            self.assertGreater(summary["metrics"]["schema_completeness_score"], 0.9)

            completed_resume = FakeCompactDiscoverServer(
                {"blueprint": ["a1", "a2", "a3"]},
                page_size=2,
            )
            resume_args = self._scan_args(out)
            resume_args.resume = True
            with mock.patch.object(scb, "mcp_call", completed_resume):
                self.assertEqual(scb.cmd_scan(resume_args), 1)
            self.assertTrue((out / "summary.json").is_file())
            self.assertFalse((out / "run_failure.json").exists())
            self.assertFalse(
                any(
                    call.get("mode") == "schema"
                    for call in completed_resume.calls
                )
            )


class CheckpointQualityDerivationTests(unittest.TestCase):
    @staticmethod
    def _row() -> dict:
        raw_schema = scb.canonical_raw_schema({"params": {"Broken": {}}})
        return {
            "namespace": "blueprint",
            "action": "create_blueprint",
            "failure_kind": "ok",
            "error": "",
            "transport_error": False,
            "transport_status": None,
            "transport_error_raw": "",
            "raw_schema_hash_kind": scb.RAW_SCHEMA_HASH_KIND,
            "raw_schema": raw_schema,
            "raw_schema_sha256": scb._sha256_json(raw_schema),
            **scb.score_schema_quality(raw_schema),
        }

    def test_schema_score_must_be_derived_from_dimension_results(self) -> None:
        row = self._row()
        row["schema_score"] = 1.0
        with self.assertRaisesRegex(ValueError, "schema_score is not derived"):
            scb.validate_checkpoint_result_row(
                row,
                expected_action_ids={"blueprint.create_blueprint"},
            )

    def test_every_dimension_key_must_be_explicit_even_when_na_is_allowed(self) -> None:
        row = self._row()
        del row["param_types_declared"]
        with self.assertRaisesRegex(ValueError, "missing dimension param_types_declared"):
            scb.validate_checkpoint_result_row(
                row,
                expected_action_ids={"blueprint.create_blueprint"},
            )

    def test_consistent_quality_failure_row_is_reusable_but_not_a_quality_pass(self) -> None:
        row = self._row()
        action_id = scb.validate_checkpoint_result_row(
            row,
            expected_action_ids={"blueprint.create_blueprint"},
        )
        self.assertEqual(action_id, "blueprint.create_blueprint")
        self.assertFalse(scb.schema_quality_pass(row))

    def test_missing_raw_schema_is_not_reusable(self) -> None:
        row = self._row()
        del row["raw_schema"]
        with self.assertRaisesRegex(ValueError, "missing canonical raw_schema"):
            scb.validate_checkpoint_result_row(
                row,
                expected_action_ids={"blueprint.create_blueprint"},
            )

    def test_raw_schema_tamper_with_stale_sha_is_rejected(self) -> None:
        row = self._row()
        row["raw_schema"]["planning_signals"] = ["forged"]
        with self.assertRaisesRegex(ValueError, "does not match raw_schema"):
            scb.validate_checkpoint_result_row(
                row,
                expected_action_ids={"blueprint.create_blueprint"},
            )

    def test_rehashed_raw_schema_requires_exact_rescoring(self) -> None:
        row = self._row()
        row["raw_schema"]["planning_signals"] = ["now_present"]
        row["raw_schema_sha256"] = scb._sha256_json(row["raw_schema"])
        with self.assertRaisesRegex(
            ValueError,
            "planning_signals_present is not derived from raw_schema",
        ):
            scb.validate_checkpoint_result_row(
                row,
                expected_action_ids={"blueprint.create_blueprint"},
            )

    def test_forged_all_green_dimensions_cannot_override_broken_raw_schema(self) -> None:
        row = self._row()
        for field in scb.CHECKPOINT_DIMENSION_FIELDS:
            row[field] = True
        row["schema_score"] = 1.0
        row["value_domain_diagnostics"] = [{
            "param": "Broken",
            "ok": True,
            "reason": "forged_ok",
        }]
        with self.assertRaisesRegex(
            ValueError,
            "param_types_declared is not derived from raw_schema",
        ):
            scb.validate_checkpoint_result_row(
                row,
                expected_action_ids={"blueprint.create_blueprint"},
            )

    def test_canonical_schema_hash_preserves_json_scalar_types(self) -> None:
        bool_hash = scb._sha256_json({"value": True})
        int_hash = scb._sha256_json({"value": 1})
        float_hash = scb._sha256_json({"value": 1.0})
        self.assertEqual(len({bool_hash, int_hash, float_hash}), 3)

    def test_canonical_schema_hash_rejects_nonfinite_numbers(self) -> None:
        with self.assertRaisesRegex(ValueError, "non-finite"):
            scb._sha256_json({"value": float("nan")})


class InvalidParameterContractScoringTests(unittest.TestCase):
    @staticmethod
    def _schema(params) -> dict:
        return {
            "params": params,
            "planning_signals": ["call directly"],
            "skill": "unreal-blueprints",
            "output_contract_status": "declared",
        }

    def test_non_object_params_container_is_a_hard_quality_failure(self) -> None:
        quality = scb.score_schema_quality(self._schema(["bad"]))
        self.assertEqual(
            [
                quality["param_types_declared"],
                quality["required_params_marked"],
                quality["value_domain"],
            ],
            [False, False, False],
        )
        self.assertEqual(
            quality["value_domain_diagnostics"][0]["reason"],
            "params_must_be_object",
        )
        self.assertFalse(scb.schema_quality_pass(quality))

    def test_non_object_param_entry_is_not_filtered_into_na(self) -> None:
        quality = scb.score_schema_quality(self._schema({"Broken": "not-an-object"}))
        self.assertEqual(
            [
                quality["param_types_declared"],
                quality["required_params_marked"],
                quality["value_domain"],
            ],
            [False, False, False],
        )
        self.assertEqual(
            quality["value_domain_diagnostics"][0]["reason"],
            "param_schema_must_be_object",
        )

    def test_absent_or_empty_params_are_the_only_paramless_na_forms(self) -> None:
        absent = self._schema({})
        absent.pop("params")
        for schema in (absent, self._schema({})):
            quality = scb.score_schema_quality(schema)
            self.assertIsNone(quality["param_types_declared"])
            self.assertIsNone(quality["required_params_marked"])
            self.assertIsNone(quality["value_domain"])
            self.assertTrue(scb.schema_quality_pass(quality))

    def test_valid_param_object_remains_a_quality_pass(self) -> None:
        quality = scb.score_schema_quality(self._schema({
            "Name": {
                "type": "string",
                "description": "Stable asset name.",
                "required": True,
            },
        }))
        self.assertTrue(quality["param_types_declared"])
        self.assertTrue(quality["required_params_marked"])
        self.assertTrue(quality["value_domain"])
        self.assertTrue(scb.schema_quality_pass(quality))


if __name__ == "__main__":
    unittest.main(verbosity=2)
