#!/usr/bin/env python3
"""Fail-closed contracts shared by task-corpus benchmark runners."""

from __future__ import annotations

import json
import pathlib
import sys
import tempfile
import unittest

SCRIPTS_DIR = pathlib.Path(__file__).resolve().parent.parent
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

from benchmark_common import (  # noqa: E402
    TaskCorpusContractError,
    build_benchmark_inputs,
    load_task_corpus,
    status_identity,
    status_identity_mismatches,
    task_corpus_metadata,
    validate_mcp_status_response,
)


def task(task_id: str = "T-1", category: str = "probe") -> dict:
    return {
        "id": task_id,
        "category": category,
        "namespace": "source",
        "action": "read_file",
        "tool": "source_query",
        "arguments": {"action": "read_file", "path": "Source/Test.cpp"},
        "expected": {},
    }


class TaskCorpusContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        self.canonical_tasks = self.root / "Benchmarks" / "Suite" / "tasks.jsonl"
        self.manifest = self.canonical_tasks.parent / "manifest.json"
        self.canonical_tasks.parent.mkdir(parents=True)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def write_manifest(self, *, count: int = 1, categories: dict | None = None) -> None:
        self.manifest.write_text(
            json.dumps({"task_count": count, "category_counts": categories or {"probe": count}}),
            encoding="utf-8",
        )

    def load(self, path: pathlib.Path, *, allow_subset: bool = False):
        return load_task_corpus(
            path,
            suite="Suite",
            canonical_tasks_path=self.canonical_tasks,
            canonical_manifest_path=self.manifest,
            allow_subset=allow_subset,
            allowed_categories={"probe"},
            require_arguments=True,
            plugin_root=self.root,
        )

    def test_empty_canonical_corpus_is_rejected(self) -> None:
        self.canonical_tasks.write_text("", encoding="utf-8")
        self.write_manifest(count=0, categories={})
        with self.assertRaisesRegex(TaskCorpusContractError, "task corpus is empty"):
            self.load(self.canonical_tasks)

    def test_non_object_row_is_rejected_with_line_identity(self) -> None:
        self.canonical_tasks.write_text("[]\n", encoding="utf-8")
        self.write_manifest()
        with self.assertRaisesRegex(TaskCorpusContractError, r"tasks\.jsonl:1: task row"):
            self.load(self.canonical_tasks)

    def test_noncanonical_subset_requires_explicit_opt_in_and_is_noncomparable(self) -> None:
        subset = self.root / "subset.jsonl"
        subset.write_text(json.dumps(task()) + "\n", encoding="utf-8")
        with self.assertRaisesRegex(TaskCorpusContractError, "requires explicit --allow-subset"):
            self.load(subset)
        corpus = self.load(subset, allow_subset=True)
        self.assertEqual(
            task_corpus_metadata(corpus),
            {
                "mode": "explicit_subset",
                "canonical": False,
                "comparable": False,
                "validated_task_count": 1,
            },
        )

    def test_manifest_count_and_category_contract_is_enforced(self) -> None:
        self.canonical_tasks.write_text(json.dumps(task()) + "\n", encoding="utf-8")
        self.write_manifest(count=2, categories={"probe": 2})
        with self.assertRaisesRegex(TaskCorpusContractError, "parsed_task_count=1"):
            self.load(self.canonical_tasks)


class StatusBoundaryTests(unittest.TestCase):
    @staticmethod
    def result_payload(response):
        value = response.get("result") if isinstance(response, dict) else None
        return value if isinstance(value, dict) else {}

    @classmethod
    def result_data(cls, response):
        return dict(cls.result_payload(response).get("structuredContent") or {})

    def test_status_result_iserror_is_not_a_valid_running_endpoint(self) -> None:
        response = {
            "result": {
                "isError": True,
                "structuredContent": {"server_running": True},
            }
        }
        validation = validate_mcp_status_response(
            response,
            result_payload=self.result_payload,
            result_data=self.result_data,
        )
        self.assertFalse(validation["ok"])
        self.assertEqual(validation["failure_kind"], "server_error")

    def test_status_requires_the_local_project_identity(self) -> None:
        for project_name in (None, "AnotherProject"):
            with self.subTest(project_name=project_name):
                status = {"server_running": True}
                if project_name is not None:
                    status["project_name"] = project_name
                response = {
                    "result": {
                        "isError": False,
                        "structuredContent": status,
                    }
                }
                validation = validate_mcp_status_response(
                    response,
                    result_payload=self.result_payload,
                    result_data=self.result_data,
                )
                self.assertFalse(validation["ok"])
                self.assertEqual(validation["failure_kind"], "invalid_status_identity")

    def test_status_identity_detects_changed_and_disappeared_fields(self) -> None:
        start = status_identity(
            {"catalog_version": "sha256:v1", "editor_pid": 10, "project_name": "Speed"},
            endpoint="http://localhost:9316/mcp",
        )
        end = status_identity(
            {"catalog_version": "sha256:v2", "project_name": "Speed"},
            endpoint="http://localhost:9316/mcp",
        )
        mismatches = status_identity_mismatches(start, end)
        self.assertEqual(mismatches["catalog_version"], {"start": "sha256:v1", "end": "sha256:v2"})
        self.assertEqual(mismatches["process_id"], {"start": "10", "end": ""})


class BenchmarkInputFingerprintTests(unittest.TestCase):
    def test_exact_database_scope_excludes_unrelated_live_indexes(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            saved = root / "Saved"
            saved.mkdir()
            (saved / "EngineSource.db").write_bytes(b"source")
            (saved / "ProjectIndex.db").write_bytes(b"project")

            inputs = build_benchmark_inputs(
                "Synthetic",
                database_paths=("Saved/EngineSource.db",),
                plugin_root=root,
            )

        self.assertEqual(
            [row["path"] for row in inputs["database_files"]],
            ["Saved/EngineSource.db"],
        )

    def test_default_database_scope_is_suite_specific(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            saved = root / "Saved"
            saved.mkdir()
            (saved / "EngineSource.db").write_bytes(b"source")
            (saved / "ProjectIndex.db").write_bytes(b"project")

            guidance = build_benchmark_inputs("ActionGuidance", plugin_root=root)
            source = build_benchmark_inputs("SourceIndex", plugin_root=root)
            asset = build_benchmark_inputs("AssetEditing", plugin_root=root)
            (saved / "ProjectIndex.db").write_bytes(b"unrelated-project-churn")
            source_after_project_churn = build_benchmark_inputs("SourceIndex", plugin_root=root)

        self.assertEqual(guidance["database_files"], [])
        self.assertEqual(
            guidance["database_files_scope"],
            "not_applicable_to_registry_routing",
        )
        self.assertEqual(
            [row["path"] for row in source["database_files"]],
            ["Saved/EngineSource.db"],
        )
        self.assertEqual(
            source["fingerprint_sha256"],
            source_after_project_churn["fingerprint_sha256"],
        )
        self.assertEqual(
            [row["path"] for row in asset["database_files"]],
            ["Saved/ProjectIndex.db"],
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
