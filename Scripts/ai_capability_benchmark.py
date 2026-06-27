#!/usr/bin/env python3
"""
Monolith MCP AICapability benchmark.

Measures the MCP server's capability to support engine-native AI authoring workflows
across three of the four largest `ai` subsystems with granular authoring actions: Behavior Trees,
Blackboards, and EQS, plus the StateTree SCHEMA surface that the `ai` namespace exposes
(create_st_from_template + lint_state_tree are registered in the catalog, so read_schema/edit_schema
schema-presence tasks cover them). NOTE: StateTree is compiled OUT of this build (WITH_STATETREE=0),
so the StateTree EXECUTE/lint actions are runtime stubs that return isError "StateTree module not
available (WITH_STATETREE=0)". No StateTree edit_execute / lint compile_gate / lint error_path can
pass on this build, so this runner exercises StateTree only through schema discovery, and the
falsifiable lint/validate gate + error paths are built from Behavior Tree actions that DO execute.
The `ai` namespace is the largest uncovered surface in the suite (182 actions); before this
benchmark its only coverage was a handful of SchemaCompleteness param-scans and ActionGuidance
discovery probes — i.e. NOTHING executed an AI asset edit and read it back.

It is modelled closely on the gold-standard AssetEditing runner. Like that runner, the
weight is concentrated on ADVERSARIAL categories that a clean-fixture happy-path suite cannot fake:

  discovery        - ai.list_* / search_ai_assets find the seeded fixtures (portable; not GO content)
  read_schema      - monolith_discover schema for read actions (lenient)
  edit_schema      - monolith_discover schema for edit actions (strict: isError fails)
  edit_execute     - call real edit actions against fixtures AND read the mutation back
  error_path       - send invalid inputs and verify an INPUT-SPECIFIC structured isError that
                     names the OFFENDING IDENTIFIER (a reject-everything server cannot pass)
  duplicate_reject - a second identical create_* / add_bb_key must be refused (no silent suffix/no-op)
  compile_gate     - the FALSIFIABLE quality gate, built entirely from real Behavior Tree
                     validate actions (StateTree lint is unavailable on WITH_STATETREE=0):
                     NEGATIVE: a deliberately-empty Behavior Tree (a `create_behavior_tree` whose
                     root has no children) must make `validate_behavior_tree` report valid==false
                     with an error-severity issue "Root has no children — empty Behavior Tree"
                     (a stub that always says "valid" fails here).
                     POSITIVE: a well-formed Behavior Tree (a Selector-rooted scratch BT) must make
                     `validate_behavior_tree` report valid==true (a reject-everything stub that
                     always says "invalid" fails here).

Why no large clean happy-path block: the ROI report (PART B) is explicit — a clean-fixture-only
AI suite "would inherit the exact AssetEditing blind spot" (a green benchmark over broken
actions). So happy-path reads are minimized and the bulk of the weight sits on read-back-verified
edits, the offending-identifier error gate, duplicate rejection, and the lint/validate gate.

The weights live in the single ``WEIGHTS`` dict below and are the sole source of truth consumed by
aggregate(), the manifest score_formula, the comparison renderer, and this docstring. Every `ai`
action name and param used here is verified against the live action catalog
(`Saved/Monolith/LogAnalysis/_ai_catalog.txt`, 182 ai actions) and Source/MonolithAI/Private/*.cpp
(RegisterAction(TEXT("ai"), TEXT("<name>"), ...)) — no invented names. In particular, the `ai`
namespace exposes NO granular StateTree authoring (no create_state_tree / add_st_state /
compile_state_tree / add_st_task / add_st_transition / *_st_state); StateTree is reachable in the
catalog only via `create_st_from_template` and the `lint_state_tree` quality lint, BOTH of which are
runtime stubs returning isError "StateTree module not available (WITH_STATETREE=0)" on this build —
so they remain ONLY in the read_schema/edit_schema/discovery lists (schema-presence tasks that pass
because the action is registered), never in execute/lint gates. Verified handler error/return
shapes (file:line in METRICS.md):
  - add_bb_key duplicate -> Error("Key '<name>' already exists in this blackboard")  [Blackboard L767]
  - remove_bb_key missing -> Error("Key '<name>' not found in blackboard ...")        [Blackboard L839]
  - create_*/EnsureAssetPathFree dup -> Error("Asset already exists at '<path>'. ...") [Internal L155]
  - add_bt_node bad class -> Error("BT node class not found: <class>")                 [BehaviorTree L2283]
  - validate_behavior_tree -> {asset_path, valid:bool, issue_count:int, issues:[...]}  [BehaviorTree L4164]
  - validate_behavior_tree (empty BT, root no children) -> {valid:false, issue_count:1,
        issues:[{severity:"error","message":"Root has no children — empty Behavior Tree"}]} [BT L4097]
  - validate_behavior_tree / get_behavior_tree missing asset -> Error("BehaviorTree not found:
        <asset_path>") (echoes the offending path AND the literal "not found")            [Internal L65]
  - add_bt_node -> {node_id (NodeGuid), ...}; get_bt_graph lists node ids              [BehaviorTree]
  - add_bb_key -> success msg "Key '<name>' (<type>) added"; get_blackboard lists keys [Blackboard L810]
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
DEFAULT_TASKS = pathlib.Path("Benchmarks/AICapability/tasks.jsonl")
DEFAULT_MANIFEST = pathlib.Path("Benchmarks/AICapability/manifest.json")
DEFAULT_RESULTS_ROOT = pathlib.Path("Saved/Monolith/Benchmarks/AICapability")

# The MCP tool name for the ai namespace (verified: Skills/unreal-ai/SKILL.md "245 actions via
# ai_query(action, params)"). All ai.* actions are dispatched through this single tool.
AI_TOOL = "ai_query"

# Single source of truth for the composite score. aggregate(), the manifest score_formula,
# write_comparison_markdown(), and the module docstring all derive from this dict, so a weight
# can never drift between the code, the docs, and the comparison report again. Must sum to 1.0.
#
# Weighting rationale (mirrors AssetEditing's adversarial concentration): the executed,
# read-back-verified edit_execute dominates; the three falsifiability dimensions (error_path,
# duplicate_reject, compile_gate) together carry 0.34 so a green-on-broken server cannot pass;
# schema-only signals are residual tripwires; happy-path discovery is the smallest slice.
WEIGHTS: Dict[str, float] = {
    "edit_execute": 0.34,      # executed AI edits, each read back from get_blackboard/get_behavior_tree/get_eqs_query
    "error_path": 0.16,        # input-specific: the isError must name the offending identifier
    "compile_gate": 0.12,      # the falsifiable validate gate: an empty BT must validate valid==false (error issue) and a clean BT must validate valid==true
    "duplicate_reject": 0.10,  # second create_* / add_bb_key must be refused (no silent suffix/no-op)
    "edit_schema": 0.10,       # strict schema coverage tripwire for edit actions (isError fails)
    "discovery": 0.10,         # list_*/search find the seeded fixtures (require >=1 result)
    "read_schema": 0.08,       # lenient introspection tripwire for read actions
}
assert abs(sum(WEIGHTS.values()) - 1.0) < 1e-9, "AICapability WEIGHTS must sum to 1.0"

# Dimension rate keys in canonical (scored) order — drives the metrics dict and the
# comparison-report row order so a scored dimension can never be silently omitted.
SCORE_DIMENSIONS: List[str] = [f"{name}_rate" for name in WEIGHTS]


def score_formula_string() -> str:
    """Render the human-readable score formula from WEIGHTS (used in the manifest)."""
    return " + ".join(f"{w:g}*{name}_rate" for name, w in WEIGHTS.items())


# ---------------------------------------------------------------------------
# Fixtures (seeded by `setup_fixtures`)
# ---------------------------------------------------------------------------
# All under /Game/Benchmarks/AI/ — created by setup_fixtures, portable to any project (do not depend
# on GO content). subsystem identifies which ai sub-namespace owns the fixture.
FIXTURE_ROOT = "/Game/Benchmarks/AI"
BB_PATH = f"{FIXTURE_ROOT}/BB_BenchAI"
BT_PATH = f"{FIXTURE_ROOT}/BT_BenchAI"
EQS_PATH = f"{FIXTURE_ROOT}/EQS_BenchAI"
# A throwaway, EMPTY Behavior Tree used ONLY by the compile_gate NEGATIVE validate probe. A
# `create_behavior_tree` with no nodes has a root with no children, so validate_behavior_tree always
# reports valid==false with an error-severity issue "Root has no children — empty Behavior Tree".
# (StateTree lint is unavailable on WITH_STATETREE=0, so the negative gate is a BT validate, not a
# StateTree lint.)
BT_EMPTY_SCRATCH = f"{FIXTURE_ROOT}/BT_BenchEmptyScratch"
# A throwaway, well-formed Behavior Tree used ONLY by the compile_gate POSITIVE validate probe
# (Selector root child + a linked Blackboard -> validate_behavior_tree reports valid==true).
BT_VALIDATE_SCRATCH = f"{FIXTURE_ROOT}/BT_BenchValidateScratch"

# Blackboard keys seeded by setup_fixtures (name -> key_type, exact tokens accepted by
# CreateKeyTypeFromString: Bool/Int/Float/String/Name/Vector/Rotator/Object/Class/Enum/NativeEnum —
# Blackboard handler L55-170).
FIXTURE_BB_KEYS: List[Tuple[str, str]] = [
    ("BenchTargetActor", "Object"),
    ("BenchHomeLocation", "Vector"),
    ("BenchIsAlerted", "Bool"),
    ("BenchPatrolIndex", "Int"),
]


# ---------------------------------------------------------------------------
# Read actions (read_schema) — all verified in Source/MonolithAI (RegisterAction TEXT("ai")).
# These are lenient schema-introspection tripwires (planning_signals + skill metadata).
# ---------------------------------------------------------------------------
AI_READ_ACTIONS: List[Tuple[str, str]] = [
    # Blackboard
    ("get_blackboard", "blackboard"),
    ("list_blackboards", "blackboard"),
    ("get_bb_key_details", "blackboard"),
    # Behavior Tree
    ("get_behavior_tree", "behavior_tree"),
    ("get_bt_graph", "behavior_tree"),
    ("get_bt_node_properties", "behavior_tree"),
    ("list_behavior_trees", "behavior_tree"),
    ("list_bt_node_classes", "behavior_tree"),
    ("validate_behavior_tree", "behavior_tree"),
    # StateTree (the ai namespace exposes only template-create + lint; no granular ST read/authoring)
    ("lint_state_tree", "state_tree"),
    # EQS
    ("get_eqs_query", "eqs"),
    ("list_eqs_queries", "eqs"),
    ("validate_eqs_query", "eqs"),
    # Discovery / lint
    ("get_ai_overview", "discovery"),
    ("search_ai_assets", "discovery"),
    ("lint_behavior_tree", "discovery"),
]

# Edit actions (edit_schema) — strict schema coverage tripwire. (action, subsystem). All verified.
AI_EDIT_ACTIONS: List[Tuple[str, str]] = [
    # Blackboard
    ("create_blackboard", "blackboard"),
    ("add_bb_key", "blackboard"),
    ("remove_bb_key", "blackboard"),
    ("rename_bb_key", "blackboard"),
    ("batch_add_bb_keys", "blackboard"),
    ("set_bb_parent", "blackboard"),
    ("duplicate_blackboard", "blackboard"),
    ("delete_blackboard", "blackboard"),
    # Behavior Tree
    ("create_behavior_tree", "behavior_tree"),
    ("add_bt_node", "behavior_tree"),
    ("remove_bt_node", "behavior_tree"),
    ("move_bt_node", "behavior_tree"),
    ("add_bt_decorator", "behavior_tree"),
    ("add_bt_service", "behavior_tree"),
    ("set_bt_node_property", "behavior_tree"),
    ("set_bt_blackboard", "behavior_tree"),
    ("reorder_bt_children", "behavior_tree"),
    ("build_behavior_tree_from_spec", "behavior_tree"),
    ("duplicate_behavior_tree", "behavior_tree"),
    ("delete_behavior_tree", "behavior_tree"),
    # StateTree — the ai namespace exposes only template creation (no granular ST authoring).
    ("create_st_from_template", "state_tree"),
    # EQS
    ("create_eqs_query", "eqs"),
    ("add_eqs_generator", "eqs"),
    ("add_eqs_test", "eqs"),
    ("configure_eqs_test", "eqs"),
    ("reorder_eqs_tests", "eqs"),
    ("build_eqs_query_from_spec", "eqs"),
    ("delete_eqs_query", "eqs"),
]


# ---------------------------------------------------------------------------
# discovery tasks (query, subsystem, require_results)
# require_results=True: must return >=1 hit (targets the OWN seeded fixtures, so portable —
# a capable server scores 1.0 in any project, not only where AI assets happen to exist).
# require_results=False: broad recall that may legitimately return 0.
# ---------------------------------------------------------------------------
AI_DISCOVERY_TASKS: List[Dict[str, Any]] = [
    {"action": "list_blackboards", "subsystem": "blackboard", "require_results": True,
     "arguments": {"action": "list_blackboards", "path_filter": FIXTURE_ROOT},
     "contains": ["BB_BenchAI"],
     "description": "list_blackboards finds the seeded BB_BenchAI fixture"},
    {"action": "list_behavior_trees", "subsystem": "behavior_tree", "require_results": True,
     "arguments": {"action": "list_behavior_trees", "path_filter": FIXTURE_ROOT},
     "contains": ["BT_BenchAI"],
     "description": "list_behavior_trees finds the seeded BT_BenchAI fixture"},
    {"action": "list_eqs_queries", "subsystem": "eqs", "require_results": True,
     "arguments": {"action": "list_eqs_queries", "path_filter": FIXTURE_ROOT},
     "contains": ["EQS_BenchAI"],
     "description": "list_eqs_queries finds the seeded EQS_BenchAI fixture"},
    {"action": "search_ai_assets", "subsystem": "discovery", "require_results": True,
     "arguments": {"action": "search_ai_assets", "query": "BenchAI"},
     "contains": ["BenchAI"],
     "description": "search_ai_assets recalls the BenchAI fixture family by name"},
    {"action": "get_ai_overview", "subsystem": "discovery", "require_results": False,
     "arguments": {"action": "get_ai_overview"},
     "description": "get_ai_overview returns a project AI summary (non-error)"},
    {"action": "list_bt_node_classes", "subsystem": "behavior_tree", "require_results": True,
     "arguments": {"action": "list_bt_node_classes", "category": "composite"},
     "contains": ["Selector"],
     "description": "list_bt_node_classes enumerates composite node classes (Selector present)"},
]


# ---------------------------------------------------------------------------
# edit_execute tasks — each executes a real ai edit and reads it back.
# Two shapes:
#   flat:    {action, arguments, verify}            (single call + read-back)
#   chain:   {chain:[{op,args,capture?}], verify}   (multi-step; captures node/state ids as ${label})
# verify verbs handled by _verify_readback: read_action/read_args + contains/not_contains/absent.
# All names + params verified against Source/MonolithAI; idempotent (delete-first where a create
# has a clean inverse, so the read-back proves THIS run made the edit).
# ---------------------------------------------------------------------------
AI_EDIT_EXECUTE_TASKS: List[Dict[str, Any]] = [
    # --- Blackboard: add a key (delete-first) and read it back from get_blackboard ---
    {"subsystem": "blackboard", "edit_action": "add_bb_key",
     "description": "Add a Bool BB key to BB_BenchAI and read it back from get_blackboard",
     "chain": [
         {"op": "remove_bb_key", "args": {"action": "remove_bb_key", "asset_path": BB_PATH,
                                          "key_name": "BenchEditBoolKey"}},
         {"op": "add_bb_key", "args": {"action": "add_bb_key", "asset_path": BB_PATH,
                                       "key_name": "BenchEditBoolKey", "key_type": "Bool"}}],
     "verify": {"read_action": "get_blackboard", "read_args": {"asset_path": BB_PATH},
                "contains": ["BenchEditBoolKey"]}},
    {"subsystem": "blackboard", "edit_action": "add_bb_key",
     "description": "Add a Vector BB key to BB_BenchAI and read it back from get_blackboard",
     "chain": [
         {"op": "remove_bb_key", "args": {"action": "remove_bb_key", "asset_path": BB_PATH,
                                          "key_name": "BenchEditVectorKey"}},
         {"op": "add_bb_key", "args": {"action": "add_bb_key", "asset_path": BB_PATH,
                                       "key_name": "BenchEditVectorKey", "key_type": "Vector"}}],
     "verify": {"read_action": "get_blackboard", "read_args": {"asset_path": BB_PATH},
                "contains": ["BenchEditVectorKey"]}},
    # --- Blackboard: rename a key and assert old gone / new present ---
    {"subsystem": "blackboard", "edit_action": "rename_bb_key",
     "description": "Rename a BB key on BB_BenchAI; get_blackboard must show the new name, not the old",
     "chain": [
         {"op": "remove_bb_key", "args": {"action": "remove_bb_key", "asset_path": BB_PATH,
                                          "key_name": "BenchRenameKeyNew"}},
         {"op": "remove_bb_key", "args": {"action": "remove_bb_key", "asset_path": BB_PATH,
                                          "key_name": "BenchRenameKeyOld"}},
         {"op": "add_bb_key", "args": {"action": "add_bb_key", "asset_path": BB_PATH,
                                       "key_name": "BenchRenameKeyOld", "key_type": "Int"}},
         {"op": "rename_bb_key", "args": {"action": "rename_bb_key", "asset_path": BB_PATH,
                                          "old_name": "BenchRenameKeyOld", "new_name": "BenchRenameKeyNew"}}],
     "verify": {"read_action": "get_blackboard", "read_args": {"asset_path": BB_PATH},
                "contains": ["BenchRenameKeyNew"], "absent": ["BenchRenameKeyOld"]}},
    # --- Blackboard: batch add and read back two keys at once ---
    {"subsystem": "blackboard", "edit_action": "batch_add_bb_keys",
     "description": "batch_add_bb_keys adds two keys to BB_BenchAI; both must appear in get_blackboard",
     "chain": [
         {"op": "remove_bb_key", "args": {"action": "remove_bb_key", "asset_path": BB_PATH,
                                          "key_name": "BenchBatchKeyA"}},
         {"op": "remove_bb_key", "args": {"action": "remove_bb_key", "asset_path": BB_PATH,
                                          "key_name": "BenchBatchKeyB"}},
         {"op": "batch_add_bb_keys", "args": {"action": "batch_add_bb_keys", "asset_path": BB_PATH,
                                              "keys": [{"name": "BenchBatchKeyA", "type": "Float"},
                                                       {"name": "BenchBatchKeyB", "type": "Name"}]}}],
     "verify": {"read_action": "get_blackboard", "read_args": {"asset_path": BB_PATH},
                "contains": ["BenchBatchKeyA", "BenchBatchKeyB"]}},
    # --- Blackboard: remove a key round-trip (add then remove; must be gone) ---
    {"subsystem": "blackboard", "edit_action": "remove_bb_key",
     "description": "Add then remove a BB key on BB_BenchAI; get_blackboard must no longer list it",
     "chain": [
         {"op": "add_bb_key", "args": {"action": "add_bb_key", "asset_path": BB_PATH,
                                       "key_name": "BenchRemoveKey", "key_type": "Bool"}},
         {"op": "remove_bb_key", "args": {"action": "remove_bb_key", "asset_path": BB_PATH,
                                          "key_name": "BenchRemoveKey"}}],
     "verify": {"read_action": "get_blackboard", "read_args": {"asset_path": BB_PATH},
                "absent": ["BenchRemoveKey"]}},

    # NOTE: there is intentionally NO StateTree edit_execute task. The ai namespace's only StateTree
    # create action (create_st_from_template) and its only read (lint_state_tree) are runtime stubs
    # that return isError "StateTree module not available (WITH_STATETREE=0)" on this build, so no
    # StateTree edit_execute chain can read its mutation back. StateTree is covered by schema tasks
    # only (read_schema/edit_schema), and the falsifiable gate uses Behavior Tree validate instead.

    # --- Behavior Tree: add a composite node and read it back from get_behavior_tree ---
    {"subsystem": "behavior_tree", "edit_action": "add_bt_node",
     "description": "Add a Sequence composite under BT_BenchAI root (captured node_id) and read it back",
     "chain": [
         {"op": "add_bt_node", "capture": "seq",
          "args": {"action": "add_bt_node", "asset_path": BT_PATH,
                   "node_class": "BTComposite_Sequence"}}],
     "verify": {"read_action": "get_bt_graph", "read_args": {"asset_path": BT_PATH},
                "contains": ["${seq}"]}},
    # --- Behavior Tree: set the blackboard reference and read it back ---
    {"subsystem": "behavior_tree", "edit_action": "set_bt_blackboard",
     "description": "Set BB_BenchAI as the Blackboard on BT_BenchAI and read the linkage back",
     "chain": [
         {"op": "set_bt_blackboard", "args": {"action": "set_bt_blackboard", "asset_path": BT_PATH,
                                              "blackboard_path": BB_PATH}}],
     "verify": {"read_action": "get_behavior_tree", "read_args": {"asset_path": BT_PATH},
                "contains": ["BB_BenchAI"]}},
    # --- Behavior Tree: add a task under a captured composite, read both back ---
    {"subsystem": "behavior_tree", "edit_action": "add_bt_node",
     "description": "Add a Selector then a Wait task under it on BT_BenchAI (captured parent_id), read back",
     "chain": [
         {"op": "add_bt_node", "capture": "sel",
          "args": {"action": "add_bt_node", "asset_path": BT_PATH,
                   "node_class": "BTComposite_Selector"}},
         {"op": "add_bt_node", "capture": "wait",
          "args": {"action": "add_bt_node", "asset_path": BT_PATH,
                   "node_class": "BTTask_Wait", "parent_id": "${sel}"}}],
     "verify": {"read_action": "get_bt_graph", "read_args": {"asset_path": BT_PATH},
                "contains": ["${sel}", "${wait}"]}},

    # --- EQS: add a generator and read it back from get_eqs_query ---
    {"subsystem": "eqs", "edit_action": "add_eqs_generator",
     "description": "Add an ActorsOfClass generator to EQS_BenchAI and read it back from get_eqs_query",
     "chain": [
         {"op": "add_eqs_generator", "args": {"action": "add_eqs_generator", "asset_path": EQS_PATH,
                                              "generator_class": "EnvQueryGenerator_ActorsOfClass"}}],
     "verify": {"read_action": "get_eqs_query", "read_args": {"asset_path": EQS_PATH},
                "contains": ["ActorsOfClass"]}},
]


# ---------------------------------------------------------------------------
# error_path tasks — every input is a REAL action with REAL param names against an existing fixture
# (so this exercises actual handler error handling, not unknown-action/missing-param rejection).
# Scored inverted AND input-specific: pass = a structured isError whose message names the offending
# identifier (specific_tokens). A reject-everything canned message that never echoes the bad
# identifier fails. error_tokens (generic words) are kept for diagnostics only.
# Each offending identifier + verified error wording is anchored in METRICS.md.
# ---------------------------------------------------------------------------
AI_ERROR_PATH_TASKS: List[Dict[str, Any]] = [
    {"action": "get_blackboard", "subsystem": "blackboard",
     "description": "get_blackboard on a non-existent Blackboard asset",
     "arguments": {"action": "get_blackboard",
                   "asset_path": f"{FIXTURE_ROOT}/NONEXISTENT_BB_ZZZZ"},
     "specific_tokens": ["NONEXISTENT_BB_ZZZZ"],
     "error_tokens": ["not found", "could not", "does not exist", "failed"]},
    {"action": "remove_bb_key", "subsystem": "blackboard",
     "description": "remove_bb_key for a key that does not exist on BB_BenchAI",
     "arguments": {"action": "remove_bb_key", "asset_path": BB_PATH,
                   "key_name": "NONEXISTENT_BBKEY_ZZZZ"},
     "specific_tokens": ["NONEXISTENT_BBKEY_ZZZZ"],
     "error_tokens": ["key", "not found", "does not exist", "no such"]},
    {"action": "rename_bb_key", "subsystem": "blackboard",
     "description": "rename_bb_key for a non-existent source key on BB_BenchAI",
     "arguments": {"action": "rename_bb_key", "asset_path": BB_PATH,
                   "old_name": "NONEXISTENT_RENAMEKEY_ZZZZ", "new_name": "BenchWhatever"},
     "specific_tokens": ["NONEXISTENT_RENAMEKEY_ZZZZ"],
     "error_tokens": ["key", "not found", "does not exist", "no such"]},
    # validate_behavior_tree (not lint_state_tree) on a missing asset: StateTree lint is unavailable
    # on WITH_STATETREE=0, and validate_behavior_tree returns Error("BehaviorTree not found:
    # <asset_path>") which echoes the offending path AND the literal "not found" (Internal L65).
    {"action": "validate_behavior_tree", "subsystem": "behavior_tree",
     "description": "validate_behavior_tree on a non-existent Behavior Tree asset",
     "arguments": {"action": "validate_behavior_tree",
                   "asset_path": f"{FIXTURE_ROOT}/NONEXISTENT_BT_ZZZZ"},
     "specific_tokens": ["NONEXISTENT_BT_ZZZZ"],
     "error_tokens": ["not found", "could not load", "behaviortree", "does not exist"]},
    {"action": "add_bt_node", "subsystem": "behavior_tree",
     "description": "add_bt_node with a node_class that does not resolve on BT_BenchAI",
     "arguments": {"action": "add_bt_node", "asset_path": BT_PATH,
                   "node_class": "NONEXISTENT_BTNodeClass_ZZZZ"},
     "specific_tokens": ["NONEXISTENT_BTNodeClass_ZZZZ"],
     "error_tokens": ["node class", "not found", "invalid", "could not"]},
    {"action": "remove_bt_node", "subsystem": "behavior_tree",
     "description": "remove_bt_node for a node_id GUID that does not exist on BT_BenchAI",
     "arguments": {"action": "remove_bt_node", "asset_path": BT_PATH,
                   "node_id": "NONEXISTENT_BTNODE_ZZZZ"},
     "specific_tokens": ["NONEXISTENT_BTNODE_ZZZZ"],
     "error_tokens": ["node", "not found", "does not exist", "no such"]},
    {"action": "get_behavior_tree", "subsystem": "behavior_tree",
     "description": "get_behavior_tree on a non-existent Behavior Tree asset",
     "arguments": {"action": "get_behavior_tree",
                   "asset_path": f"{FIXTURE_ROOT}/NONEXISTENT_BT_ZZZZ"},
     "specific_tokens": ["NONEXISTENT_BT_ZZZZ"],
     "error_tokens": ["not found", "could not", "does not exist", "failed"]},
    {"action": "add_eqs_generator", "subsystem": "eqs",
     "description": "add_eqs_generator on a non-existent EQS query asset",
     "arguments": {"action": "add_eqs_generator",
                   "asset_path": f"{FIXTURE_ROOT}/NONEXISTENT_EQS_ZZZZ",
                   "generator_class": "EnvQueryGenerator_ActorsOfClass"},
     "specific_tokens": ["NONEXISTENT_EQS_ZZZZ"],
     "error_tokens": ["not found", "could not", "does not exist", "failed"]},
]


# ---------------------------------------------------------------------------
# duplicate_reject tasks — calling a create_*/add_bb_key action TWICE with the same name must be
# REFUSED on the second call with a duplicate-specific isError (not a silent suffix or no-op).
# Each `arguments` is a create with a fixed name; `cleanup` (optional, run first) deletes a prior
# run's leftover so the first call is a CLEAN create. Verified duplicate wording in METRICS.md.
# ---------------------------------------------------------------------------
AI_DUPLICATE_REJECT_TASKS: List[Dict[str, Any]] = [
    {"action": "add_bb_key", "subsystem": "blackboard",
     "description": "add_bb_key must reject a duplicate key name on BB_BenchAI",
     "setup_arguments": [{"action": "remove_bb_key", "asset_path": BB_PATH, "key_name": "BenchDupKey"}],
     "arguments": {"action": "add_bb_key", "asset_path": BB_PATH,
                   "key_name": "BenchDupKey", "key_type": "Bool"}},
    {"action": "create_blackboard", "subsystem": "blackboard",
     "description": "create_blackboard must reject creating a Blackboard at an occupied path",
     "setup_arguments": [{"action": "delete_blackboard", "asset_path": f"{FIXTURE_ROOT}/BB_BenchDup"}],
     "arguments": {"action": "create_blackboard", "save_path": f"{FIXTURE_ROOT}/BB_BenchDup",
                   "name": "BB_BenchDup"}},
    {"action": "create_behavior_tree", "subsystem": "behavior_tree",
     "description": "create_behavior_tree must reject creating a BT at an occupied path",
     "setup_arguments": [{"action": "delete_behavior_tree", "asset_path": f"{FIXTURE_ROOT}/BT_BenchDup"}],
     "arguments": {"action": "create_behavior_tree", "save_path": f"{FIXTURE_ROOT}/BT_BenchDup",
                   "name": "BT_BenchDup"}},
    {"action": "create_eqs_query", "subsystem": "eqs",
     "description": "create_eqs_query must reject creating an EQS query at an occupied path",
     "setup_arguments": [{"action": "delete_eqs_query", "asset_path": f"{FIXTURE_ROOT}/EQS_BenchDup"}],
     "arguments": {"action": "create_eqs_query", "save_path": f"{FIXTURE_ROOT}/EQS_BenchDup",
                   "name": "EQS_BenchDup"}},
]


# ---------------------------------------------------------------------------
# compile_gate tasks — the falsifiable QUALITY gate, built entirely from REAL Behavior Tree validate
# actions (StateTree lint is unavailable on WITH_STATETREE=0). Two complementary probes that no
# green-on-broken server can satisfy:
#   negative: a deliberately-EMPTY Behavior Tree must make validate_behavior_tree report
#             valid==false. The probe creates BT_BenchEmptyScratch via create_behavior_tree with no
#             nodes, so its root has no children and validate emits an error-severity issue
#             "Root has no children — empty Behavior Tree" with valid==false (verified live:
#             {valid:false, issue_count:1}). The gate passes only if validate reports valid==false
#             (ideally with an error-severity issue). A stub that always reports valid==true FAILS.
#   positive: a well-formed Behavior Tree must make validate_behavior_tree report valid==true.
#             The probe builds BT_BenchValidateScratch with a Selector root child (and a linked
#             Blackboard), which has no error-severity issues, so validate reports valid==true
#             (verified live: {valid:true, issue_count:2 (warnings)}). A reject-everything stub that
#             always reports valid==false FAILS.
# Verified shape: validate_behavior_tree -> {valid:bool, issue_count:int, issues:[...]} (BT L4164);
#                 empty-BT error issue at BT L4097.
# Each probe owns its own scratch asset; setup is tolerant of "already exists" so the gate is
# idempotent across re-runs (the scratch assets are left in place — they are isolated and harmless).
# A `gate` field selects the scorer's verdict mode: "validate_invalid" or "validate_valid".
# ---------------------------------------------------------------------------
AI_COMPILE_GATE_TASKS: List[Dict[str, Any]] = [
    {"polarity": "negative", "subsystem": "behavior_tree", "gate": "validate_invalid",
     "description": "An empty Behavior Tree (root with no children) must make "
                    "validate_behavior_tree report valid==false with an error-severity issue",
     "asset_path": BT_EMPTY_SCRATCH,
     "setup_chain": [],
     "gate_args": {"action": "validate_behavior_tree", "asset_path": BT_EMPTY_SCRATCH},
     "cleanup_chain": [],
     "expect_valid": False},
    {"polarity": "positive", "subsystem": "behavior_tree", "gate": "validate_valid",
     "description": "A well-formed Behavior Tree (Selector root child + linked Blackboard) must make "
                    "validate_behavior_tree report valid==true",
     "asset_path": BT_VALIDATE_SCRATCH,
     "setup_chain": [
         {"op": "set_bt_blackboard",
          "args": {"action": "set_bt_blackboard", "asset_path": BT_VALIDATE_SCRATCH,
                   "blackboard_path": BB_PATH}},
         {"op": "add_bt_node",
          "args": {"action": "add_bt_node", "asset_path": BT_VALIDATE_SCRATCH,
                   "node_class": "BTComposite_Selector"}}],
     "gate_args": {"action": "validate_behavior_tree", "asset_path": BT_VALIDATE_SCRATCH},
     "cleanup_chain": [],
     "expect_valid": True},
]


# ---------------------------------------------------------------------------
# Utilities (shared shapes with the gold-standard runner)
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

    all_data: List[str] = []
    current_event: List[str] = []
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        line = response.readline()
        if not line:
            break
        text = line.decode("utf-8", errors="replace").rstrip("\r\n")
        if text.startswith("data:"):
            current_event.append(text[5:].lstrip())
        elif text == "" and current_event:
            all_data.append("\n".join(current_event))
            current_event = []
    if current_event:
        all_data.append("\n".join(current_event))
    for chunk in reversed(all_data):
        if chunk.strip():
            return chunk
    return "\n".join(all_data)


def extract_sse_data(raw: str) -> str:
    lines = raw.splitlines()
    data_lines = [line[5:].lstrip() for line in lines if line.startswith("data:")]
    return "\n".join(data_lines) if data_lines else raw


def mcp_call(url: str, tool: str, arguments: Dict[str, Any], timeout_s: float = 45.0) -> Dict[str, Any]:
    body = {
        "jsonrpc": "2.0",
        "id": int(time.time() * 1000) % 1000000000,
        "method": "tools/call",
        "params": {"name": tool, "arguments": arguments},
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


# ---------------------------------------------------------------------------
# Domain helpers
# ---------------------------------------------------------------------------

def is_valid_non_error_response(data: Dict[str, Any], response: Dict[str, Any]) -> bool:
    if response.get("transport_error"):
        return False
    if response.get("parse_error"):
        return False
    if result_payload(response).get("isError"):
        return False
    return isinstance(data, dict)


def schema_has_planning_signals(schema: Dict[str, Any]) -> bool:
    signals = schema.get("planning_signals")
    return isinstance(signals, list) and len(signals) > 0


def schema_has_skill(schema: Dict[str, Any]) -> bool:
    return isinstance(schema.get("skill"), str) and bool(schema.get("skill"))


def _is_error(response: Dict[str, Any]) -> bool:
    """True if the MCP result carries isError=True (structured server-side error)."""
    return bool(result_payload(response).get("isError"))


def classify_mcp_failure(response: Dict[str, Any]) -> str:
    if response.get("transport_error"):
        return "transport_error"
    if response.get("parse_error"):
        return "parse_error"
    if _is_error(response):
        return "server_error"
    return ""


def _count_results(data: Dict[str, Any]) -> int:
    """Count list-shaped result rows in a list_* / search response. ai list handlers return their
    rows under a variety of keys (blackboards/behavior_trees/state_trees/queries/results/items/
    assets/classes/node_classes/task_types); count the first list found."""
    if not isinstance(data, dict):
        return 0
    for key in ("results", "items", "assets", "blackboards", "behavior_trees", "state_trees",
                "queries", "eqs_queries", "classes", "node_classes", "task_types", "matches",
                "ai_assets"):
        value = data.get(key)
        if isinstance(value, list):
            return len(value)
    # Some overviews nest counts; fall back to any top-level list.
    for value in data.values():
        if isinstance(value, list):
            return len(value)
    return 0


def _response_text_contains_all(response: Dict[str, Any], tokens: List[str]) -> bool:
    text = result_text(response)
    return all(tok and str(tok) in text for tok in tokens)


# ---------------------------------------------------------------------------
# Read-back helpers
# ---------------------------------------------------------------------------

def _extract_id(response: Dict[str, Any], keys: Tuple[str, ...]) -> Optional[str]:
    """Pull an id from an edit response. Verified live ai shape: add_bt_node returns ``node_id``
    (NodeGuid)."""
    data = result_data(response)
    if not isinstance(data, dict):
        return None
    for key in keys:
        val = data.get(key)
        if isinstance(val, str) and val:
            return val
    return None


# Per-op capture key preference: which id field each edit op returns.
_CAPTURE_KEYS: Dict[str, Tuple[str, ...]] = {
    "add_bt_node": ("node_id",),
    "add_bt_decorator": ("node_id",),
    "add_bt_service": ("node_id",),
}


def _subst_ids(value: Any, captured: Dict[str, str]) -> Any:
    """Recursively replace ${label} placeholders with captured ids."""
    if isinstance(value, str):
        out = value
        for label, nid in captured.items():
            out = out.replace("${%s}" % label, nid)
        return out
    if isinstance(value, dict):
        return {k: _subst_ids(v, captured) for k, v in value.items()}
    if isinstance(value, list):
        return [_subst_ids(v, captured) for v in value]
    return value


def _collect_names(obj: Any) -> set:
    """Recursively collect every ``name`` field value (BB keys carry ``key_name``; states carry
    ``name``). Used by exact-membership ``absent`` checks where a substring not_contains gives a
    false fail (old name a prefix of new name)."""
    names: set = set()
    if isinstance(obj, dict):
        for nk in ("name", "key_name", "state_name"):
            nm = obj.get(nk)
            if isinstance(nm, str):
                names.add(nm)
        for v in obj.values():
            names |= _collect_names(v)
    elif isinstance(obj, list):
        for v in obj:
            names |= _collect_names(v)
    return names


def _verify_readback(url: str, verify: Dict[str, Any], timeout_s: float) -> Tuple[bool, Dict[str, Any]]:
    """Run a read action and assert the edit is observable.

    verify verbs (all optional; all present must hold):
      read_action  : ai read action (get_blackboard / get_behavior_tree / get_bt_graph / get_eqs_query)
      read_args    : extra args for the read (asset_path is required and lives here)
      contains     : tokens that must ALL appear in the read response text (presence)
      not_contains : tokens that must NOT appear (text-level absence)
      absent       : entity names that must NOT exist as an exact name/key_name (parsed; survives prefix collisions)
    """
    read_action = verify.get("read_action")
    if not read_action:
        return True, {"skipped": "no_read_action"}
    read_call = {"action": read_action}
    read_call.update(verify.get("read_args", {}))
    resp = mcp_call(url, AI_TOOL, read_call, timeout_s=timeout_s)
    if resp.get("transport_error") or resp.get("parse_error") or _is_error(resp):
        return False, {"read_action": read_action, "read_failed": True,
                       "snippet": result_text(resp)[:200]}
    ok = True
    detail: Dict[str, Any] = {"read_action": read_action}
    text = result_text(resp)
    data = result_data(resp)

    tokens = verify.get("contains")
    if tokens:
        ok = all(str(tok) in text for tok in tokens)
        detail["contains"] = {"tokens": tokens, "ok": ok}

    not_tokens = verify.get("not_contains")
    if ok and not_tokens:
        ok = all(str(tok) not in text for tok in not_tokens)
        detail["not_contains"] = {"tokens": not_tokens, "ok": ok}

    absent = verify.get("absent")
    if ok and absent:
        present = _collect_names(data)
        bad = [n for n in absent if n in present]
        ok = not bad
        detail["absent"] = {"names": absent, "still_present": bad, "ok": ok}

    return ok, detail


# ---------------------------------------------------------------------------
# Scorers
# ---------------------------------------------------------------------------

def _run_chain_step(url: str, op_args: Dict[str, Any], captured: Dict[str, str],
                    timeout_s: float, tolerant: bool) -> Tuple[bool, Dict[str, Any], Dict[str, Any]]:
    """Run one chain step. Returns (step_ok, evidence, raw_response). In tolerant mode a leading
    remove_*/delete_* reporting "not found" and an "already exists" create are accepted (they only
    construct the precondition); the scored signal is the final read-back."""
    args = _subst_ids(dict(op_args), captured)
    resp = mcp_call(url, AI_TOOL, args, timeout_s=timeout_s)
    action = str(args.get("action", ""))
    transport_ok = not resp.get("transport_error") and not resp.get("parse_error")
    is_err = _is_error(resp)
    text_l = result_text(resp).lower()
    already = is_err and ("exist" in text_l or "already" in text_l)
    remove_missing = (is_err and action.startswith(("remove_", "delete_"))
                      and any(t in text_l for t in ("not found", "does not exist", "no such")))
    step_ok = transport_ok and (not is_err or (tolerant and (already or remove_missing)))
    evidence = {"action": action, "is_error": is_err, "already": already,
                "remove_missing": remove_missing, "ok": step_ok,
                "snippet": result_text(resp)[:120]}
    return step_ok, evidence, resp


def _score_edit_execute_chain(url: str, task: Dict[str, Any], timeout_s: float) -> Dict[str, Any]:
    """Score an edit_execute task: run each chain step (tolerant on the leading delete-first /
    already-exists construction), capturing returned ids into ${label}, then require the final
    read-back to observe the end state. A flat task is normalized to a one-step chain by the builder."""
    captured: Dict[str, str] = {}
    steps_evidence: List[Dict[str, Any]] = []
    ok = True

    for step in task.get("chain", []):
        # The leading construction ops (remove/delete to reset, or a re-add of a host entity) are
        # tolerant; the FINAL mutating op of the chain is the one under test and must succeed
        # cleanly — but "already exists" is still an idempotent success because the read-back gates.
        step_ok, ev, resp = _run_chain_step(url, step.get("args", {}), captured, timeout_s, tolerant=True)
        steps_evidence.append(ev)
        capture = step.get("capture")
        if capture:
            op_action = str(step.get("args", {}).get("action", ""))
            nid = _extract_id(resp, _CAPTURE_KEYS.get(op_action, ("id", "node_id", "state_id")))
            if nid:
                captured[capture] = nid
            else:
                step_ok = False
                steps_evidence[-1]["no_captured_id"] = True
        if not step_ok:
            ok = False
            break

    verify_detail: Dict[str, Any] = {}
    if ok:
        verify = _subst_ids(task.get("verify", {}), captured)
        if isinstance(verify, dict) and verify.get("read_action"):
            v_ok, verify_detail = _verify_readback(url, verify, timeout_s)
            ok = ok and v_ok

    return {
        "task_id": task.get("id"),
        "category": "edit_execute",
        "namespace": "ai",
        "action": task.get("action"),
        "subsystem": task.get("subsystem", ""),
        "edit_action": task.get("edit_action", ""),
        "direct_success": ok,
        "planning_signals": False,
        "evidence": {"steps": steps_evidence, "captured": captured, "verify": verify_detail},
        "transport_error": False,
        "transport_error_raw": "",
        "response_is_error": not ok,
        "response_text": json.dumps(verify_detail)[:500],
    }


def _score_duplicate_reject(url: str, task: Dict[str, Any], timeout_s: float) -> Dict[str, Any]:
    """Call the create action TWICE; the SECOND identical call must be refused with a
    duplicate-specific isError. The first must be a CLEAN create (a leading setup delete resets a
    prior run), so a reject-everything server fails first_ok."""
    args = dict(task.get("arguments", {}))

    setup_ok = True
    for s in task.get("setup_arguments", []) or []:
        s_resp = mcp_call(url, AI_TOOL, dict(s), timeout_s=timeout_s)
        s_action = str(s.get("action", ""))
        s_err = _is_error(s_resp)
        s_tolerable = (s_action.startswith(("remove_", "delete_"))
                       or "exist" in result_text(s_resp).lower()
                       or "not found" in result_text(s_resp).lower())
        setup_ok = setup_ok and (not s_resp.get("transport_error") and not s_resp.get("parse_error")
                                 and (not s_err or s_tolerable))

    first = mcp_call(url, AI_TOOL, dict(args), timeout_s=timeout_s)
    second = mcp_call(url, AI_TOOL, dict(args), timeout_s=timeout_s)

    transport_error = bool(second.get("transport_error"))
    parse_error = bool(second.get("parse_error"))
    server_handled = not transport_error and not parse_error
    second_is_error = _is_error(second)

    first_is_error = _is_error(first)
    first_ok = (not first.get("transport_error") and not first.get("parse_error") and not first_is_error)

    second_text = result_text(second).lower()
    second_is_duplicate = second_is_error and any(
        tok in second_text for tok in ("already", "exist", "duplicate", "in use", "taken"))

    direct_success = setup_ok and first_ok and server_handled and second_is_duplicate

    return {
        "task_id": task.get("id"),
        "category": "duplicate_reject",
        "namespace": "ai",
        "action": task.get("action"),
        "subsystem": task.get("subsystem", ""),
        "direct_success": direct_success,
        "planning_signals": False,
        "evidence": {
            "setup_ok": setup_ok,
            "first_call_ok": first_ok,
            "first_call_is_error": first_is_error,
            "second_call_handled": server_handled,
            "second_call_is_error": second_is_error,
            "second_is_duplicate": second_is_duplicate,
            "response_snippet": result_text(second)[:200],
        },
        "transport_error": transport_error,
        "transport_error_raw": str(second.get("raw", ""))[:300] if transport_error else "",
        "response_is_error": second_is_error,
        "response_text": result_text(second)[:500],
    }


def _validate_is_valid(response: Dict[str, Any]) -> Tuple[Optional[bool], Dict[str, Any]]:
    """Read the boolean ``valid`` from a validate_behavior_tree response. Returns (None, detail) if
    the call errored at transport/parse/isError level (the asset failed to LOAD, not to validate).
    validate_behavior_tree is verified to return Success with {valid:bool, issue_count:int,
    issues:[{severity,message}]}. detail.has_error_issue records whether an error-severity issue is
    present (used by the negative gate to assert the empty-BT error issue, not just valid==false)."""
    if response.get("transport_error") or response.get("parse_error") or _is_error(response):
        return None, {"reason": "transport_parse_or_iserror_not_a_validate_signal",
                      "snippet": result_text(response)[:200]}
    data = result_data(response)
    val = data.get("valid")
    if not isinstance(val, bool):
        return None, {"reason": "no_valid_field", "snippet": result_text(response)[:200]}
    has_error_issue = False
    issues = data.get("issues")
    if isinstance(issues, list):
        has_error_issue = any(isinstance(i, dict) and str(i.get("severity", "")).lower() == "error"
                              for i in issues)
    return val, {"valid": val, "issue_count": data.get("issue_count"),
                 "has_error_issue": has_error_issue}


def _score_compile_gate(url: str, task: Dict[str, Any], timeout_s: float) -> Dict[str, Any]:
    """Score the falsifiable quality gate. Two real, isolated probes (StateTree lint is unavailable
    on WITH_STATETREE=0, so both are Behavior Tree validate):
      validate_invalid (negative): an EMPTY Behavior Tree must make validate_behavior_tree report
                                   valid==false (with an error-severity issue) — a stub that always
                                   reports valid==true fails.
      validate_valid   (positive): a well-formed Behavior Tree must make validate_behavior_tree
                                   report valid==true — a reject-everything stub that always says
                                   invalid fails.
    The setup_chain (tolerant: already-exists is fine) constructs the scratch asset; the scored
    signal is the gate response. A None verdict (the call errored, not produced a verdict) never
    passes — that's the anti-reject-everything guard."""
    gate = str(task.get("gate", ""))
    captured: Dict[str, str] = {}
    steps_evidence: List[Dict[str, Any]] = []
    for step in task.get("setup_chain", []):
        _ok, ev, resp = _run_chain_step(url, step.get("args", {}), captured, timeout_s, tolerant=True)
        capture = step.get("capture")
        if capture:
            op_action = str(step.get("args", {}).get("action", ""))
            nid = _extract_id(resp, _CAPTURE_KEYS.get(op_action, ("state_id", "node_id", "id")))
            if nid:
                captured[capture] = nid
        steps_evidence.append(ev)

    gate_args = _subst_ids(dict(task.get("gate_args", {})), captured)
    gate_resp = mcp_call(url, AI_TOOL, gate_args, timeout_s=timeout_s)

    if gate == "validate_invalid":
        # Negative gate: an EMPTY Behavior Tree must validate valid==false with an error-severity
        # issue. Requiring the error issue (not just valid==false) is the anti-reject-everything
        # guard — a stub that returns valid==false with no issues, or that errors, cannot pass.
        valid, gdet = _validate_is_valid(gate_resp)
        expect_valid = bool(task.get("expect_valid"))  # False for this gate
        has_error_issue = bool(gdet.get("has_error_issue"))
        direct_success = (valid is not None) and (valid == expect_valid) and has_error_issue
        gate_action = str(gate_args.get("action", "validate_behavior_tree"))
        gdet = {**gdet, "expect_valid": expect_valid, "requires_error_issue": True}
    elif gate == "validate_valid":
        valid, gdet = _validate_is_valid(gate_resp)
        expect_valid = bool(task.get("expect_valid"))
        direct_success = (valid is not None) and (valid == expect_valid)
        gate_action = str(gate_args.get("action", "validate_behavior_tree"))
        gdet = {**gdet, "expect_valid": expect_valid}
    else:
        direct_success = False
        gate_action = str(gate_args.get("action", ""))
        gdet = {"reason": "unknown_gate_mode", "gate": gate}

    for step in task.get("cleanup_chain", []):
        try:
            mcp_call(url, AI_TOOL, _subst_ids(dict(step.get("args", {})), captured), timeout_s=timeout_s)
        except Exception:
            pass

    return {
        "task_id": task.get("id"),
        "category": "compile_gate",
        "namespace": "ai",
        "action": gate_action,
        "subsystem": task.get("subsystem", "behavior_tree"),
        "polarity": task.get("polarity", ""),
        "gate": gate,
        "direct_success": direct_success,
        "planning_signals": False,
        "evidence": {"setup_steps": steps_evidence, "gate_detail": gdet,
                     "gate_snippet": result_text(gate_resp)[:300]},
        "transport_error": bool(gate_resp.get("transport_error")),
        "transport_error_raw": str(gate_resp.get("raw", ""))[:300] if gate_resp.get("transport_error") else "",
        "response_is_error": not direct_success,
        "response_text": result_text(gate_resp)[:500],
    }


def score_task(url: str, task: Dict[str, Any], timeout_s: float) -> Dict[str, Any]:
    category = task.get("category", "")
    if category == "edit_execute":
        return _score_edit_execute_chain(url, task, timeout_s)
    if category == "duplicate_reject":
        return _score_duplicate_reject(url, task, timeout_s)
    if category == "compile_gate":
        return _score_compile_gate(url, task, timeout_s)

    response = mcp_call(url, str(task["tool"]), dict(task.get("arguments", {})), timeout_s=timeout_s)
    data = result_data(response)

    transport_error = bool(response.get("transport_error"))
    parse_error = bool(response.get("parse_error"))
    server_is_error = _is_error(response)
    server_handled = not transport_error and not parse_error

    direct_success = False
    planning_signals = False
    evidence: Dict[str, Any] = {}

    if category == "discovery":
        valid_resp = is_valid_non_error_response(data, response)
        count = _count_results(data)
        require_results = task.get("require_results", True)
        contains = task.get("expected", {}).get("contains", [])
        contains_ok = (not contains) or _response_text_contains_all(response, contains)
        direct_success = valid_resp and contains_ok and (count > 0 if require_results else True)
        evidence = {"valid_response": valid_resp, "results_count": count,
                    "require_results": require_results, "contains_ok": contains_ok,
                    "response_snippet": result_text(response)[:200]}

    elif category == "read_schema":
        schema = data.get("schema") if isinstance(data, dict) else None
        if not isinstance(schema, dict):
            schema = data
        has_signals = schema_has_planning_signals(schema)
        has_skill = schema_has_skill(schema)
        planning_signals = has_signals
        direct_success = bool(has_signals and has_skill)
        evidence = {"has_planning_signals": has_signals, "has_skill": has_skill,
                    "subsystem": task.get("subsystem", "")}

    elif category == "edit_schema":
        schema = data.get("schema") if isinstance(data, dict) else None
        if not isinstance(schema, dict):
            schema = data
        has_signals = False if server_is_error else schema_has_planning_signals(schema)
        has_skill = False if server_is_error else schema_has_skill(schema)
        planning_signals = has_signals
        direct_success = bool(has_signals and has_skill and not server_is_error)
        evidence = {"has_planning_signals": has_signals, "has_skill": has_skill,
                    "is_error": server_is_error, "subsystem": task.get("subsystem", "")}

    elif category == "error_path":
        # Inverted + input-SPECIFIC: pass = a structured isError whose message references the
        # OFFENDING IDENTIFIER (specific_tokens). A reject-everything canned message that never
        # echoes the bad identifier fails. The generic error_tokens are kept for diagnostics only.
        reason_tokens = task.get("expected", {}).get("error_tokens", [])
        specific_tokens = task.get("expected", {}).get("specific_tokens", [])
        text_l = result_text(response).lower()
        reason_ok = (not specific_tokens) or any(str(t).lower() in text_l for t in specific_tokens)
        generic_only = (bool(reason_tokens) and not reason_ok
                        and any(str(t).lower() in text_l for t in reason_tokens))
        direct_success = server_handled and server_is_error and reason_ok
        evidence = {"server_handled": server_handled, "returned_is_error": server_is_error,
                    "reason_ok": reason_ok, "generic_only": generic_only,
                    "specific_tokens": specific_tokens, "response_snippet": result_text(response)[:200]}

    else:
        evidence = {"unsupported_category": category}

    return {
        "task_id": task.get("id"),
        "category": category,
        "namespace": "ai",
        "action": task.get("action"),
        "subsystem": task.get("subsystem", ""),
        "direct_success": direct_success,
        "planning_signals": planning_signals,
        "evidence": evidence,
        "transport_error": transport_error,
        "transport_error_raw": str(response.get("raw", ""))[:300] if transport_error else "",
        "response_is_error": server_is_error,
        "response_text": result_text(response)[:500],
    }


# ---------------------------------------------------------------------------
# Aggregate
# ---------------------------------------------------------------------------

def aggregate(label: str, status: Dict[str, Any], tasks: List[Dict[str, Any]], rows: List[Dict[str, Any]]) -> Dict[str, Any]:
    def rate(cat: str) -> float:
        cat_rows = [r for r in rows if r["category"] == cat]
        return avg([1.0 if r["direct_success"] else 0.0 for r in cat_rows])

    missing_categories = [name for name in WEIGHTS
                          if not any(r["category"] == name for r in rows)]
    for name in missing_categories:
        print(f"WARNING: weighted category '{name}' has 0 rows; its dimension is zeroed "
              f"(-{WEIGHTS[name]} max score). The composite is not comparable to a full run.",
              flush=True)

    rates: Dict[str, float] = {name: rate(name) for name in WEIGHTS}
    ai_capability_score = sum(WEIGHTS[name] * rates[name] for name in WEIGHTS)

    # Per-subsystem breakdown over the executed edit_execute + error_path + duplicate_reject rows
    # (the adversarial categories), so a regression localized to one subsystem is visible.
    adversarial = [r for r in rows if r["category"] in ("edit_execute", "error_path",
                                                        "duplicate_reject", "compile_gate")]
    subsystem_breakdown: Dict[str, Dict[str, Any]] = {}
    for sub in sorted({r.get("subsystem") or "" for r in adversarial} - {""}):
        s_rows = [r for r in adversarial if r.get("subsystem") == sub]
        subsystem_breakdown[sub] = {
            "count": len(s_rows),
            "rate": round(avg([1.0 if r["direct_success"] else 0.0 for r in s_rows]), 6),
        }

    error_count = sum(1 for r in rows if r.get("transport_error") or r.get("response_is_error"))

    return {
        "label": label,
        "created_at": utc_now(),
        "mcp_status": status,
        "task_count": len(rows),
        "error_count": error_count,
        "category_counts": count_by(tasks, "category"),
        "metrics": {
            "ai_capability_score": round(ai_capability_score, 6),
            **{f"{name}_rate": round(rates[name], 6) for name in WEIGHTS},
            "task_count": len(rows),
            "error_count": error_count,
        },
        "missing_categories": missing_categories,
        "subsystem_breakdown": subsystem_breakdown,
    }


# ---------------------------------------------------------------------------
# Generate
# ---------------------------------------------------------------------------

def _build_edit_execute_task(spec: Dict[str, Any]) -> Dict[str, Any]:
    """Normalize a flat edit_execute spec to a one-step chain so the scorer handles both uniformly."""
    base = {
        "category": "edit_execute", "namespace": "ai", "tool": AI_TOOL,
        "expected": {"direct_success": True}, "safety": "mutating_fixture",
        "subsystem": spec["subsystem"], "edit_action": spec.get("edit_action", ""),
        "action": spec.get("edit_action", ""), "description": spec["description"],
    }
    if "chain" in spec:
        return {**base, "chain": spec["chain"], "verify": spec["verify"]}
    return {**base,
            "chain": [{"op": spec["edit_action"], "args": spec["arguments"]}],
            "verify": spec["verify"]}


_VERIFY_ASSERT_VERBS = ("contains", "not_contains", "absent")


def _verify_is_meaningful(verify: Any) -> bool:
    if not isinstance(verify, dict) or not verify.get("read_action"):
        return True
    for verb in _VERIFY_ASSERT_VERBS:
        v = verify.get(verb)
        if isinstance(v, list) and len(v) > 0:
            return True
    return False


def validate_task_integrity(tasks: List[Dict[str, Any]]) -> None:
    seen_ids: Dict[str, int] = {}
    seen_action_desc: Dict[Tuple[str, str], str] = {}
    for index, task in enumerate(tasks, 1):
        task_id = str(task.get("id", ""))
        if not task_id:
            raise RuntimeError(f"task at index {index} is missing id")
        if task_id in seen_ids:
            raise RuntimeError(f"duplicate task id {task_id} at indexes {seen_ids[task_id]} and {index}")
        seen_ids[task_id] = index
        description = str(task.get("description", "")).strip()
        if not description:
            raise RuntimeError(f"{task_id} is missing description")
        key = (str(task.get("action", "")), description)
        previous = seen_action_desc.get(key)
        if previous:
            raise RuntimeError(f"duplicate action+description for {previous} and {task_id}: {key}")
        seen_action_desc[key] = task_id
        if task.get("category") == "edit_execute" and not _verify_is_meaningful(task.get("verify")):
            raise RuntimeError(f"{task_id} edit_execute has a read_action verify with no assertion verb "
                               f"(content-free no-op read-back): {task.get('verify')}")


def build_static_tasks() -> List[Dict[str, Any]]:
    tasks: List[Dict[str, Any]] = []

    def next_id() -> str:
        return f"AIB-{len(tasks) + 1:03d}"

    # --- discovery ---
    for spec in AI_DISCOVERY_TASKS:
        tasks.append({
            "id": next_id(), "category": "discovery", "namespace": "ai",
            "action": spec["action"], "tool": AI_TOOL, "arguments": spec["arguments"],
            "expected": {"contains": spec.get("contains", []),
                         "min_results": 1 if spec.get("require_results") else 0},
            "require_results": spec.get("require_results", True),
            "safety": "read_only", "subsystem": spec["subsystem"],
            "description": spec["description"],
        })

    # --- read_schema ---
    for action, subsystem in AI_READ_ACTIONS:
        tasks.append({
            "id": next_id(), "category": "read_schema", "namespace": "ai",
            "action": action, "tool": "monolith_discover",
            "arguments": {"action": action, "mode": "schema", "namespace": "ai"},
            "expected": {"requires_planning_signals": True},
            "safety": "read_only_discovery", "subsystem": subsystem,
            "description": f"Discover read-action schema for ai.{action}",
        })

    # --- edit_schema ---
    for action, subsystem in AI_EDIT_ACTIONS:
        tasks.append({
            "id": next_id(), "category": "edit_schema", "namespace": "ai",
            "action": action, "tool": "monolith_discover",
            "arguments": {"action": action, "mode": "schema", "namespace": "ai"},
            "expected": {"requires_planning_signals": True},
            "safety": "read_only_discovery", "subsystem": subsystem,
            "description": f"Discover edit-action schema for ai.{action}",
        })

    # --- edit_execute (executed + read-back verified) ---
    for spec in AI_EDIT_EXECUTE_TASKS:
        task = _build_edit_execute_task(spec)
        task["id"] = next_id()
        tasks.append(task)

    # --- error_path (input-specific offending identifier) ---
    for spec in AI_ERROR_PATH_TASKS:
        tasks.append({
            "id": next_id(), "category": "error_path", "namespace": "ai",
            "action": spec["action"], "tool": AI_TOOL, "arguments": spec["arguments"],
            "expected": {"is_error": True, "error_tokens": spec.get("error_tokens", []),
                         "specific_tokens": spec.get("specific_tokens", [])},
            "safety": "read_only_invalid", "subsystem": spec["subsystem"],
            "description": spec["description"],
        })

    # --- duplicate_reject ---
    for spec in AI_DUPLICATE_REJECT_TASKS:
        task: Dict[str, Any] = {
            "id": next_id(), "category": "duplicate_reject", "namespace": "ai",
            "action": spec["action"], "tool": AI_TOOL, "arguments": spec["arguments"],
            "expected": {"is_error": True}, "safety": "mutating_idempotency",
            "subsystem": spec["subsystem"], "description": spec["description"],
        }
        if "setup_arguments" in spec:
            task["setup_arguments"] = spec["setup_arguments"]
        tasks.append(task)

    # --- compile_gate (falsifiable validate gate: an empty BT must validate valid==false; a
    #     well-formed BT must validate valid==true). Built only from real BT validate actions
    #     (StateTree lint is unavailable on WITH_STATETREE=0). ---
    for spec in AI_COMPILE_GATE_TASKS:
        task: Dict[str, Any] = {
            "id": next_id(), "category": "compile_gate", "namespace": "ai",
            "action": spec["gate_args"]["action"], "tool": AI_TOOL,
            "asset_path": spec["asset_path"], "polarity": spec["polarity"], "gate": spec["gate"],
            "setup_chain": spec["setup_chain"], "gate_args": spec["gate_args"],
            "cleanup_chain": spec["cleanup_chain"],
            "safety": "mutating_fixture", "subsystem": spec["subsystem"],
            "description": spec["description"],
        }
        # validate_invalid (negative) and validate_valid (positive) both assert on the boolean
        # `valid` field; expect_valid carries the required verdict (False for the empty-BT gate).
        task["expect_valid"] = spec["expect_valid"]
        task["expected"] = {spec["gate"]: spec["expect_valid"]}
        tasks.append(task)

    for i, task in enumerate(tasks, 1):
        task["id"] = f"AIB-{i:03d}"

    validate_task_integrity(tasks)
    present = {t["category"] for t in tasks}
    missing = [name for name in WEIGHTS if name not in present]
    if missing:
        raise RuntimeError(f"WEIGHTS categories with no tasks (would silently zero the score): {missing}")
    return tasks


def generate_tasks(tasks_path: pathlib.Path, manifest_path: pathlib.Path) -> Dict[str, Any]:
    tasks_path = resolve_plugin_path(tasks_path)
    manifest_path = resolve_plugin_path(manifest_path)
    tasks = build_static_tasks()
    write_jsonl(tasks_path, tasks)

    edit_schema_subsystems: Dict[str, int] = {}
    for _, sub in AI_EDIT_ACTIONS:
        edit_schema_subsystems[sub] = edit_schema_subsystems.get(sub, 0) + 1

    manifest = {
        "benchmark": "AICapability",
        "description": (
            "Measures engine-native AI authoring capability over the ai namespace's largest "
            "subsystems (Behavior Tree, Blackboard, EQS, plus the StateTree SCHEMA surface): "
            "fixture discovery, read/edit action schemas, read-back-verified edit execution, "
            "input-specific error handling that names the offending identifier, duplicate-name "
            "rejection, and a falsifiable Behavior Tree validate gate (an empty BT must validate "
            "valid==false with an error issue; a clean BT must validate valid==true). NOTE: "
            "StateTree is compiled out on this build (WITH_STATETREE=0), so its create/lint actions "
            "are runtime stubs and are exercised only via schema-presence tasks, never execute/gate"
        ),
        "primary_score": "ai_capability_score",
        "expected_namespace": "ai",
        "ai_tool": AI_TOOL,
        "generated_at": utc_now(),
        "task_count": len(tasks),
        "category_counts": count_by(tasks, "category"),
        "edit_schema_subsystems": edit_schema_subsystems,
        "subsystems_tested": ["blackboard", "behavior_tree", "state_tree", "eqs"],
        "score_formula": score_formula_string(),
        "weights": dict(WEIGHTS),
        "fixture_paths": {
            "blackboard": BB_PATH, "behavior_tree": BT_PATH, "eqs": EQS_PATH,
            "empty_scratch": BT_EMPTY_SCRATCH, "validate_scratch": BT_VALIDATE_SCRATCH,
        },
        "fixture_root": FIXTURE_ROOT,
        "setup_fixtures_command": "python Scripts/ai_capability_benchmark.py setup_fixtures --mcp-url http://localhost:9316/mcp",
        "run_command": "python Scripts/ai_capability_benchmark.py run --mcp-url http://localhost:9316/mcp --output-dir Saved/Monolith/Benchmarks/AICapability/<label> --label <label>",
        "score_dimensions": list(SCORE_DIMENSIONS),
        "catalog_version_verified": "ai-182-actions (Saved/Monolith/LogAnalysis/_ai_catalog.txt + Source/MonolithAI RegisterAction verified 2026-06-18; StateTree create/lint registered but stubbed on WITH_STATETREE=0)",
        "task_file": display_path(tasks_path),
    }
    write_json(manifest_path, manifest)
    return manifest


# ---------------------------------------------------------------------------
# Preflight + Setup Fixtures
# ---------------------------------------------------------------------------

def endpoint_preflight(url: str, timeout_s: float) -> Dict[str, Any]:
    response = mcp_call(url, "monolith_status", {}, timeout_s=timeout_s)
    failure_kind = classify_mcp_failure(response)
    ok = not failure_kind
    return {
        "ok": ok, "phase": "endpoint", "failure_kind": failure_kind,
        "message": "MCP endpoint reachable" if ok else "MCP endpoint did not return a usable monolith_status response",
        "status": result_data(response) if ok else None,
        "raw": str(response.get("raw", ""))[:300] if response.get("transport_error") or response.get("parse_error") else result_text(response)[:300],
    }


# (fixture_asset, read_action, contains_token) — the readiness probe for each seeded fixture.
_FIXTURE_READINESS: List[Tuple[str, str, str, List[str]]] = [
    (BB_PATH, "get_blackboard", "blackboard", [k for k, _ in FIXTURE_BB_KEYS][:1]),
    (BT_PATH, "get_behavior_tree", "behavior_tree", []),
    (EQS_PATH, "get_eqs_query", "eqs", []),
    # The compile_gate scratch Behavior Trees — the empty BT drives the negative validate gate, the
    # Selector-rooted BT drives the positive gate. Existence/contract checked via get_behavior_tree.
    (BT_EMPTY_SCRATCH, "get_behavior_tree", "behavior_tree", []),
    (BT_VALIDATE_SCRATCH, "get_behavior_tree", "behavior_tree", []),
]


def fixture_readiness_preflight(url: str, timeout_s: float, require_fixtures: bool = True) -> Dict[str, Any]:
    endpoint = endpoint_preflight(url, timeout_s)
    if not endpoint["ok"] or not require_fixtures:
        return {"ok": endpoint["ok"], "phase": endpoint["phase"], "failure_kind": endpoint["failure_kind"],
                "message": endpoint["message"], "endpoint": endpoint, "fixtures": []}

    fixtures: List[Dict[str, Any]] = []
    first_failure: Optional[Dict[str, Any]] = None
    for asset_path, read_action, subsystem, contains in _FIXTURE_READINESS:
        resp = mcp_call(url, AI_TOOL, {"action": read_action, "asset_path": asset_path}, timeout_s=timeout_s)
        failure = classify_mcp_failure(resp)
        ok = (not failure) and ((not contains) or _response_text_contains_all(resp, contains))
        fixture = {"subsystem": subsystem, "asset_path": asset_path, "read_action": read_action,
                   "ok": ok, "failure_kind": failure or ("" if ok else "fixture_contract_missing"),
                   "snippet": result_text(resp)[:200]}
        fixtures.append(fixture)
        if not ok and first_failure is None:
            first_failure = {"phase": "fixtures", "failure_kind": fixture["failure_kind"],
                             "message": f"Fixture preflight failed for {subsystem} at {asset_path}",
                             "fixture": fixture}

    ok = first_failure is None
    return {
        "ok": ok, "phase": "ready" if ok else "fixtures",
        "failure_kind": "" if ok else str(first_failure.get("failure_kind", "fixture_readiness_failed")),
        "message": "All AICapability fixtures are ready" if ok else str(first_failure.get("message", "Fixture preflight failed")),
        "endpoint": endpoint, "fixtures": fixtures, "first_failure": first_failure,
    }


def print_preflight_summary(preflight: Dict[str, Any]) -> None:
    status = "ok" if preflight.get("ok") else "FAILED"
    print(f"preflight: {status} phase={preflight.get('phase')} failure_kind={preflight.get('failure_kind', '')}", flush=True)
    if preflight.get("message"):
        print(f"  {preflight['message']}", flush=True)
    for fixture in preflight.get("fixtures", []):
        f_status = "ok" if fixture.get("ok") else "FAIL"
        kind = f" ({fixture.get('failure_kind')})" if fixture.get("failure_kind") else ""
        print(f"  [{f_status}] {fixture.get('subsystem')} {fixture.get('asset_path')}{kind}", flush=True)


def _setup_step(url: str, args: Dict[str, Any], timeout_s: float, tolerate_exists: bool = True) -> Dict[str, Any]:
    resp = mcp_call(url, AI_TOOL, dict(args), timeout_s=timeout_s)
    is_err = _is_error(resp)
    already = is_err and ("exist" in result_text(resp).lower() or "already" in result_text(resp).lower())
    success = (not resp.get("transport_error") and not resp.get("parse_error")
               and (not is_err or (tolerate_exists and already)))
    return {"action": str(args.get("action", "")), "success": success, "already_exists": already,
            "is_error": is_err, "failure_kind": classify_mcp_failure(resp),
            "snippet": result_text(resp)[:120]}


def setup_fixtures(url: str, timeout_s: float) -> Dict[str, Any]:
    """Create the AICapability fixtures at /Game/Benchmarks/AI/: a Blackboard (with seed keys), a
    Behavior Tree (linked to the Blackboard), an EQS query, an EMPTY scratch Behavior Tree for the
    negative validate gate, and a well-formed scratch Behavior Tree for the positive validate gate.
    Safe to re-run (already-exists tolerated). NOTE: StateTree is compiled out on this build
    (WITH_STATETREE=0), so no StateTree fixture is seeded; StateTree is covered by schema tasks only
    and the falsifiable gate uses Behavior Tree validate."""
    preflight = endpoint_preflight(url, timeout_s)
    print(f"endpoint: {'ok' if preflight['ok'] else 'FAILED'} {preflight.get('message')}", flush=True)
    if not preflight["ok"]:
        return {"endpoint": preflight, "ready": False,
                "failure_kind": preflight.get("failure_kind", "transport_error")}

    steps: List[Dict[str, Any]] = []

    # 1. Blackboard + seed keys
    steps.append(_setup_step(url, {"action": "create_blackboard", "save_path": BB_PATH, "name": "BB_BenchAI"}, timeout_s))
    for key_name, key_type in FIXTURE_BB_KEYS:
        steps.append(_setup_step(url, {"action": "add_bb_key", "asset_path": BB_PATH,
                                       "key_name": key_name, "key_type": key_type}, timeout_s))

    # 2. Behavior Tree (link the Blackboard)
    steps.append(_setup_step(url, {"action": "create_behavior_tree", "save_path": BT_PATH,
                                   "name": "BT_BenchAI", "blackboard_path": BB_PATH}, timeout_s))

    # 3. Empty-scratch Behavior Tree (compile_gate NEGATIVE probe): create_behavior_tree with NO
    #    nodes leaves the root with no children, so validate_behavior_tree reports valid==false with
    #    an error-severity "Root has no children — empty Behavior Tree" issue. (StateTree lint is
    #    unavailable on WITH_STATETREE=0, so the negative gate is a BT validate, not a ST lint.)
    #    Intentionally NO add_bt_node here — the tree must stay empty for the gate to fire.
    steps.append(_setup_step(url, {"action": "create_behavior_tree", "save_path": BT_EMPTY_SCRATCH,
                                   "name": "BT_BenchEmptyScratch"}, timeout_s))

    # 4. Validate-scratch Behavior Tree (compile_gate POSITIVE probe): a Selector root child + the
    #    linked Blackboard make validate_behavior_tree report valid==true with no error issues.
    steps.append(_setup_step(url, {"action": "create_behavior_tree", "save_path": BT_VALIDATE_SCRATCH,
                                   "name": "BT_BenchValidateScratch", "blackboard_path": BB_PATH}, timeout_s))
    steps.append(_setup_step(url, {"action": "add_bt_node", "asset_path": BT_VALIDATE_SCRATCH,
                                   "node_class": "BTComposite_Selector"}, timeout_s))

    # 5. EQS query
    steps.append(_setup_step(url, {"action": "create_eqs_query", "save_path": EQS_PATH, "name": "EQS_BenchAI"}, timeout_s))

    for s in steps:
        flag = "ok" if s["success"] else "FAIL"
        extra = " (already existed)" if s.get("already_exists") else ""
        kind = f" [{s.get('failure_kind')}]" if s.get("failure_kind") else ""
        print(f"  {flag} {s['action']}{extra}{kind} {s.get('snippet', '')[:80]}", flush=True)

    setup_ok = all(s["success"] for s in steps)
    readiness = fixture_readiness_preflight(url, timeout_s, require_fixtures=True)
    print_preflight_summary(readiness)
    ready = setup_ok and readiness.get("ok", False)
    print(f"\nsetup_fixtures: {'ready' if ready else 'NOT ready'} ({sum(1 for s in steps if s['success'])}/{len(steps)} steps ok)", flush=True)
    return {
        "endpoint": preflight, "steps": steps, "post_setup_readiness": readiness,
        "ready": ready, "failure_kind": "" if ready else readiness.get("failure_kind", "fixture_edit_failure"),
    }


# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------

def run_benchmark(url: str, tasks_path: pathlib.Path, output_dir: pathlib.Path,
                  label: str, timeout_s: float) -> Dict[str, Any]:
    tasks_path = resolve_plugin_path(tasks_path)
    tasks = load_jsonl(tasks_path)
    output_dir.mkdir(parents=True, exist_ok=True)
    status_response = mcp_call(url, "monolith_status", {}, timeout_s=timeout_s)
    status = result_data(status_response)
    benchmark_inputs = build_benchmark_inputs("AICapability", tasks_path=tasks_path, mcp_status=status)

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
        print(f"[{index}/{len(tasks)}] {row['task_id']} category={row['category']} success={row['direct_success']}", flush=True)
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
    comparison = {"created_at": utc_now(), "baseline": baseline, "current": current, "deltas": deltas}
    output_dir.mkdir(parents=True, exist_ok=True)
    write_json(output_dir / "comparison.json", comparison)
    write_comparison_markdown(output_dir / "comparison.md", comparison)
    return comparison


def write_comparison_markdown(path: pathlib.Path, comparison: Dict[str, Any]) -> None:
    baseline = comparison["baseline"]
    current = comparison["current"]
    deltas = comparison["deltas"]
    metrics = ["ai_capability_score"] + list(SCORE_DIMENSIONS)
    for extra in list(current.get("metrics", {})) + list(baseline.get("metrics", {})):
        if extra.endswith("_rate") and extra not in metrics:
            metrics.append(extra)
    lines = [
        "# Monolith AICapability Benchmark Comparison",
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
        lines.append(f"| `{metric}` | {baseline['metrics'].get(metric)} | "
                     f"{current['metrics'].get(metric)} | {deltas.get(metric)} |")
    lines.append("")
    lines.append("Higher is better for all metrics.")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)

    gen = sub.add_parser("generate", help="Generate static task fixtures + manifest")
    gen.add_argument("--tasks", type=pathlib.Path, default=DEFAULT_TASKS)
    gen.add_argument("--manifest", type=pathlib.Path, default=DEFAULT_MANIFEST)

    sf_cmd = sub.add_parser("setup_fixtures",
                            help="Create AICapability AI fixtures at /Game/Benchmarks/AI/ via MCP")
    sf_cmd.add_argument("--mcp-url", default=DEFAULT_MCP_URL)
    sf_cmd.add_argument("--request-timeout-s", type=float, default=30.0)

    pf_cmd = sub.add_parser("preflight",
                            help="Check MCP endpoint and AICapability fixture readiness")
    pf_cmd.add_argument("--mcp-url", default=DEFAULT_MCP_URL)
    pf_cmd.add_argument("--request-timeout-s", type=float, default=15.0)
    pf_cmd.add_argument("--endpoint-only", action="store_true")

    run_cmd = sub.add_parser("run", help="Run tasks against a live MCP endpoint and score results")
    run_cmd.add_argument("--mcp-url", default=DEFAULT_MCP_URL)
    run_cmd.add_argument("--tasks", type=pathlib.Path, default=DEFAULT_TASKS)
    run_cmd.add_argument("--output-dir", type=pathlib.Path, required=True)
    run_cmd.add_argument("--label", required=True)
    run_cmd.add_argument("--request-timeout-s", type=float, default=20.0)
    run_cmd.add_argument("--skip-preflight", action="store_true")

    cmp_cmd = sub.add_parser("compare", help="Compare two run summary files")
    cmp_cmd.add_argument("--baseline", type=pathlib.Path, required=True)
    cmp_cmd.add_argument("--current", type=pathlib.Path, required=True)
    cmp_cmd.add_argument("--output-dir", type=pathlib.Path, required=True)

    args = parser.parse_args(argv)

    if args.cmd == "generate":
        manifest = generate_tasks(args.tasks, args.manifest)
        sys.stdout.buffer.write((json.dumps(manifest, indent=2, ensure_ascii=False) + "\n").encode("utf-8"))
        return 0

    if args.cmd == "setup_fixtures":
        print("Creating AICapability fixtures...", flush=True)
        result = setup_fixtures(args.mcp_url, args.request_timeout_s)
        sys.stdout.buffer.write((json.dumps(result, indent=2, ensure_ascii=False) + "\n").encode("utf-8"))
        return 0 if result.get("ready") else 1

    if args.cmd == "preflight":
        result = fixture_readiness_preflight(args.mcp_url, args.request_timeout_s,
                                             require_fixtures=not args.endpoint_only)
        print_preflight_summary(result)
        sys.stdout.buffer.write((json.dumps(result, indent=2, ensure_ascii=False) + "\n").encode("utf-8"))
        return 0 if result.get("ok") else 1

    if args.cmd == "run":
        if not args.skip_preflight:
            preflight = fixture_readiness_preflight(args.mcp_url, args.request_timeout_s, require_fixtures=True)
            print_preflight_summary(preflight)
            if not preflight.get("ok"):
                sys.stdout.buffer.write((json.dumps({"preflight": preflight}, indent=2, ensure_ascii=False) + "\n").encode("utf-8"))
                return 1
        summary = run_benchmark(args.mcp_url, args.tasks, args.output_dir, args.label, args.request_timeout_s)
        sys.stdout.buffer.write((json.dumps(summary, indent=2, ensure_ascii=False) + "\n").encode("utf-8"))
        return 0

    if args.cmd == "compare":
        comparison = compare_runs(args.baseline, args.current, args.output_dir)
        print(json.dumps({"output_dir": str(args.output_dir), "deltas": comparison["deltas"]}, indent=2))
        return 0

    return 2


if __name__ == "__main__":
    raise SystemExit(main())
