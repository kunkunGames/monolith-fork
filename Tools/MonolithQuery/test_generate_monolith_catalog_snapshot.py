#!/usr/bin/env python3
"""Regression tests for the offline catalog generator."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("generate_monolith_catalog_snapshot.py")
SPEC = importlib.util.spec_from_file_location("monolith_catalog_generator", MODULE_PATH)
assert SPEC and SPEC.loader
GENERATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GENERATOR)


class CatalogGeneratorTests(unittest.TestCase):
    def test_test_source_registrations_are_excluded_from_runtime_catalog(self) -> None:
        with tempfile.TemporaryDirectory(prefix="monolith-catalog-tests-boundary-") as temp:
            root = Path(temp)
            production = root / "Source" / "Fixture" / "Private" / "FixtureActions.cpp"
            test_source = (
                root
                / "Source"
                / "Fixture"
                / "Private"
                / "Tests"
                / "FixtureSyntheticActions.cpp"
            )
            production.parent.mkdir(parents=True)
            test_source.parent.mkdir(parents=True)
            production.write_text(
                """
Registry.RegisterAction(
    TEXT("fixture"),
    TEXT("production_action"),
    TEXT("Runtime action"),
    FMonolithActionHandler());
""",
                encoding="utf-8",
            )
            test_source.write_text(
                """
Registry.RegisterAction(
    TEXT("fixture_test"),
    TEXT("synthetic_action"),
    TEXT("Automation-only action"),
    FMonolithActionHandler());
""",
                encoding="utf-8",
            )

            actions, scanned_files = GENERATOR.collect_actions(root)

            self.assertEqual(["fixture.production_action"], [row["full_name"] for row in actions])
            self.assertEqual([production], scanned_files)
            self.assertNotIn("/Tests/", actions[0]["source_file"].replace("\\", "/"))

    def test_check_ignores_line_shift_but_detects_action_contract_drift(self) -> None:
        with tempfile.TemporaryDirectory(prefix="monolith-catalog-check-") as temp:
            root = Path(temp)
            source = root / "Source" / "Fixture.cpp"
            source.parent.mkdir(parents=True)
            registration = """
void RegisterFixture(FMonolithToolRegistry& Registry)
{
    Registry.RegisterAction(
        TEXT("fixture"),
        TEXT("ping"),
        TEXT("Read-only fixture action"),
        FMonolithActionHandler());
}
"""
            source.write_text(registration, encoding="utf-8")
            output = root / "snapshot.json"

            generated = subprocess.run(
                [
                    sys.executable,
                    str(MODULE_PATH),
                    "--root",
                    str(root),
                    "--out",
                    str(output),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)

            source.write_text(
                "// line-only implementation comment\n" + registration,
                encoding="utf-8",
            )
            line_only_check = subprocess.run(
                [
                    sys.executable,
                    str(MODULE_PATH),
                    "--root",
                    str(root),
                    "--out",
                    str(output),
                    "--check",
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(0, line_only_check.returncode, line_only_check.stdout)

            source.write_text(
                registration.replace('TEXT("ping")', 'TEXT("pong")'),
                encoding="utf-8",
            )
            contract_check = subprocess.run(
                [
                    sys.executable,
                    str(MODULE_PATH),
                    "--root",
                    str(root),
                    "--out",
                    str(output),
                    "--check",
                ],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(0, contract_check.returncode)
            self.assertIn("catalog snapshot drift", contract_check.stdout)

    def test_template_registration_is_extracted_with_semantic_hash(self) -> None:
        with tempfile.TemporaryDirectory(prefix="monolith-catalog-test-") as temp:
            root = Path(temp)
            action_dir = root / "Source" / "MonolithIndex" / "Private" / "Actions"
            action_dir.mkdir(parents=True)

            (action_dir / "ProjectExportAssetTextAction.h").write_text(
                """
class FProjectExportAssetTextAction
{
public:
    static FString GetName() { return TEXT("export_asset_text"); }
};
""",
                encoding="utf-8",
            )
            (action_dir / "ProjectExportAssetTextAction.cpp").write_text(
                """
FString FProjectExportAssetTextAction::GetDescription()
{
    return TEXT("Read an asset's native text "
        "without mutating it.");
}
""",
                encoding="utf-8",
            )
            registration = action_dir / "ProjectActionRegistration.cpp"
            registration.write_text(
                """
template <typename TAction>
void RegisterProjectAction(FMonolithToolRegistry& Registry)
{
    Registry.RegisterAction(
        TEXT("project"),
        TAction::GetName(),
        TAction::GetDescription(),
        FMonolithActionHandler::CreateStatic(&TAction::Execute),
        TAction::GetSchema());
}

void RegisterAll(FMonolithToolRegistry& Registry)
{
    RegisterProjectAction<FProjectExportAssetTextAction>(Registry);
}
""",
                encoding="utf-8",
            )

            actions, _ = GENERATOR.collect_actions(root)
            rows = {row["full_name"]: row for row in actions}
            self.assertIn("project.export_asset_text", rows)
            export = rows["project.export_asset_text"]
            self.assertFalse(export["available_offline"])
            self.assertTrue(export["requires_live_editor"])
            self.assertEqual("source_scanned_candidate", export["implementation_status"])
            self.assertFalse(export["mutates_assets"])
            self.assertIn("without mutating", export["summary"])

            cleanup = GENERATOR.action_metadata(
                "project",
                "cleanup_generated_assets",
                "Delete generated test assets after reference checks.",
                "Source/Fixture.cpp",
                1,
            )
            self.assertTrue(cleanup["mutates_assets"])

            tag_search = GENERATOR.action_metadata(
                "project",
                "search_gameplay_tags",
                "Search indexed gameplay tags.",
                "Source/Fixture.cpp",
                1,
            )
            self.assertTrue(tag_search["available_offline"])
            self.assertFalse(tag_search["requires_live_editor"])
            self.assertEqual("offline_query_implemented", tag_search["implementation_status"])

            first_hash = GENERATOR.action_semantic_hash(actions)
            registration.write_text(
                "// unrelated implementation-only comment\n"
                + registration.read_text(encoding="utf-8"),
                encoding="utf-8",
            )
            shifted_actions, _ = GENERATOR.collect_actions(root)
            self.assertEqual(
                first_hash,
                GENERATOR.action_semantic_hash(shifted_actions),
            )

    def test_inline_description_and_line_only_drift_are_semantically_stable(self) -> None:
        inline_body = """
class FProjectSearchAction
{
public:
    static FString GetName() { return TEXT("search"); }
    static FString GetDescription()
    {
        return TEXT("Search indexed assets "
            "without mutation.");
    }
};
"""
        description_match = GENERATOR.INLINE_DESCRIPTION_RE.search(inline_body)
        self.assertIsNotNone(description_match)
        self.assertEqual(
            "Search indexed assets without mutation.",
            GENERATOR.extract_return_text(description_match.group(1)),
        )

        action = {
            "namespace": "project",
            "action": "search",
            "full_name": "project.search",
            "summary": "Search indexed assets without mutation.",
            "source_line": 10,
        }
        first = {
            "schema_version": 1,
            "source": "cpp_registry_scan",
            "source_root": "Source",
            "source_hash": GENERATOR.action_semantic_hash([action]),
            "source_hash_kind": "action_semantics_v1",
            "action_count": 1,
            "actions": [action],
            "proof_anchors": {},
        }
        shifted = {**first, "actions": [{**action, "source_line": 99}]}
        self.assertEqual(
            GENERATOR.catalog_contract(first),
            GENERATOR.catalog_contract(shifted),
        )

        changed = {**first, "actions": [{**action, "summary": "Changed"}]}
        self.assertNotEqual(
            GENERATOR.catalog_contract(first),
            GENERATOR.catalog_contract(changed),
        )


if __name__ == "__main__":
    unittest.main()
