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

    direct_success = False
    results_count = 0
    field_complete_count = 0
    stale = False
    planning_signals = False
    evidence: Dict[str, Any] = {}

    if category in ("asset_search", "gameplay_tag_lookup"):
        results = project_results(data)
        results_count = len(results)
        # Lenient scoring: empty result set is OK if the response is a valid non-error object
        valid_resp = is_valid_non_error_response(data, response)
        direct_success = valid_resp
        field_complete_count = sum(1 for r in results if result_has_project_fields(r))
        evidence = {
            "results_count": results_count,
            "field_complete_count": field_complete_count,
            "valid_response": valid_resp,
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

def aggregate(label: str, status: Dict[str, Any], tasks: List[Dict[str, Any]], rows: List[Dict[str, Any]]) -> Dict[str, Any]:
    search_rows = [r for r in rows if r["category"] in ("asset_search", "gameplay_tag_lookup")]
    health_rows = [r for r in rows if r["category"] == "health_check"]
    stats_rows = [r for r in rows if r["category"] == "stats_check"]
    schema_rows = [r for r in rows if r["category"] == "schema_field_presence"]

    # search_hit_rate: fraction of search rows where direct_success (lenient -- empty OK if valid response)
    search_hit_rate = avg([1.0 if r["direct_success"] else 0.0 for r in search_rows])

    # field_completeness_rate: fraction of actual result items with >=2 required fields present
    # If no results anywhere, vacuously 1.0
    total_results = sum(r["results_count"] for r in search_rows if r["results_count"] > 0)
    total_complete = sum(r["field_complete_count"] for r in search_rows if r["results_count"] > 0)
    field_completeness_rate = total_complete / total_results if total_results > 0 else 1.0

    # schema_adherence_rate: fraction of schema_field_presence tasks with planning_signals present
    schema_adherence_rate = avg([1.0 if r["planning_signals"] else 0.0 for r in schema_rows])

    # stale_rate: fraction of health_check tasks that are stale or error
    stale_rate = avg([1.0 if r["stale"] else 0.0 for r in health_rows])

    # stats_check_rate: fraction of get_stats tasks returning the success/indexing/stats
    # contract. Informational only -- not folded into project_index_score so the
    # existing weighted formula and its weights stay unchanged.
    stats_check_rate = avg([1.0 if r["direct_success"] else 0.0 for r in stats_rows])

    # project_index_score
    project_index_score = (
        0.40 * search_hit_rate
        + 0.30 * field_completeness_rate
        + 0.20 * schema_adherence_rate
        + 0.10 * (1.0 - stale_rate)
    )

    error_count = sum(1 for r in rows if r.get("transport_error") or r.get("response_is_error"))

    return {
        "label": label,
        "created_at": utc_now(),
        "mcp_status": status,
        "task_count": len(rows),
        "error_count": error_count,
        "category_counts": count_by(tasks, "category"),
        "metrics": {
            "project_index_score": round(project_index_score, 6),
            "search_hit_rate": round(search_hit_rate, 6),
            "field_completeness_rate": round(field_completeness_rate, 6),
            "schema_adherence_rate": round(schema_adherence_rate, 6),
            "stale_rate": round(stale_rate, 6),
            "stats_check_rate": round(stats_check_rate, 6),
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

    # Re-assign IDs to be monotonic after any additions.
    for index, task in enumerate(tasks, 1):
        task["id"] = f"PIB-{index:03d}"

    return tasks


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
        "score_formula": "0.40*search_hit_rate + 0.30*field_completeness_rate + 0.20*schema_adherence_rate + 0.10*(1-stale_rate)",
        "score_dimensions": [
            "search_hit_rate",
            "field_completeness_rate",
            "schema_adherence_rate",
            "stale_rate",
        ],
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
        "field_completeness_rate",
        "schema_adherence_rate",
        "stale_rate",
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
