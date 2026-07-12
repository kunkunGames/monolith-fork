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
from typing import Any, Callable, Dict, Iterable, List, Optional, Tuple

from benchmark_common import (
    benchmark_routing_context,
    DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES,
    DEFAULT_MAX_TRANSPORT_FAILED_FRACTION,
    DEFAULT_MIN_TRANSPORT_FRACTION_SAMPLES,
    TaskCorpus,
    TaskCorpusContractError,
    TransportFailureTracker,
    attach_benchmark_inputs,
    build_benchmark_inputs,
    classify_mcp_protocol_failure,
    display_path,
    load_task_corpus,
    resolve_plugin_path,
    status_identity,
    status_identity_mismatches,
    task_corpus_metadata,
    validate_mcp_status_response,
)


DEFAULT_MCP_URL = "http://localhost:9316/mcp"
DEFAULT_TASKS = pathlib.Path("Benchmarks/AICapability/tasks.jsonl")
DEFAULT_MANIFEST = pathlib.Path("Benchmarks/AICapability/manifest.json")
DEFAULT_RESULTS_ROOT = pathlib.Path("Saved/Monolith/Benchmarks/AICapability")

RUN_OUTPUT_FILENAMES = (
    "summary.json",
    "partial_summary.json",
    "per_task.json",
    "per_task.jsonl",
    "run_failure.json",
)

AI_TASK_CATEGORIES = {
    "compile_gate",
    "discovery",
    "duplicate_reject",
    "edit_execute",
    "edit_schema",
    "error_path",
    "read_schema",
}

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
# Throwaway Behavior Trees owned entirely by the two compile-gate tasks. They deliberately do not
# reuse the historical source-controlled BT_BenchEmptyScratch/BT_BenchValidateScratch fixtures: each
# gate resets its package, creates the exact topology under test, validates it, and deletes it before
# the task can pass. This makes the positive and negative verdicts independent of earlier runs.
BT_EMPTY_SCRATCH = f"{FIXTURE_ROOT}/BT_BenchNegativeGateScratch"
BT_VALIDATE_SCRATCH = f"{FIXTURE_ROOT}/BT_BenchPositiveGateScratch"
# Throwaway scaffold-template outputs used ONLY by the create_bt_from_template edit_execute chain.
# HandleCreateBTFromTemplate derives the companion Blackboard path from the BT name (BT_ -> BB_,
# MonolithAIScaffoldActions.cpp), so both paths are pinned here and the chain delete-first resets
# them for idempotent re-runs.
BT_TEMPLATE_SCRATCH = f"{FIXTURE_ROOT}/BT_BenchTemplateScratch"
BB_TEMPLATE_SCRATCH = f"{FIXTURE_ROOT}/BB_BenchTemplateScratch"

# Duplicate-rejection probes must own their lifecycle instead of reusing persistent benchmark
# fixtures. The historical BB/BT/EQS_BenchDup packages are source-controlled fixtures in another
# changelist, so deleting them is neither a valid benchmark precondition nor safe. These dedicated
# scratch packages are reset before the first create and deleted after the second create, leaving no
# generated package behind after a successful run.
BB_DUPLICATE_SCRATCH = f"{FIXTURE_ROOT}/BB_BenchDuplicateScratch"
BT_DUPLICATE_SCRATCH = f"{FIXTURE_ROOT}/BT_BenchDuplicateScratch"
EQS_DUPLICATE_SCRATCH = f"{FIXTURE_ROOT}/EQS_BenchDuplicateScratch"

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

    # --- Scaffold: create_bt_from_template must complete its INTERNAL set_bt_blackboard linkage ---
    # Guards cross-action param-contract drift inside composite scaffolds (2026-07-10 handoff N3: a
    # scaffold that dispatches set_bt_blackboard with drifted param names fails the whole template
    # create with "Failed to link BT to Blackboard"). The delete-first steps reset both scaffold
    # outputs so re-runs stay idempotent; the read-back proves the internal linkage actually landed.
    # The task then deletes both generated packages and proves both public reads report explicit
    # absence, so a successful benchmark cannot leave transient assets or Perforce adds behind.
    {"subsystem": "behavior_tree", "edit_action": "create_bt_from_template",
     "description": "create_bt_from_template(patrol) scaffolds BT+BB; get_behavior_tree must read the "
                    "internal Blackboard linkage back (guards internal set_bt_blackboard contract)",
     "chain": [
         {"op": "delete_behavior_tree", "allow_absent": True,
          "args": {"action": "delete_behavior_tree",
                                                  "asset_path": BT_TEMPLATE_SCRATCH}},
         {"op": "delete_blackboard", "allow_absent": True,
          "args": {"action": "delete_blackboard",
                                               "asset_path": BB_TEMPLATE_SCRATCH}},
         {"op": "create_bt_from_template", "args": {"action": "create_bt_from_template",
                                                    "save_path": BT_TEMPLATE_SCRATCH,
                                                    "template": "patrol"}}],
      "verify": {"read_action": "get_behavior_tree", "read_args": {"asset_path": BT_TEMPLATE_SCRATCH},
                 "contains": ["BB_BenchTemplateScratch"]},
      "cleanup_chain": [
          {"op": "delete_behavior_tree", "allow_absent": True,
           "args": {"action": "delete_behavior_tree", "asset_path": BT_TEMPLATE_SCRATCH}},
          {"op": "delete_blackboard", "allow_absent": True,
           "args": {"action": "delete_blackboard", "asset_path": BB_TEMPLATE_SCRATCH}}],
      "cleanup_verify": [
          {"action": "get_behavior_tree", "asset_path": BT_TEMPLATE_SCRATCH,
           "expect_not_found": True},
          {"action": "get_blackboard", "asset_path": BB_TEMPLATE_SCRATCH,
           "expect_not_found": True}]},
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
# Each `arguments` is a create with a benchmark-owned scratch name. `setup_arguments` removes only a
# prior interrupted run's leftover, while `cleanup_arguments` removes the entity created by this
# task. A setup error is tolerated only when it explicitly says the entity is absent; arbitrary
# delete failures are not a clean precondition. Verified duplicate wording in METRICS.md.
# ---------------------------------------------------------------------------
AI_DUPLICATE_REJECT_TASKS: List[Dict[str, Any]] = [
    {"action": "add_bb_key", "subsystem": "blackboard",
     "description": "add_bb_key must reject a duplicate key name on BB_BenchAI",
     "setup_arguments": [{"action": "remove_bb_key", "asset_path": BB_PATH, "key_name": "BenchDupKey"}],
     "cleanup_arguments": [{"action": "remove_bb_key", "asset_path": BB_PATH, "key_name": "BenchDupKey"}],
     "cleanup_verify": {"action": "get_blackboard", "asset_path": BB_PATH,
                        "absent": ["BenchDupKey"]},
     "arguments": {"action": "add_bb_key", "asset_path": BB_PATH,
                   "key_name": "BenchDupKey", "key_type": "Bool"}},
    {"action": "create_blackboard", "subsystem": "blackboard",
     "description": "create_blackboard must reject creating a Blackboard at an occupied path",
     "setup_arguments": [{"action": "delete_blackboard", "asset_path": BB_DUPLICATE_SCRATCH}],
     "cleanup_arguments": [{"action": "delete_blackboard", "asset_path": BB_DUPLICATE_SCRATCH}],
     "cleanup_verify": {"action": "get_blackboard", "asset_path": BB_DUPLICATE_SCRATCH,
                        "expect_not_found": True},
     "arguments": {"action": "create_blackboard", "save_path": BB_DUPLICATE_SCRATCH,
                    "name": "BB_BenchDuplicateScratch"}},
    {"action": "create_behavior_tree", "subsystem": "behavior_tree",
     "description": "create_behavior_tree must reject creating a BT at an occupied path",
     "setup_arguments": [{"action": "delete_behavior_tree", "asset_path": BT_DUPLICATE_SCRATCH}],
     "cleanup_arguments": [{"action": "delete_behavior_tree", "asset_path": BT_DUPLICATE_SCRATCH}],
     "cleanup_verify": {"action": "get_behavior_tree", "asset_path": BT_DUPLICATE_SCRATCH,
                        "expect_not_found": True},
     "arguments": {"action": "create_behavior_tree", "save_path": BT_DUPLICATE_SCRATCH,
                    "name": "BT_BenchDuplicateScratch"}},
    {"action": "create_eqs_query", "subsystem": "eqs",
     "description": "create_eqs_query must reject creating an EQS query at an occupied path",
     "setup_arguments": [{"action": "delete_eqs_query", "asset_path": EQS_DUPLICATE_SCRATCH}],
     "cleanup_arguments": [{"action": "delete_eqs_query", "asset_path": EQS_DUPLICATE_SCRATCH}],
     "cleanup_verify": {"action": "get_eqs_query", "asset_path": EQS_DUPLICATE_SCRATCH,
                        "expect_not_found": True},
     "arguments": {"action": "create_eqs_query", "save_path": EQS_DUPLICATE_SCRATCH,
                    "name": "EQS_BenchDuplicateScratch"}},
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
# Each probe owns its scratch asset from reset through cleanup. Only an explicit "not found" reset is
# tolerated; create/edit failures and cleanup failures fail the task. No generated package remains
# after a passing gate.
# A `gate` field selects the scorer's verdict mode: "validate_invalid" or "validate_valid".
# ---------------------------------------------------------------------------
AI_COMPILE_GATE_TASKS: List[Dict[str, Any]] = [
    {"polarity": "negative", "subsystem": "behavior_tree", "gate": "validate_invalid",
     "description": "An empty Behavior Tree (root with no children) must make "
                    "validate_behavior_tree report valid==false with an error-severity issue",
     "asset_path": BT_EMPTY_SCRATCH,
     "setup_chain": [
         {"op": "delete_behavior_tree", "allow_absent": True,
          "args": {"action": "delete_behavior_tree", "asset_path": BT_EMPTY_SCRATCH}},
         {"op": "create_behavior_tree",
          "args": {"action": "create_behavior_tree", "save_path": BT_EMPTY_SCRATCH,
                   "name": "BT_BenchNegativeGateScratch"}}],
     "gate_args": {"action": "validate_behavior_tree", "asset_path": BT_EMPTY_SCRATCH},
     "cleanup_chain": [
         {"op": "delete_behavior_tree",
          "args": {"action": "delete_behavior_tree", "asset_path": BT_EMPTY_SCRATCH}}],
     "cleanup_verify": {"action": "get_behavior_tree", "asset_path": BT_EMPTY_SCRATCH,
                        "expect_not_found": True},
     "expect_valid": False},
    {"polarity": "positive", "subsystem": "behavior_tree", "gate": "validate_valid",
     "description": "A well-formed Behavior Tree (Selector root child + linked Blackboard) must make "
                    "validate_behavior_tree report valid==true",
     "asset_path": BT_VALIDATE_SCRATCH,
     "setup_chain": [
         {"op": "delete_behavior_tree", "allow_absent": True,
          "args": {"action": "delete_behavior_tree", "asset_path": BT_VALIDATE_SCRATCH}},
         {"op": "create_behavior_tree",
          "args": {"action": "create_behavior_tree", "save_path": BT_VALIDATE_SCRATCH,
                   "name": "BT_BenchPositiveGateScratch", "blackboard_path": BB_PATH}},
         {"op": "add_bt_node",
          "args": {"action": "add_bt_node", "asset_path": BT_VALIDATE_SCRATCH,
                   "node_class": "BTComposite_Selector"}}],
     "gate_args": {"action": "validate_behavior_tree", "asset_path": BT_VALIDATE_SCRATCH},
     "cleanup_chain": [
         {"op": "delete_behavior_tree",
          "args": {"action": "delete_behavior_tree", "asset_path": BT_VALIDATE_SCRATCH}}],
     "cleanup_verify": {"action": "get_behavior_tree", "asset_path": BT_VALIDATE_SCRATCH,
                        "expect_not_found": True},
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


def clear_run_outputs(output_dir: pathlib.Path) -> None:
    """Remove only known run outputs so a failed rerun cannot expose stale success."""
    for filename in RUN_OUTPUT_FILENAMES:
        path = output_dir / filename
        if path.exists():
            path.unlink()


def write_run_failure(output_dir: pathlib.Path, payload: Dict[str, Any]) -> None:
    """Persist one machine-readable invalid-run record; never write summary.json."""
    payload.setdefault("created_at", utc_now())
    payload["run_valid"] = False
    payload["metrics_valid"] = False
    write_json(output_dir / "run_failure.json", payload)


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


# Declares this traffic as synthetic benchmark fixtures so the invocation-log
# analyzer does not report deliberate negative probes as real, unmet demand.
_BENCHMARK_ROUTING_CONTEXT = benchmark_routing_context("AICapability")


def mcp_call(url: str, tool: str, arguments: Dict[str, Any], timeout_s: float = 45.0) -> Dict[str, Any]:
    body = {
        "jsonrpc": "2.0",
        "id": int(time.time() * 1000) % 1000000000,
        "method": "tools/call",
        "params": {"name": tool, "arguments": arguments},
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


def searchable_text(response: Dict[str, Any]) -> str:
    """Token-search surface for a response: content[0].text plus the structured
    payload JSON. The live server returns a terse text ("OK; see
    structuredContent.") with the real data in structuredContent, so text-only
    token checks silently fail on healthy responses (observed 2026-07-11: the
    blackboard fixture preflight missed BenchTargetActor although
    get_blackboard returned all keys in structuredContent)."""
    text = result_text(response)
    data = result_data(response)
    if isinstance(data, dict) and data:
        return f"{text}\n{json.dumps(data, ensure_ascii=False, sort_keys=True)}"
    return text


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
    if classify_protocol_failure(response):
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


def _is_expected_absence_error(response: Dict[str, Any], action: str) -> bool:
    """Return true only for an explicit remove/delete "already absent" result.

    Setup reset steps may legitimately encounter a missing scratch entity. They must not accept a
    generic failure merely because the action name starts with ``remove_`` or ``delete_``: doing so
    can make the subsequent first create fail while the benchmark incorrectly claims setup passed.
    """
    if not _is_error(response) or not action.startswith(("remove_", "delete_")):
        return False
    text_l = result_text(response).lower()
    return any(token in text_l for token in ("not found", "does not exist", "no such"))


def classify_protocol_failure(response: Any) -> str:
    """Classify malformed JSON-RPC/MCP envelopes without conflating valid isError results."""
    return classify_mcp_protocol_failure(response)


def classify_mcp_failure(response: Dict[str, Any]) -> str:
    if response.get("transport_error"):
        return "transport_error"
    if classify_protocol_failure(response):
        return "protocol_error"
    if _is_error(response):
        return "server_error"
    return ""


def validate_status_response(response: Any) -> Dict[str, Any]:
    """Validate the mandatory status boundary before any scored task executes."""
    return validate_mcp_status_response(
        response,
        result_payload=result_payload,
        result_data=result_data,
    )


class TaskMcpRecorder:
    """Record every MCP transport/protocol event produced while scoring one task."""

    def __init__(self) -> None:
        self.transport_events: List[Dict[str, Any]] = []
        self.protocol_events: List[Dict[str, Any]] = []

    def call(
        self,
        url: str,
        tool: str,
        arguments: Dict[str, Any],
        timeout_s: float = 45.0,
    ) -> Dict[str, Any]:
        response = mcp_call(url, tool, arguments, timeout_s=timeout_s)
        if not isinstance(response, dict):
            response = {
                "protocol_error": True,
                "raw": str(response)[:500],
                "error": "MCP response top-level JSON must be an object",
            }
        event = {
            "tool": tool,
            "action": str(arguments.get("action", "")),
            "raw": str(response.get("raw", response))[:500],
        }
        if response.get("transport_error"):
            status = response.get("status")
            event["status"] = (
                status if isinstance(status, int) and not isinstance(status, bool) else None
            )
            self.transport_events.append(event)
        elif classify_protocol_failure(response):
            self.protocol_events.append(event)
        return response

    def decorate(self, row: Dict[str, Any]) -> Dict[str, Any]:
        last_transport = self.transport_events[-1] if self.transport_events else {}
        last_protocol = self.protocol_events[-1] if self.protocol_events else {}
        row["transport_error"] = bool(self.transport_events)
        row["transport_status"] = last_transport.get("status")
        row["transport_error_raw"] = str(last_transport.get("raw", ""))
        row["transport_failure_call_count"] = len(self.transport_events)
        row["last_transport_tool"] = str(last_transport.get("tool", ""))
        row["last_transport_action"] = str(last_transport.get("action", ""))
        row["protocol_error"] = bool(self.protocol_events)
        row["protocol_error_raw"] = str(last_protocol.get("raw", ""))
        row["protocol_failure_call_count"] = len(self.protocol_events)
        row["last_protocol_tool"] = str(last_protocol.get("tool", ""))
        row["last_protocol_action"] = str(last_protocol.get("action", ""))
        row["failure_kind"] = "protocol_error" if self.protocol_events else ""
        if self.transport_events or self.protocol_events:
            row["direct_success"] = False
        return row


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
    text = searchable_text(response)
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


def _verify_readback(
    url: str,
    verify: Dict[str, Any],
    timeout_s: float,
    call_fn: Callable[..., Dict[str, Any]],
) -> Tuple[bool, Dict[str, Any]]:
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
    resp = call_fn(url, AI_TOOL, read_call, timeout_s=timeout_s)
    if (
        resp.get("transport_error")
        or classify_protocol_failure(resp)
        or _is_error(resp)
    ):
        return False, {"read_action": read_action, "read_failed": True,
                       "snippet": str(resp.get("raw") or result_text(resp))[:200]}
    ok = True
    detail: Dict[str, Any] = {"read_action": read_action}
    text = searchable_text(resp)
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


def _verify_cleanup_state(
    url: str,
    verify: Dict[str, Any],
    timeout_s: float,
    call_fn: Callable[..., Dict[str, Any]],
) -> Tuple[bool, Dict[str, Any]]:
    """Prove cleanup from the public read surface instead of trusting a delete return value."""
    action = str(verify.get("action", ""))
    args = {
        key: value for key, value in verify.items()
        if key not in {"expect_not_found", "absent"}
    }
    response = call_fn(url, AI_TOOL, args, timeout_s=timeout_s)
    protocol_ok = not response.get("transport_error") and not classify_protocol_failure(response)
    text_l = result_text(response).lower()
    detail: Dict[str, Any] = {
        "action": action,
        "is_error": _is_error(response),
        "snippet": result_text(response)[:200],
    }

    if verify.get("expect_not_found") is True:
        asset_path = str(verify.get("asset_path", ""))
        explicit_absence = (
            protocol_ok
            and _is_error(response)
            and asset_path.lower() in text_l
            and any(token in text_l for token in ("not found", "does not exist", "no such"))
        )
        detail.update({"expect_not_found": True, "asset_path": asset_path,
                       "explicit_absence": explicit_absence})
        return explicit_absence, detail

    absent = verify.get("absent")
    if isinstance(absent, list) and absent:
        data = result_data(response)
        present_names = _collect_names(data)
        still_present = [str(name) for name in absent if str(name) in present_names]
        ok = protocol_ok and not _is_error(response) and not still_present
        detail.update({"absent": [str(name) for name in absent],
                       "still_present": still_present})
        return ok, detail

    detail["reason"] = "cleanup_verify has no supported assertion"
    return False, detail


def _verify_cleanup_contract(
    url: str,
    verify: Any,
    timeout_s: float,
    call_fn: Callable[..., Dict[str, Any]],
) -> Tuple[bool, Dict[str, Any]]:
    """Verify one or more cleanup postconditions without weakening the single-check contract."""
    if isinstance(verify, dict):
        return _verify_cleanup_state(url, verify, timeout_s, call_fn)
    if not isinstance(verify, list) or not verify or not all(isinstance(item, dict) for item in verify):
        return False, {"reason": "cleanup_verify must be a non-empty object or object list"}

    checks: List[Dict[str, Any]] = []
    all_ok = True
    for item in verify:
        check_ok, detail = _verify_cleanup_state(url, item, timeout_s, call_fn)
        all_ok = all_ok and check_ok
        checks.append({"ok": check_ok, **detail})
    return all_ok, {"checks": checks}


# ---------------------------------------------------------------------------
# Scorers
# ---------------------------------------------------------------------------

def _run_chain_step(url: str, op_args: Dict[str, Any], captured: Dict[str, str],
                    timeout_s: float, tolerant: bool,
                    call_fn: Callable[..., Dict[str, Any]]) -> Tuple[bool, Dict[str, Any], Dict[str, Any]]:
    """Run one chain step. Returns (step_ok, evidence, raw_response). In tolerant mode a leading
    remove_*/delete_* reporting "not found" and an "already exists" create are accepted (they only
    construct the precondition); the scored signal is the final read-back."""
    args = _subst_ids(dict(op_args), captured)
    resp = call_fn(url, AI_TOOL, args, timeout_s=timeout_s)
    action = str(args.get("action", ""))
    transport_ok = not resp.get("transport_error") and not classify_protocol_failure(resp)
    is_err = _is_error(resp)
    text_l = result_text(resp).lower()
    already = is_err and ("exist" in text_l or "already" in text_l)
    remove_missing = _is_expected_absence_error(resp, action)
    step_ok = transport_ok and (not is_err or (tolerant and (already or remove_missing)))
    evidence = {"action": action, "is_error": is_err, "already": already,
                "remove_missing": remove_missing, "ok": step_ok,
                "snippet": result_text(resp)[:120]}
    return step_ok, evidence, resp


def _score_edit_execute_chain(
    url: str,
    task: Dict[str, Any],
    timeout_s: float,
    call_fn: Callable[..., Dict[str, Any]],
) -> Dict[str, Any]:
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
        step_ok, ev, resp = _run_chain_step(
            url,
            step.get("args", {}),
            captured,
            timeout_s,
            tolerant=True,
            call_fn=call_fn,
        )
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
            v_ok, verify_detail = _verify_readback(url, verify, timeout_s, call_fn)
            ok = ok and v_ok

    primary_ok = ok
    cleanup_ok = True
    cleanup_evidence: List[Dict[str, Any]] = []
    cleanup_steps = task.get("cleanup_chain", [])
    cleanup_attempted = isinstance(cleanup_steps, list) and bool(cleanup_steps)
    for step in cleanup_steps if cleanup_attempted else []:
        cleanup_args = _subst_ids(dict(step.get("args", {})), captured)
        cleanup_step_ok, cleanup_detail, _ = _run_chain_step(
            url,
            cleanup_args,
            captured,
            timeout_s,
            tolerant=bool(step.get("allow_absent", False)),
            call_fn=call_fn,
        )
        cleanup_ok = cleanup_ok and cleanup_step_ok
        cleanup_evidence.append(cleanup_detail)

    cleanup_verify = task.get("cleanup_verify")
    cleanup_verify_attempted = cleanup_attempted and cleanup_ok and cleanup_verify is not None
    cleanup_verify_ok = cleanup_verify is None
    cleanup_verify_evidence: Dict[str, Any] = {}
    if cleanup_verify_attempted:
        cleanup_verify_ok, cleanup_verify_evidence = _verify_cleanup_contract(
            url, cleanup_verify, timeout_s, call_fn)
    ok = primary_ok and cleanup_ok and cleanup_verify_ok

    return {
        "task_id": task.get("id"),
        "category": "edit_execute",
        "namespace": "ai",
        "action": task.get("action"),
        "subsystem": task.get("subsystem", ""),
        "edit_action": task.get("edit_action", ""),
        "direct_success": ok,
        "planning_signals": False,
        "evidence": {"steps": steps_evidence, "captured": captured, "verify": verify_detail,
                     "primary_ok": primary_ok,
                     "cleanup_attempted": cleanup_attempted,
                     "cleanup_ok": cleanup_ok,
                     "cleanup_steps": cleanup_evidence,
                     "cleanup_verify_attempted": cleanup_verify_attempted,
                     "cleanup_verify_ok": cleanup_verify_ok,
                     "cleanup_verify": cleanup_verify_evidence},
        "transport_error": False,
        "transport_error_raw": "",
        "response_is_error": not ok,
        "response_text": json.dumps(verify_detail)[:500],
    }


def _score_duplicate_reject(
    url: str,
    task: Dict[str, Any],
    timeout_s: float,
    call_fn: Callable[..., Dict[str, Any]],
) -> Dict[str, Any]:
    """Call the create action twice and require a duplicate-specific second-call error.

    The first call must start from a demonstrably clean benchmark-owned scratch entity. A reset
    error is accepted only when it explicitly reports that the entity is already absent. After the
    assertion, cleanup must succeed so a valid run cannot leave mutable benchmark state behind.
    """
    args = dict(task.get("arguments", {}))

    setup_ok = True
    setup_evidence: List[Dict[str, Any]] = []
    for s in task.get("setup_arguments", []) or []:
        s_resp = call_fn(url, AI_TOOL, dict(s), timeout_s=timeout_s)
        s_action = str(s.get("action", ""))
        s_err = _is_error(s_resp)
        expected_absence = _is_expected_absence_error(s_resp, s_action)
        step_ok = (
            not s_resp.get("transport_error")
            and not classify_protocol_failure(s_resp)
            and (not s_err or expected_absence)
        )
        setup_ok = setup_ok and step_ok
        setup_evidence.append({
            "action": s_action,
            "ok": step_ok,
            "is_error": s_err,
            "expected_absence": expected_absence,
            "snippet": result_text(s_resp)[:200],
        })
        if not step_ok:
            break

    if not setup_ok:
        return {
            "task_id": task.get("id"),
            "category": "duplicate_reject",
            "namespace": "ai",
            "action": task.get("action"),
            "subsystem": task.get("subsystem", ""),
            "direct_success": False,
            "planning_signals": False,
            "evidence": {
                "setup_ok": False,
                "setup_steps": setup_evidence,
                "first_call_attempted": False,
                "second_call_attempted": False,
                "cleanup_attempted": False,
            },
            "transport_error": False,
            "transport_error_raw": "",
            "response_is_error": True,
            "response_text": "duplicate probe skipped because scratch reset failed",
        }

    first = call_fn(url, AI_TOOL, dict(args), timeout_s=timeout_s)
    first_is_error = _is_error(first)
    first_ok = (
        not first.get("transport_error")
        and not classify_protocol_failure(first)
        and not first_is_error
    )

    # A second call is meaningful only after a confirmed clean first create. If the first response
    # is unavailable or failed, skip the assertion call and proceed directly to cleanup: the server
    # may still have applied a request whose response was lost, so cleanup remains mandatory.
    second_attempted = first_ok
    second = call_fn(url, AI_TOOL, dict(args), timeout_s=timeout_s) if second_attempted else {}

    cleanup_ok = True
    cleanup_evidence: List[Dict[str, Any]] = []
    for cleanup in task.get("cleanup_arguments", []) or []:
        cleanup_resp = call_fn(url, AI_TOOL, dict(cleanup), timeout_s=timeout_s)
        cleanup_action = str(cleanup.get("action", ""))
        cleanup_step_ok = (
            not cleanup_resp.get("transport_error")
            and not classify_protocol_failure(cleanup_resp)
            and not _is_error(cleanup_resp)
        )
        cleanup_ok = cleanup_ok and cleanup_step_ok
        cleanup_evidence.append({
            "action": cleanup_action,
            "ok": cleanup_step_ok,
            "is_error": _is_error(cleanup_resp),
            "snippet": result_text(cleanup_resp)[:200],
        })

    cleanup_verify = task.get("cleanup_verify")
    cleanup_verify_attempted = cleanup_ok and isinstance(cleanup_verify, dict)
    cleanup_verify_ok = not isinstance(cleanup_verify, dict)
    cleanup_verify_evidence: Dict[str, Any] = {}
    if cleanup_verify_attempted:
        cleanup_verify_ok, cleanup_verify_evidence = _verify_cleanup_contract(
            url, cleanup_verify, timeout_s, call_fn)

    transport_error = bool(second.get("transport_error")) if second_attempted else False
    parse_error = bool(classify_protocol_failure(second)) if second_attempted else False
    server_handled = second_attempted and not transport_error and not parse_error
    second_is_error = _is_error(second) if second_attempted else False

    second_text = result_text(second).lower()
    second_is_duplicate = second_is_error and any(
        tok in second_text for tok in ("already", "exist", "duplicate", "in use", "taken"))

    direct_success = (
        setup_ok
        and first_ok
        and server_handled
        and second_is_duplicate
        and cleanup_ok
        and cleanup_verify_ok
    )

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
            "setup_steps": setup_evidence,
            "first_call_ok": first_ok,
            "first_call_is_error": first_is_error,
            "second_call_attempted": second_attempted,
            "second_call_handled": server_handled,
            "second_call_is_error": second_is_error,
            "second_is_duplicate": second_is_duplicate,
            "cleanup_ok": cleanup_ok,
            "cleanup_steps": cleanup_evidence,
            "cleanup_verify_attempted": cleanup_verify_attempted,
            "cleanup_verify_ok": cleanup_verify_ok,
            "cleanup_verify": cleanup_verify_evidence,
            "response_snippet": result_text(second)[:200],
        },
        "transport_error": transport_error,
        "transport_error_raw": str(second.get("raw", ""))[:300] if transport_error else "",
        "response_is_error": second_is_error or first_is_error,
        "response_text": result_text(second if second_attempted else first)[:500],
    }


def _validate_is_valid(response: Dict[str, Any]) -> Tuple[Optional[bool], Dict[str, Any]]:
    """Read the boolean ``valid`` from a validate_behavior_tree response. Returns (None, detail) if
    the call errored at transport/parse/isError level (the asset failed to LOAD, not to validate).
    validate_behavior_tree is verified to return Success with {valid:bool, issue_count:int,
    issues:[{severity,message}]}. detail.has_error_issue records whether an error-severity issue is
    present (used by the negative gate to assert the empty-BT error issue, not just valid==false)."""
    if response.get("transport_error") or classify_protocol_failure(response) or _is_error(response):
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


def _score_compile_gate(
    url: str,
    task: Dict[str, Any],
    timeout_s: float,
    call_fn: Callable[..., Dict[str, Any]],
) -> Dict[str, Any]:
    """Score the falsifiable quality gate. Two real, isolated probes (StateTree lint is unavailable
    on WITH_STATETREE=0, so both are Behavior Tree validate):
      validate_invalid (negative): an EMPTY Behavior Tree must make validate_behavior_tree report
                                   valid==false (with an error-severity issue) — a stub that always
                                   reports valid==true fails.
      validate_valid   (positive): a well-formed Behavior Tree must make validate_behavior_tree
                                   report valid==true — a reject-everything stub that always says
                                   invalid fails.
    The setup_chain constructs a task-owned scratch asset. Only steps explicitly marked
    ``allow_absent`` may accept a missing remove/delete target; creates and edits are strict. The
    gate is skipped after setup failure, and a created scratch package must be deleted before the
    task can pass. A None verdict (the call errored, not produced a verdict) never passes — that's
    the anti-reject-everything guard."""
    gate = str(task.get("gate", ""))
    captured: Dict[str, str] = {}
    steps_evidence: List[Dict[str, Any]] = []
    setup_ok = True
    has_lifecycle_create = any(
        str(step.get("args", {}).get("action", "")).startswith("create_")
        for step in task.get("setup_chain", [])
    )
    lifecycle_created = False
    for step in task.get("setup_chain", []):
        step_ok, ev, resp = _run_chain_step(
            url,
            step.get("args", {}),
            captured,
            timeout_s,
            tolerant=bool(step.get("allow_absent", False)),
            call_fn=call_fn,
        )
        step_action = str(step.get("args", {}).get("action", ""))
        if step_action.startswith("create_") and step_ok and not _is_error(resp):
            lifecycle_created = True
        capture = step.get("capture")
        if capture:
            op_action = str(step.get("args", {}).get("action", ""))
            nid = _extract_id(resp, _CAPTURE_KEYS.get(op_action, ("state_id", "node_id", "id")))
            if nid:
                captured[capture] = nid
            else:
                step_ok = False
                ev["no_captured_id"] = True
        setup_ok = setup_ok and step_ok
        steps_evidence.append(ev)
        if not step_ok:
            break

    gate_args = _subst_ids(dict(task.get("gate_args", {})), captured)
    gate_attempted = setup_ok
    gate_resp = call_fn(url, AI_TOOL, gate_args, timeout_s=timeout_s) if gate_attempted else {}

    if gate == "validate_invalid":
        # Negative gate: an EMPTY Behavior Tree must validate valid==false with an error-severity
        # issue. Requiring the error issue (not just valid==false) is the anti-reject-everything
        # guard — a stub that returns valid==false with no issues, or that errors, cannot pass.
        valid, gdet = _validate_is_valid(gate_resp)
        expect_valid = bool(task.get("expect_valid"))  # False for this gate
        has_error_issue = bool(gdet.get("has_error_issue"))
        direct_success = setup_ok and (valid is not None) and (valid == expect_valid) and has_error_issue
        gate_action = str(gate_args.get("action", "validate_behavior_tree"))
        gdet = {**gdet, "expect_valid": expect_valid, "requires_error_issue": True}
    elif gate == "validate_valid":
        valid, gdet = _validate_is_valid(gate_resp)
        expect_valid = bool(task.get("expect_valid"))
        direct_success = setup_ok and (valid is not None) and (valid == expect_valid)
        gate_action = str(gate_args.get("action", "validate_behavior_tree"))
        gdet = {**gdet, "expect_valid": expect_valid}
    else:
        direct_success = False
        gate_action = str(gate_args.get("action", ""))
        gdet = {"reason": "unknown_gate_mode", "gate": gate}

    cleanup_ok = True
    cleanup_evidence: List[Dict[str, Any]] = []
    cleanup_attempted = not has_lifecycle_create or lifecycle_created
    cleanup_steps = task.get("cleanup_chain", []) if cleanup_attempted else []
    for step in cleanup_steps:
        cleanup_args = _subst_ids(dict(step.get("args", {})), captured)
        cleanup_resp = call_fn(
            url,
            AI_TOOL,
            cleanup_args,
            timeout_s=timeout_s,
        )
        cleanup_step_ok = (
            not cleanup_resp.get("transport_error")
            and not classify_protocol_failure(cleanup_resp)
            and not _is_error(cleanup_resp)
        )
        cleanup_ok = cleanup_ok and cleanup_step_ok
        cleanup_evidence.append({
            "action": str(cleanup_args.get("action", "")),
            "ok": cleanup_step_ok,
            "is_error": _is_error(cleanup_resp),
            "snippet": str(cleanup_resp.get("raw") or result_text(cleanup_resp))[:120],
        })

    cleanup_verify = task.get("cleanup_verify")
    cleanup_verify_attempted = cleanup_attempted and cleanup_ok and isinstance(cleanup_verify, dict)
    cleanup_verify_ok = not isinstance(cleanup_verify, dict)
    cleanup_verify_evidence: Dict[str, Any] = {}
    if cleanup_verify_attempted:
        cleanup_verify_ok, cleanup_verify_evidence = _verify_cleanup_contract(
            url, cleanup_verify, timeout_s, call_fn)
    direct_success = direct_success and cleanup_ok and cleanup_verify_ok

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
        "evidence": {"setup_ok": setup_ok, "setup_steps": steps_evidence,
                     "gate_attempted": gate_attempted,
                     "lifecycle_created": lifecycle_created,
                     "cleanup_attempted": cleanup_attempted,
                     "cleanup_ok": cleanup_ok, "cleanup_steps": cleanup_evidence,
                     "cleanup_verify_attempted": cleanup_verify_attempted,
                     "cleanup_verify_ok": cleanup_verify_ok,
                     "cleanup_verify": cleanup_verify_evidence,
                     "gate_detail": gdet,
                     "gate_snippet": result_text(gate_resp)[:300]},
        "transport_error": bool(gate_resp.get("transport_error")),
        "transport_error_raw": str(gate_resp.get("raw", ""))[:300] if gate_resp.get("transport_error") else "",
        "response_is_error": not direct_success,
        "response_text": result_text(gate_resp)[:500],
    }


def score_task(url: str, task: Dict[str, Any], timeout_s: float) -> Dict[str, Any]:
    category = task.get("category", "")
    recorder = TaskMcpRecorder()
    if category == "edit_execute":
        return recorder.decorate(_score_edit_execute_chain(url, task, timeout_s, recorder.call))
    if category == "duplicate_reject":
        return recorder.decorate(_score_duplicate_reject(url, task, timeout_s, recorder.call))
    if category == "compile_gate":
        return recorder.decorate(_score_compile_gate(url, task, timeout_s, recorder.call))

    response = recorder.call(
        url,
        str(task["tool"]),
        dict(task.get("arguments", {})),
        timeout_s=timeout_s,
    )
    data = result_data(response)

    transport_error = bool(response.get("transport_error"))
    parse_error = bool(classify_protocol_failure(response))
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

    return recorder.decorate({
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
    })


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
        task = {**base, "chain": spec["chain"], "verify": spec["verify"]}
        if "cleanup_chain" in spec:
            task["cleanup_chain"] = spec["cleanup_chain"]
        if "cleanup_verify" in spec:
            task["cleanup_verify"] = spec["cleanup_verify"]
        return task
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


def _cleanup_verify_is_meaningful(verify: Any) -> bool:
    checks = verify if isinstance(verify, list) else [verify]
    if not checks or not all(isinstance(check, dict) for check in checks):
        return False
    return all(
        check.get("expect_not_found") is True
        or (isinstance(check.get("absent"), list) and bool(check.get("absent")))
        for check in checks
    )


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
        category = task.get("category")
        if category == "edit_execute" and not _verify_is_meaningful(task.get("verify")):
            raise RuntimeError(f"{task_id} edit_execute has a read_action verify with no assertion verb "
                               f"(content-free no-op read-back): {task.get('verify')}")
        if category == "edit_execute" and any(
            str(step.get("args", {}).get("action", "")).startswith("create_")
            for step in task.get("chain", [])
        ):
            cleanup_chain = task.get("cleanup_chain")
            if not isinstance(cleanup_chain, list) or not cleanup_chain:
                raise RuntimeError(f"{task_id} create edit_execute must declare cleanup actions")
            if not _cleanup_verify_is_meaningful(task.get("cleanup_verify")):
                raise RuntimeError(f"{task_id} create edit_execute must verify every cleaned state")
        if category == "duplicate_reject":
            setup = task.get("setup_arguments")
            cleanup = task.get("cleanup_arguments")
            if not isinstance(setup, list) or not setup or not isinstance(cleanup, list) or not cleanup:
                raise RuntimeError(f"{task_id} duplicate_reject must declare reset and cleanup actions")
            if setup != cleanup:
                raise RuntimeError(f"{task_id} duplicate_reject reset and cleanup targets must match")
            cleanup_verify = task.get("cleanup_verify")
            if not _cleanup_verify_is_meaningful(cleanup_verify):
                raise RuntimeError(f"{task_id} duplicate_reject must verify the cleaned state")
            save_path = str(task.get("arguments", {}).get("save_path", ""))
            if save_path in {BB_PATH, BT_PATH, EQS_PATH}:
                raise RuntimeError(f"{task_id} duplicate_reject cannot mutate persistent fixture {save_path}")
        if category == "compile_gate":
            setup_chain = task.get("setup_chain")
            cleanup_chain = task.get("cleanup_chain")
            if not isinstance(setup_chain, list) or not setup_chain:
                raise RuntimeError(f"{task_id} compile_gate must create its scratch asset")
            if not isinstance(cleanup_chain, list) or not cleanup_chain:
                raise RuntimeError(f"{task_id} compile_gate must delete its scratch asset")
            cleanup_verify = task.get("cleanup_verify")
            if not _cleanup_verify_is_meaningful(cleanup_verify):
                raise RuntimeError(f"{task_id} compile_gate must verify scratch deletion")
            create_steps = [
                step for step in setup_chain
                if str(step.get("args", {}).get("action", "")).startswith("create_")
            ]
            if len(create_steps) != 1:
                raise RuntimeError(f"{task_id} compile_gate must contain exactly one scratch create step")
            asset_path = str(task.get("asset_path", ""))
            create_path = str(create_steps[0].get("args", {}).get("save_path", ""))
            cleanup_path = str(cleanup_chain[-1].get("args", {}).get("asset_path", ""))
            if not asset_path or create_path != asset_path or cleanup_path != asset_path:
                raise RuntimeError(f"{task_id} compile_gate create/gate/cleanup paths must match")
            if asset_path in {BB_PATH, BT_PATH, EQS_PATH}:
                raise RuntimeError(f"{task_id} compile_gate cannot mutate persistent fixture {asset_path}")


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
        if "cleanup_arguments" in spec:
            task["cleanup_arguments"] = spec["cleanup_arguments"]
        if "cleanup_verify" in spec:
            task["cleanup_verify"] = spec["cleanup_verify"]
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
            "cleanup_verify": spec.get("cleanup_verify"),
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
        },
        "task_owned_scratch_paths": {
            "template_behavior_tree": BT_TEMPLATE_SCRATCH,
            "template_blackboard": BB_TEMPLATE_SCRATCH,
            "negative_compile_gate": BT_EMPTY_SCRATCH,
            "positive_compile_gate": BT_VALIDATE_SCRATCH,
            "duplicate_blackboard": BB_DUPLICATE_SCRATCH,
            "duplicate_behavior_tree": BT_DUPLICATE_SCRATCH,
            "duplicate_eqs": EQS_DUPLICATE_SCRATCH,
        },
        "fixture_root": FIXTURE_ROOT,
        "setup_fixtures_command": "python Scripts/ai_capability_benchmark.py setup_fixtures --mcp-url http://localhost:9316/mcp",
        "run_command": "python Scripts/ai_capability_benchmark.py run --mcp-url http://localhost:9316/mcp --output-dir Saved/Monolith/Benchmarks/AICapability/<label> --label <label>",
        "score_dimensions": list(SCORE_DIMENSIONS),
        "run_gates": {
            "max_transport_failed_fraction": DEFAULT_MAX_TRANSPORT_FAILED_FRACTION,
            "max_consecutive_transport_failures": DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES,
            "min_transport_fraction_sample": DEFAULT_MIN_TRANSPORT_FRACTION_SAMPLES,
            "status_transport_failure_aborts_before_tasks": True,
            "invalid_status_response_aborts_before_tasks": True,
            "invalid_run_writes_summary": False,
        },
        "catalog_version_verified": "ai-182-actions (Saved/Monolith/LogAnalysis/_ai_catalog.txt + Source/MonolithAI RegisterAction verified 2026-06-18; StateTree create/lint registered but stubbed on WITH_STATETREE=0)",
        "task_file": display_path(tasks_path),
    }
    write_json(manifest_path, manifest)
    return manifest


# ---------------------------------------------------------------------------
# Preflight + Setup Fixtures
# ---------------------------------------------------------------------------

def endpoint_preflight(url: str, timeout_s: float) -> Dict[str, Any]:
    try:
        response = mcp_call(url, "monolith_status", {}, timeout_s=timeout_s)
    except Exception as exc:  # noqa: BLE001 - preflight must return structured diagnostics.
        response = {
            "runner_exception": True,
            "raw": f"{type(exc).__name__}: {exc}",
        }
    validation = validate_status_response(response)
    if isinstance(response, dict) and response.get("runner_exception"):
        validation = {
            "ok": False,
            "failure_kind": "runner_exception",
            "raw": str(response.get("raw", ""))[:500],
            "transport_status": None,
        }
    ok = bool(validation.get("ok"))
    failure_kind = str(validation.get("failure_kind", ""))
    return {
        "ok": ok, "phase": "endpoint", "failure_kind": failure_kind,
        "message": "MCP endpoint reachable" if ok else "MCP endpoint did not return a usable monolith_status response",
        "status": validation.get("status") if ok else None,
        "raw": str(validation.get("raw", ""))[:300],
        "transport_status": validation.get("transport_status"),
    }


# (fixture_asset, read_action, contains_token) — the readiness probe for each seeded fixture.
_FIXTURE_READINESS: List[Tuple[str, str, str, List[str]]] = [
    (BB_PATH, "get_blackboard", "blackboard", [k for k, _ in FIXTURE_BB_KEYS][:1]),
    (BT_PATH, "get_behavior_tree", "behavior_tree", []),
    (EQS_PATH, "get_eqs_query", "eqs", []),
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
        transport_status = resp.get("status")
        fixture = {"subsystem": subsystem, "asset_path": asset_path, "read_action": read_action,
                   "ok": ok, "failure_kind": failure or ("" if ok else "fixture_contract_missing"),
                   "transport_status": (
                       transport_status
                       if isinstance(transport_status, int) and not isinstance(transport_status, bool)
                       else None
                   ),
                   "snippet": str(resp.get("raw") or result_text(resp))[:200]}
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


def _setup_step(url: str, args: Dict[str, Any], timeout_s: float, tolerate_exists: bool = True,
                tool: str = AI_TOOL) -> Dict[str, Any]:
    resp = mcp_call(url, tool, dict(args), timeout_s=timeout_s)
    is_err = _is_error(resp)
    already = is_err and ("exist" in result_text(resp).lower() or "already" in result_text(resp).lower())
    success = (not resp.get("transport_error") and not classify_protocol_failure(resp)
               and (not is_err or (tolerate_exists and already)))
    return {"action": str(args.get("action", "")), "success": success, "already_exists": already,
            "is_error": is_err, "failure_kind": classify_mcp_failure(resp),
            "snippet": result_text(resp)[:120]}


def setup_fixtures(url: str, timeout_s: float) -> Dict[str, Any]:
    """Create the AICapability fixtures at /Game/Benchmarks/AI/: a Blackboard (with seed keys), a
    Behavior Tree (linked to the Blackboard), and an EQS query. The compile-gate Behavior Trees are
    task-owned transient packages and are intentionally not persistent setup fixtures.
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
    # Persist the seeded keys: add_bb_key edits the loaded asset in memory only,
    # and an editor restart reverts the Blackboard to its on-disk state
    # (observed 2026-07-11: BB_BenchAI came back SelfActor-only after a crash,
    # failing the fixture preflight until re-seeded). create_* actions save
    # themselves; the key edits need an explicit save.
    steps.append(_setup_step(url, {"action": "save_asset", "asset_path": BB_PATH}, timeout_s,
                             tool="asset_query"))

    # 2. Behavior Tree (link the Blackboard)
    steps.append(_setup_step(url, {"action": "create_behavior_tree", "save_path": BT_PATH,
                                   "name": "BT_BenchAI", "blackboard_path": BB_PATH}, timeout_s))

    # 3. EQS query
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

def runner_exception_task_row(task: Dict[str, Any], error: str) -> Dict[str, Any]:
    """Preserve the task that exposed a benchmark implementation failure."""
    return {
        "task_id": task.get("id"),
        "category": task.get("category"),
        "namespace": task.get("namespace", "ai"),
        "action": task.get("action"),
        "subsystem": task.get("subsystem", ""),
        "direct_success": False,
        "planning_signals": False,
        "evidence": {"runner_exception": error},
        "transport_error": False,
        "transport_status": None,
        "transport_error_raw": "",
        "transport_failure_call_count": 0,
        "last_transport_tool": "",
        "last_transport_action": "",
        "protocol_error": False,
        "protocol_error_raw": "",
        "protocol_failure_call_count": 0,
        "last_protocol_tool": "",
        "last_protocol_action": "",
        "response_is_error": False,
        "response_text": "",
        "failure_kind": "runner_exception",
        "error": error,
    }


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
    """Build partial diagnostics without letting aggregate defects hide the invalid run."""
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
    failure["run_valid"] = False
    failure["metrics_valid"] = False
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
    *,
    require_fixtures: bool = True,
    allow_subset: bool = False,
    max_transport_failed_fraction: float = DEFAULT_MAX_TRANSPORT_FAILED_FRACTION,
    max_consecutive_transport_failures: int = DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES,
    min_transport_fraction_sample: int = DEFAULT_MIN_TRANSPORT_FRACTION_SAMPLES,
) -> Dict[str, Any]:
    output_dir.mkdir(parents=True, exist_ok=True)
    clear_run_outputs(output_dir)

    try:
        corpus = load_task_corpus(
            tasks_path,
            suite="AICapability",
            canonical_tasks_path=DEFAULT_TASKS,
            canonical_manifest_path=DEFAULT_MANIFEST,
            allow_subset=allow_subset,
            allowed_categories=AI_TASK_CATEGORIES,
            require_arguments=False,
        )
        tasks_path = resolve_plugin_path(tasks_path)
        tasks = corpus.tasks
        validate_task_integrity(tasks)
    except Exception as exc:  # noqa: BLE001 - invalid inputs must invalidate stale baselines.
        failure = {
            "label": label,
            "completion_status": "aborted_input_preflight",
            "failure_stage": "input_preflight",
            "failure_kind": "runner_exception",
            "metrics_scope": "not_started",
            "completed_task_count": 0,
            "total_task_count": 0,
            "error": f"{type(exc).__name__}: {exc}",
        }
        write_run_failure(output_dir, failure)
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
            "completion_status": "aborted_invalid_configuration",
            "failure_stage": "configuration",
            "failure_kind": "invalid_configuration",
            "metrics_scope": "not_started",
            "completed_task_count": 0,
            "total_task_count": len(tasks),
            "error": str(exc),
        }
        write_run_failure(output_dir, failure)
        return failure

    try:
        status_response: Any = mcp_call(url, "monolith_status", {}, timeout_s=timeout_s)
        status_validation = validate_status_response(status_response)
    except Exception as exc:  # noqa: BLE001 - status defects must create invalid artifacts.
        status_validation = {
            "ok": False,
            "failure_kind": "runner_exception",
            "raw": f"{type(exc).__name__}: {exc}",
            "transport_status": None,
        }

    if not status_validation.get("ok"):
        failure_kind = str(status_validation.get("failure_kind", "protocol_error"))
        raw = str(status_validation.get("raw", ""))[:500]
        failure = {
            "label": label,
            "completion_status": (
                "aborted_status_transport_failure"
                if failure_kind == "transport_error"
                else "aborted_status_preflight"
            ),
            "failure_stage": "status_preflight",
            "failure_kind": failure_kind,
            "metrics_scope": "not_started",
            "completed_task_count": 0,
            "total_task_count": len(tasks),
            "transport_failure_count": 1 if failure_kind == "transport_error" else 0,
            "last_transport_status": status_validation.get("transport_status"),
            "last_transport_error_raw": raw if failure_kind == "transport_error" else "",
            "protocol_error_raw": raw if failure_kind != "transport_error" else "",
            "max_transport_failed_fraction": max_transport_failed_fraction,
            "max_consecutive_transport_failures": max_consecutive_transport_failures,
            "min_transport_fraction_sample": min_transport_fraction_sample,
        }
        write_run_failure(output_dir, failure)
        return failure

    status = dict(status_validation["status"])
    start_identity = status_identity(status, endpoint=url)
    benchmark_inputs = build_benchmark_inputs(
        "AICapability", tasks_path=tasks_path, mcp_status=status
    )

    if require_fixtures:
        try:
            fixture_preflight = fixture_readiness_preflight(
                url, timeout_s, require_fixtures=True
            )
        except Exception as exc:  # noqa: BLE001 - fixture runner defects invalidate the run.
            fixture_preflight = {
                "ok": False,
                "phase": "fixtures",
                "failure_kind": "runner_exception",
                "message": f"{type(exc).__name__}: {exc}",
            }
        if not fixture_preflight.get("ok"):
            failure_kind = str(fixture_preflight.get(
                "failure_kind", "fixture_readiness_failed"
            ))
            first_failure = fixture_preflight.get("first_failure")
            first_fixture = (
                first_failure.get("fixture", {})
                if isinstance(first_failure, dict)
                else {}
            )
            failure_diagnostic = first_fixture or (
                fixture_preflight.get("endpoint", {})
                if isinstance(fixture_preflight.get("endpoint"), dict)
                else {}
            )
            failure = {
                "label": label,
                "completion_status": "aborted_fixture_preflight",
                "failure_stage": "fixture_preflight",
                "failure_kind": failure_kind,
                "metrics_scope": "not_started",
                "completed_task_count": 0,
                "total_task_count": len(tasks),
                "transport_failure_count": 1 if failure_kind == "transport_error" else 0,
                "last_transport_status": failure_diagnostic.get("transport_status"),
                "last_transport_error_raw": (
                    str(failure_diagnostic.get("snippet", failure_diagnostic.get("raw", "")))
                    if failure_kind == "transport_error" else ""
                ),
                "protocol_error_raw": (
                    str(failure_diagnostic.get("snippet", failure_diagnostic.get("raw", "")))
                    if failure_kind == "protocol_error" else ""
                ),
                "preflight": fixture_preflight,
            }
            attach_benchmark_inputs(failure, benchmark_inputs)
            attach_run_context(failure, corpus, start_identity)
            write_run_failure(output_dir, failure)
            return failure

    rows: List[Dict[str, Any]] = []
    per_task_jsonl = output_dir / "per_task.jsonl"

    for index, task in enumerate(tasks, 1):
        runner_exception = ""
        try:
            row = score_task(url, task, timeout_s)
        except Exception as exc:  # noqa: BLE001 - preserve the triggering task and abort.
            runner_exception = f"{type(exc).__name__}: {exc}"
            row = runner_exception_task_row(task, runner_exception)
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
            f"[{index}/{len(tasks)}] {row['task_id']} category={row['category']} "
            f"success={row['direct_success']}",
            flush=True,
        )

        if runner_exception:
            failure = build_attempt_failure(
                label,
                status,
                tasks,
                rows,
                transport_tracker,
                benchmark_inputs,
                corpus,
                start_identity,
                {
                    "completion_status": "aborted_runner_exception",
                    "failure_stage": "task_scoring",
                    "failure_kind": "runner_exception",
                    "metrics_scope": "attempted_prefix_runner_exception",
                    "completed_task_count": index,
                    "total_task_count": len(tasks),
                    "last_task_id": str(task.get("id", "")),
                    "exception": runner_exception,
                },
            )
            write_run_failure(output_dir, failure)
            write_json(output_dir / "partial_summary.json", failure)
            return failure

        if row.get("failure_kind") == "protocol_error":
            failure = build_attempt_failure(
                label,
                status,
                tasks,
                rows,
                transport_tracker,
                benchmark_inputs,
                corpus,
                start_identity,
                {
                    "completion_status": "aborted_protocol_error",
                    "failure_stage": "task_response",
                    "failure_kind": "protocol_error",
                    "metrics_scope": "attempted_prefix_protocol_error",
                    "completed_task_count": index,
                    "total_task_count": len(tasks),
                    "last_task_id": str(task.get("id", "")),
                    "protocol_error_raw": str(row.get("protocol_error_raw", "")),
                },
            )
            write_run_failure(output_dir, failure)
            write_json(output_dir / "partial_summary.json", failure)
            return failure

        if transport_decision:
            failure = build_attempt_failure(
                label,
                status,
                tasks,
                rows,
                transport_tracker,
                benchmark_inputs,
                corpus,
                start_identity,
                {
                    "completion_status": "aborted_transport_failure_budget",
                    "failure_stage": "task_scoring",
                    "failure_kind": "transport_error",
                    "metrics_scope": "attempted_prefix_including_transport_failures",
                    "completed_task_count": index,
                    "total_task_count": len(tasks),
                    "transport_gate_reason": transport_decision.reason,
                    "last_task_id": transport_decision.item_id,
                },
            )
            write_run_failure(output_dir, failure)
            write_json(output_dir / "partial_summary.json", failure)
            return failure

        if index == 1 or index == len(tasks) or index % 10 == 0:
            partial = aggregate(label, status, tasks[:index], rows)
            partial.update({
                "completed_task_count": index,
                "total_task_count": len(tasks),
                "run_valid": None,
                "metrics_valid": False,
                "metrics_scope": "attempted_prefix",
                "completion_status": "in_progress",
            })
            partial.update(transport_tracker.snapshot())
            attach_benchmark_inputs(partial, benchmark_inputs)
            attach_run_context(partial, corpus, start_identity)
            write_json(output_dir / "partial_summary.json", partial)

    try:
        summary = aggregate(label, status, tasks, rows)
    except Exception as exc:  # noqa: BLE001 - aggregate defects invalidate the run.
        error = f"{type(exc).__name__}: {exc}"
        failure = build_attempt_failure(
            label,
            status,
            tasks,
            rows,
            transport_tracker,
            benchmark_inputs,
            corpus,
            start_identity,
            {
                "completion_status": "aborted_runner_exception",
                "failure_stage": "final_aggregate",
                "failure_kind": "runner_exception",
                "metrics_scope": "complete_run_invalid",
                "completed_task_count": len(rows),
                "total_task_count": len(tasks),
                "exception": error,
            },
        )
        write_run_failure(output_dir, failure)
        write_json(output_dir / "partial_summary.json", failure)
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
            "run_valid": False,
            "metrics_valid": False,
            "metrics_scope": "complete_run_invalid",
            "completion_status": "completed_transport_failure_budget_exceeded",
            "failure_kind": "transport_error",
            "transport_gate_reason": final_transport_decision.reason,
            "last_task_id": final_transport_decision.item_id,
        })
        write_run_failure(output_dir, summary)
        write_json(output_dir / "partial_summary.json", summary)
        return summary

    try:
        end_status_response: Any = mcp_call(url, "monolith_status", {}, timeout_s=timeout_s)
        end_status_validation = validate_status_response(end_status_response)
    except Exception as exc:  # noqa: BLE001 - postflight must invalidate the run.
        end_status_validation = {
            "ok": False,
            "failure_kind": "runner_exception",
            "raw": f"{type(exc).__name__}: {exc}",
            "transport_status": None,
        }
    if not end_status_validation.get("ok"):
        failure_kind = str(end_status_validation.get("failure_kind", "protocol_error"))
        summary.update({
            "run_valid": False,
            "metrics_valid": False,
            "metrics_scope": "complete_run_invalid",
            "completion_status": "aborted_status_postflight",
            "failure_stage": "status_postflight",
            "failure_kind": failure_kind,
            "postflight_status_raw": str(end_status_validation.get("raw", ""))[:500],
            "postflight_transport_status": end_status_validation.get("transport_status"),
        })
        write_run_failure(output_dir, summary)
        write_json(output_dir / "partial_summary.json", summary)
        return summary

    end_status = dict(end_status_validation["status"])
    end_identity = status_identity(end_status, endpoint=url)
    identity_drift = status_identity_mismatches(start_identity, end_identity)
    attach_run_context(summary, corpus, start_identity, end_identity)
    if identity_drift:
        summary.update({
            "run_valid": False,
            "metrics_valid": False,
            "metrics_scope": "complete_run_invalid",
            "completion_status": "aborted_status_identity_drift",
            "failure_stage": "status_postflight",
            "failure_kind": "status_identity_drift",
            "status_identity_mismatches": identity_drift,
        })
        write_run_failure(output_dir, summary)
        write_json(output_dir / "partial_summary.json", summary)
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
    run_cmd.add_argument(
        "--allow-subset",
        action="store_true",
        help="Permit an explicit non-canonical diagnostic corpus; output is marked non-comparable.",
    )
    run_cmd.add_argument(
        "--skip-preflight",
        action="store_true",
        help=(
            "Skip fixture readiness checks only; mandatory monolith_status validation "
            "always runs."
        ),
    )
    run_cmd.add_argument(
        "--max-transport-failed-fraction",
        type=float,
        default=DEFAULT_MAX_TRANSPORT_FAILED_FRACTION,
        help="Abort without summary when transport failures exceed this fraction after 20 tasks.",
    )
    run_cmd.add_argument(
        "--max-consecutive-transport-failures",
        type=int,
        default=DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES,
        help="Abort without summary after this many consecutive transport-failed tasks.",
    )
    run_cmd.add_argument(
        "--min-transport-fraction-sample",
        type=int,
        default=DEFAULT_MIN_TRANSPORT_FRACTION_SAMPLES,
        help="Minimum attempted tasks before applying the transport-fraction gate.",
    )

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
        summary = run_benchmark(
            args.mcp_url,
            args.tasks,
            args.output_dir,
            args.label,
            args.request_timeout_s,
            require_fixtures=not args.skip_preflight,
            allow_subset=args.allow_subset,
            max_transport_failed_fraction=args.max_transport_failed_fraction,
            max_consecutive_transport_failures=args.max_consecutive_transport_failures,
            min_transport_fraction_sample=args.min_transport_fraction_sample,
        )
        sys.stdout.buffer.write((json.dumps(summary, indent=2, ensure_ascii=False) + "\n").encode("utf-8"))
        return 0 if summary.get("run_valid") else 1

    if args.cmd == "compare":
        comparison = compare_runs(args.baseline, args.current, args.output_dir)
        print(json.dumps({"output_dir": str(args.output_dir), "deltas": comparison["deltas"]}, indent=2))
        return 0

    return 2


if __name__ == "__main__":
    raise SystemExit(main())
