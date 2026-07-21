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

from benchmark_common import (
    benchmark_routing_context,
    DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES,
    DEFAULT_MAX_TRANSPORT_FAILED_FRACTION,
    DEFAULT_MIN_TRANSPORT_FRACTION_SAMPLES,
    TaskCorpus,
    TransportFailureTracker,
    attach_benchmark_inputs,
    build_benchmark_inputs,
    classify_mcp_protocol_failure,
    display_path,
    load_task_corpus,
    paginate_discover_action_names,
    resolve_plugin_path,
    status_identity,
    status_identity_mismatches,
    task_corpus_metadata,
    validate_mcp_status_response,
)

DEFAULT_MCP_URL = "http://localhost:9316/mcp"
DEFAULT_TASKS = pathlib.Path("Benchmarks/ProjectIndex/tasks.jsonl")
DEFAULT_MANIFEST = pathlib.Path("Benchmarks/ProjectIndex/manifest.json")
DEFAULT_RESULTS_ROOT = pathlib.Path("Saved/Monolith/Benchmarks/ProjectIndex")
# Project-derived fixtures (schema action list + known-answer recall pairs).
# Written by `refresh_live_fixtures` against the CURRENT project's live index;
# consumed by `generate`. Historical cross-project schema/known-answer snapshots
# are deliberately not executable: fixtures must travel with the project, and
# every task builder call requires the current validated fixture contract.
DEFAULT_LIVE_FIXTURES = pathlib.Path("Benchmarks/ProjectIndex/live_fixtures.json")

PROJECT_INDEX_TASK_CATEGORIES = {
    "asset_search",
    "gameplay_tag_lookup",
    "health_check",
    "known_answer",
    "schema_field_presence",
    "stats_check",
}
# Naming-convention seed prefixes used to derive known-answer candidates.
KNOWN_ANSWER_SEED_PREFIXES = [
    "DA_", "DT_", "BP_", "WBP_", "IA_", "IMC_", "L_", "BT_", "BB_", "SM_", "M_", "T_",
]

# Mutable benchmark assets are created, edited, and deleted by other benchmark
# suites. They may exist in ProjectIndex only because another benchmark ran
# first, so they can never be clean-checkout known answers even if a particular
# fixture eventually becomes source-controlled.
KNOWN_ANSWER_MUTABLE_PATH_PREFIXES = (
    "/Game/Benchmarks/",
)

LIVE_FIXTURE_SCHEMA_VERSION = 2
LIVE_FIXTURE_SELECTION_POLICY = "deterministic_prefix_round_robin"

# Required fields for project search result rows (from CLAUDE.md project search contract)
PROJECT_REQUIRED_FIELDS = {"match_object_path", "match_value", "match_source"}

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
    "AssetEditing",
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


# Declares this traffic as synthetic benchmark fixtures so the invocation-log
# analyzer does not report deliberate negative probes as real, unmet demand.
_BENCHMARK_ROUTING_CONTEXT = benchmark_routing_context("ProjectIndex")


def mcp_call(url: str, tool: str, arguments: Dict[str, Any], timeout_s: float = 45.0) -> Dict[str, Any]:
    body = {
        "jsonrpc": "2.0",
        "id": int(time.time() * 1000) % 1000000000,
        "method": "tools/call",
        "params": {
            "name": tool,
            "arguments": arguments,
        },
        "_monolith_routing_context": _BENCHMARK_ROUTING_CONTEXT,
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
    if not isinstance(parsed, dict):
        return {
            "protocol_error": True,
            "raw": raw,
            "error": "MCP response top-level JSON must be an object",
            "request": body,
        }
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


def normalize_mcp_response(response: Any, context: str) -> Dict[str, Any]:
    """Return one structured response object even for an injected bad runner value."""
    if isinstance(response, dict):
        return response
    return {
        "protocol_error": True,
        "raw": str(response)[:500],
        "error": f"{context} response top-level JSON must be an object",
    }


def mcp_protocol_failure_kind(response: Dict[str, Any]) -> str:
    """Classify failures that invalidate a run instead of measuring index quality."""
    if response.get("parse_error"):
        return "parse_error"
    if response.get("protocol_error"):
        return "protocol_error"
    if response.get("error") is not None:
        return "mcp_protocol_error"
    return classify_mcp_protocol_failure(response)


def valid_status_payload(status: Dict[str, Any]) -> bool:
    """The benchmark may score tasks only against a confirmed running endpoint."""
    return bool(status) and status.get("server_running") is True


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
    """All asset-identity paths present across the result rows.

    The live search contract carries the asset's package path in "asset_path";
    "match_object_path" is the object path of the matching field WITHIN the
    asset (e.g. "MapID" for a variable match) and only equals the package path
    on legacy identity rows. Both are collected so known-answer matching works
    against the current contract without dropping older cached fixtures.
    """
    paths: List[str] = []
    for result in results:
        for key in ("asset_path", "match_object_path"):
            value = result.get(key)
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

def invalid_task_row(
    task: Dict[str, Any],
    failure_kind: str,
    error: str,
    *,
    transport_error: bool = False,
    transport_status: Optional[int] = None,
    raw: str = "",
) -> Dict[str, Any]:
    """Preserve the triggering task without feeding invalid data into scoring."""
    expected = task.get("expected", {}) if isinstance(task.get("expected"), dict) else {}
    return {
        "task_id": task.get("id"),
        "category": task.get("category"),
        "namespace": task.get("namespace"),
        "action": task.get("action"),
        "direct_success": False,
        "results_count": 0,
        "field_complete_count": 0,
        "expected_nonempty": int(expected.get("min_results", 0) or 0) >= 1,
        "known_answer_hit": False,
        "stale": False,
        "planning_signals": False,
        "evidence": {failure_kind: error},
        "transport_error": transport_error,
        "transport_status": transport_status,
        "transport_error_raw": raw[:300] if transport_error else "",
        "response_is_error": False,
        "response_text": "",
        "failure_kind": failure_kind,
        "error": error,
        "protocol_error_raw": raw[:500] if not transport_error else "",
    }


def score_task(url: str, task: Dict[str, Any], timeout_s: float) -> Dict[str, Any]:
    response = normalize_mcp_response(
        mcp_call(url, str(task["tool"]), dict(task.get("arguments", {})), timeout_s=timeout_s),
        str(task.get("tool", "task")),
    )
    protocol_failure_kind = mcp_protocol_failure_kind(response)
    if protocol_failure_kind:
        raw = str(response.get("raw", response))
        return invalid_task_row(
            task,
            protocol_failure_kind,
            str(response.get("error") or raw)[:500],
            raw=raw,
        )
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

    transport_status = response.get("status")
    if not isinstance(transport_status, int) or isinstance(transport_status, bool):
        transport_status = None
    response_is_error = bool(result_payload(response).get("isError"))
    transport_error = bool(response.get("transport_error"))
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
        "transport_error": transport_error,
        "transport_status": transport_status,
        "transport_error_raw": str(response.get("raw", ""))[:300] if transport_error else "",
        "response_is_error": response_is_error,
        "response_text": result_text(response)[:1000],
        "failure_kind": (
            "transport_error" if transport_error else "mcp_error" if response_is_error else ""
        ),
        "error": "",
        "protocol_error_raw": "",
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
    # Environment failures (endpoint died mid-run) are tracked separately from
    # server isError responses: they poison the run rather than measure the
    # index, and the run-integrity gate keys off this count.
    transport_failed_task_count = sum(1 for r in rows if r.get("transport_error"))

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
        "transport_failed_task_count": transport_failed_task_count,
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

def normalized_package_path(path: str) -> str:
    """Normalize an Unreal package path for deterministic policy checks."""
    normalized = str(path or "").strip().replace("\\", "/")
    while "//" in normalized:
        normalized = normalized.replace("//", "/")
    return normalized


def is_mutable_known_answer_path(path: str) -> bool:
    """True when path belongs to a benchmark-owned mutable asset root."""
    candidate = normalized_package_path(path).casefold()
    for prefix in KNOWN_ANSWER_MUTABLE_PATH_PREFIXES:
        normalized_prefix = normalized_package_path(prefix).casefold()
        if candidate == normalized_prefix.rstrip("/") or candidate.startswith(normalized_prefix):
            return True
    return False


def live_fixture_stability_policy() -> Dict[str, Any]:
    """Checked-in contract proving how known-answer candidates were admitted."""
    return {
        "selection": LIVE_FIXTURE_SELECTION_POLICY,
        "excluded_path_prefixes": list(KNOWN_ANSWER_MUTABLE_PATH_PREFIXES),
        "requires_saved_asset_state": True,
        "requires_source_control": True,
    }


def validate_live_fixtures(data: Any, source: str = "live fixtures") -> Dict[str, Any]:
    """Validate the project-local fixture contract or raise RuntimeError.

    Generation is intentionally fail-closed. A missing field, duplicate known
    answer, mutable benchmark path, or pre-stability schema must never fall back
    to a historical fixture snapshot captured on another project.
    """
    if not isinstance(data, dict):
        raise RuntimeError(f"{source}: root must be a JSON object")
    if data.get("schema_version") != LIVE_FIXTURE_SCHEMA_VERSION:
        raise RuntimeError(
            f"{source}: schema_version must be {LIVE_FIXTURE_SCHEMA_VERSION}; "
            "refresh fixtures with the current generator"
        )
    if data.get("benchmark") != "ProjectIndex":
        raise RuntimeError(f"{source}: benchmark must be 'ProjectIndex'")
    for field in ("project_name", "catalog_version", "generated_at"):
        if not isinstance(data.get(field), str) or not str(data.get(field)).strip():
            raise RuntimeError(f"{source}: '{field}' must be a non-empty string")

    seed_prefixes = data.get("seed_prefixes")
    if seed_prefixes != KNOWN_ANSWER_SEED_PREFIXES:
        raise RuntimeError(
            f"{source}: seed_prefixes do not match the current deterministic selection contract"
        )

    policy = data.get("stability_policy")
    expected_policy = live_fixture_stability_policy()
    if policy != expected_policy:
        raise RuntimeError(
            f"{source}: stability_policy must equal {json.dumps(expected_policy, sort_keys=True)}"
        )

    schema_actions = data.get("schema_actions")
    if not isinstance(schema_actions, list) or not schema_actions:
        raise RuntimeError(f"{source}: schema_actions must be a non-empty list")
    normalized_actions = [str(action).strip() for action in schema_actions]
    if any(not action for action in normalized_actions):
        raise RuntimeError(f"{source}: schema_actions cannot contain empty names")
    if len(set(normalized_actions)) != len(normalized_actions):
        raise RuntimeError(f"{source}: schema_actions must be unique")

    rows = data.get("known_answers")
    if not isinstance(rows, list) or not rows:
        raise RuntimeError(f"{source}: known_answers must be a non-empty list")
    seen_queries: set[str] = set()
    seen_paths: set[str] = set()
    for index, row in enumerate(rows):
        row_source = f"{source}: known_answers[{index}]"
        if not isinstance(row, dict):
            raise RuntimeError(f"{row_source} must be an object")
        query = str(row.get("query") or "").strip()
        path = normalized_package_path(str(row.get("expected_object_path") or ""))
        if not query or not path.startswith("/"):
            raise RuntimeError(f"{row_source} requires a non-empty query and absolute package path")
        if path.rsplit("/", 1)[-1] != query:
            raise RuntimeError(f"{row_source} query must equal the package asset name")
        if is_mutable_known_answer_path(path):
            raise RuntimeError(f"{row_source} uses excluded mutable benchmark path '{path}'")
        query_key = query.casefold()
        path_key = path.casefold()
        if query_key in seen_queries:
            raise RuntimeError(f"{row_source} duplicates query '{query}'")
        if path_key in seen_paths:
            raise RuntimeError(f"{row_source} duplicates path '{path}'")
        seen_queries.add(query_key)
        seen_paths.add(path_key)

    return data

def build_static_tasks(live_fixtures: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Build deterministic tasks from a required validated project fixture."""
    live_fixtures = validate_live_fixtures(
        live_fixtures, "build_static_tasks live fixtures"
    )
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

    # --- schema_field_presence: current live project actions only ---
    schema_actions = [str(action) for action in live_fixtures["schema_actions"]]
    for action in schema_actions:
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
    known_answers = [
        (str(row["query"]), str(row["expected_object_path"]))
        for row in live_fixtures["known_answers"]
    ]
    append_project_known_answer_tasks(tasks, next_id, known_answers)

    return dedupe_tasks(tasks, "PIB")


def load_live_fixtures(path: pathlib.Path) -> Dict[str, Any]:
    """Load and strictly validate the required project-derived fixtures file."""
    resolved = resolve_plugin_path(path)
    if not resolved.is_file():
        raise RuntimeError(
            f"required live fixtures file is missing: {display_path(resolved)}; "
            "run refresh_live_fixtures before generate"
        )
    try:
        data = json.loads(resolved.read_text(encoding="utf-8"))
    except OSError as exc:
        raise RuntimeError(f"failed to read live fixtures {display_path(resolved)}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"invalid JSON in live fixtures {display_path(resolved)}: {exc}") from exc
    return validate_live_fixtures(data, display_path(resolved))


def generate_tasks(
    tasks_path: pathlib.Path,
    manifest_path: pathlib.Path,
    live_fixtures_path: pathlib.Path = DEFAULT_LIVE_FIXTURES,
) -> Dict[str, Any]:
    """Generate tasks from required validated live fixtures and write the manifest."""
    tasks_path = resolve_plugin_path(tasks_path)
    manifest_path = resolve_plugin_path(manifest_path)
    live_fixtures = load_live_fixtures(live_fixtures_path)
    tasks = build_static_tasks(live_fixtures)

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
    manifest["live_fixtures"] = {
        "file": display_path(resolve_plugin_path(live_fixtures_path)),
        "schema_version": live_fixtures.get("schema_version"),
        "project_name": live_fixtures.get("project_name"),
        "catalog_version": live_fixtures.get("catalog_version"),
        "generated_at": live_fixtures.get("generated_at"),
        "schema_action_count": len(live_fixtures.get("schema_actions") or []),
        "known_answer_count": len(live_fixtures.get("known_answers") or []),
        "stability_policy": live_fixtures.get("stability_policy"),
    }
    write_json(manifest_path, manifest)
    return manifest


# ---------------------------------------------------------------------------
# Live fixture derivation (refresh_live_fixtures)
# ---------------------------------------------------------------------------

def require_mcp_data(response: Dict[str, Any], context: str) -> Dict[str, Any]:
    """Return parsed MCP data or fail the refresh instead of writing partial fixtures."""
    if response.get("transport_error"):
        raise RuntimeError(f"{context}: transport failure: {str(response.get('raw', ''))[:200]}")
    if response.get("error") is not None:
        raise RuntimeError(f"{context}: JSON-RPC error: {str(response.get('error'))[:200]}")
    payload = result_payload(response)
    if payload.get("isError"):
        raise RuntimeError(f"{context}: MCP action error: {result_text(response)[:200]}")
    data = result_data(response)
    if not isinstance(data, dict):
        raise RuntimeError(f"{context}: MCP response did not contain an object result")
    return data


def candidate_exists_on_disk(
    url: str,
    timeout_s: float,
    package_path: str,
) -> bool:
    """Require project.get_saved_asset_state to prove a non-empty package file exists."""
    response = mcp_call(
        url,
        "project_query",
        {"action": "get_saved_asset_state", "asset_path": package_path},
        timeout_s=timeout_s,
    )
    if response.get("transport_error") or response.get("error") is not None:
        require_mcp_data(response, f"get_saved_asset_state({package_path})")
    payload = result_payload(response)
    if payload.get("isError"):
        # A stale ProjectIndex row may legitimately point at a package that no
        # longer exists. Reject that candidate and continue with stable rows.
        return False
    data = result_data(response)
    state = data.get("asset_state") if isinstance(data, dict) else None
    if not isinstance(state, dict):
        raise RuntimeError(
            f"get_saved_asset_state({package_path}): missing asset_state object"
        )
    state_path = normalized_package_path(str(state.get("package_path") or ""))
    try:
        file_size = float(state.get("file_size", -1))
    except (TypeError, ValueError):
        file_size = -1
    return (
        state.get("exists_on_disk") is True
        and file_size > 0
        and state_path.casefold() == normalized_package_path(package_path).casefold()
    )


def candidate_is_submitted_source_control_asset(
    url: str,
    timeout_s: float,
    package_path: str,
) -> bool:
    """Require a current, submitted source-control state for one package path.

    Provider unavailability is a run-level failure: skipping the check would
    allow local adds back into checked-in fixtures. Individual untracked,
    added, deleted, ignored, or conflicted candidates are simply rejected.
    """
    response = mcp_call(
        url,
        "source_control_query",
        {"action": "get_status", "paths": [package_path]},
        timeout_s=timeout_s,
    )
    data = require_mcp_data(response, f"source_control.get_status({package_path})")
    provider = data.get("provider")
    if (
        data.get("available") is not True
        or not isinstance(provider, dict)
        or provider.get("enabled") is not True
        or provider.get("available") is not True
    ):
        provider_name = provider.get("name") if isinstance(provider, dict) else "unknown"
        raise RuntimeError(
            f"source-control provider unavailable while validating known-answer fixtures "
            f"(provider={provider_name}); refusing to overwrite fixtures"
        )

    states = data.get("states")
    if not isinstance(states, list) or len(states) != 1 or not isinstance(states[0], dict):
        raise RuntimeError(
            f"source_control.get_status({package_path}): expected exactly one state row"
        )
    state = states[0]
    return (
        state.get("state_known") is True
        and state.get("source_controlled") is True
        and state.get("current") is True
        and state.get("added") is not True
        and state.get("deleted") is not True
        and state.get("ignored") is not True
        and state.get("conflicted") is not True
    )


def verify_known_answer_candidate(
    url: str,
    timeout_s: float,
    name: str,
    package_path: str,
) -> bool:
    """Verify clean-checkout stability plus the benchmark-shaped identity hit."""
    if is_mutable_known_answer_path(package_path):
        return False
    if not candidate_exists_on_disk(url, timeout_s, package_path):
        return False
    if not candidate_is_submitted_source_control_asset(url, timeout_s, package_path):
        return False
    response = mcp_call(
        url,
        "project_query",
        {"action": "search", "query": name},
        timeout_s=timeout_s,
    )
    data = require_mcp_data(response, f"project.search({name})")
    return known_answer_hit(project_results(data), package_path)


def collect_known_answer_candidate_pools(
    url: str,
    timeout_s: float,
    min_name_length: int,
) -> Dict[str, List[Tuple[str, str]]]:
    """Collect and deterministically sort identity candidates per seed prefix."""
    pools: Dict[str, List[Tuple[str, str]]] = {}
    for prefix in KNOWN_ANSWER_SEED_PREFIXES:
        response = mcp_call(
            url,
            "project_query",
            {"action": "search", "query": prefix, "limit": 50, "include_content": False},
            timeout_s=timeout_s,
        )
        data = require_mcp_data(response, f"project.search seed prefix {prefix}")
        candidates: Dict[Tuple[str, str], Tuple[str, str]] = {}
        for row in project_results(data):
            name = row.get("asset_name")
            path = row.get("asset_path")
            if not isinstance(name, str) or not isinstance(path, str):
                continue
            name = name.strip()
            path = normalized_package_path(path)
            if not name.casefold().startswith(prefix.casefold()) or len(name) < min_name_length:
                continue
            if not path.startswith("/") or is_mutable_known_answer_path(path):
                continue
            key = (name.casefold(), path.casefold())
            candidates[key] = (name, path)
        pools[prefix] = [candidates[key] for key in sorted(candidates)]
    return pools

def derive_known_answer_fixtures(
    url: str,
    timeout_s: float,
    target_count: int,
    per_prefix_cap: Optional[int] = None,
    min_name_length: int = 8,
) -> List[Dict[str, str]]:
    """Derive stable, prefix-balanced known-answer fixture pairs.

    Candidate pools are sorted independently of live search ordering, then
    selected one per prefix per round. Every admitted row must exist on disk,
    be current and submitted in the active source-control provider, avoid
    mutable benchmark roots, and pass the exact content-inclusive search used
    by the benchmark task.
    """
    if target_count < 1:
        raise RuntimeError("known-answer target_count must be at least 1")
    if per_prefix_cap is None:
        # Continue balanced rounds until the target can be filled even when
        # some prefixes have no stable submitted assets in the current project.
        per_prefix_cap = target_count
    if per_prefix_cap < 1:
        raise RuntimeError("known-answer per_prefix_cap must be at least 1")

    pools = collect_known_answer_candidate_pools(url, timeout_s, min_name_length)
    fixtures: List[Dict[str, str]] = []
    seen_names: set[str] = set()
    seen_paths: set[str] = set()
    cursors = {prefix: 0 for prefix in KNOWN_ANSWER_SEED_PREFIXES}

    for _round in range(per_prefix_cap):
        made_progress = False
        for prefix in KNOWN_ANSWER_SEED_PREFIXES:
            pool = pools.get(prefix, [])
            while cursors[prefix] < len(pool):
                name, path = pool[cursors[prefix]]
                cursors[prefix] += 1
                name_key = name.casefold()
                path_key = path.casefold()
                if name_key in seen_names or path_key in seen_paths:
                    continue
                if not verify_known_answer_candidate(url, timeout_s, name, path):
                    continue
                fixtures.append({"query": name, "expected_object_path": path})
                seen_names.add(name_key)
                seen_paths.add(path_key)
                made_progress = True
                break
            if len(fixtures) >= target_count:
                return fixtures
        if not made_progress:
            break
    return fixtures


def refresh_live_fixtures(
    url: str,
    fixtures_path: pathlib.Path,
    timeout_s: float,
    known_answer_count: int,
    min_known_answers: int,
) -> Dict[str, Any]:
    """Derive and write the project-local fixtures file from the live index.

    Raises RuntimeError when the derivation is too thin to be a trustworthy
    corpus (no schema actions, or fewer than min_known_answers verified
    pairs) — a thin fixtures file must never silently replace a full one.
    """
    if min_known_answers < 1:
        raise RuntimeError("min_known_answers must be at least 1")
    if known_answer_count < min_known_answers:
        raise RuntimeError("known_answer_count must be >= min_known_answers")

    status_response = mcp_call(url, "monolith_status", {}, timeout_s=timeout_s)
    status = require_mcp_data(status_response, f"monolith_status at {url}")
    if not status:
        raise RuntimeError(f"monolith_status returned no data at {url}")

    def fetch_page(arguments: Dict[str, Any]) -> Dict[str, Any]:
        response = mcp_call(url, "monolith_discover", arguments, timeout_s=timeout_s)
        return require_mcp_data(response, "monolith_discover(project actions)")

    schema_actions = paginate_discover_action_names(fetch_page, "project")
    if not schema_actions:
        raise RuntimeError("live project namespace enumerated 0 actions — refusing to write fixtures")

    known_answers = derive_known_answer_fixtures(url, timeout_s, known_answer_count)
    if len(known_answers) < min_known_answers:
        raise RuntimeError(
            f"only {len(known_answers)} verified known-answer fixtures derived "
            f"(minimum {min_known_answers}) — index too thin or search contract drifted; "
            "refusing to write fixtures"
        )

    fixtures = {
        "schema_version": LIVE_FIXTURE_SCHEMA_VERSION,
        "benchmark": "ProjectIndex",
        "project_name": status.get("project_name"),
        "catalog_version": status.get("catalog_version"),
        "generated_at": utc_now(),
        "seed_prefixes": list(KNOWN_ANSWER_SEED_PREFIXES),
        "stability_policy": live_fixture_stability_policy(),
        "schema_actions": schema_actions,
        "known_answers": known_answers,
    }
    resolved = resolve_plugin_path(fixtures_path)
    validate_live_fixtures(fixtures, display_path(resolved))
    write_json(resolved, fixtures)
    return fixtures


# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------

RUN_OUTPUT_FILENAMES = (
    "summary.json",
    "partial_summary.json",
    "per_task.json",
    "per_task.jsonl",
    "run_failure.json",
)


def clear_known_run_outputs(output_dir: pathlib.Path) -> None:
    """Remove stale success/failure artifacts before a new run starts."""
    output_dir.mkdir(parents=True, exist_ok=True)
    for name in RUN_OUTPUT_FILENAMES:
        path = output_dir / name
        if path.exists():
            path.unlink()


def write_invalid_run_artifacts(output_dir: pathlib.Path, failure: Dict[str, Any]) -> None:
    """Invalid runs always publish diagnostics, never a normal final summary."""
    failure["run_valid"] = False
    failure["metrics_valid"] = False
    write_json(output_dir / "run_failure.json", failure)
    write_json(output_dir / "partial_summary.json", failure)


def attach_run_context(
    payload: Dict[str, Any],
    corpus: TaskCorpus,
    start_identity: Dict[str, str],
    end_identity: Optional[Dict[str, str]] = None,
) -> Dict[str, Any]:
    payload["task_corpus"] = task_corpus_metadata(corpus)
    payload["comparison_valid"] = corpus.comparable
    payload["status_identity_start"] = start_identity
    if end_identity is not None:
        payload["status_identity_end"] = end_identity
    return payload


def validate_live_fixture_runtime_provenance(
    corpus: TaskCorpus,
    status: Dict[str, Any],
) -> Dict[str, Any]:
    """Validate canonical fixture provenance against the live MCP identity.

    A global catalog-version change may include contract changes in ``project``
    schemas even when the action-name count happens to stay constant. Comparable
    canonical runs therefore require fixtures refreshed from the exact current
    catalog. Explicit diagnostic subsets remain non-comparable and are not bound
    to the canonical fixture manifest.
    """
    if not corpus.canonical:
        return {"ok": True, "checked": False, "reason": "explicit_subset"}

    fixture_metadata = corpus.manifest.get("live_fixtures")
    if not isinstance(fixture_metadata, dict):
        return {
            "ok": False,
            "checked": True,
            "failure_kind": "invalid_fixture_provenance",
            "message": "ProjectIndex canonical manifest has no live_fixtures object",
        }

    expected_catalog_version = str(fixture_metadata.get("catalog_version", "")).strip()
    observed_catalog_version = str(status.get("catalog_version", "")).strip()
    expected_project_name = str(fixture_metadata.get("project_name", "")).strip()
    observed_project_name = str(status.get("project_name", "")).strip()
    expected_schema_version = fixture_metadata.get("schema_version")

    if expected_schema_version != LIVE_FIXTURE_SCHEMA_VERSION:
        return {
            "ok": False,
            "checked": True,
            "failure_kind": "invalid_fixture_provenance",
            "message": (
                "ProjectIndex canonical fixture schema_version mismatch: "
                f"expected {LIVE_FIXTURE_SCHEMA_VERSION}, got {expected_schema_version!r}"
            ),
            "expected_fixture_schema_version": LIVE_FIXTURE_SCHEMA_VERSION,
            "observed_fixture_schema_version": expected_schema_version,
        }
    if not expected_catalog_version or not observed_catalog_version:
        return {
            "ok": False,
            "checked": True,
            "failure_kind": "invalid_fixture_provenance",
            "message": "ProjectIndex fixture/runtime catalog_version must both be non-empty",
            "expected_catalog_version": expected_catalog_version,
            "observed_catalog_version": observed_catalog_version,
        }
    if expected_catalog_version != observed_catalog_version:
        return {
            "ok": False,
            "checked": True,
            "failure_kind": "catalog_version_mismatch",
            "message": (
                "ProjectIndex live fixtures are stale for the current MCP catalog "
                f"({expected_catalog_version} -> {observed_catalog_version}); run "
                "refresh_live_fixtures against a healthy endpoint, then regenerate the corpus"
            ),
            "expected_catalog_version": expected_catalog_version,
            "observed_catalog_version": observed_catalog_version,
        }
    if not expected_project_name or not observed_project_name:
        return {
            "ok": False,
            "checked": True,
            "failure_kind": "invalid_fixture_provenance",
            "message": "ProjectIndex fixture/runtime project_name must both be non-empty",
            "expected_project_name": expected_project_name,
            "observed_project_name": observed_project_name,
        }
    if expected_project_name != observed_project_name:
        return {
            "ok": False,
            "checked": True,
            "failure_kind": "project_identity_mismatch",
            "message": (
                "ProjectIndex live fixtures belong to a different project "
                f"({expected_project_name} -> {observed_project_name})"
            ),
            "expected_project_name": expected_project_name,
            "observed_project_name": observed_project_name,
        }
    return {
        "ok": True,
        "checked": True,
        "catalog_version": observed_catalog_version,
        "project_name": observed_project_name,
        "fixture_schema_version": expected_schema_version,
    }


def build_attempt_failure(
    label: str,
    status: Dict[str, Any],
    tasks: List[Dict[str, Any]],
    rows: List[Dict[str, Any]],
    tracker: TransportFailureTracker,
    benchmark_inputs: Dict[str, Any],
    corpus: TaskCorpus,
    start_identity: Dict[str, str],
    fields: Dict[str, Any],
) -> Dict[str, Any]:
    try:
        failure = aggregate(label, status, tasks[:len(rows)], rows)
    except Exception as exc:  # noqa: BLE001 - retain a minimal invalid artifact.
        failure = {
            "label": label,
            "created_at": utc_now(),
            "task_count": len(rows),
            "aggregate_error": f"{type(exc).__name__}: {exc}",
        }
    failure.update(fields)
    failure.update(tracker.snapshot())
    attach_benchmark_inputs(failure, benchmark_inputs)
    attach_run_context(failure, corpus, start_identity)
    return failure


def run_benchmark(
    url: str,
    tasks_path: pathlib.Path,
    output_dir: pathlib.Path,
    label: str,
    timeout_s: float,
    max_transport_failed_fraction: float = DEFAULT_MAX_TRANSPORT_FAILED_FRACTION,
    max_consecutive_transport_failures: int = DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES,
    min_transport_fraction_sample: int = DEFAULT_MIN_TRANSPORT_FRACTION_SAMPLES,
    allow_subset: bool = False,
) -> Dict[str, Any]:
    clear_known_run_outputs(output_dir)
    try:
        corpus = load_task_corpus(
            tasks_path,
            suite="ProjectIndex",
            canonical_tasks_path=DEFAULT_TASKS,
            canonical_manifest_path=DEFAULT_MANIFEST,
            allow_subset=allow_subset,
            allowed_categories=PROJECT_INDEX_TASK_CATEGORIES,
            require_arguments=True,
        )
        tasks_path = resolve_plugin_path(tasks_path)
        tasks = corpus.tasks
    except Exception as exc:  # noqa: BLE001 - task corpus defects invalidate the run.
        failure = {
            "label": label,
            "created_at": utc_now(),
            "metrics_scope": "not_started",
            "completion_status": "aborted_task_loading",
            "failure_stage": "task_loading",
            "failure_kind": "runner_exception",
            "completed_task_count": 0,
            "total_task_count": 0,
            "exception": f"{type(exc).__name__}: {exc}",
        }
        write_invalid_run_artifacts(output_dir, failure)
        return failure

    try:
        transport_tracker = TransportFailureTracker(
            max_failed_fraction=max_transport_failed_fraction,
            max_consecutive_failures=max_consecutive_transport_failures,
            min_fraction_samples=min_transport_fraction_sample,
        )
    except ValueError as exc:
        failure = {
            "label": label,
            "created_at": utc_now(),
            "metrics_scope": "not_started",
            "completion_status": "aborted_invalid_configuration",
            "failure_stage": "configuration",
            "failure_kind": "invalid_configuration",
            "completed_task_count": 0,
            "total_task_count": len(tasks),
            "error": str(exc),
            "max_transport_failed_fraction": max_transport_failed_fraction,
            "max_consecutive_transport_failures": max_consecutive_transport_failures,
            "min_transport_fraction_sample": min_transport_fraction_sample,
        }
        write_invalid_run_artifacts(output_dir, failure)
        return failure

    try:
        status_response: Any = mcp_call(url, "monolith_status", {}, timeout_s=timeout_s)
        status_validation = validate_mcp_status_response(
            status_response,
            result_payload=result_payload,
            result_data=result_data,
        )
    except Exception as exc:  # noqa: BLE001 - status runner defects must leave artifacts.
        status_validation = {
            "ok": False,
            "failure_kind": "runner_exception",
            "raw": f"{type(exc).__name__}: {exc}",
            "transport_status": None,
        }

    if not status_validation.get("ok"):
        status_failure_kind = str(status_validation.get("failure_kind", "protocol_error"))
        raw = str(status_validation.get("raw", ""))[:500]
        transport_status = status_validation.get("transport_status")
        failure = {
            "label": label,
            "created_at": utc_now(),
            "metrics_scope": "not_started",
            "completion_status": (
                "aborted_status_transport_failure"
                if status_failure_kind == "transport_error"
                else "aborted_status_preflight"
            ),
            "failure_stage": "status_preflight",
            "failure_kind": status_failure_kind,
            "completed_task_count": 0,
            "total_task_count": len(tasks),
            "transport_failure_count": 1 if status_failure_kind == "transport_error" else 0,
            "transport_status": transport_status,
            "transport_error_raw": raw if status_failure_kind == "transport_error" else "",
            "protocol_error_raw": raw if status_failure_kind != "transport_error" else "",
            "max_transport_failed_fraction": max_transport_failed_fraction,
            "max_consecutive_transport_failures": max_consecutive_transport_failures,
            "min_transport_fraction_sample": min_transport_fraction_sample,
        }
        write_invalid_run_artifacts(output_dir, failure)
        return failure

    status = dict(status_validation["status"])
    start_identity = status_identity(status, endpoint=url)

    try:
        benchmark_inputs = build_benchmark_inputs(
            "ProjectIndex",
            tasks_path=tasks_path,
            mcp_status=status,
            extra_files={"runner": pathlib.Path(__file__)},
        )
    except Exception as exc:  # noqa: BLE001 - provenance defects invalidate the run.
        failure = {
            "label": label,
            "created_at": utc_now(),
            "metrics_scope": "not_started",
            "completion_status": "aborted_runner_exception",
            "failure_stage": "benchmark_inputs",
            "failure_kind": "runner_exception",
            "completed_task_count": 0,
            "total_task_count": len(tasks),
            "exception": f"{type(exc).__name__}: {exc}",
        }
        failure.update(transport_tracker.snapshot())
        write_invalid_run_artifacts(output_dir, failure)
        return failure

    fixture_provenance = validate_live_fixture_runtime_provenance(corpus, status)
    if not fixture_provenance.get("ok"):
        failure = {
            "label": label,
            "created_at": utc_now(),
            "metrics_scope": "not_started",
            "completion_status": "aborted_fixture_provenance",
            "failure_stage": "fixture_provenance_preflight",
            "failure_kind": str(
                fixture_provenance.get("failure_kind", "invalid_fixture_provenance")
            ),
            "completed_task_count": 0,
            "total_task_count": len(tasks),
            "fixture_provenance": fixture_provenance,
        }
        failure.update(transport_tracker.snapshot())
        attach_benchmark_inputs(failure, benchmark_inputs)
        attach_run_context(failure, corpus, start_identity)
        write_invalid_run_artifacts(output_dir, failure)
        return failure

    rows: List[Dict[str, Any]] = []
    per_task_jsonl = output_dir / "per_task.jsonl"

    for index, task in enumerate(tasks, 1):
        runner_exception = ""
        try:
            row = score_task(url, task, timeout_s)
        except Exception as exc:  # noqa: BLE001 - preserve triggering task and abort.
            runner_exception = f"{type(exc).__name__}: {exc}"
            row = invalid_task_row(task, "runner_exception", runner_exception)
        rows.append(row)
        transport_decision = transport_tracker.observe(
            transport_error=bool(row.get("transport_error")),
            item_id=str(row.get("task_id", "")),
            status=(
                row.get("transport_status")
                if isinstance(row.get("transport_status"), int)
                and not isinstance(row.get("transport_status"), bool)
                else None
            ),
            raw=str(row.get("transport_error_raw", "")),
        )
        with per_task_jsonl.open("a", encoding="utf-8", newline="\n") as handle:
            handle.write(json.dumps(row, ensure_ascii=False, sort_keys=True))
            handle.write("\n")
        print(
            f"[{index}/{len(tasks)}] {row['task_id']} success={row['direct_success']}",
            flush=True,
        )
        invalid_failure_kind = str(row.get("failure_kind", ""))
        if runner_exception or invalid_failure_kind in {
            "parse_error",
            "protocol_error",
            "mcp_protocol_error",
            "runner_exception",
        }:
            failure = build_attempt_failure(
                label, status, tasks, rows, transport_tracker, benchmark_inputs,
                corpus, start_identity,
                {
                    "metrics_scope": (
                        "attempted_prefix_runner_exception"
                        if runner_exception
                        else "attempted_prefix_protocol_failure"
                    ),
                    "completion_status": (
                        "aborted_runner_exception"
                        if runner_exception
                        else "aborted_protocol_failure"
                    ),
                    "failure_stage": "task_scoring",
                    "failure_kind": "runner_exception" if runner_exception else invalid_failure_kind,
                    "completed_task_count": index,
                    "total_task_count": len(tasks),
                    "last_task_id": str(task.get("id", "")),
                    "exception": runner_exception,
                    "protocol_error_raw": str(row.get("protocol_error_raw", "")),
                },
            )
            write_invalid_run_artifacts(output_dir, failure)
            return failure
        if transport_decision:
            failure = build_attempt_failure(
                label, status, tasks, rows, transport_tracker, benchmark_inputs,
                corpus, start_identity,
                {
                    "metrics_scope": "attempted_prefix_including_transport_failures",
                    "completion_status": "aborted_transport_failure_budget",
                    "failure_stage": "task_transport",
                    "failure_kind": "transport_error",
                    "completed_task_count": index,
                    "total_task_count": len(tasks),
                    "transport_gate_reason": transport_decision.reason,
                    "last_task_id": transport_decision.item_id,
                },
            )
            write_invalid_run_artifacts(output_dir, failure)
            return failure
        if index == 1 or index == len(tasks) or index % 10 == 0:
            partial = aggregate(label, status, tasks[:index], rows)
            partial["completed_task_count"] = index
            partial["total_task_count"] = len(tasks)
            partial["run_valid"] = None
            partial["metrics_valid"] = False
            partial["metrics_scope"] = "attempted_prefix"
            partial["completion_status"] = "in_progress"
            partial.update(transport_tracker.snapshot())
            attach_benchmark_inputs(partial, benchmark_inputs)
            attach_run_context(partial, corpus, start_identity)
            write_json(output_dir / "partial_summary.json", partial)

    try:
        summary = aggregate(label, status, tasks, rows)
    except Exception as exc:  # noqa: BLE001 - aggregate defects invalidate the run.
        failure = build_attempt_failure(
            label, status, tasks, rows, transport_tracker, benchmark_inputs,
            corpus, start_identity,
            {
                "metrics_scope": "complete_run_invalid",
                "completion_status": "aborted_runner_exception",
                "failure_stage": "final_aggregate",
                "failure_kind": "runner_exception",
                "completed_task_count": len(rows),
                "total_task_count": len(tasks),
                "exception": f"{type(exc).__name__}: {exc}",
            },
        )
        write_invalid_run_artifacts(output_dir, failure)
        return failure
    final_transport_decision = transport_tracker.finalize()
    summary.update({
        "run_valid": True,
        "metrics_valid": True,
        "metrics_scope": "complete_run" if corpus.comparable else "complete_subset_run",
        "completion_status": "completed",
    })
    summary.update(transport_tracker.snapshot())
    attach_benchmark_inputs(summary, benchmark_inputs)
    attach_run_context(summary, corpus, start_identity)
    if final_transport_decision:
        summary.update({
            "metrics_scope": "complete_run_invalid",
            "completion_status": "completed_transport_failure_budget_exceeded",
            "failure_stage": "task_transport_finalize",
            "failure_kind": "transport_error",
            "completed_task_count": len(rows),
            "total_task_count": len(tasks),
            "transport_gate_reason": final_transport_decision.reason,
            "last_task_id": final_transport_decision.item_id,
        })
        write_invalid_run_artifacts(output_dir, summary)
        return summary

    try:
        end_status_response: Any = mcp_call(url, "monolith_status", {}, timeout_s=timeout_s)
        end_status_validation = validate_mcp_status_response(
            end_status_response,
            result_payload=result_payload,
            result_data=result_data,
        )
    except Exception as exc:  # noqa: BLE001 - postflight must invalidate the run.
        end_status_validation = {
            "ok": False,
            "failure_kind": "runner_exception",
            "raw": f"{type(exc).__name__}: {exc}",
            "transport_status": None,
        }
    if not end_status_validation.get("ok"):
        summary.update({
            "metrics_scope": "complete_run_invalid",
            "completion_status": "aborted_status_postflight",
            "failure_stage": "status_postflight",
            "failure_kind": str(end_status_validation.get("failure_kind", "protocol_error")),
            "postflight_status_raw": str(end_status_validation.get("raw", ""))[:500],
            "postflight_transport_status": end_status_validation.get("transport_status"),
        })
        write_invalid_run_artifacts(output_dir, summary)
        return summary

    end_status = dict(end_status_validation["status"])
    end_identity = status_identity(end_status, endpoint=url)
    identity_drift = status_identity_mismatches(start_identity, end_identity)
    attach_run_context(summary, corpus, start_identity, end_identity)
    if identity_drift:
        summary.update({
            "metrics_scope": "complete_run_invalid",
            "completion_status": "aborted_status_identity_drift",
            "failure_stage": "status_postflight",
            "failure_kind": "status_identity_drift",
            "status_identity_mismatches": identity_drift,
        })
        write_invalid_run_artifacts(output_dir, summary)
        return summary
    write_json(output_dir / "summary.json", summary)
    write_json(output_dir / "per_task.json", rows)
    partial_path = output_dir / "partial_summary.json"
    if partial_path.exists():
        partial_path.unlink()
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
    gen.add_argument("--live-fixtures", type=pathlib.Path, default=DEFAULT_LIVE_FIXTURES,
                     help="Project-derived fixtures file written by refresh_live_fixtures")

    refresh = sub.add_parser(
        "refresh_live_fixtures",
        help="Derive the project-local schema-action list and verified known-answer pairs "
             "from the live index and write them to the fixtures file",
    )
    refresh.add_argument("--mcp-url", default=DEFAULT_MCP_URL)
    refresh.add_argument("--fixtures", type=pathlib.Path, default=DEFAULT_LIVE_FIXTURES)
    refresh.add_argument("--request-timeout-s", type=float, default=45.0)
    refresh.add_argument("--known-answer-count", type=int, default=30)
    refresh.add_argument("--min-known-answers", type=int, default=10)

    run_cmd = sub.add_parser("run", help="Run tasks against a live MCP endpoint and score results")
    run_cmd.add_argument("--mcp-url", default=DEFAULT_MCP_URL)
    run_cmd.add_argument("--tasks", type=pathlib.Path, default=DEFAULT_TASKS)
    run_cmd.add_argument("--output-dir", type=pathlib.Path, required=True)
    run_cmd.add_argument("--label", required=True)
    run_cmd.add_argument(
        "--allow-subset",
        action="store_true",
        help="Permit an explicit non-canonical diagnostic corpus; output is marked non-comparable.",
    )
    run_cmd.add_argument(
        "--max-transport-failed-fraction",
        type=float,
        default=DEFAULT_MAX_TRANSPORT_FAILED_FRACTION,
        help="Abort without a normal summary when transport failures exceed this fraction.",
    )
    run_cmd.add_argument(
        "--max-consecutive-transport-failures",
        type=int,
        default=DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES,
        help="Abort after this many consecutive task transport failures.",
    )
    run_cmd.add_argument(
        "--min-transport-fraction-sample",
        type=int,
        default=DEFAULT_MIN_TRANSPORT_FRACTION_SAMPLES,
        help="Minimum attempted tasks before applying the in-run fraction gate.",
    )
    run_cmd.add_argument("--request-timeout-s", type=float, default=12.0)

    cmp_cmd = sub.add_parser("compare", help="Compare two run summary files")
    cmp_cmd.add_argument("--baseline", type=pathlib.Path, required=True)
    cmp_cmd.add_argument("--current", type=pathlib.Path, required=True)
    cmp_cmd.add_argument("--output-dir", type=pathlib.Path, required=True)

    args = parser.parse_args(argv)

    if args.cmd == "generate":
        try:
            manifest = generate_tasks(args.tasks, args.manifest, args.live_fixtures)
        except RuntimeError as exc:
            print(f"[project_index] ERROR: {exc}", file=sys.stderr)
            return 1
        print(json.dumps(manifest, indent=2, ensure_ascii=False))
        return 0

    if args.cmd == "refresh_live_fixtures":
        try:
            fixtures = refresh_live_fixtures(
                args.mcp_url, args.fixtures, args.request_timeout_s,
                args.known_answer_count, args.min_known_answers,
            )
        except RuntimeError as exc:
            print(f"[project_index] ERROR: {exc}", file=sys.stderr)
            return 1
        print(json.dumps(
            {
                "fixtures_file": display_path(resolve_plugin_path(args.fixtures)),
                "project_name": fixtures.get("project_name"),
                "catalog_version": fixtures.get("catalog_version"),
                "schema_action_count": len(fixtures.get("schema_actions") or []),
                "known_answer_count": len(fixtures.get("known_answers") or []),
            },
            indent=2, ensure_ascii=False,
        ))
        return 0

    if args.cmd == "run":
        summary = run_benchmark(
            args.mcp_url,
            args.tasks,
            args.output_dir,
            args.label,
            args.request_timeout_s,
            args.max_transport_failed_fraction,
            args.max_consecutive_transport_failures,
            args.min_transport_fraction_sample,
            allow_subset=args.allow_subset,
        )
        print(json.dumps(summary, indent=2, ensure_ascii=False))
        return 0 if summary.get("run_valid") else 1

    if args.cmd == "compare":
        comparison = compare_runs(args.baseline, args.current, args.output_dir)
        print(json.dumps({"output_dir": str(args.output_dir), "deltas": comparison["deltas"]}, indent=2))
        return 0

    return 2


if __name__ == "__main__":
    raise SystemExit(main())
