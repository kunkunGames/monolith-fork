#!/usr/bin/env python3
"""Generate the offline Monolith action catalog snapshot from C++ registrations."""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import json
import re
from pathlib import Path


REGISTER_RE = re.compile(
    r"Registry\.RegisterAction\s*\(\s*TEXT\(\"([^\"]+)\"\)\s*,\s*TEXT\(\"([^\"]+)\"\)",
    re.S,
)
TEMPLATE_HELPER_RE = re.compile(
    r"(?:static\s+)?(?:void|bool)\s+(Register[A-Za-z0-9_]*Action)\s*\([^)]*\)\s*\{"
    r"(?:(?!\n\}).)*?Registry\.RegisterAction\s*\(\s*TEXT\(\"([^\"]+)\"\)\s*,"
    r"\s*TAction::GetName\s*\(\s*\)",
    re.S,
)
TEMPLATE_CALL_RE = re.compile(
    r"\b(Register[A-Za-z0-9_]*Action)\s*<\s*([A-Za-z_][A-Za-z0-9_]*)\s*>\s*\(",
)
GET_NAME_RE = re.compile(
    r"static\s+FString\s+GetName\s*\(\s*\)\s*\{\s*return\s+TEXT\(\"([^\"]+)\"\)",
    re.S,
)
DESCRIPTION_RE = re.compile(
    r"\b([A-Za-z_][A-Za-z0-9_]*)::GetDescription\s*\(\s*\)\s*\{(.*?)\r?\n[ \t]*\}",
    re.S,
)
INLINE_DESCRIPTION_RE = re.compile(
    r"static\s+FString\s+GetDescription\s*\(\s*\)\s*\{(.*?)\}",
    re.S,
)
RETURN_TEXT_RE = re.compile(
    r"return\s+TEXT\s*\(\s*((?:\"(?:\\.|[^\"\\])*\"\s*)+)\)\s*;",
    re.S,
)
CPP_STRING_RE = re.compile(r'\"((?:\\.|[^\"\\])*)\"', re.S)
TEXT_RE = re.compile(r"TEXT\(\"((?:\\.|[^\"])*)\"\)", re.S)

READ_PREFIXES = (
    "get_",
    "list_",
    "find_",
    "search_",
    "analyze_",
    "compare_",
    "estimate_",
    "preview_",
    "validate_",
    "suggest_",
    "describe_",
    "inspect_",
    "report_",
    "check_",
)

WRITE_PREFIXES = (
    "add_",
    "apply_",
    "auto_",
    "bake_",
    "batch_",
    "build_",
    "clear_",
    "commit_",
    "compile_",
    "configure_",
    "create_",
    "delete_",
    "discard_",
    "duplicate_",
    "edit_",
    "export_",
    "fix_",
    "generate_",
    "import_",
    "load_",
    "merge_",
    "move_",
    "pack_",
    "rebuild_",
    "remove_",
    "rename_",
    "repair_",
    "run_",
    "save_",
    "set_",
    "spawn_",
    "start_",
    "stop_",
    "toggle_",
    "unload_",
    "update_",
)

LONG_RUNNING_TOKENS = (
    "batch",
    "build",
    "compile",
    "export",
    "generate",
    "import",
    "index",
    "package",
    "rebuild",
    "scan",
)

PROGRESS_ACTIONS = {
    ("ai", "rebuild_zone_graph"),
    ("monolith", "reindex"),
}

OFFLINE_AVAILABLE_ACTIONS = {
    "source": {
        "search_source",
        "read_source",
        "find_references",
        "find_callers",
        "find_callees",
        "get_class_hierarchy",
        "get_module_info",
        "get_symbol_context",
        "read_file",
        "impact_radius",
        "find_overrides",
        "health",
        "repair_fts",
        "repair_crg_cache",
        "search_crg_graph",
        "risk_score",
        "review_hotspots",
        "review_context",
        "detect_changes",
        "find_unused",
        "pre_merge_check",
        "snapshot",
        "diff_snapshots",
        "get_include_path",
        "get_signature",
        "check_deprecations",
        "verify_symbols",
        "find_example_usage",
        "lint_header",
        "generate_class_stub",
    },
    "project": {
        "search",
        "find_by_type",
        "find_references",
        "get_stats",
        "list_gameplay_tags",
        "search_gameplay_tags",
        "get_asset_details",
        "impact_radius",
        "health",
        "repair_fts",
        "repair_crg_cache",
        "risk_score",
        "review_hotspots",
        "review_context",
        "detect_changes",
        "find_unused",
        "pre_merge_check",
        "snapshot",
        "diff_snapshots",
    },
    "bridge": {"search_asset_symbols"},
    "console": {"search_objects", "get_object", "health"},
    "monolith": {"guide", "status", "discover", "find", "get_action_metadata_coverage"},
}

NAMESPACE_SKILLS = {
    "console": "unreal-console",
    "workflow": "monolith-mcp",
}


WORKFLOW_METADATA = {
    "game_ready_asset_static_mesh": {
        "proof_anchor": "game_ready_asset.static_mesh_workflow",
        "skill": "unreal-asset",
        "workflow_contract": "game_ready_asset",
        "schema_signal": "schema:required mesh_asset_path string",
        "preconditions": [
            "mesh_asset_path must identify a StaticMesh asset.",
            "material_asset_path is optional and drives material diagnostics when supplied.",
            "dry_run=true is read-only and must not dirty packages.",
        ],
        "outputs": [
            "status:string",
            "workflow_id:string",
            "plan.steps[]",
            "actions[]",
            "touched.assets[]",
            "dirty_packages[]",
            "source_control:{prepared,status,blocked[]}",
            "validation:{compile,asset_validation,budget}",
            "proof:{read_back,preview_artifacts,logs,benchmarks}",
            "warnings[]",
            "errors[]",
            "next_actions[]",
        ],
    },
    "gameplay_feature_manifest": {
        "proof_anchor": "gameplay_feature.manifest_workflow",
        "skill": "unreal-gas",
        "workflow_contract": "gameplay_feature",
        "schema_signal": "schema:required feature_id string manifest object",
        "preconditions": [
            "feature_id must identify the gameplay feature.",
            "manifest must group input, GAS, Blueprint, AI, GameFeatures, WorldConditions, and runtime proof sections.",
            "dry_run=true is read-only and must not dirty packages.",
            "runtime proof is declared but blocked until a later confirmed PIE workflow slice.",
        ],
        "outputs": [
            "workflow_id:gameplay_feature",
            "workflow_slice:manifest_read_only_preflight_v1",
            "validation:{input,gas,blueprint,ai,gamefeatures,world_conditions,runtime}",
            "touched.assets[]",
            "dirty_packages[]",
            "proof.read_back[]",
            "next_actions[]",
        ],
    },
    "level_world_builder_blockout": {
        "proof_anchor": "level_workflow.blockout_volume",
        "skill": "unreal-worldgen",
        "workflow_contract": "level_workflow",
        "schema_signal": "schema:required map_path volume seed",
        "mutates_assets": True,
        "preconditions": [
            "map_path must be a new /Game map package path.",
            "volume must include name, location, extent, and room_type.",
            "seed must be non-zero.",
            "dry_run=false requires confirm=true before mutation.",
            "primitives are capped at 200 before mutation.",
        ],
        "outputs": [
            "workflow_id:level_workflow",
            "workflow_slice:blockout_volume_v1",
            "validation:{world_context,scene_statistics,blockout,leveldesign,save}",
            "touched:{actors,assets,packages,files}",
            "dirty_packages[]",
            "source_control{}",
            "proof.read_back[]",
            "next_actions[]",
        ],
    },
    "ui_shipping_widget_blueprint": {
        "proof_anchor": "ui_shipping.widget_blueprint_workflow",
        "skill": "unreal-ui",
        "workflow_contract": "ui_shipping",
        "schema_signal": "schema:required widget_asset_path string",
        "preconditions": [
            "widget_asset_path must identify a Widget Blueprint asset.",
            "dry_run=true is read-only and must not dirty packages.",
            "compile, preview, save, and source-control prepare are explicit next actions.",
        ],
        "outputs": [
            "workflow_id:ui_shipping",
            "workflow_slice:widget_blueprint_readiness_proof_v1",
            "validation:{compile,asset_validation,accessibility,ui}",
            "proof:{read_back,preview_artifacts,logs,benchmarks}",
            "dirty_packages[]",
            "source_control{}",
            "next_actions[]",
        ],
    },
    "shot_render_level_sequence": {
        "proof_anchor": "shot_render.level_sequence_mrq_workflow",
        "skill": "unreal-level-sequences",
        "workflow_contract": "shot_render",
        "schema_signal": "schema:required sequence_asset_path string",
        "preconditions": [
            "sequence_asset_path must identify a Level Sequence asset.",
            "Movie Render Queue render launch requires an explicit follow-up action with confirm=true.",
            "dry_run=true is read-only and must not dirty packages.",
        ],
        "outputs": [
            "workflow_id:shot_render",
            "workflow_slice:level_sequence_mrq_readiness_proof_v1",
            "validation:{asset_validation,render,runtime}",
            "proof:{read_back,preview_artifacts,logs,benchmarks}",
            "artifacts[]",
            "dirty_packages[]",
            "source_control{}",
            "next_actions[]",
        ],
    },
    "audio_shipping_asset": {
        "proof_anchor": "audio_shipping.asset_workflow",
        "skill": "unreal-audio",
        "workflow_contract": "audio_shipping",
        "schema_signal": "schema:required audio_asset_path string",
        "preconditions": [
            "audio_asset_path must identify a SoundWave, SoundCue, MetaSoundSource, or other SoundBase-derived asset.",
            "asset_kind=auto avoids guessed type-specific validators; pass SoundWave, SoundCue, or MetaSoundSource for targeted proof.",
            "dry_run=true is read-only and must not dirty packages.",
        ],
        "outputs": [
            "workflow_id:audio_shipping",
            "workflow_slice:audio_asset_readiness_proof_v1",
            "validation:{asset_validation,runtime,budget}",
            "proof:{read_back,preview_artifacts,logs,benchmarks}",
            "dirty_packages[]",
            "source_control{}",
            "next_actions[]",
        ],
    },
    "localization_shipping_string_table": {
        "proof_anchor": "localization_shipping.string_table_workflow",
        "skill": "unreal-localization",
        "workflow_contract": "localization_shipping",
        "schema_signal": "schema:required string_table_path string",
        "preconditions": [
            "string_table_path must identify a StringTable asset under /Game.",
            "CSV import/export and entry mutation require explicit localization actions with dry_run or confirm.",
            "dry_run=true is read-only and must not dirty packages.",
        ],
        "outputs": [
            "workflow_id:localization_shipping",
            "workflow_slice:string_table_readiness_proof_v1",
            "validation:{asset_validation,localization}",
            "proof:{read_back,preview_artifacts,logs,benchmarks}",
            "artifacts[]",
            "dirty_packages[]",
            "source_control{}",
            "next_actions[]",
        ],
    },
    "slate_euw_test_flow": {
        "proof_anchor": "slate_euw.test_flow_workflow",
        "skill": "unreal-slate",
        "workflow_contract": "slate_euw_test_flow",
        "schema_signal": "schema:required target string",
        "preconditions": [
            "target must identify a Slate widget, window, text, path, or Editor Utility Widget surface.",
            "click/type/key simulation is unavailable until a test-mode gated Slate input action exists.",
            "dry_run=true is read-only and must not send input or write capture artifacts.",
        ],
        "outputs": [
            "workflow_id:slate_euw_test_flow",
            "workflow_slice:slate_euw_readiness_proof_v1",
            "validation:{ui,runtime,interaction}",
            "proof:{read_back,preview_artifacts,logs,benchmarks}",
            "artifacts[]",
            "dirty_packages[]",
            "next_actions[]",
        ],
    },
}


def relpath(path: Path, root: Path) -> str:
    return path.resolve().relative_to(root.resolve()).as_posix()


def action_semantic_view(action: dict) -> dict:
    """Return the stable action contract, excluding line-number provenance."""

    return {
        key: value
        for key, value in action.items()
        if key != "source_line"
    }


def action_semantic_hash(actions: list[dict]) -> str:
    """Hash extracted registry semantics instead of unrelated source bytes."""

    digest = hashlib.sha256()
    for action in sorted(actions, key=lambda row: (row["namespace"], row["action"])):
        semantic = action_semantic_view(action)
        digest.update(
            json.dumps(
                semantic,
                ensure_ascii=False,
                sort_keys=True,
                separators=(",", ":"),
            ).encode("utf-8")
        )
        digest.update(b"\0")
    return digest.hexdigest()


def catalog_contract(snapshot: dict) -> dict:
    """Normalize a snapshot for drift checks without line-only churn."""

    contract_keys = (
        "schema_version",
        "source",
        "source_root",
        "source_hash",
        "source_hash_kind",
        "action_count",
        "proof_anchors",
    )
    contract = {key: snapshot.get(key) for key in contract_keys}
    contract["actions"] = [
        action_semantic_view(action)
        for action in snapshot.get("actions", [])
    ]
    return contract


def write_catalog_if_changed(path: Path, snapshot: dict) -> bool:
    """Publish platform-native text only when the semantic catalog changed."""

    if path.is_file():
        try:
            existing = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            pass
        else:
            if catalog_contract(existing) == catalog_contract(snapshot):
                return False

    content = json.dumps(snapshot, indent=2, sort_keys=True) + "\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return True


def clean_text(value: str) -> str:
    value = value.replace('\\"', '"')
    value = value.replace("\\n", " ")
    return " ".join(value.split())


def extract_return_text(body: str) -> str:
    """Extract adjacent C++ string literals from a return TEXT(...) body."""

    match = RETURN_TEXT_RE.search(body)
    if not match:
        return ""
    return clean_text("".join(CPP_STRING_RE.findall(match.group(1))))


def extract_summary(text: str, match_end: int) -> str:
    snippet = text[match_end : match_end + 1200]
    handler_pos = snippet.find("FMonolithActionHandler")
    if handler_pos != -1:
        snippet = snippet[:handler_pos]
    matches = TEXT_RE.findall(snippet)
    if not matches:
        return ""
    return clean_text(matches[0])


def mutates_assets(action: str) -> bool:
    if action.startswith(READ_PREFIXES):
        return False
    if action in {"health", "status", "guide"}:
        return False
    return action.startswith(WRITE_PREFIXES)


ACTION_FIELD_OVERRIDES = {
    ("source", "generate_class_stub"): {
        "mutates_assets": False,
        "long_running": False,
    },
    ("project", "export_asset_text"): {
        "mutates_assets": False,
        "long_running": False,
    },
    ("project", "cleanup_generated_assets"): {
        "mutates_assets": True,
    },
}


def action_metadata(namespace: str, action: str, summary: str, source_file: str, line: int) -> dict:
    mutating = mutates_assets(action)
    long_running = any(token in action for token in LONG_RUNNING_TOKENS)
    overrides = ACTION_FIELD_OVERRIDES.get((namespace, action), {})
    mutating = overrides.get("mutates_assets", mutating)
    long_running = overrides.get("long_running", long_running)
    supports_progress = (namespace, action) in PROGRESS_ACTIONS
    proof_anchor = ""
    implementation_status = "source_scanned_candidate"
    skill = NAMESPACE_SKILLS.get(namespace, namespace)
    preconditions_status = "snapshot_only"
    output_contract_status = "snapshot_only"
    next_actions_status = "availability_required"
    preconditions: list[str] = []
    outputs: list[str] = []
    planning_signals = [f"skill:{skill}", namespace, "source_scanned_candidate", "read_only" if not mutating else "mutating"]
    if namespace == "mesh" and action == "validate_game_ready":
        proof_anchor = "game_ready_asset.static_mesh_readiness"
        implementation_status = "source_scanned_proof_anchor"
        skill = "unreal-mesh"
        preconditions_status = "declared_or_derived"
        output_contract_status = "declared"
        next_actions_status = "not_declared"
        mutating = False
        preconditions = [
            "asset_path must identify a loadable UStaticMesh.",
            "The UStaticMesh must have render data and LOD0.",
        ]
        outputs = [
            "asset_path:string",
            "checks[]:{name,result,severity,message,degenerate_count?}",
            "game_ready:boolean",
            "critical_failures:number",
            "high_failures:number",
            "total_checks:number",
        ]
        planning_signals = [
            "skill:unreal-mesh",
            "mcp_tool:mesh_query",
            "schema:required asset_path string",
            "execution_policy:read_only can_mutate=false",
            "search_metadata:declared",
            "proof_anchor:game_ready_asset.static_mesh_readiness",
        ]
    elif namespace == "workflow" and action in WORKFLOW_METADATA:
        workflow = WORKFLOW_METADATA[action]
        proof_anchor = workflow["proof_anchor"]
        implementation_status = "source_scanned_workflow_proof_anchor"
        skill = workflow["skill"]
        preconditions_status = "declared_or_derived"
        output_contract_status = "declared"
        next_actions_status = "declared_with_availability"
        mutating = bool(workflow.get("mutates_assets", False))
        preconditions = workflow["preconditions"]
        outputs = workflow["outputs"]
        planning_signals = [
            f"skill:{skill}",
            "mcp_tool:workflow_query",
            workflow["schema_signal"],
            "execution_policy:track_dirty_packages can_mutate=true" if mutating else "execution_policy:read_only can_mutate=false",
            f"workflow_contract:{workflow['workflow_contract']}",
            f"proof_anchor:{proof_anchor}",
        ]

    available_offline = action in OFFLINE_AVAILABLE_ACTIONS.get(namespace, set())
    if available_offline:
        implementation_status = "offline_query_implemented"

    return {
        "namespace": namespace,
        "action": action,
        "full_name": f"{namespace}.{action}",
        "summary": summary,
        "source_file": source_file,
        "source_line": line,
        "source": "cpp_registry_scan",
        "available_offline": available_offline,
        "requires_live_editor": not available_offline,
        "mutates_assets": mutating,
        "writes_logs": True,
        "long_running": long_running,
        "supports_progress": supports_progress,
        "skill": skill,
        "preconditions": preconditions,
        "outputs": outputs,
        "planning_signals": planning_signals,
        "preconditions_status": preconditions_status,
        "output_contract_status": output_contract_status,
        "next_actions_status": next_actions_status,
        "proof_anchor": proof_anchor,
        "implementation_status": implementation_status,
    }


def is_catalog_source(path: Path, source_root: Path) -> bool:
    """Return whether a source file can contribute runtime catalog entries.

    Unreal automation fixtures register synthetic actions in directories named
    ``Tests``.  Those registrations are compiled only for tests and must never
    leak into the distributable offline catalog.
    """
    relative_parts = path.relative_to(source_root).parts
    return all(part.casefold() != "tests" for part in relative_parts)


def collect_actions(root: Path) -> tuple[list[dict], list[Path]]:
    source_root = root / "Source"
    source_files = sorted(
        path for path in source_root.rglob("*.cpp") if is_catalog_source(path, source_root)
    )
    header_files = sorted(
        path for path in source_root.rglob("*.h") if is_catalog_source(path, source_root)
    )
    actions: dict[tuple[str, str], dict] = {}

    helper_namespaces: dict[str, str] = {}
    class_action_names: dict[str, str] = {}
    class_summaries: dict[str, str] = {}

    for path in source_files:
        text = path.read_text(encoding="utf-8", errors="ignore")
        for match in TEMPLATE_HELPER_RE.finditer(text):
            helper_namespaces[match.group(1)] = match.group(2)
        for match in DESCRIPTION_RE.finditer(text):
            description = extract_return_text(match.group(2))
            if description:
                class_summaries[match.group(1)] = description

    for path in header_files:
        text = path.read_text(encoding="utf-8", errors="ignore")
        class_matches = list(
            re.finditer(r"\bclass\s+([A-Za-z_][A-Za-z0-9_]*)", text)
        )
        for name_match in GET_NAME_RE.finditer(text):
            owners = [
                match for match in class_matches if match.start() < name_match.start()
            ]
            if owners:
                class_action_names[owners[-1].group(1)] = name_match.group(1)
        for description_match in INLINE_DESCRIPTION_RE.finditer(text):
            owners = [
                match for match in class_matches if match.start() < description_match.start()
            ]
            if not owners:
                continue
            description = extract_return_text(description_match.group(1))
            if description:
                class_summaries[owners[-1].group(1)] = description

    for path in source_files:
        text = path.read_text(encoding="utf-8", errors="ignore")
        for match in REGISTER_RE.finditer(text):
            namespace, action = match.group(1), match.group(2)
            line = text.count("\n", 0, match.start()) + 1
            key = (namespace, action)
            if key in actions:
                continue
            actions[key] = action_metadata(
                namespace,
                action,
                extract_summary(text, match.end()),
                relpath(path, root),
                line,
            )

        for match in TEMPLATE_CALL_RE.finditer(text):
            helper, class_name = match.groups()
            namespace = helper_namespaces.get(helper)
            action = class_action_names.get(class_name)
            if not namespace or not action:
                continue
            key = (namespace, action)
            if key in actions:
                continue
            line = text.count("\n", 0, match.start()) + 1
            actions[key] = action_metadata(
                namespace,
                action,
                class_summaries.get(class_name, ""),
                relpath(path, root),
                line,
            )

    return [actions[key] for key in sorted(actions)], source_files + header_files


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument(
        "--out",
        type=Path,
        default=Path(__file__).resolve().parent / "Generated" / "monolith_catalog_snapshot.json",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Exit non-zero when the existing output differs from extracted action semantics.",
    )
    args = parser.parse_args()

    root = args.root.resolve()
    actions, _source_files = collect_actions(root)
    now = _dt.datetime.now(_dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")

    snapshot = {
        "schema_version": 1,
        "generated_at": now,
        "generator": "Tools/MonolithQuery/generate_monolith_catalog_snapshot.py",
        "source": "cpp_registry_scan",
        "source_root": "Source",
        "source_hash": action_semantic_hash(actions),
        "source_hash_kind": "action_semantics_v1",
        "action_count": len(actions),
        "partial": False,
        "actions": actions,
        "proof_anchors": {
            "game_ready_asset.static_mesh_readiness": {
                "namespace": "mesh",
                "action": "validate_game_ready",
                "mutates_assets": False,
                "fixture_root": "/Game/Tests/Monolith/WorkflowROI/GameReadyAsset",
                "proof_shape": {
                    "status": "dry_run|applied|partial|blocked",
                    "proof.read_back": [],
                    "proof.preview_artifacts": [],
                    "warnings": [],
                    "errors": [],
                    "next_actions": [],
                },
            },
            "game_ready_asset.static_mesh_workflow": {
                "namespace": "workflow",
                "action": "game_ready_asset_static_mesh",
                "mutates_assets": False,
                "fixture_root": "/Game/Tests/Monolith/WorkflowROI/GameReadyAsset",
                "proof_shape": {
                    "status": "planned|partial|blocked",
                    "workflow_id": "game_ready_asset",
                    "plan.steps": [],
                    "actions": [],
                    "validation.asset_validation": {},
                    "validation.compile": {},
                    "proof.read_back": [],
                    "source_control": {},
                    "warnings": [],
                    "errors": [],
                    "next_actions": [],
                },
            },
            "gameplay_feature.manifest_workflow": {
                "namespace": "workflow",
                "action": "gameplay_feature_manifest",
                "mutates_assets": False,
                "fixture_root": "/Game/Tests/Monolith/WorkflowROI/GameplayFeature",
                "proof_shape": {
                    "status": "planned|partial|blocked",
                    "workflow_id": "gameplay_feature",
                    "validation.input": {},
                    "validation.gas": {},
                    "validation.blueprint": {},
                    "validation.ai": {},
                    "validation.gamefeatures": {},
                    "validation.world_conditions": {},
                    "validation.runtime": {},
                    "proof.read_back": [],
                    "next_actions": [],
                },
            },
            "level_workflow.blockout_volume": {
                "namespace": "workflow",
                "action": "level_world_builder_blockout",
                "mutates_assets": True,
                "fixture_root": "/Game/Tests/Monolith/WorkflowROI/LevelWorkflow",
                "proof_shape": {
                    "status": "planned|partial|blocked",
                    "workflow_id": "level_workflow",
                    "touched.actors": [],
                    "touched.assets": [],
                    "touched.packages": [],
                    "dirty_packages": [],
                    "validation.blockout": {},
                    "validation.leveldesign": {},
                    "validation.save": {},
                    "source_control": {},
                    "proof.read_back": [],
                    "next_actions": [],
                },
            },
            "ui_shipping.widget_blueprint_workflow": {
                "namespace": "workflow",
                "action": "ui_shipping_widget_blueprint",
                "mutates_assets": False,
                "fixture_root": "/Game/Tests/Monolith/WorkflowROI/UIShipping",
                "proof_shape": {
                    "status": "planned|partial|blocked",
                    "workflow_id": "ui_shipping",
                    "validation.compile": {},
                    "validation.asset_validation": {},
                    "validation.accessibility": {},
                    "validation.ui": {},
                    "proof.read_back": [],
                    "proof.preview_artifacts": [],
                    "source_control": {},
                    "next_actions": [],
                },
            },
            "shot_render.level_sequence_mrq_workflow": {
                "namespace": "workflow",
                "action": "shot_render_level_sequence",
                "mutates_assets": False,
                "fixture_root": "/Game/Tests/Monolith/WorkflowROI/ShotRender",
                "proof_shape": {
                    "status": "planned|partial|blocked",
                    "workflow_id": "shot_render",
                    "validation.asset_validation": {},
                    "validation.render": {},
                    "proof.read_back": [],
                    "proof.preview_artifacts": [],
                    "artifacts": [],
                    "source_control": {},
                    "next_actions": [],
                },
            },
            "audio_shipping.asset_workflow": {
                "namespace": "workflow",
                "action": "audio_shipping_asset",
                "mutates_assets": False,
                "fixture_root": "/Game/Tests/Monolith/WorkflowROI/AudioShipping",
                "proof_shape": {
                    "status": "planned|partial|blocked",
                    "workflow_id": "audio_shipping",
                    "validation.asset_validation": {},
                    "validation.runtime": {},
                    "validation.budget": {},
                    "proof.read_back": [],
                    "proof.preview_artifacts": [],
                    "dirty_packages": [],
                    "source_control": {},
                    "next_actions": [],
                },
            },
            "localization_shipping.string_table_workflow": {
                "namespace": "workflow",
                "action": "localization_shipping_string_table",
                "mutates_assets": False,
                "fixture_root": "/Game/Tests/Monolith/WorkflowROI/LocalizationShipping",
                "proof_shape": {
                    "status": "planned|partial|blocked",
                    "workflow_id": "localization_shipping",
                    "validation.asset_validation": {},
                    "validation.localization": {},
                    "proof.read_back": [],
                    "artifacts": [],
                    "dirty_packages": [],
                    "source_control": {},
                    "next_actions": [],
                },
            },
            "slate_euw.test_flow_workflow": {
                "namespace": "workflow",
                "action": "slate_euw_test_flow",
                "mutates_assets": False,
                "fixture_root": "/Game/Tests/Monolith/WorkflowROI/SlateEuw",
                "proof_shape": {
                    "status": "planned|partial|blocked",
                    "workflow_id": "slate_euw_test_flow",
                    "validation.ui": {},
                    "validation.runtime": {},
                    "validation.interaction": {},
                    "proof.read_back": [],
                    "proof.preview_artifacts": [],
                    "artifacts": [],
                    "dirty_packages": [],
                    "next_actions": [],
                },
            },
        },
    }

    if args.check:
        if not args.out.is_file():
            print(f"catalog snapshot missing: {args.out}")
            return 1
        try:
            existing = json.loads(args.out.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            print(f"catalog snapshot unreadable: {error}")
            return 1
        expected_contract = catalog_contract(snapshot)
        actual_contract = catalog_contract(existing)
        if actual_contract != expected_contract:
            print(
                "catalog snapshot drift: run "
                "python Tools/MonolithQuery/generate_monolith_catalog_snapshot.py"
            )
            return 1
        print(f"catalog snapshot current: {len(actions)} actions")
        return 0

    changed = write_catalog_if_changed(args.out, snapshot)
    status = "wrote" if changed else "catalog snapshot current"
    print(f"{status}: {args.out} ({len(actions)} actions)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
