#!/usr/bin/env python3
"""
Monolith MCP ProjectIndex Quality benchmark.

Measures whether the project namespace (asset index) returns data that is
rich enough for agents to find assets, look up gameplay tags, and understand
the project structure.  The benchmark is deterministic and does not call an LLM.

Five task categories:
  asset_search           - project.search for various query strings (Blueprint, Widget, etc.)
  gameplay_tag_lookup    - project.list_gameplay_tags / project.search_gameplay_tags
  health_check           - project.health, presence of status field
  stats_check            - project.get_stats, presence of success/indexing/stats fields
  schema_field_presence  - monolith_discover schema check for planning signals
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import pathlib
import socket
import sys
import time
import urllib.error
import urllib.request
from typing import Any, Dict, Iterable, List, Optional, Tuple

from benchmark_common import attach_benchmark_inputs, build_benchmark_inputs, display_path, resolve_plugin_path

DEFAULT_MCP_URL = "http://localhost:9316/mcp"
DEFAULT_TASKS = pathlib.Path("Benchmarks/ProjectIndex/tasks.jsonl")
DEFAULT_MANIFEST = pathlib.Path("Benchmarks/ProjectIndex/manifest.json")
DEFAULT_RESULTS_ROOT = pathlib.Path("Saved/Monolith/Benchmarks/ProjectIndex")

# Required fields for project search result rows (from CLAUDE.md project search contract)
PROJECT_REQUIRED_FIELDS = {"match_object_path", "match_value", "match_source"}

SCHEMA_ACTIONS = [
    "search",
    "health",
    "get_asset_details",
    "list_gameplay_tags",
    "search_gameplay_tags",
    "find_by_type",
    "find_references",
    "get_stats",
    "repair_fts",
    "list_asset_registry_tags",
    "get_blueprint_info",
    "find_data_table_rows",
    "search_data_tables",
    "get_level_info",
    "list_map_check_results",
    "find_unreferenced_assets",
    "get_redirect_map",
    "find_soft_references",
    "find_hard_references",
    "find_actors_of_class",
    "get_asset_size_report",
    "list_asset_dependencies",
    "find_circular_dependencies",
    "validate_assets",
    "audit_asset_naming",
]

ASSET_SEARCH_QUERIES = [
    "Actor", "Widget", "Blueprint", "Material", "Sound", "Level", "Niagara",
    "Animation", "Sequence", "DataTable", "GameplayAbility", "AttributeSet",
    "Tag", "Component", "Controller", "GameMode", "GameState", "HUD", "Camera",
    "Character", "PlayerController", "AIController", "BehaviorTree", "Blackboard",
    "EnvQuery", "Montage", "StateMachine", "PhysicsAsset", "Cloth", "Landscape",
    "Sky", "Lighting", "Particle", "VFX", "UI", "Menu", "Inventory", "Quest",
    "Dialog", "Trigger", "Volume", "Checkpoint", "SpawnPoint", "Pickup", "Item",
    "Weapon", "Enemy", "Interactable", "Save", "Config",
]

GAMEPLAY_TAG_PREFIXES = [
    "Combat", "Status", "Ability", "Item", "UI", "Game",
]

GAMEPLAY_TAG_SEARCH_QUERIES = [
    "Combat", "Status", "Ability", "Item", "UI", "Player",
    "Enemy", "Character", "Skill", "Effect",
]

_PRESERVED_ASSET_SEARCH_QUERIES_20260617 = [
    "BP_",
    "WBP_",
    "DT_",
    "DA_",
    "GA_",
    "GE_",
    "Player",
    "Enemy",
    "Boss",
    "Character",
    "Monster",
    "Skill",
    "Ability",
    "Attack",
    "Move",
    "Item",
    "Weapon",
    "Inventory",
    "Map",
    "Level",
    "Stage",
    "World",
    "Scene",
    "UI",
    "HUD",
    "Menu",
    "Slot",
    "Icon",
    "Sound",
    "Music",
    "SFX",
    "Effect",
    "Material",
    "Texture",
    "Mesh",
    "Animation",
    "Data",
    "Config",
    "Table",
    "Asset",
    "WBP_HUD",
    "DT_Ability",
    "BP_Enemy",
    "GA_Skill",
    "GE_Damage",
    "WBP_Menu",
    "DT_Item",
    "BP_Boss",
    "WBP_Slot",
    "DA_Character",
]

_PRESERVED_GAMEPLAY_TAG_PREFIXES_20260617 = [
    "Ability",
    "Ability.Skill",
    "State",
    "State.Buff",
    "State.Debuff",
    "Damage",
    "Damage.Type",
    "Cue",
    "Cue.Hit",
    "Combat",
    "Combat.Melee",
    "Player",
    "Enemy",
]

_PRESERVED_GAMEPLAY_TAG_SEARCH_QUERIES_20260617 = [
    "Stun",
    "Burn",
    "Poison",
    "Shield",
    "Heal",
    "Cooldown",
    "Cast",
    "Dash",
    "Block",
    "Crit",
    "Aura",
    "Slow",
]

_PRESERVED_SCHEMA_ACTIONS_20260617 = [
    "impact_radius",
    "risk_score",
    "review_hotspots",
    "review_context",
    "detect_changes",
    "find_unused",
    "pre_merge_check",
    "snapshot",
    "diff_snapshots",
    "build_crg_graph",
    "repair_crg_cache",
    "list_assets",
    "get_asset_by_path",
    "get_dependencies",
    "search_assets",
]

_PRESERVED_HEALTH_VARIANTS_20260617 = [
    {"action": "health"},
    {"action": "health", "include_counts": True},
    {"action": "health", "include_counts": False},
    {"action": "health", "detail": "full"},
    {"action": "health", "detail": "minimal"},
    {"action": "health", "mode": "summary"},
    {"action": "health", "detail": "full", "include_counts": True},
    {"action": "health", "include_counts": True, "mode": "summary"},
    {"action": "health", "detail": "minimal", "include_counts": False},
    {"action": "health", "verbose": True},
]

_ADDED_ASSET_SEARCH_QUERIES_20260617 = [
    "GameplayCue",
    "GameplayEffect",
    "GameplayAbility",
    "AbilitySet",
    "AttributeSet",
    "InputAction",
    "InputMappingContext",
    "NiagaraSystem",
    "NiagaraEmitter",
    "MaterialInstance",
    "MaterialFunction",
    "TextureAtlas",
    "SpriteSheet",
    "PaperZD",
    "Flipbook",
    "AnimMontage",
    "BlendSpace",
    "LevelSequence",
    "CameraShake",
    "MetaSound",
    "SoundCue",
    "Submix",
    "CommonButton",
    "ViewModel",
    "WidgetBlueprint",
    "SaveGame",
    "DataAsset",
    "PrimaryDataAsset",
    "WorldData",
    "StageLayout",
    "Spawner",
    "SpawnTable",
    "Encounter",
    "BehaviorTree",
    "EQS",
    "BlackboardData",
    "StateTree",
    "SmartObject",
    "PCG",
    "HLOD",
    "WorldPartition",
    "LandscapeLayer",
    "WaterBody",
    "Input",
    "Localization",
    "StringTable",
    "CurveTable",
    "DataRegistry",
    "GameplayTagTable",
    "DamageType",
    "Projectile",
    "HitReact",
    "Cooldown",
    "Targeting",
    "Buff",
    "Debuff",
    "Loot",
    "QuestObjective",
    "Dialogue",
    "InteractPrompt",
    "ComboGraph",
    "Chooser",
    "LevelInstance",
    "PackedLevelActor",
    "GeometryCollection",
    "Dataflow",
    "ClothAsset",
    "MetaHuman",
    "MovieRender",
    "RenderQueue",
]

_ADDED_GAMEPLAY_TAG_PREFIXES_20260617 = [
    "Ability.Cooldown",
    "Ability.Targeting",
    "Combat.Ranged",
    "Combat.HitReact",
    "Damage.Element",
    "State.CrowdControl",
    "UI.HUD",
    "UI.Menu",
    "Input.Gameplay",
    "Quest.Objective",
    "Cue.Gameplay",
    "Item.Weapon",
    "Status.Invulnerable",
]

_ADDED_GAMEPLAY_TAG_SEARCH_QUERIES_20260617 = [
    "GameplayCue",
    "Cooldown",
    "HitReact",
    "Invulnerable",
    "RootMotion",
    "Target",
    "Projectile",
    "Loot",
    "Interact",
    "Objective",
    "Elemental",
    "Combo",
]

_ADDED_HEALTH_VARIANTS_20260617 = [
    {"action": "health", "detail": "asset_registry"},
    {"action": "health", "detail": "asset_registry", "include_counts": True},
    {"action": "health", "detail": "gameplay_tags"},
    {"action": "health", "mode": "smoke", "include_counts": False},
    {"action": "health", "verbose": False},
]

_LOG_DERIVED_ASSET_SEARCH_QUERIES_20260617 = [
    "ActionGuidance",
    "BlueprintEditing",
    "OfflineParity",
    "SourceIndex",
    "ProjectIndex",
    "SchemaCompleteness",
    "InvocationLog",
    "ActionAudit",
    "ToolProfile",
    "ExecutionGuard",
    "Readiness",
    "Onboarding",
    "RecoverMcp",
    "HeadlessMcp",
    "IndexFreshness",
    "CRG",
    "CallGraph",
    "BulkFill",
    "ReflectionIntel",
    "MCPProxy",
    "RunPython",
    "AgentOps",
]

# Known-answer recall fixtures: (query, expected_object_path).
#
# Unlike the broad single-token searches above (which use min_results:0 and have no
# ground truth), each of these queries a distinctive, project-unique asset name and
# asserts the response actually contains the exact /Game object path for that asset.
# This closes the "empty/broken index still scores 1.000" loophole: a known-answer
# task is a HIT only when expected_object_path appears in the result set's
# match_object_path values, and it requires >=1 result.
#
# expected_object_path is the package-relative object path emitted by project.search
# for an asset-name match (the package path; no trailing `.AssetName` object suffix),
# matching the live `match_object_path` field. Every pair below was verified against
# Saved/ProjectIndex.db via `Binaries\monolith_query.exe project search <query>` so the
# expected path is present in the default content-inclusive search results.
_KNOWN_ANSWER_FIXTURES_20260618: List[Tuple[str, str]] = [
    ("DA_Monster_001_Wiggly", "/Game/Design/DataAsset/Monsters/DA_Monster_001_Wiggly"),
    ("DA_Monster_002_Puffshroom", "/Game/Design/DataAsset/Monsters/DA_Monster_002_Puffshroom"),
    ("DA_Monster_003_Chonkbee", "/Game/Design/DataAsset/Monsters/DA_Monster_003_Chonkbee"),
    ("DA_Character_001_Kain", "/Game/Design/DataAsset/Characters/DA_Character_001_Kain"),
    ("DA_Character_002_Igna", "/Game/Design/DataAsset/Characters/DA_Character_002_Igna"),
    ("DA_Character_003_Leia", "/Game/Design/DataAsset/Characters/DA_Character_003_Leia"),
    ("DA_Weapon_001_Whip", "/Game/Design/DataAsset/Weapons/DA_Weapon_001_Whip"),
    ("DA_Weapon_002_Magic_Wand", "/Game/Design/DataAsset/Weapons/DA_Weapon_002_Magic_Wand"),
    ("DA_Weapon_004_Axe", "/Game/Design/DataAsset/Weapons/DA_Weapon_004_Axe"),
    ("DA_Item_001_Egg", "/Game/Design/DataAsset/Item/DA_Item_001_Egg"),
    ("DA_Potion_001_Potion_of_Life", "/Game/Design/DataAsset/Item/Potions/DA_Potion_001_Potion_of_Life"),
    ("DA_Potion_005_Potion_of_Vampirism", "/Game/Design/DataAsset/Item/Potions/DA_Potion_005_Potion_of_Vampirism"),
    ("DA_Pawn_001_PlayerCharacter", "/Game/Design/DataAsset/PawnTable/DA_Pawn_001_PlayerCharacter"),
    ("DA_Synergy_001_s2_01", "/Game/Design/DataAsset/Synergies/DA_Synergy_001_s2_01"),
    ("DA_Synergy_010_s2_10", "/Game/Design/DataAsset/Synergies/DA_Synergy_010_s2_10"),
    ("DA_Stage_001_A", "/Game/Design/DataAsset/Stages/DA_Stage_001_A"),
    ("DA_NodeMap_001_Generation_Default", "/Game/Design/DataAsset/NodeMap/DA_NodeMap_001_Generation_Default"),
    ("DA_NodeMap_002_Style_Default", "/Game/Design/DataAsset/NodeMap/DA_NodeMap_002_Style_Default"),
    ("DA_StageGeneration_001_Config", "/Game/Design/DataAsset/Stages/Generation/DA_StageGeneration_001_Config"),
    ("DA_GameplayExperience_001_LobbyExperience", "/Game/Design/DataAsset/Experience/DA_GameplayExperience_001_LobbyExperience"),
    ("DA_World_002_Lobby", "/Game/Design/DataAsset/WorldTable/DA_World_002_Lobby"),
    ("EUW_StageMaker", "/Game/Editor/StageMaker/EUW_StageMaker"),
    ("EUW_CheatPanel", "/Game/CheatBoard/EUW_CheatPanel"),
    ("DT_AbilityTags", "/Game/Design/DataTable/GameplayTags/DT_AbilityTags"),
    ("DT_CommonTags", "/Game/Design/DataTable/GameplayTags/DT_CommonTags"),
    ("DT_DamageTypeTags", "/Game/Design/DataTable/GameplayTags/DT_DamageTypeTags"),
    ("DT_GameplayCueTags", "/Game/Design/DataTable/GameplayTags/DT_GameplayCueTags"),
    ("IA_Attack", "/Game/Design/PC/Input/Actions/IA_Attack"),
    ("IA_Interaction", "/Game/Design/PC/Input/Actions/IA_Interaction"),
    ("IMC_Default", "/Game/GameMode/GameModeSub/IMC_Default"),
]


def append_project_search_tasks(tasks: List[Dict[str, Any]], next_id: Any, queries: List[str]) -> None:
    for query in queries:
        tasks.append({
            "id": next_id(),
            "category": "asset_search",
            "namespace": "project",
            "action": "search",
            "tool": "project_query",
            "arguments": {"action": "search", "query": query},
            "expected": {"valid_response": True, "min_results": 0},
            "safety": "read_only",
        })


def append_project_known_answer_tasks(
    tasks: List[Dict[str, Any]],
    next_id: Any,
    fixtures: List[Tuple[str, str]],
) -> None:
    """Append known-answer recall tasks.

    Each task searches a distinctive asset name and asserts the response contains
    the exact expected /Game object path. min_results is 1 (require_results), so an
    empty or broken index cannot pass these tasks.
    """
    for query, expected_object_path in fixtures:
        tasks.append({
            "id": next_id(),
            "category": "known_answer",
            "namespace": "project",
            "action": "search",
            "tool": "project_query",
            "arguments": {"action": "search", "query": query},
            "expected": {
                "valid_response": True,
                "min_results": 1,
                "expected_object_path": expected_object_path,
            },
            "require_results": True,
            "safety": "read_only",
        })


def append_project_gameplay_tag_tasks(
    tasks: List[Dict[str, Any]],
    next_id: Any,
    *,
    prefixes: List[str],
    queries: List[str],
) -> None:
    for prefix in prefixes:
        tasks.append({
            "id": next_id(),
            "category": "gameplay_tag_lookup",
            "namespace": "project",
            "action": "list_gameplay_tags",
            "tool": "project_query",
            "arguments": {"action": "list_gameplay_tags", "prefix": prefix},
            "expected": {"valid_response": True},
            "safety": "read_only",
        })
    for query in queries:
        tasks.append({
            "id": next_id(),
            "category": "gameplay_tag_lookup",
            "namespace": "project",
            "action": "search_gameplay_tags",
            "tool": "project_query",
            "arguments": {"action": "search_gameplay_tags", "query": query},
            "expected": {"valid_response": True},
            "safety": "read_only",
        })


def append_project_schema_tasks(tasks: List[Dict[str, Any]], next_id: Any, actions: List[str]) -> None:
    for action in actions:
        tasks.append({
            "id": next_id(),
            "category": "schema_field_presence",
            "namespace": "project",
            "action": action,
            "tool": "monolith_discover",
            "arguments": {"action": action, "mode": "schema", "namespace": "project"},
            "expected": {"requires_planning_signals": True, "requires_skill": True},
            "safety": "read_only_discovery",
        })


def append_project_health_tasks(tasks: List[Dict[str, Any]], next_id: Any, variants: List[Dict[str, Any]]) -> None:
    for args in variants:
        tasks.append({
            "id": next_id(),
            "category": "health_check",
            "namespace": "project",
            "action": "health",
            "tool": "project_query",
            "arguments": dict(args),
            "expected": {"fields": ["status"]},
            "safety": "read_only",
        })


# ---------------------------------------------------------------------------
# Utilities
# ---------------------------------------------------------------------------

def utc_now() -> str:
    return _dt.datetime.now(_dt.timezone.utc).isoformat()


def load_jsonl(path: pathlib.Path) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as handle:
        for line_no, line in enumerate(handle, 1):
            stripped = line.strip()
            if not stripped:
                continue
            try:
                rows.append(json.loads(stripped))
            except json.JSONDecodeError as exc:
                raise RuntimeError(f"{path}:{line_no}: invalid JSONL row: {exc}") from exc
    return rows


def write_json(path: pathlib.Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(payload, handle, indent=2, ensure_ascii=False)
        handle.write("\n")


def write_jsonl(path: pathlib.Path, rows: Iterable[Dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        for row in rows:
            handle.write(json.dumps(row, ensure_ascii=False, sort_keys=True))
            handle.write("\n")


def read_http_body(response: Any, timeout_s: float) -> str:
    content_type = str(response.headers.get("Content-Type", "")).lower()
    if "text/event-stream" not in content_type:
        return response.read().decode("utf-8", errors="replace")

    data_lines: List[str] = []
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        line = response.readline()
        if not line:
            break
        text = line.decode("utf-8", errors="replace").rstrip("\r\n")
        if text.startswith("data:"):
            data_lines.append(text[5:].lstrip())
            continue
        if text == "" and data_lines:
            return "\n".join(data_lines)
    return "\n".join(data_lines)


def extract_sse_data(raw: str) -> str:
    lines = raw.splitlines()
    data_lines = [line[5:].lstrip() for line in lines if line.startswith("data:")]
    return "\n".join(data_lines) if data_lines else raw


def mcp_call(url: str, tool: str, arguments: Dict[str, Any], timeout_s: float = 45.0) -> Dict[str, Any]:
    body = {
        "jsonrpc": "2.0",
        "id": int(time.time() * 1000) % 1000000000,
        "method": "tools/call",
        "params": {
            "name": tool,
            "arguments": arguments,
        },
    }
    data = json.dumps(body, separators=(",", ":")).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=data,
        headers={
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout_s) as response:
            raw = read_http_body(response, timeout_s)
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8", errors="replace")
        return {"transport_error": True, "status": exc.code, "raw": raw, "request": body}
    except (TimeoutError, socket.timeout) as exc:
        return {"transport_error": True, "status": None, "raw": f"timeout: {exc}", "request": body}
    except urllib.error.URLError as exc:
        return {"transport_error": True, "status": None, "raw": str(exc), "request": body}
    except OSError as exc:
        return {"transport_error": True, "status": None, "raw": str(exc), "request": body}

    raw = extract_sse_data(raw)
    try:
        parsed = json.loads(raw)
    except json.JSONDecodeError:
        parsed = {"parse_error": True, "raw": raw}
    parsed["request"] = body
    return parsed


def result_text(response: Dict[str, Any]) -> str:
    result = response.get("result") if isinstance(response, dict) else None
    if isinstance(result, dict):
        content = result.get("content")
        if isinstance(content, list) and content:
            first = content[0]
            if isinstance(first, dict):
                return str(first.get("text", ""))
    error = response.get("error") if isinstance(response, dict) else None
    if error is not None:
        return json.dumps(error, ensure_ascii=False)
    return ""


def result_payload(response: Dict[str, Any]) -> Dict[str, Any]:
    result = response.get("result") if isinstance(response, dict) else None
    return result if isinstance(result, dict) else {}


def structured_content(payload: Dict[str, Any]) -> Dict[str, Any]:
    structured = payload.get("structuredContent") if isinstance(payload, dict) else None
    return structured if isinstance(structured, dict) else {}


def text_json(response: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    text = result_text(response)
    if not text:
        return None
    try:
        parsed = json.loads(text)
    except json.JSONDecodeError:
        return None
    return parsed if isinstance(parsed, dict) else None


def result_data(response: Dict[str, Any]) -> Dict[str, Any]:
    parsed = text_json(response)
    if parsed:
        return parsed
    payload = result_payload(response)
    structured = structured_content(payload)
    return structured if structured else payload


def count_by(rows: Iterable[Dict[str, Any]], field: str) -> Dict[str, int]:
    out: Dict[str, int] = {}
    for row in rows:
        key = str(row.get(field, ""))
        out[key] = out.get(key, 0) + 1
    return dict(sorted(out.items()))


def avg(values: List[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def task_fingerprint(task: Dict[str, Any]) -> str:
    payload = {
        "category": task.get("category"),
        "namespace": task.get("namespace"),
        "action": task.get("action"),
        "tool": task.get("tool"),
        "arguments": task.get("arguments"),
        "expected": task.get("expected"),
    }
    return json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def dedupe_tasks(tasks: Iterable[Dict[str, Any]], id_prefix: str) -> List[Dict[str, Any]]:
    unique: List[Dict[str, Any]] = []
    seen: set[str] = set()
    for task in tasks:
        fingerprint = task_fingerprint(task)
        if fingerprint in seen:
            continue
        seen.add(fingerprint)
        unique.append(dict(task))
    for index, task in enumerate(unique, 1):
        task["id"] = f"{id_prefix}-{index:03d}"
    return unique


# ---------------------------------------------------------------------------
# Result-field helpers
# ---------------------------------------------------------------------------

def project_results(data: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Return list of result dicts from a project.search response."""
    for key in ("results", "hits", "items", "assets"):
        value = data.get(key)
        if isinstance(value, list):
            return [r for r in value if isinstance(r, dict)]
    return []


def result_has_project_fields(result: Dict[str, Any]) -> bool:
    """True if at least 2 of the 3 required project search fields are present."""
    present = sum(1 for f in PROJECT_REQUIRED_FIELDS if result.get(f) is not None)
    return present >= 2


def result_object_paths(results: List[Dict[str, Any]]) -> List[str]:
    """All match_object_path values present across the result rows."""
    paths: List[str] = []
    for result in results:
        value = result.get("match_object_path")
        if isinstance(value, str) and value:
            paths.append(value)
    return paths


def known_answer_hit(results: List[Dict[str, Any]], expected_object_path: str) -> bool:
    """True if the expected /Game object path appears among the result rows.

    A HIT requires the asset to actually be present in the response, so an empty
    or broken index can never satisfy a known-answer task.
    """
    if not expected_object_path:
        return False
    return expected_object_path in result_object_paths(results)


def is_valid_non_error_response(data: Dict[str, Any], response: Dict[str, Any]) -> bool:
    """True if the response is a valid non-error JSON object (even if results are empty)."""
    if response.get("transport_error"):
        return False
    if response.get("parse_error"):
        return False
    if result_payload(response).get("isError"):
        return False
    # A dict with any keys (including empty results) is a valid response
    return isinstance(data, dict)


def project_health_fields_present(data: Dict[str, Any]) -> bool:
    """True if project health response contains at least a status field."""
    return "status" in data


def project_health_is_stale_or_error(data: Dict[str, Any]) -> bool:
    """True if project health response signals stale, error, or degraded state."""
    if not project_health_fields_present(data):
        return True
    status = str(data.get("status", "")).lower()
    if status in ("error", "stale", "degraded", "unavailable", "unknown", ""):
        return True
    stale = data.get("stale") or data.get("is_stale")
    return bool(stale)


def schema_has_planning_signals(schema: Dict[str, Any]) -> bool:
    signals = schema.get("planning_signals")
    return isinstance(signals, list) and len(signals) > 0


def schema_has_skill(schema: Dict[str, Any]) -> bool:
    return isinstance(schema.get("skill"), str) and bool(schema.get("skill"))


# ---------------------------------------------------------------------------
# Task scoring
# ---------------------------------------------------------------------------

def score_task(url: str, task: Dict[str, Any], timeout_s: float) -> Dict[str, Any]:
    response = mcp_call(url, str(task["tool"]), dict(task.get("arguments", {})), timeout_s=timeout_s)
    data = result_data(response)
    category = task.get("category")
    expected = task.get("expected", {}) if isinstance(task.get("expected"), dict) else {}
    min_results = int(expected.get("min_results", 0) or 0)

    direct_success = False
    results_count = 0
    field_complete_count = 0
    # expected_nonempty marks tasks whose contract demands >=1 result, so the
    # field_completeness aggregate can penalize zero-result responses instead of
    # vacuously crediting them. known_answer tasks are always expected-nonempty.
    expected_nonempty = min_results >= 1
    known_answer_hit_flag = False
    stale = False
    planning_signals = False
    evidence: Dict[str, Any] = {}

    if category == "known_answer":
        results = project_results(data)
        results_count = len(results)
        valid_resp = is_valid_non_error_response(data, response)
        expected_object_path = str(expected.get("expected_object_path", ""))
        hit = known_answer_hit(results, expected_object_path)
        known_answer_hit_flag = hit
        # A known-answer task succeeds only when the response is valid, has >=1
        # result, AND that result set actually contains the expected asset path.
        direct_success = valid_resp and results_count >= 1 and hit
        field_complete_count = sum(1 for r in results if result_has_project_fields(r))
        evidence = {
            "results_count": results_count,
            "field_complete_count": field_complete_count,
            "valid_response": valid_resp,
            "expected_object_path": expected_object_path,
            "known_answer_hit": hit,
        }

    elif category in ("asset_search", "gameplay_tag_lookup"):
        results = project_results(data)
        results_count = len(results)
        valid_resp = is_valid_non_error_response(data, response)
        # Tasks with expected.min_results>=1 (require_results) must return >=1 result;
        # generic broad-token searches keep min_results:0 and stay lenient (empty OK
        # if the response is a valid non-error object).
        if expected_nonempty:
            direct_success = valid_resp and results_count >= 1
        else:
            direct_success = valid_resp
        field_complete_count = sum(1 for r in results if result_has_project_fields(r))
        evidence = {
            "results_count": results_count,
            "field_complete_count": field_complete_count,
            "valid_response": valid_resp,
            "require_results": expected_nonempty,
            "first_result_keys": list(results[0].keys())[:8] if results else [],
        }

    elif category == "health_check":
        fields_ok = project_health_fields_present(data)
        is_stale = project_health_is_stale_or_error(data)
        direct_success = fields_ok
        stale = is_stale
        evidence = {
            "health_fields_present": fields_ok,
            "is_stale_or_error": is_stale,
            "status": data.get("status"),
        }

    elif category == "stats_check":
        # project.get_stats is NOT a health endpoint (project.health is). In steady
        # state it returns {success, indexing, stats} and only emits a transient
        # `status` message while indexing. Assert its actual contract here so it is
        # not scored against a non-existent steady-state `status` field, and so it
        # does not poison stale_rate (which counts only health_check rows).
        fields_ok = (
            isinstance(data, dict)
            and data.get("success") is True
            and "indexing" in data
            and isinstance(data.get("stats"), dict)
        )
        direct_success = fields_ok
        evidence = {
            "has_success": data.get("success") is True,
            "has_indexing": "indexing" in data,
            "has_stats": isinstance(data.get("stats"), dict),
        }

    elif category == "schema_field_presence":
        schema = data.get("schema") if isinstance(data, dict) else None
        if not isinstance(schema, dict):
            schema = data
        has_signals = schema_has_planning_signals(schema)
        has_skill = schema_has_skill(schema)
        planning_signals = has_signals
        direct_success = bool(has_signals and has_skill)
        evidence = {
            "has_planning_signals": has_signals,
            "has_skill": has_skill,
        }

    else:
        evidence = {"unsupported_category": category}

    return {
        "task_id": task.get("id"),
        "category": category,
        "namespace": task.get("namespace"),
        "action": task.get("action"),
        "direct_success": direct_success,
        "results_count": results_count,
        "field_complete_count": field_complete_count,
        "expected_nonempty": expected_nonempty,
        "known_answer_hit": known_answer_hit_flag,
        "stale": stale,
        "planning_signals": planning_signals,
        "evidence": evidence,
        "transport_error": bool(response.get("transport_error")),
        "transport_error_raw": str(response.get("raw", ""))[:300] if response.get("transport_error") else "",
        "response_is_error": bool(result_payload(response).get("isError")),
        "response_text": result_text(response)[:1000],
    }


# ---------------------------------------------------------------------------
# Aggregate
# ---------------------------------------------------------------------------

# Hard cap applied to project_index_score when a run returns zero results across
# every result-bearing task (asset_search + gameplay_tag_lookup + known_answer). An
# empty or broken index can no longer score near 1.000 -- it is pinned at/below this.
ALL_EMPTY_SCORE_CAP = 0.30


def aggregate(label: str, status: Dict[str, Any], tasks: List[Dict[str, Any]], rows: List[Dict[str, Any]]) -> Dict[str, Any]:
    search_rows = [r for r in rows if r["category"] in ("asset_search", "gameplay_tag_lookup")]
    known_answer_rows = [r for r in rows if r["category"] == "known_answer"]
    health_rows = [r for r in rows if r["category"] == "health_check"]
    stats_rows = [r for r in rows if r["category"] == "stats_check"]
    schema_rows = [r for r in rows if r["category"] == "schema_field_presence"]

    # Result-bearing rows: every task that calls project.search / list_gameplay_tags.
    result_rows = search_rows + known_answer_rows

    # search_hit_rate: fraction of asset_search + gameplay_tag_lookup rows where
    # direct_success (lenient for min_results:0 rows -- empty OK if valid response;
    # strict for require_results rows -- needs >=1 result).
    search_hit_rate = avg([1.0 if r["direct_success"] else 0.0 for r in search_rows])

    # known_answer_hit_rate: fraction of known-answer recall tasks whose response
    # actually contained the expected /Game object path. Zero ground-truth before;
    # this is the dimension an empty/broken index cannot fake.
    known_answer_hit_rate = avg([1.0 if r["direct_success"] else 0.0 for r in known_answer_rows])

    # field_completeness_rate: fraction of returned result items with >=2 required
    # fields present, computed over EXPECTED-NONEMPTY tasks (known_answer + any
    # require_results search). Zero-result responses no longer score a vacuous 1.0:
    #   - if there are expected-nonempty tasks but they returned no rows at all, the
    #     rate is 0.0 (a broken index is penalized, not credited);
    #   - only when there are no expected-nonempty tasks defined does it stay 1.0
    #     (nothing to measure), which cannot happen in the generated corpus.
    expected_nonempty_rows = [r for r in result_rows if r.get("expected_nonempty")]
    if expected_nonempty_rows:
        total_results = sum(r["results_count"] for r in expected_nonempty_rows)
        total_complete = sum(r["field_complete_count"] for r in expected_nonempty_rows)
        field_completeness_rate = total_complete / total_results if total_results > 0 else 0.0
    else:
        field_completeness_rate = 1.0

    # schema_adherence_rate: fraction of schema_field_presence tasks with planning_signals present
    schema_adherence_rate = avg([1.0 if r["planning_signals"] else 0.0 for r in schema_rows])

    # stale_rate: fraction of health_check tasks that are stale or error
    stale_rate = avg([1.0 if r["stale"] else 0.0 for r in health_rows])

    # stats_check_rate: fraction of get_stats tasks returning the success/indexing/stats
    # contract. Informational only -- not folded into project_index_score.
    stats_check_rate = avg([1.0 if r["direct_success"] else 0.0 for r in stats_rows])

    error_count = sum(1 for r in rows if r.get("transport_error") or r.get("response_is_error"))
    error_free_rate = 1.0 - (error_count / len(rows) if rows else 0.0)

    # all_empty: loud integrity signal. True when there are result-bearing tasks but
    # NONE of them returned a single result -- i.e. the index is empty or broken.
    total_result_count = sum(r["results_count"] for r in result_rows)
    all_empty = bool(result_rows) and total_result_count == 0

    # project_index_score (weights sum to 1.0)
    project_index_score = (
        0.25 * search_hit_rate
        + 0.20 * known_answer_hit_rate
        + 0.20 * field_completeness_rate
        + 0.15 * schema_adherence_rate
        + 0.10 * (1.0 - stale_rate)
        + 0.10 * error_free_rate
    )
    # An all-empty run cannot score near-perfect even if schema/health probes pass.
    if all_empty:
        project_index_score = min(project_index_score, ALL_EMPTY_SCORE_CAP)

    if all_empty:
        print(
            f"[ALL-EMPTY] WARNING: {len(result_rows)} result-bearing tasks returned 0 results total; "
            f"index appears EMPTY or BROKEN. project_index_score capped at {ALL_EMPTY_SCORE_CAP}.",
            file=sys.stderr,
            flush=True,
        )

    return {
        "label": label,
        "created_at": utc_now(),
        "mcp_status": status,
        "task_count": len(rows),
        "error_count": error_count,
        "all_empty": all_empty,
        "category_counts": count_by(tasks, "category"),
        "metrics": {
            "project_index_score": round(project_index_score, 6),
            "search_hit_rate": round(search_hit_rate, 6),
            "known_answer_hit_rate": round(known_answer_hit_rate, 6),
            "field_completeness_rate": round(field_completeness_rate, 6),
            "schema_adherence_rate": round(schema_adherence_rate, 6),
            "stale_rate": round(stale_rate, 6),
            "stats_check_rate": round(stats_check_rate, 6),
            "error_free_rate": round(error_free_rate, 6),
            "all_empty": all_empty,
            "task_count": len(rows),
            "error_count": error_count,
        },
    }


# ---------------------------------------------------------------------------
# Generate
# ---------------------------------------------------------------------------

def build_static_tasks() -> List[Dict[str, Any]]:
    """Build a deterministic task list for the project namespace benchmark."""
    tasks: List[Dict[str, Any]] = []

    def next_id() -> str:
        return f"PIB-{len(tasks) + 1:03d}"

    # --- asset_search: project.search for 50 queries ---
    for query in ASSET_SEARCH_QUERIES:
        tasks.append({
            "id": next_id(),
            "category": "asset_search",
            "namespace": "project",
            "action": "search",
            "tool": "project_query",
            "arguments": {"action": "search", "query": query},
            "expected": {"valid_response": True, "min_results": 0},
            "safety": "read_only",
        })

    # --- gameplay_tag_lookup: list_gameplay_tags (10 tasks) ---
    list_tag_variants: List[Dict[str, Any]] = [
        {"action": "list_gameplay_tags"},
        {"action": "list_gameplay_tags", "limit": 10},
        {"action": "list_gameplay_tags", "limit": 50},
        {"action": "list_gameplay_tags", "limit": 100},
    ]
    for prefix in GAMEPLAY_TAG_PREFIXES:
        list_tag_variants.append({"action": "list_gameplay_tags", "prefix": prefix})

    for args in list_tag_variants:
        tasks.append({
            "id": next_id(),
            "category": "gameplay_tag_lookup",
            "namespace": "project",
            "action": "list_gameplay_tags",
            "tool": "project_query",
            "arguments": args,
            "expected": {"valid_response": True},
            "safety": "read_only",
        })

    # --- gameplay_tag_lookup: search_gameplay_tags (10 tasks) ---
    for query in GAMEPLAY_TAG_SEARCH_QUERIES:
        tasks.append({
            "id": next_id(),
            "category": "gameplay_tag_lookup",
            "namespace": "project",
            "action": "search_gameplay_tags",
            "tool": "project_query",
            "arguments": {"action": "search_gameplay_tags", "query": query},
            "expected": {"valid_response": True},
            "safety": "read_only",
        })

    # --- health_check (5 tasks) ---
    tasks.append({
        "id": next_id(),
        "category": "health_check",
        "namespace": "project",
        "action": "health",
        "tool": "project_query",
        "arguments": {"action": "health"},
        "expected": {"fields": ["status"]},
        "safety": "read_only",
    })
    tasks.append({
        "id": next_id(),
        "category": "health_check",
        "namespace": "project",
        "action": "health",
        "tool": "project_query",
        "arguments": {"action": "health", "include_counts": True},
        "expected": {"fields": ["status", "total_assets"]},
        "safety": "read_only",
    })
    tasks.append({
        "id": next_id(),
        "category": "stats_check",
        "namespace": "project",
        "action": "get_stats",
        "tool": "project_query",
        "arguments": {"action": "get_stats"},
        "expected": {"fields": ["success", "indexing", "stats"]},
        "safety": "read_only",
    })
    tasks.append({
        "id": next_id(),
        "category": "health_check",
        "namespace": "project",
        "action": "health",
        "tool": "project_query",
        "arguments": {"action": "health", "detail": "full"},
        "expected": {"fields": ["status"]},
        "safety": "read_only",
    })
    tasks.append({
        "id": next_id(),
        "category": "health_check",
        "namespace": "project",
        "action": "health",
        "tool": "project_query",
        "arguments": {"action": "health", "include_counts": False},
        "expected": {"fields": ["status"]},
        "safety": "read_only",
    })

    # --- schema_field_presence: monolith_discover schema for 25 project actions ---
    for action in SCHEMA_ACTIONS:
        tasks.append({
            "id": next_id(),
            "category": "schema_field_presence",
            "namespace": "project",
            "action": action,
            "tool": "monolith_discover",
            "arguments": {"action": action, "mode": "schema", "namespace": "project"},
            "expected": {"requires_planning_signals": True, "requires_skill": True},
            "safety": "read_only_discovery",
        })

    append_project_search_tasks(tasks, next_id, _PRESERVED_ASSET_SEARCH_QUERIES_20260617)
    append_project_gameplay_tag_tasks(
        tasks,
        next_id,
        prefixes=_PRESERVED_GAMEPLAY_TAG_PREFIXES_20260617,
        queries=_PRESERVED_GAMEPLAY_TAG_SEARCH_QUERIES_20260617,
    )
    append_project_schema_tasks(tasks, next_id, _PRESERVED_SCHEMA_ACTIONS_20260617)
    append_project_health_tasks(tasks, next_id, _PRESERVED_HEALTH_VARIANTS_20260617)

    append_project_search_tasks(tasks, next_id, _ADDED_ASSET_SEARCH_QUERIES_20260617)
    append_project_gameplay_tag_tasks(
        tasks,
        next_id,
        prefixes=_ADDED_GAMEPLAY_TAG_PREFIXES_20260617,
        queries=_ADDED_GAMEPLAY_TAG_SEARCH_QUERIES_20260617,
    )
    append_project_health_tasks(tasks, next_id, _ADDED_HEALTH_VARIANTS_20260617)
    append_project_search_tasks(tasks, next_id, _LOG_DERIVED_ASSET_SEARCH_QUERIES_20260617)

    # --- known_answer: ground-truth recall fixtures (require_results, HIT-checked) ---
    append_project_known_answer_tasks(tasks, next_id, _KNOWN_ANSWER_FIXTURES_20260618)

    return dedupe_tasks(tasks, "PIB")


def generate_tasks(tasks_path: pathlib.Path, manifest_path: pathlib.Path) -> Dict[str, Any]:
    """Generate task fixtures and write to tasks_path. Write manifest to manifest_path."""
    tasks_path = resolve_plugin_path(tasks_path)
    manifest_path = resolve_plugin_path(manifest_path)
    tasks = build_static_tasks()

    write_jsonl(tasks_path, tasks)

    manifest = {
        "benchmark": "ProjectIndex",
        "description": "Measures project namespace quality: asset search recall, gameplay tag lookup, schema adherence",
        "primary_score": "project_index_score",
        "expected_namespace": "project",
        "generated_at": utc_now(),
        "task_count": len(tasks),
        "category_counts": count_by(tasks, "category"),
        "score_formula": "0.25*search_hit_rate + 0.20*known_answer_hit_rate + 0.20*field_completeness_rate + 0.15*schema_adherence_rate + 0.10*(1-stale_rate) + 0.10*error_free_rate",
        "score_dimensions": [
            "search_hit_rate",
            "known_answer_hit_rate",
            "field_completeness_rate",
            "schema_adherence_rate",
            "stale_rate",
            "error_free_rate",
        ],
        "all_empty_score_cap": ALL_EMPTY_SCORE_CAP,
        "task_file": display_path(tasks_path),
    }
    write_json(manifest_path, manifest)
    return manifest


# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------

def run_benchmark(
    url: str,
    tasks_path: pathlib.Path,
    output_dir: pathlib.Path,
    label: str,
    timeout_s: float,
) -> Dict[str, Any]:
    tasks_path = resolve_plugin_path(tasks_path)
    tasks = load_jsonl(tasks_path)
    output_dir.mkdir(parents=True, exist_ok=True)
    status_response = mcp_call(url, "monolith_status", {}, timeout_s=timeout_s)
    status = result_data(status_response)
    benchmark_inputs = build_benchmark_inputs("ProjectIndex", tasks_path=tasks_path, mcp_status=status)

    rows: List[Dict[str, Any]] = []
    per_task_jsonl = output_dir / "per_task.jsonl"
    if per_task_jsonl.exists():
        per_task_jsonl.unlink()

    for index, task in enumerate(tasks, 1):
        row = score_task(url, task, timeout_s)
        rows.append(row)
        with per_task_jsonl.open("a", encoding="utf-8", newline="\n") as handle:
            handle.write(json.dumps(row, ensure_ascii=False, sort_keys=True))
            handle.write("\n")
        print(
            f"[{index}/{len(tasks)}] {row['task_id']} success={row['direct_success']}",
            flush=True,
        )
        if index == 1 or index == len(tasks) or index % 10 == 0:
            partial = aggregate(label, status, tasks[:index], rows)
            partial["completed_task_count"] = index
            partial["total_task_count"] = len(tasks)
            attach_benchmark_inputs(partial, benchmark_inputs)
            write_json(output_dir / "partial_summary.json", partial)

    summary = aggregate(label, status, tasks, rows)
    attach_benchmark_inputs(summary, benchmark_inputs)
    write_json(output_dir / "summary.json", summary)
    write_json(output_dir / "per_task.json", rows)
    return summary


# ---------------------------------------------------------------------------
# Compare
# ---------------------------------------------------------------------------

def compare_runs(baseline_path: pathlib.Path, current_path: pathlib.Path, output_dir: pathlib.Path) -> Dict[str, Any]:
    baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
    current = json.loads(current_path.read_text(encoding="utf-8"))
    base_metrics = baseline["metrics"]
    cur_metrics = current["metrics"]
    deltas: Dict[str, float] = {}
    for key, cur_value in cur_metrics.items():
        base_value = base_metrics.get(key)
        if isinstance(cur_value, (int, float)) and isinstance(base_value, (int, float)):
            deltas[key] = round(cur_value - base_value, 6)
    comparison = {
        "created_at": utc_now(),
        "baseline": baseline,
        "current": current,
        "deltas": deltas,
    }
    output_dir.mkdir(parents=True, exist_ok=True)
    write_json(output_dir / "comparison.json", comparison)
    write_comparison_markdown(output_dir / "comparison.md", comparison)
    return comparison


def write_comparison_markdown(path: pathlib.Path, comparison: Dict[str, Any]) -> None:
    baseline = comparison["baseline"]
    current = comparison["current"]
    deltas = comparison["deltas"]
    metrics = [
        "project_index_score",
        "search_hit_rate",
        "known_answer_hit_rate",
        "field_completeness_rate",
        "schema_adherence_rate",
        "stale_rate",
        "error_free_rate",
    ]
    lines = [
        "# Monolith ProjectIndex Benchmark Comparison",
        "",
        f"- Created: `{comparison['created_at']}`",
        f"- Baseline: `{baseline['label']}`",
        f"- Current: `{current['label']}`",
        f"- Task count: `{current['task_count']}`",
        "",
        "| Metric | Baseline | Current | Delta |",
        "| --- | ---: | ---: | ---: |",
    ]
    for metric in metrics:
        base_value = baseline["metrics"].get(metric)
        cur_value = current["metrics"].get(metric)
        delta = deltas.get(metric)
        lines.append(f"| `{metric}` | {base_value} | {cur_value} | {delta} |")
    lines.append("")
    lines.append("Higher is better for all metrics except `stale_rate`.")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)

    gen = sub.add_parser("generate", help="Generate task fixtures for the project namespace benchmark")
    gen.add_argument("--tasks", type=pathlib.Path, default=DEFAULT_TASKS)
    gen.add_argument("--manifest", type=pathlib.Path, default=DEFAULT_MANIFEST)

    run_cmd = sub.add_parser("run", help="Run tasks against a live MCP endpoint and score results")
    run_cmd.add_argument("--mcp-url", default=DEFAULT_MCP_URL)
    run_cmd.add_argument("--tasks", type=pathlib.Path, default=DEFAULT_TASKS)
    run_cmd.add_argument("--output-dir", type=pathlib.Path, required=True)
    run_cmd.add_argument("--label", required=True)
    run_cmd.add_argument("--request-timeout-s", type=float, default=12.0)

    cmp_cmd = sub.add_parser("compare", help="Compare two run summary files")
    cmp_cmd.add_argument("--baseline", type=pathlib.Path, required=True)
    cmp_cmd.add_argument("--current", type=pathlib.Path, required=True)
    cmp_cmd.add_argument("--output-dir", type=pathlib.Path, required=True)

    args = parser.parse_args(argv)

    if args.cmd == "generate":
        manifest = generate_tasks(args.tasks, args.manifest)
        print(json.dumps(manifest, indent=2, ensure_ascii=False))
        return 0

    if args.cmd == "run":
        summary = run_benchmark(args.mcp_url, args.tasks, args.output_dir, args.label, args.request_timeout_s)
        print(json.dumps(summary, indent=2, ensure_ascii=False))
        return 0

    if args.cmd == "compare":
        comparison = compare_runs(args.baseline, args.current, args.output_dir)
        print(json.dumps({"output_dir": str(args.output_dir), "deltas": comparison["deltas"]}, indent=2))
        return 0

    return 2


if __name__ == "__main__":
    raise SystemExit(main())
