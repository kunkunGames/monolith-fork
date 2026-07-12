#!/usr/bin/env python3
"""Focused contract tests for immutable Query bundle publication."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("publish_query_bundle.py")
SPEC = importlib.util.spec_from_file_location("publish_query_bundle", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
PUBLISHER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PUBLISHER)

PLUGIN_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_ROOT = PLUGIN_ROOT / "Scripts"
sys.path.insert(0, str(SCRIPTS_ROOT))
try:
    from source_generation_hash import (
        canonicalize_text_source,
        compute_source_generation_hash,
    )
finally:
    sys.path.remove(str(SCRIPTS_ROOT))


SOURCE_HASH = "0123456789abcdef"
VERSION = {
    "tool": "monolith_query",
    "runtime": "native-cpp",
    "plugin_version": "0.20.3",
    "parity_spec_rev": "2026-05-29.1",
    "source_hash": SOURCE_HASH,
}


def make_catalog(
    generated_at: str = "2026-07-11T00:00:00Z",
    proof_anchors: dict[str, object] | None = None,
) -> bytes:
    action = {
        "namespace": "source",
        "action": "health",
        "full_name": "source.health",
        "source_line": 42,
        "summary": "Read source index health.",
    }
    catalog: dict[str, object] = {
        "schema_version": 1,
        "generated_at": generated_at,
        "source_hash_kind": "action_semantics_v1",
        "action_count": 1,
        "actions": [action],
        "proof_anchors": proof_anchors or {},
    }
    catalog["source_hash"] = PUBLISHER._catalog_semantic_hash(catalog)
    return (json.dumps(catalog, sort_keys=True, indent=2) + "\n").encode("utf-8")


class QueryBundlePublicationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.binaries = self.root / "Binaries"
        self.built = self.root / "monolith_query.build.exe"
        self.catalog = self.root / "catalog.json"
        self.built.write_bytes(b"query-executable-v1")
        self.catalog.write_bytes(make_catalog())
        self.version_patch = mock.patch.object(
            PUBLISHER, "_run_bounded_version", return_value=dict(VERSION)
        )
        self.version_patch.start()

    def tearDown(self) -> None:
        self.version_patch.stop()
        self.temp.cleanup()

    def publish(self) -> dict[str, object]:
        return PUBLISHER.publish_bundle(
            self.built,
            self.catalog,
            self.binaries,
            SOURCE_HASH,
        )

    def test_publish_writes_exact_manifest_and_immutable_pair(self) -> None:
        result = self.publish()
        manifest = result["manifest"]
        self.assertEqual(set(manifest), set(PUBLISHER.MANIFEST_FIELDS))
        self.assertEqual(manifest["file"], f"monolith_query-{SOURCE_HASH}.exe")
        self.assertEqual(
            manifest["catalog_file"],
            f"monolith_catalog-{manifest['catalog_source_hash']}.json",
        )
        self.assertEqual(manifest["runtime"], "native-cpp")
        self.assertEqual(
            (self.binaries / str(manifest["file"])).read_bytes(),
            self.built.read_bytes(),
        )
        self.assertEqual(
            (self.binaries / str(manifest["catalog_file"])).read_bytes(),
            self.catalog.read_bytes(),
        )
        self.assertEqual(
            (self.binaries / "monolith_query.exe").read_bytes(),
            self.built.read_bytes(),
        )
        self.assertNotEqual(
            (self.binaries / "monolith_query.exe").stat().st_ino,
            (self.binaries / str(manifest["file"])).stat().st_ino,
            "the mutable compatibility name must not hard-link the immutable image",
        )

    def test_validate_rejects_catalog_sha_tamper(self) -> None:
        result = self.publish()
        catalog_path = Path(result["catalog"])
        catalog_path.write_bytes(catalog_path.read_bytes() + b" ")
        with self.assertRaisesRegex(PUBLISHER.BundleError, "catalog SHA-256"):
            PUBLISHER.validate_bundle(self.binaries, SOURCE_HASH)

    def test_published_artifact_sha_uses_exact_raw_bytes(self) -> None:
        artifact = b"query-artifact\r\nwith-checkout-like-bytes\r"
        self.built.write_bytes(artifact)
        manifest = self.publish()["manifest"]
        raw_sha = hashlib.sha256(artifact).hexdigest()
        canonical_sha = hashlib.sha256(canonicalize_text_source(artifact)).hexdigest()
        self.assertNotEqual(raw_sha, canonical_sha)
        self.assertEqual(manifest["sha256"], raw_sha)

    def test_validate_rejects_unexpected_manifest_field(self) -> None:
        self.publish()
        manifest_path = self.binaries / PUBLISHER.MANIFEST_NAME
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["unexpected"] = True
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(PUBLISHER.BundleError, "exactly"):
            PUBLISHER.validate_bundle(self.binaries, SOURCE_HASH)

    def test_strict_json_rejects_duplicate_and_non_finite_values(self) -> None:
        with self.assertRaisesRegex(PUBLISHER.BundleError, "duplicate JSON key"):
            PUBLISHER._load_strict_json_bytes(b'{"x":1,"x":2}', "fixture")
        with self.assertRaisesRegex(PUBLISHER.BundleError, "non-finite"):
            PUBLISHER._load_strict_json_bytes(b'{"x":NaN}', "fixture")
        catalog = {
            "action_count": 1,
            "actions": [{"namespace": "source", "action": "health", "score": float("nan")}],
        }
        with self.assertRaisesRegex(PUBLISHER.BundleError, "strict JSON"):
            PUBLISHER._catalog_semantic_hash(catalog)

    def test_nonsemantic_catalog_churn_reuses_immutable_bytes(self) -> None:
        first = self.publish()
        first_catalog = Path(first["catalog"])
        original_bytes = first_catalog.read_bytes()
        self.catalog.write_bytes(make_catalog("2026-07-12T00:00:00Z"))
        second = self.publish()
        self.assertEqual(first_catalog.read_bytes(), original_bytes)
        self.assertEqual(second["manifest"]["catalog_sha256"], first["manifest"]["catalog_sha256"])

    def test_semantic_catalog_collision_never_overwrites(self) -> None:
        first = self.publish()
        first_catalog = Path(first["catalog"])
        original_bytes = first_catalog.read_bytes()
        self.catalog.write_bytes(make_catalog(proof_anchors={"changed": True}))
        with self.assertRaisesRegex(PUBLISHER.BundleError, "semantic contract differs"):
            self.publish()
        self.assertEqual(first_catalog.read_bytes(), original_bytes)

    def test_publish_rejects_version_identity_drift(self) -> None:
        with mock.patch.object(
            PUBLISHER,
            "_run_bounded_version",
            return_value={**VERSION, "source_hash": "fedcba9876543210"},
        ):
            with self.assertRaisesRegex(PUBLISHER.BundleError, "does not match build.bat"):
                self.publish()


class SourceGenerationHashTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def source_hash(self, content: bytes) -> str:
        path = self.root / "fixture.cpp"
        path.write_bytes(content)
        return compute_source_generation_hash(b"fixture-build-contract", [path])

    def test_lf_crlf_and_lone_cr_have_one_generation(self) -> None:
        lf_hash = self.source_hash(b"alpha\nbeta\ngamma\n")
        crlf_hash = self.source_hash(b"alpha\r\nbeta\r\ngamma\r\n")
        lone_cr_hash = self.source_hash(b"alpha\rbeta\rgamma\r")
        self.assertEqual(crlf_hash, lf_hash)
        self.assertEqual(lone_cr_hash, lf_hash)

    def test_text_content_change_changes_generation(self) -> None:
        original = self.source_hash(b"alpha\nbeta\ngamma\n")
        changed = self.source_hash(b"alpha\nbeta\ndelta\n")
        self.assertNotEqual(changed, original)


if __name__ == "__main__":
    unittest.main()
