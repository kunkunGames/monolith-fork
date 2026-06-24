#!/usr/bin/env python3
"""
Monolith MCP BlueprintEditing benchmark.

Measures the MCP server's capability to support blueprint editing workflows
across 7 Blueprint types: Actor, Character, Widget, AnimInstance, GameplayAbility,
ActorComponent, and Interface.

Eleven task categories:
  type_discovery   - project.search for benchmark-fixture names (portable; not GO-content)
  graph_read       - blueprint.list_graphs / get_graph_data / get_graph_summary
  variable_read    - blueprint.get_variables with options
  read_schema      - monolith_discover schema for 23 read actions (lenient)
  edit_schema      - monolith_discover schema for 46 edit actions (strict: isError fails)
  workflow_execute - run real multi-step workflows end-to-end and READ BACK the result
  edit_execute     - call real edit actions against fixtures AND read back the mutation
  asset_authoring  - create/edit/save high-ROI cross-domain UE asset types and read them back
  error_path       - send invalid inputs and verify an INPUT-SPECIFIC structured isError
  duplicate_reject - second identical add_* call must be refused (no silent suffix/no-op)
  negative_compile - deliberately break a graph and require a REPORTED compile error

v5.3 (2026-06-25): generative/import-pipeline and deep asset-authoring expansion.
  - asset_authoring now covers 53 create/edit/save/read-back chains, adding ImageGen Texture2D,
    ImageGen MSDF texture/material, Interchange typed texture import/static-mesh reimport-export,
    Interchange audio import, ModelGen provenance StaticMesh import, mesh LOD quality metadata,
    Montage/curve/sync metadata, IK Rig/Retargeter metadata, CommonUI, Niagara module, GAS input binding,
    Material graph/parameter instances, BehaviorTree spec-build, BehaviorTree node Blueprint,
    AIController perception, HLODLayer, Content Browser collections, UWidgetAnimation v2,
    external generated-image import, and WorldGen blockout Blueprint coverage.

v5.2 (2026-06-24): high-ROI asset-authoring expansion.
  - asset_authoring now covers 25 create/edit/save/read-back chains across Texture/Font/DataTable/
    StringTable/Input/Material/MIC/MaterialFunction/StaticMesh/CurveTable/DataAsset/UMG/
    Animation/Audio/Niagara/AI/GAS/Chooser assets.
  - The composite reserves 0.10 weight for cross-domain asset authoring while preserving v5.1's
    strict read-back, duplicate, error-path, and negative-compile gates.

v5.1 (2026-06-18): adversarial hardening + practical-coverage expansion (295->305 tasks).
  - error_path scores on the OFFENDING IDENTIFIER (specific_tokens), not generic words, so a
  reject-everything server no longer passes the inverted category.
- edit_execute CREATES are delete-first op_chains (a same-named leftover is removed first), so
  the read-back proves THIS run made the edit; add_node reads back the returned node id (${self});
  set_component_property reads back the value; add_replicated_variable asserts the replication
  flag; CDO writes assert property+value. Data-pin wiring, set_pin_default, and delete round-trips
  added.
- negative_compile (new): break a scratch BP (variable with an invalid struct type) and require
  compile_blueprint error_count>0 — makes "compile is clean" falsifiable.
- duplicate_reject requires a CLEAN first create (delete-reset each run).
- Empty contains-read-backs and empty weighted categories are now build-time errors.
- Re-weighted away from schema-fetch (0.19->0.10) toward executed work.
- Benchmark defects fixed (surfaced by the baseline-v5-pre live run = 0.967): bIsActive fixture
  var collided with UActorComponent's native bIsActive (renamed bComponentActive);
  validate_blueprint's lint-report shape is now scored correctly; interface tasks pass the full
  /Game path, backed by a MonolithBlueprint ResolveInterfaceClass fix.

v5 (2026-06-17): edit_execute became read-back verified; node-wiring executed for real; compile
steps assert error_count == 0; workflow_completeness (existence-only) -> executed workflow_execute.

The weights live in the single ``WEIGHTS`` dict below and are the sole source of truth consumed by
aggregate(), the manifest score_formula, the comparison renderer, and this docstring. Action names
verified against the live blueprint namespace catalog (v0.20.3).
"""

from __future__ import annotations

import argparse
import base64
import concurrent.futures
import datetime as _dt
import json
import math
import pathlib
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
import wave
from typing import Any, Dict, Iterable, List, Optional, Set, Tuple

from benchmark_common import attach_benchmark_inputs, build_benchmark_inputs, display_path, resolve_plugin_path


DEFAULT_MCP_URL = "http://localhost:9316/mcp"
DEFAULT_TASKS = pathlib.Path("Benchmarks/BlueprintEditing/tasks.jsonl")
DEFAULT_MANIFEST = pathlib.Path("Benchmarks/BlueprintEditing/manifest.json")
DEFAULT_RESULTS_ROOT = pathlib.Path("Saved/Monolith/Benchmarks/BlueprintEditing")

# Single source of truth for the composite score. aggregate(), the manifest score_formula,
# write_comparison_markdown(), and the module docstring all derive from this dict, so a weight
# can never drift between the code, the docs, and the comparison report again. Must sum to 1.0.
# v5 (2026-06-17): edit_execute is now read-back verified, so it absorbs weight back from the
# schema-only signals; workflow_execute (executed chains) replaces the existence-only
# workflow_completeness (0.03) at 0.10.
WEIGHTS: Dict[str, float] = {
    "edit_execute": 0.27,      # read-back-verified BP graph/class/component edits
    "asset_authoring": 0.10,   # non-graph UE asset creation/edit/save/read-back coverage
    "edit_schema": 0.07,       # residual coverage tripwire; 46 names must still resolve
    "graph_read": 0.11,
    "variable_read": 0.11,
    "error_path": 0.09,        # now input-specific (offending identifier required)
    "workflow_execute": 0.09,
    "duplicate_reject": 0.06,  # first-call-gated, delete-reset
    "negative_compile": 0.05,  # NEW: the only test that makes "compile is clean" falsifiable
    "read_schema": 0.02,       # lenient introspection tripwire (was 0.05)
    "type_discovery": 0.03,
}
assert abs(sum(WEIGHTS.values()) - 1.0) < 1e-9, "BlueprintEditing WEIGHTS must sum to 1.0"

# Dimension rate keys in canonical (scored) order — drives the metrics dict and the
# comparison-report row order so a scored dimension can never be silently omitted.
SCORE_DIMENSIONS: List[str] = [f"{name}_rate" for name in WEIGHTS]


def score_formula_string() -> str:
    """Render the human-readable score formula from WEIGHTS (used in the manifest)."""
    return " + ".join(f"{w:g}*{name}_rate" for name, w in WEIGHTS.items())


# Interface has no EventGraph; default_graph=None causes get_graph_data to be called
# without a graph_name argument (server returns the first available graph or isError).
BP_TYPES = [
    {"type": "Actor", "domain": "gameplay", "path": "/Game/Benchmarks/BPB_TestActor",
     "default_graph": "EventGraph", "fixture_vars": ["Health", "MaxHealth", "ActorTag"],
     "fixture_graphs": ["EventGraph"]},
    {"type": "Character", "domain": "gameplay", "path": "/Game/Benchmarks/BPB_TestCharacter",
     "default_graph": "EventGraph", "fixture_vars": ["MoveSpeed", "bIsSprinting", "CharacterName"],
     "fixture_graphs": ["EventGraph"]},
    {"type": "Widget", "domain": "ui", "path": "/Game/Benchmarks/WBP_TestWidget",
     "default_graph": "EventGraph", "fixture_vars": ["DisplayText", "bIsVisible"],
     "fixture_graphs": ["EventGraph"]},
    {"type": "AnimInstance", "domain": "animation", "path": "/Game/Benchmarks/ABP_TestAnim",
     "default_graph": "EventGraph", "fixture_vars": ["Speed", "bIsInAir"],
     "fixture_graphs": ["AnimGraph"]},
    {"type": "GameplayAbility", "domain": "ability", "path": "/Game/Benchmarks/GA_TestAbility",
     "default_graph": "EventGraph", "fixture_vars": ["AbilityCooldown", "AbilityCost"],
     "fixture_graphs": ["EventGraph"]},
    {"type": "ActorComponent", "domain": "component", "path": "/Game/Benchmarks/BC_TestComponent",
     "default_graph": "EventGraph", "fixture_vars": ["ComponentID", "bComponentActive"],
     "fixture_graphs": ["EventGraph"]},
    {"type": "Interface", "domain": "interface", "path": "/Game/Benchmarks/BPI_TestInterface",
     "default_graph": None, "fixture_vars": [], "fixture_graphs": []},
]

# (query, bp_type, domain, require_results)
# require_results=True: task fails if response has 0 results.
# require_results=False: any non-error response passes (broad text searches that may find 0).
#
# The require_results=True queries target the benchmark's OWN fixtures (guaranteed to exist
# after setup_fixtures), NOT incidental project content. This keeps type_discovery portable:
# a capable server scores 1.0 in any project, not only in GO where "BP_"/"AI Controller"
# assets happen to exist. The require_results=False queries keep broad-recall coverage.
TYPE_SEARCH_QUERIES: List[Tuple[str, str, str, bool]] = [
    ("BPB_TestActor", "Actor", "gameplay", True),
    ("BPB_TestCharacter", "Character", "gameplay", True),
    ("WBP_TestWidget", "Widget", "ui", True),
    ("ABP_TestAnim", "AnimInstance", "animation", True),
    ("GA_TestAbility", "GameplayAbility", "ability", True),
    ("BC_TestComponent", "ActorComponent", "component", True),
    ("BPI_TestInterface", "Interface", "interface", True),
    ("Benchmarks", "Actor", "gameplay", True),         # the fixture folder; always >=1 fixture
    ("Actor Blueprint", "Actor", "gameplay", False),   # broad text recall; project-dependent
    ("Character Blueprint", "Character", "gameplay", False),
    ("UserWidget Blueprint", "Widget", "ui", False),
    ("AnimInstance Blueprint", "AnimInstance", "animation", False),
    ("GameplayAbility Blueprint", "GameplayAbility", "ability", False),
    ("Blueprint Interface", "Interface", "interface", False),
    ("/Game/Benchmarks/BPB_TestActor", "Actor", "gameplay", True),
    ("/Game/Benchmarks/BPB_TestCharacter", "Character", "gameplay", True),
    ("/Game/Benchmarks/WBP_TestWidget", "Widget", "ui", True),
    ("/Game/Benchmarks/ABP_TestAnim", "AnimInstance", "animation", True),
    ("/Game/Benchmarks/GA_TestAbility", "GameplayAbility", "ability", True),
    ("/Game/Benchmarks/BC_TestComponent", "ActorComponent", "component", True),
    ("/Game/Benchmarks/BPI_TestInterface", "Interface", "interface", True),
]

# All 23 names verified against live blueprint namespace catalog (monolith_discover mode=actions).
BLUEPRINT_READ_ACTIONS: List[str] = [
    "list_graphs",
    "get_graph_data",
    "get_graph_summary",
    "get_execution_flow",       # replaces non-existent get_event_graph
    "get_functions",            # replaces non-existent list_functions
    "get_variables",
    "get_dependencies",         # replaces non-existent get_class_hierarchy
    "find_variable_references", # replaces non-existent find_references
    "get_components",           # replaces non-existent list_components
    "get_blueprint_info",
    "validate_blueprint",       # replaces non-existent get_compile_status
    "describe_cdo_schema",      # replaces non-existent get_compile_errors
    "get_interfaces",
    "get_event_dispatchers",
    "get_parent_class",
    "search_nodes",
    "get_component_details",
    "get_construction_script",
    "search_functions",
    "get_node_details",
    "get_interface_functions",
    "get_function_signature",
    "get_event_dispatcher_details",
]

# All 46 names verified against live blueprint namespace catalog.
BLUEPRINT_EDIT_ACTIONS: List[Tuple[str, str]] = [
    # node (7)
    ("add_node", "node"),
    ("remove_node", "node"),            # was delete_node
    ("connect_pins", "node"),
    ("disconnect_pins", "node"),        # was disconnect_pin
    ("set_node_position", "node"),      # was move_node
    ("set_pin_default", "node"),        # was set_node_property (non-existent)
    ("copy_nodes", "node"),             # was duplicate_node
    # function (8)
    ("add_function", "function"),
    ("remove_function", "function"),    # was delete_function
    ("add_event_node", "function"),     # was add_event (non-existent)
    ("rename_function", "function"),
    ("set_function_params", "function"),      # replaces set_function_inputs + set_function_outputs
    ("set_function_thread_safe", "function"), # was set_function_outputs (absorbed into params)
    ("add_macro", "function"),
    ("add_local_variable", "function"),
    # variable (7)
    ("add_variable", "variable"),
    ("remove_variable", "variable"),    # was delete_variable
    ("rename_variable", "variable"),
    ("set_variable_type", "variable"),
    ("set_variable_defaults", "variable"),    # was set_variable_default
    ("add_replicated_variable", "variable"),  # was set_variable_instance_editable (non-existent)
    ("promote_pin_to_variable", "variable"),  # was set_variable_replication (non-existent)
    # class (4)
    ("reparent_blueprint", "class"),    # was set_parent_class (non-existent)
    ("implement_interface", "class"),   # catalog name is implement_interface
    ("remove_interface", "class"),
    ("set_cdo_properties", "class"),    # was set_class_defaults (non-existent)
    # component (5) — all were already correct
    ("add_component", "component"),
    ("remove_component", "component"),
    ("rename_component", "component"),
    ("set_component_property", "component"),
    ("reparent_component", "component"),
    # compilation (2)
    ("compile_blueprint", "compilation"),  # was compile (non-existent)
    ("save_asset", "compilation"),         # was save (non-existent)
    # state (2) — undo/redo do not exist in catalog; replaced with valid state-mutation actions
    ("auto_layout", "state"),          # was undo (non-existent)
    ("duplicate_graph", "state"),      # was redo (non-existent)
    # event (3)
    ("add_event_dispatcher", "event"),
    ("remove_event_dispatcher", "event"),      # was delete_event_dispatcher
    ("set_event_dispatcher_params", "event"),  # was add_event_dispatcher_binding (non-existent)
    # v5 fixture-lifecycle expansion: schema coverage for high-use authoring actions that
    # already exist in the live catalog but were not part of the original 38-action slice.
    ("create_blueprint", "class"),
    ("duplicate_blueprint", "class"),
    ("save_dirty_assets", "compilation"),
    ("duplicate_component", "component"),
    ("add_comment_node", "node"),
    ("add_timeline", "node"),
    ("set_cdo_property", "class"),
    ("scaffold_interface_implementation", "class"),
]

# Executed end-to-end workflows (category workflow_execute). Unlike the old
# workflow_completeness check (which only asserted the action NAMES exist in the catalog),
# each of these is actually RUN step-by-step against a fixture, with add_node ids threaded
# into connect_pins (${label}) and a final read-back that must observe the end state.
# Scored by _score_op_chain. All action names + params verified against the live catalog
# (v0.20.3) and the captured live response shapes.
BLUEPRINT_WORKFLOW_EXECUTE: List[Dict[str, Any]] = [
    {
        "name": "build_function",
        "blueprint_type": "Actor", "domain": "gameplay", "edit_domain": "function",
        "asset_path": "/Game/Benchmarks/BPB_TestActor", "graph_name": "BenchWfBuildFunc",
        "description": "Add a function, build its body (two wired PrintString nodes in the function graph), compile clean, read back the function",
        "chain": [
            {"op": "add_function",
             "args": {"action": "add_function", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "function_name": "BenchWfBuildFunc"}},
            {"op": "add_node", "capture": "a",
             "args": {"action": "add_node", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "graph_name": "BenchWfBuildFunc", "node_type": "CallFunction", "function_name": "PrintString"}},
            {"op": "add_node", "capture": "b",
             "args": {"action": "add_node", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "graph_name": "BenchWfBuildFunc", "node_type": "CallFunction", "function_name": "PrintString"}},
            {"op": "connect_pins",
             "args": {"action": "connect_pins", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "graph_name": "BenchWfBuildFunc", "source_node": "${a}", "source_pin": "then",
                      "target_node": "${b}", "target_pin": "execute"}},
            {"op": "compile_blueprint",
             "args": {"action": "compile_blueprint", "asset_path": "/Game/Benchmarks/BPB_TestActor"}},
        ],
        "verify_connection": {"from": "a", "from_pin": "then", "to": "b", "to_pin": "execute"},
        "verify": {"read_action": "get_functions", "contains": ["BenchWfBuildFunc"]},
    },
    {
        "name": "implement_interface",
        "blueprint_type": "Character", "domain": "gameplay", "edit_domain": "class",
        "asset_path": "/Game/Benchmarks/BPB_TestCharacter", "graph_name": "EventGraph",
        "description": "Implement BPI_TestInterface on the Character, compile clean, read back the implemented interface",
        "chain": [
            {"op": "implement_interface",
             "args": {"action": "implement_interface", "asset_path": "/Game/Benchmarks/BPB_TestCharacter",
                      "interface_class": "/Game/Benchmarks/BPI_TestInterface"}},
            {"op": "compile_blueprint",
             "args": {"action": "compile_blueprint", "asset_path": "/Game/Benchmarks/BPB_TestCharacter"}},
        ],
        "verify": {"read_action": "get_interfaces", "contains": ["BPI_TestInterface"]},
    },
    {
        "name": "component_assembly",
        "blueprint_type": "Character", "domain": "gameplay", "edit_domain": "component",
        "asset_path": "/Game/Benchmarks/BPB_TestCharacter", "graph_name": "EventGraph",
        "description": "Add a SphereComponent, set a real property on it, compile clean, read back the component",
        "chain": [
            {"op": "add_component",
             "args": {"action": "add_component", "asset_path": "/Game/Benchmarks/BPB_TestCharacter",
                      "component_class": "SphereComponent", "component_name": "BenchWfAsmComp"}},
            {"op": "set_component_property",
             "args": {"action": "set_component_property", "asset_path": "/Game/Benchmarks/BPB_TestCharacter",
                      "component_name": "BenchWfAsmComp", "property_name": "SphereRadius", "value": "128.0"}},
            {"op": "compile_blueprint",
             "args": {"action": "compile_blueprint", "asset_path": "/Game/Benchmarks/BPB_TestCharacter"}},
        ],
        "verify": {"read_action": "get_components", "contains": ["BenchWfAsmComp"]},
    },
    {
        "name": "widget_style_refresh",
        "blueprint_type": "Widget", "domain": "ui", "edit_domain": "function",
        "asset_path": "/Game/Benchmarks/WBP_TestWidget", "graph_name": "BenchWfWidgetApplyStyle",
        "description": "Create a Widget style-refresh function, add typed style inputs, place a FormatText node, compile clean, read back the signature",
        "chain": [
            {"op": "add_function",
             "args": {"action": "add_function", "asset_path": "/Game/Benchmarks/WBP_TestWidget",
                      "function_name": "BenchWfWidgetApplyStyle", "category": "Benchmark|UI"}},
            {"op": "set_function_params",
             "args": {"action": "set_function_params", "asset_path": "/Game/Benchmarks/WBP_TestWidget",
                      "function_name": "BenchWfWidgetApplyStyle",
                      "inputs": [{"name": "AccentColor", "type": "LinearColor"}]}},
            {"op": "add_node",
             "args": {"action": "add_node", "asset_path": "/Game/Benchmarks/WBP_TestWidget",
                      "graph_name": "BenchWfWidgetApplyStyle", "node_type": "FormatText",
                      "format": "Style {AccentColor}"}},
            {"op": "compile_blueprint",
             "args": {"action": "compile_blueprint", "asset_path": "/Game/Benchmarks/WBP_TestWidget"}},
        ],
        "verify": {"read_action": "get_function_signature",
                   "read_args": {"function_name": "BenchWfWidgetApplyStyle"},
                   "contains": ["BenchWfWidgetApplyStyle", "AccentColor"]},
    },
    {
        "name": "widget_comment_layout",
        "blueprint_type": "Widget", "domain": "ui", "edit_domain": "node",
        "asset_path": "/Game/Benchmarks/WBP_TestWidget", "graph_name": "EventGraph",
        "description": "Add a UMG layout comment box, auto-layout the Widget graph, compile clean, read back the comment text",
        "chain": [
            {"op": "add_comment_node",
             "args": {"action": "add_comment_node", "asset_path": "/Game/Benchmarks/WBP_TestWidget",
                      "graph_name": "EventGraph", "text": "BenchWfWidgetLayoutZone",
                      "position": [400, 120], "width": 520, "height": 260}},
            {"op": "auto_layout",
             "args": {"action": "auto_layout", "asset_path": "/Game/Benchmarks/WBP_TestWidget",
                      "graph_name": "EventGraph"}},
            {"op": "compile_blueprint",
             "args": {"action": "compile_blueprint", "asset_path": "/Game/Benchmarks/WBP_TestWidget"}},
        ],
        "verify": {"read_action": "get_graph_data", "read_args": {"graph_name": "EventGraph"},
                   "contains": ["BenchWfWidgetLayoutZone"]},
    },
    {
        "name": "anim_threadsafe_graph",
        "blueprint_type": "AnimInstance", "domain": "animation", "edit_domain": "function",
        "asset_path": "/Game/Benchmarks/ABP_TestAnim", "graph_name": "BenchWfAnimThreadSafeUpdate",
        "description": "Create an AnimBP thread-safe helper function, read Speed in its graph, compile clean, read back the function",
        "chain": [
            {"op": "add_function",
             "args": {"action": "add_function", "asset_path": "/Game/Benchmarks/ABP_TestAnim",
                      "function_name": "BenchWfAnimThreadSafeUpdate"}},
            {"op": "set_function_thread_safe",
             "args": {"action": "set_function_thread_safe", "asset_path": "/Game/Benchmarks/ABP_TestAnim",
                      "function_name": "BenchWfAnimThreadSafeUpdate", "thread_safe": True}},
            {"op": "add_node",
             "args": {"action": "add_node", "asset_path": "/Game/Benchmarks/ABP_TestAnim",
                      "graph_name": "BenchWfAnimThreadSafeUpdate", "node_type": "VariableGet",
                      "variable_name": "Speed"}},
            {"op": "compile_blueprint",
             "args": {"action": "compile_blueprint", "asset_path": "/Game/Benchmarks/ABP_TestAnim"}},
        ],
        "verify": {"read_action": "get_functions", "contains": ["BenchWfAnimThreadSafeUpdate"]},
    },
    {
        "name": "gas_activate_override",
        "blueprint_type": "GameplayAbility", "domain": "ability", "edit_domain": "function",
        "asset_path": "/Game/Benchmarks/GA_TestAbility", "graph_name": "K2_ActivateAbility",
        "description": "Override GameplayAbility activation, add a PrintString node to the override graph, compile clean, read back the override",
        "chain": [
            {"op": "override_parent_function",
             "args": {"action": "override_parent_function", "asset_path": "/Game/Benchmarks/GA_TestAbility",
                      "parent_function_name": "K2_ActivateAbility"}},
            {"op": "add_node",
             "args": {"action": "add_node", "asset_path": "/Game/Benchmarks/GA_TestAbility",
                      "graph_name": "K2_ActivateAbility", "node_type": "CallFunction",
                      "function_name": "PrintString"}},
            {"op": "compile_blueprint",
             "args": {"action": "compile_blueprint", "asset_path": "/Game/Benchmarks/GA_TestAbility"}},
        ],
        "verify": {"read_action": "get_functions", "contains": ["K2_ActivateAbility"]},
    },
    {
        "name": "actorcomponent_toggle_contract",
        "blueprint_type": "ActorComponent", "domain": "component", "edit_domain": "function",
        "asset_path": "/Game/Benchmarks/BC_TestComponent", "graph_name": "BenchWfComponentToggle",
        "description": "Create an ActorComponent toggle function, add a bool input and VariableSet node, compile clean, read back the signature",
        "chain": [
            {"op": "add_function",
             "args": {"action": "add_function", "asset_path": "/Game/Benchmarks/BC_TestComponent",
                      "function_name": "BenchWfComponentToggle"}},
            {"op": "set_function_params",
             "args": {"action": "set_function_params", "asset_path": "/Game/Benchmarks/BC_TestComponent",
                      "function_name": "BenchWfComponentToggle",
                      "inputs": [{"name": "bNewActive", "type": "bool"}]}},
            {"op": "add_node",
             "args": {"action": "add_node", "asset_path": "/Game/Benchmarks/BC_TestComponent",
                      "graph_name": "BenchWfComponentToggle", "node_type": "VariableSet",
                      "variable_name": "bComponentActive"}},
            {"op": "compile_blueprint",
             "args": {"action": "compile_blueprint", "asset_path": "/Game/Benchmarks/BC_TestComponent"}},
        ],
        "verify": {"read_action": "get_function_signature",
                   "read_args": {"function_name": "BenchWfComponentToggle"},
                   "contains": ["BenchWfComponentToggle", "bNewActive"]},
    },
    {
        "name": "interface_signature_contract",
        "blueprint_type": "Interface", "domain": "interface", "edit_domain": "function",
        "asset_path": "/Game/Benchmarks/BPI_TestInterface", "graph_name": "BenchWfInterfaceCanUse",
        "description": "Create an Interface function with a bool output, compile clean, read back the signature",
        "chain": [
            {"op": "add_function",
             "args": {"action": "add_function", "asset_path": "/Game/Benchmarks/BPI_TestInterface",
                      "function_name": "BenchWfInterfaceCanUse"}},
            {"op": "set_function_params",
             "args": {"action": "set_function_params", "asset_path": "/Game/Benchmarks/BPI_TestInterface",
                      "function_name": "BenchWfInterfaceCanUse",
                      "outputs": [{"name": "bCanUse", "type": "bool"}]}},
            {"op": "compile_blueprint",
             "args": {"action": "compile_blueprint", "asset_path": "/Game/Benchmarks/BPI_TestInterface"}},
        ],
        "verify": {"read_action": "get_function_signature",
                   "read_args": {"function_name": "BenchWfInterfaceCanUse"},
                   "contains": ["BenchWfInterfaceCanUse", "bCanUse"]},
    },
    {
        "name": "actor_component_property_assembly",
        "blueprint_type": "Actor", "domain": "gameplay", "edit_domain": "component",
        "asset_path": "/Game/Benchmarks/BPB_TestActor", "graph_name": "EventGraph",
        "description": "Add a PointLightComponent to the Actor, set Intensity, compile clean, read back the component",
        "chain": [
            {"op": "add_component",
             "args": {"action": "add_component", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "component_class": "PointLightComponent", "component_name": "BenchWfLightComp"}},
            {"op": "set_component_property",
             "args": {"action": "set_component_property", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "component_name": "BenchWfLightComp", "property_name": "Intensity", "value": "4500.0"}},
            {"op": "compile_blueprint",
             "args": {"action": "compile_blueprint", "asset_path": "/Game/Benchmarks/BPB_TestActor"}},
        ],
        "verify": {"read_action": "get_components", "contains": ["BenchWfLightComp"]},
    },
    {
        "name": "duplicate_function_graph",
        "blueprint_type": "Actor", "domain": "gameplay", "edit_domain": "state",
        "asset_path": "/Game/Benchmarks/BPB_TestActor", "graph_name": "BenchWfGraphSource",
        "description": "Create a function graph, duplicate it, compile clean, read back the duplicated graph name",
        "chain": [
            {"op": "add_function",
             "args": {"action": "add_function", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "function_name": "BenchWfGraphSource"}},
            {"op": "duplicate_graph",
             "args": {"action": "duplicate_graph", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "graph_name": "BenchWfGraphSource", "new_name": "BenchWfGraphCopy"}},
            {"op": "compile_blueprint",
             "args": {"action": "compile_blueprint", "asset_path": "/Game/Benchmarks/BPB_TestActor"}},
        ],
        "verify": {"read_action": "get_functions", "contains": ["BenchWfGraphCopy"]},
    },
]


# Inputs designed to provoke structured isError responses. Scored inverted AND input-specific:
# pass = isError whose message references the offending input (error_tokens), fail = transport
# error (crash) OR a generic isError that does not mention the bad input. All action names +
# params are real catalog names/params (verified live), so this exercises actual error handling,
# not "unknown action" or "missing required param" rejection.
BLUEPRINT_ERROR_PATH_TASKS: List[Dict[str, Any]] = [
    {
        "action": "get_graph_data", "description": "get_graph_data on non-existent asset",
        "arguments": {"action": "get_graph_data",
                      "asset_path": "/Game/Benchmarks/NONEXISTENT_BenchBP_ZZZZZ"},
        "error_tokens": ["NONEXISTENT_BenchBP_ZZZZZ", "not found", "does not exist", "could not", "failed to load"],
    },
    {
        "action": "compile_blueprint", "description": "compile_blueprint on non-existent asset",
        "arguments": {"action": "compile_blueprint",
                      "asset_path": "/Game/Benchmarks/NONEXISTENT_BenchBP_ZZZZZ"},
        "error_tokens": ["NONEXISTENT_BenchBP_ZZZZZ", "not found", "does not exist", "could not", "failed to load"],
    },
    {
        "action": "get_variables", "description": "get_variables on non-existent asset",
        "arguments": {"action": "get_variables",
                      "asset_path": "/Game/Benchmarks/NONEXISTENT_BenchBP_ZZZZZ"},
        "error_tokens": ["NONEXISTENT_BenchBP_ZZZZZ", "not found", "does not exist", "could not", "failed to load"],
    },
    {
        "action": "list_graphs", "description": "list_graphs on non-existent asset",
        "arguments": {"action": "list_graphs",
                      "asset_path": "/Game/Benchmarks/NONEXISTENT_BenchBP_ZZZZZ"},
        "error_tokens": ["NONEXISTENT_BenchBP_ZZZZZ", "not found", "does not exist", "could not", "failed to load"],
    },
    {
        "action": "connect_pins", "description": "connect_pins with invalid node IDs",
        # Real param names are source_node / target_node (NOT *_node_id) — verified live — so
        # this exercises node-not-found handling, not unknown-param rejection.
        "arguments": {"action": "connect_pins",
                      "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "graph_name": "EventGraph",
                      "source_node": "INVALID_NODE_AAAA_ZZZZ",
                      "source_pin": "then",
                      "target_node": "INVALID_NODE_BBBB_ZZZZ",
                      "target_pin": "execute"},
        "error_tokens": ["INVALID_NODE", "node", "not found", "invalid", "could not", "no node"],
    },
    {
        "action": "remove_variable", "description": "remove non-existent variable",
        "arguments": {"action": "remove_variable",
                      "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "name": "NONEXISTENT_VAR_BENCH_ZZZZ"},
        "error_tokens": ["NONEXISTENT_VAR_BENCH_ZZZZ", "variable", "not found", "does not exist", "no such"],
    },
    {
        "action": "rename_function", "description": "rename non-existent function",
        "arguments": {"action": "rename_function",
                      "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "old_name": "NONEXISTENT_FUNC_BENCH_ZZZZ",
                      "new_name": "AnotherName"},
        "error_tokens": ["NONEXISTENT_FUNC_BENCH_ZZZZ", "function", "not found", "does not exist", "no such"],
    },
    {
        "action": "remove_component", "description": "remove non-existent component",
        "arguments": {"action": "remove_component",
                      "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "component_name": "NONEXISTENT_COMP_BENCH_ZZZZ"},
        "error_tokens": ["NONEXISTENT_COMP_BENCH_ZZZZ", "component", "not found", "does not exist", "no such"],
    },
    {
        "action": "set_component_property", "description": "set property on non-existent component",
        "arguments": {"action": "set_component_property",
                      "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "component_name": "NONEXISTENT_PROP_COMP_ZZZZ",
                      "property_name": "RelativeLocation", "value": "(X=1,Y=2,Z=3)"},
        "error_tokens": ["NONEXISTENT_PROP_COMP_ZZZZ", "component", "not found", "does not exist", "no such"],
    },
    {
        "action": "add_component", "description": "add_component with invalid component class",
        "arguments": {"action": "add_component",
                      "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "component_class": "NONEXISTENT_ComponentClass_ZZZZ",
                      "component_name": "BadComponentClass"},
        "error_tokens": ["NONEXISTENT_ComponentClass_ZZZZ", "component class", "class not found", "not found"],
    },
    {
        "action": "set_variable_defaults", "description": "set default on non-existent Widget variable",
        "arguments": {"action": "set_variable_defaults",
                      "asset_path": "/Game/Benchmarks/WBP_TestWidget",
                      "name": "NONEXISTENT_WIDGET_VAR_ZZZZ", "default_value": "true"},
        "error_tokens": ["NONEXISTENT_WIDGET_VAR_ZZZZ", "variable", "not found", "does not exist", "no such"],
    },
    {
        "action": "set_function_params", "description": "set params on non-existent Interface function",
        "arguments": {"action": "set_function_params",
                      "asset_path": "/Game/Benchmarks/BPI_TestInterface",
                      "function_name": "NONEXISTENT_IFACE_FUNC_ZZZZ",
                      "outputs": [{"name": "Value", "type": "float"}]},
        "error_tokens": ["NONEXISTENT_IFACE_FUNC_ZZZZ", "function", "not found", "does not exist", "no such"],
    },
    {
        "action": "get_function_signature", "description": "get signature for non-existent Interface function",
        "arguments": {"action": "get_function_signature",
                      "asset_path": "/Game/Benchmarks/BPI_TestInterface",
                      "function_name": "NONEXISTENT_SIGNATURE_FUNC_ZZZZ"},
        "error_tokens": ["NONEXISTENT_SIGNATURE_FUNC_ZZZZ", "function", "not found", "does not exist", "no such"],
    },
    {
        "action": "set_cdo_property", "description": "set non-existent CDO property",
        "arguments": {"action": "set_cdo_property",
                      "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "property_name": "NONEXISTENT_CDO_PROP_ZZZZ", "value": "1"},
        "error_tokens": ["NONEXISTENT_CDO_PROP_ZZZZ", "property", "not found", "does not exist", "unknown"],
    },
    {
        "action": "duplicate_graph", "description": "duplicate non-existent function graph",
        "arguments": {"action": "duplicate_graph",
                      "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "graph_name": "NONEXISTENT_GRAPH_ZZZZ", "new_name": "BadGraphCopy"},
        "error_tokens": ["NONEXISTENT_GRAPH_ZZZZ", "graph", "not found", "does not exist", "no such"],
    },
    {
        "action": "add_node", "description": "add_node with invalid function name",
        "arguments": {"action": "add_node",
                      "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "graph_name": "EventGraph", "node_type": "CallFunction",
                      "function_name": "NONEXISTENT_FUNCTION_NODE_ZZZZ"},
        "error_tokens": ["NONEXISTENT_FUNCTION_NODE_ZZZZ", "function", "not found", "could not", "invalid"],
    },
    # --- v5.2: real production PRECONDITION/VALUE failure modes (valid action, bad
    # precondition or value token). These are the failures real workers hit most — the benchmark
    # scored 1.000 on clean delete-first fixtures while these fail 47-91% in production
    # (create_blueprint 87%, add_variable bad type). A valid action that violates a precondition
    # must return a structured, input-specific isError, not a crash or silent success. All four
    # verified live (2026-06-18) to echo the offending identifier.
    {
        "action": "create_blueprint", "description": "create_blueprint on an already-existing asset path (precondition: already exists)",
        "arguments": {"action": "create_blueprint", "save_path": "/Game/Benchmarks/BPB_TestActor",
                      "parent_class": "Actor", "blueprint_type": "Normal"},
        # Offending input is the existing path; the precondition phrase + recovery hint
        # (duplicate_blueprint) distinguish a real guard from a generic error.
        "error_tokens": ["already exists", "BPB_TestActor", "duplicate_blueprint", "delete it first"],
        "specific_tokens": ["already exists", "duplicate_blueprint"],
    },
    {
        "action": "add_variable", "description": "add_variable with an unknown/invalid type token",
        "arguments": {"action": "add_variable", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "name": "BadTypeVar_ZZZ", "type": "NONEXISTENT_TYPE_ZZZ"},
        "error_tokens": ["NONEXISTENT_TYPE_ZZZ", "Unknown variable type", "type", "invalid", "valid types"],
    },
    {
        "action": "reparent_blueprint", "description": "reparent_blueprint to a non-existent parent class",
        "arguments": {"action": "reparent_blueprint", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "new_parent_class": "NONEXISTENT_PARENT_ZZZ"},
        "error_tokens": ["NONEXISTENT_PARENT_ZZZ", "Parent class not found", "parent", "not found"],
    },
    {
        "action": "add_node", "description": "add_node with an unknown node_type token",
        "arguments": {"action": "add_node", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "graph_name": "EventGraph", "node_type": "NONEXISTENT_NODETYPE_ZZZ"},
        "error_tokens": ["NONEXISTENT_NODETYPE_ZZZ", "Unknown node_type", "node_type", "supported types"],
    },
]

# Duplicate-name rejection: every blueprint action that creates a uniquely-named entity
# must refuse a same-name second creation with a structured isError instead of silently
# creating a suffixed copy (e.g. BenchMeshComp -> BenchMeshComp1). Scored by calling the
# action TWICE (see _score_duplicate_reject): the SECOND identical call must return isError.
# This dimension exists because edit_execute passes on any non-error envelope and therefore
# cannot distinguish a real duplicate-guard from a silent suffix/no-op. `setup_arguments`
# (optional) runs once first to create a host entity (e.g. the function a local variable lives in).
# All target the Actor fixture; names are fixed so repeated runs stay bounded and idempotent.
BLUEPRINT_DUPLICATE_REJECT_TASKS: List[Dict[str, Any]] = [
    {
        "action": "add_variable", "edit_domain": "variable",
        "description": "add_variable must reject a duplicate member variable name",
        "arguments": {"action": "add_variable", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "name": "DupRejectVar", "type": "int"},
    },
    {
        "action": "add_function", "edit_domain": "function",
        "description": "add_function must reject a duplicate function name",
        "arguments": {"action": "add_function", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "function_name": "DupRejectFunc"},
    },
    {
        "action": "add_component", "edit_domain": "component",
        "description": "add_component must reject a duplicate component name (regression: silent suffix bug)",
        "arguments": {"action": "add_component", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "component_class": "SceneComponent", "component_name": "DupRejectComp"},
    },
    {
        "action": "add_event_node", "edit_domain": "function",
        "description": "add_event_node must reject a duplicate custom event name",
        "arguments": {"action": "add_event_node", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "event_name": "DupRejectEvent"},
    },
    {
        "action": "add_event_dispatcher", "edit_domain": "event",
        "description": "add_event_dispatcher must reject a duplicate dispatcher name",
        "arguments": {"action": "add_event_dispatcher", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "name": "DupRejectDisp"},
    },
    {
        "action": "add_local_variable", "edit_domain": "function",
        "description": "add_local_variable must reject a duplicate local variable name (regression: silent duplicate bug)",
        "setup_arguments": {"action": "add_function", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                            "function_name": "DupRejectLocalFunc"},
        "arguments": {"action": "add_local_variable", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "function_name": "DupRejectLocalFunc", "name": "DupRejectLocal", "type": "int"},
    },
    {
        "action": "add_variable", "edit_domain": "variable", "blueprint_type": "Widget", "domain": "ui",
        "description": "Widget add_variable must reject duplicate display-state variable names",
        "arguments": {"action": "add_variable", "asset_path": "/Game/Benchmarks/WBP_TestWidget",
                      "name": "DupRejectWidgetVar", "type": "bool"},
    },
    {
        "action": "add_function", "edit_domain": "function", "blueprint_type": "AnimInstance", "domain": "animation",
        "description": "AnimInstance add_function must reject duplicate update helper names",
        "arguments": {"action": "add_function", "asset_path": "/Game/Benchmarks/ABP_TestAnim",
                      "function_name": "DupRejectAnimFunc"},
    },
    {
        "action": "add_event_dispatcher", "edit_domain": "event", "blueprint_type": "GameplayAbility", "domain": "ability",
        "description": "GameplayAbility add_event_dispatcher must reject duplicate commit signals",
        "arguments": {"action": "add_event_dispatcher", "asset_path": "/Game/Benchmarks/GA_TestAbility",
                      "name": "DupRejectAbilitySignal"},
    },
    {
        "action": "add_component", "edit_domain": "component", "blueprint_type": "Character", "domain": "gameplay",
        "description": "Character add_component must reject duplicate helper component names",
        "arguments": {"action": "add_component", "asset_path": "/Game/Benchmarks/BPB_TestCharacter",
                      "component_class": "SceneComponent", "component_name": "DupRejectCharScene"},
    },
    {
        "action": "add_function", "edit_domain": "function", "blueprint_type": "Interface", "domain": "interface",
        "description": "Interface add_function must reject duplicate contract function names",
        "arguments": {"action": "add_function", "asset_path": "/Game/Benchmarks/BPI_TestInterface",
                      "function_name": "DupRejectInterfaceFunc"},
    },
]

# Parent class names for create_blueprint fixture setup.
FIXTURE_CREATE_PARAMS: Dict[str, Dict[str, str]] = {
    "Actor":           {"parent_class": "Actor",           "blueprint_type": "Normal"},
    "Character":       {"parent_class": "Character",       "blueprint_type": "Normal"},
    "Widget":          {"parent_class": "UserWidget",      "blueprint_type": "Normal"},
    "AnimInstance":    {"parent_class": "AnimInstance",    "blueprint_type": "Normal"},
    "GameplayAbility": {"parent_class": "GameplayAbility", "blueprint_type": "Normal"},
    "ActorComponent":  {"parent_class": "ActorComponent",  "blueprint_type": "Normal"},
    "Interface":       {"parent_class": "Interface",       "blueprint_type": "Interface"},
}

# Explicit per-variable pin types for fixture setup — single source of truth that matches
# test_blueprints.md (ActorTag=FName, DisplayText=FText, CharacterName=FString, ComponentID=int32).
# Replaces the old name-based heuristic that silently created ActorTag as string and DisplayText
# as float. Tokens verified accepted by the live add_variable handler
# (valid: bool, byte, int, int64, float, double, string, text, name, Vector, Rotator, ...).
FIXTURE_VAR_TYPES: Dict[str, Dict[str, str]] = {
    "Actor":          {"Health": "float", "MaxHealth": "float", "ActorTag": "name"},
    "Character":      {"MoveSpeed": "float", "bIsSprinting": "bool", "CharacterName": "string"},
    "Widget":         {"DisplayText": "text", "bIsVisible": "bool"},
    "AnimInstance":   {"Speed": "float", "bIsInAir": "bool"},
    "GameplayAbility": {"AbilityCooldown": "float", "AbilityCost": "float"},
    "ActorComponent": {"ComponentID": "int", "bComponentActive": "bool"},
    "Interface":      {},
}

# Function stubs created by setup_fixtures and asserted by the expanded read/preflight tasks.
# These match test_blueprints.md and make read-back checks independent of task execution order.
FIXTURE_FUNCTIONS_BY_TYPE: Dict[str, List[str]] = {
    "Actor": ["TakeDamage_Bench", "Heal_Bench"],
    "Character": ["StartSprint_Bench", "StopSprint_Bench"],
    "Widget": ["UpdateDisplay_Bench"],
    "AnimInstance": ["UpdateLocomotion_Bench"],
    "GameplayAbility": ["OnAbilityActivated_Bench"],
    "ActorComponent": ["Initialize_Bench", "Deactivate_Bench"],
    "Interface": ["GetDisplayName_Bench", "OnInteract_Bench"],
}

# Interface function stubs are referenced by variable_read compatibility checks.
INTERFACE_FIXTURE_FUNCTIONS: List[str] = FIXTURE_FUNCTIONS_BY_TYPE["Interface"]

# 10 type-specific edit_execute tasks per Blueprint type (70 total).
# Blueprint action names verified against live namespace catalog v0.20.3; cross-domain
# asset_authoring names are limited to actions registered in the current project catalog.
# Tasks assume fixtures created by `setup_fixtures` command exist at /Game/Benchmarks/.
# Tasks at indexes 0–N are NOT guaranteed idempotent; run `setup_fixtures` before each benchmark.
BLUEPRINT_EDIT_EXECUTE_TASKS_BY_TYPE: Dict[str, List[Dict[str, Any]]] = {
    "Actor": [
        {"action": "add_node", "edit_domain": "node",
         "arguments": {"action": "add_node", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                       "graph_name": "EventGraph", "node_type": "CallFunction",
                       "function_name": "PrintString"},
         "description": "Add PrintString CallFunction node to Actor EventGraph"},
        {"action": "add_function", "edit_domain": "function",
         "arguments": {"action": "add_function", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                       "function_name": "BenchActorInit"},
         "description": "Add BenchActorInit function graph to Actor"},
        {"action": "add_event_node", "edit_domain": "function",
         "arguments": {"action": "add_event_node", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                       "event_name": "BenchActorOnReady"},
         "description": "Add BenchActorOnReady custom event node to Actor EventGraph"},
        {"action": "add_variable", "edit_domain": "variable",
         "arguments": {"action": "add_variable", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                       "name": "BenchActorLevel", "type": "int"},
         "description": "Add int BenchActorLevel variable to Actor"},
        {"action": "set_variable_defaults", "edit_domain": "variable",
         "arguments": {"action": "set_variable_defaults", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                       "name": "MaxHealth", "default_value": "100.0"},
         "description": "Set MaxHealth fixture variable default to 100.0 (idempotent)"},
        {"action": "add_component", "edit_domain": "component",
         "arguments": {"action": "add_component", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                       "component_class": "StaticMeshComponent", "component_name": "BenchMeshComp"},
         "description": "Add StaticMeshComponent BenchMeshComp to Actor"},
        {"action": "add_event_dispatcher", "edit_domain": "event",
         "arguments": {"action": "add_event_dispatcher", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                       "name": "BenchActorOnDied"},
         "description": "Add BenchActorOnDied event dispatcher to Actor"},
        {"action": "add_replicated_variable", "edit_domain": "variable",
         "arguments": {"action": "add_replicated_variable", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                       "variable_name": "BenchActorNetFlag", "type": "bool"},
         "description": "Add replicated bool BenchActorNetFlag to Actor"},
        {"action": "auto_layout", "edit_domain": "state",
         "arguments": {"action": "auto_layout", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                       "graph_name": "EventGraph"},
         "description": "Auto-layout Actor EventGraph (idempotent)"},
        {"action": "compile_blueprint", "edit_domain": "compilation",
         "arguments": {"action": "compile_blueprint", "asset_path": "/Game/Benchmarks/BPB_TestActor"},
         "description": "Compile Actor blueprint"},
    ],
    "Character": [
        {"action": "add_function", "edit_domain": "function",
         "arguments": {"action": "add_function", "asset_path": "/Game/Benchmarks/BPB_TestCharacter",
                       "function_name": "BenchCharJump"},
         "description": "Add BenchCharJump function graph to Character"},
        {"action": "add_event_node", "edit_domain": "function",
         "arguments": {"action": "add_event_node", "asset_path": "/Game/Benchmarks/BPB_TestCharacter",
                       "event_name": "BenchCharOnLanded"},
         "description": "Add BenchCharOnLanded custom event to Character EventGraph"},
        {"action": "add_node", "edit_domain": "node",
         "arguments": {"action": "add_node", "asset_path": "/Game/Benchmarks/BPB_TestCharacter",
                       "graph_name": "EventGraph", "node_type": "CallFunction",
                       "function_name": "GetVelocity"},
         "description": "Add GetVelocity CallFunction node to Character EventGraph"},
        {"action": "add_variable", "edit_domain": "variable",
         "arguments": {"action": "add_variable", "asset_path": "/Game/Benchmarks/BPB_TestCharacter",
                       "name": "BenchCharStamina", "type": "float"},
         "description": "Add float BenchCharStamina variable to Character"},
        {"action": "set_variable_defaults", "edit_domain": "variable",
         "arguments": {"action": "set_variable_defaults", "asset_path": "/Game/Benchmarks/BPB_TestCharacter",
                       "name": "MoveSpeed", "default_value": "600.0"},
         "description": "Set MoveSpeed fixture variable default to 600.0 (idempotent)"},
        {"action": "add_component", "edit_domain": "component",
         "arguments": {"action": "add_component", "asset_path": "/Game/Benchmarks/BPB_TestCharacter",
                       "component_class": "SphereComponent", "component_name": "BenchOverlapComp"},
         "description": "Add SphereComponent BenchOverlapComp to Character"},
        {"action": "add_event_dispatcher", "edit_domain": "event",
         "arguments": {"action": "add_event_dispatcher", "asset_path": "/Game/Benchmarks/BPB_TestCharacter",
                       "name": "BenchCharOnJumped"},
         "description": "Add BenchCharOnJumped event dispatcher to Character"},
        {"action": "add_replicated_variable", "edit_domain": "variable",
         "arguments": {"action": "add_replicated_variable", "asset_path": "/Game/Benchmarks/BPB_TestCharacter",
                       "variable_name": "BenchCharNetHealth", "type": "float"},
         "description": "Add replicated float BenchCharNetHealth to Character"},
        {"action": "auto_layout", "edit_domain": "state",
         "arguments": {"action": "auto_layout", "asset_path": "/Game/Benchmarks/BPB_TestCharacter",
                       "graph_name": "EventGraph"},
         "description": "Auto-layout Character EventGraph (idempotent)"},
        {"action": "compile_blueprint", "edit_domain": "compilation",
         "arguments": {"action": "compile_blueprint", "asset_path": "/Game/Benchmarks/BPB_TestCharacter"},
         "description": "Compile Character blueprint"},
    ],
    "Widget": [
        {"action": "add_function", "edit_domain": "function",
         "arguments": {"action": "add_function", "asset_path": "/Game/Benchmarks/WBP_TestWidget",
                       "function_name": "BenchWidgetRefresh"},
         "description": "Add BenchWidgetRefresh function graph to Widget"},
        {"action": "add_event_node", "edit_domain": "function",
         "arguments": {"action": "add_event_node", "asset_path": "/Game/Benchmarks/WBP_TestWidget",
                       "event_name": "BenchWidgetOnShown"},
         "description": "Add BenchWidgetOnShown custom event to Widget EventGraph"},
        {"action": "add_node", "edit_domain": "node",
         "arguments": {"action": "add_node", "asset_path": "/Game/Benchmarks/WBP_TestWidget",
                       "graph_name": "EventGraph", "node_type": "CallFunction",
                       "function_name": "SetVisibility"},
         "description": "Add SetVisibility CallFunction node to Widget EventGraph"},
        {"action": "add_variable", "edit_domain": "variable",
         "arguments": {"action": "add_variable", "asset_path": "/Game/Benchmarks/WBP_TestWidget",
                       "name": "BenchWidgetTitle", "type": "string"},
         "description": "Add string BenchWidgetTitle variable to Widget"},
        {"action": "set_variable_defaults", "edit_domain": "variable",
         "arguments": {"action": "set_variable_defaults", "asset_path": "/Game/Benchmarks/WBP_TestWidget",
                       "name": "bIsVisible", "default_value": "false"},
         "description": "Set bIsVisible fixture variable default to false (idempotent)"},
        {"action": "add_event_dispatcher", "edit_domain": "event",
         "arguments": {"action": "add_event_dispatcher", "asset_path": "/Game/Benchmarks/WBP_TestWidget",
                       "name": "BenchWidgetOnClosed"},
         "description": "Add BenchWidgetOnClosed event dispatcher to Widget"},
        {"action": "set_cdo_properties", "edit_domain": "class",
         "arguments": {"action": "set_cdo_properties", "asset_path": "/Game/Benchmarks/WBP_TestWidget",
                       "properties": {"bIsVariable": True}},
         "description": "Set CDO bIsVariable=true on Widget (idempotent)"},
        {"action": "validate_blueprint", "edit_domain": "compilation",
         "arguments": {"action": "validate_blueprint", "asset_path": "/Game/Benchmarks/WBP_TestWidget"},
         "description": "Validate Widget blueprint (idempotent)"},
        {"action": "auto_layout", "edit_domain": "state",
         "arguments": {"action": "auto_layout", "asset_path": "/Game/Benchmarks/WBP_TestWidget",
                       "graph_name": "EventGraph"},
         "description": "Auto-layout Widget EventGraph (idempotent)"},
        {"action": "compile_blueprint", "edit_domain": "compilation",
         "arguments": {"action": "compile_blueprint", "asset_path": "/Game/Benchmarks/WBP_TestWidget"},
         "description": "Compile Widget blueprint"},
    ],
    "AnimInstance": [
        {"action": "add_function", "edit_domain": "function",
         "arguments": {"action": "add_function", "asset_path": "/Game/Benchmarks/ABP_TestAnim",
                       "function_name": "BenchAnimUpdate"},
         "description": "Add BenchAnimUpdate function graph to AnimInstance"},
        {"action": "set_function_thread_safe", "edit_domain": "function",
         "arguments": {"action": "set_function_thread_safe", "asset_path": "/Game/Benchmarks/ABP_TestAnim",
                       "function_name": "BenchAnimUpdate", "thread_safe": True},
         "description": "Mark BenchAnimUpdate as thread-safe (idempotent after creation)"},
        {"action": "add_variable", "edit_domain": "variable",
         "arguments": {"action": "add_variable", "asset_path": "/Game/Benchmarks/ABP_TestAnim",
                       "name": "BenchBlendWeight", "type": "float"},
         "description": "Add float BenchBlendWeight variable to AnimInstance"},
        {"action": "set_variable_defaults", "edit_domain": "variable",
         "arguments": {"action": "set_variable_defaults", "asset_path": "/Game/Benchmarks/ABP_TestAnim",
                       "name": "Speed", "default_value": "0.0"},
         "description": "Set Speed fixture variable default to 0.0 (idempotent)"},
        {"action": "add_event_node", "edit_domain": "function",
         "arguments": {"action": "add_event_node", "asset_path": "/Game/Benchmarks/ABP_TestAnim",
                       "event_name": "BenchAnimOnEvent"},
         "description": "Add BenchAnimOnEvent custom event to AnimInstance EventGraph"},
        {"action": "add_node", "edit_domain": "node",
         "arguments": {"action": "add_node", "asset_path": "/Game/Benchmarks/ABP_TestAnim",
                       "graph_name": "EventGraph", "node_type": "VariableGet", "variable_name": "Speed"},
         "description": "Add Speed VariableGet node to AnimInstance EventGraph"},
        {"action": "add_event_dispatcher", "edit_domain": "event",
         "arguments": {"action": "add_event_dispatcher", "asset_path": "/Game/Benchmarks/ABP_TestAnim",
                       "name": "BenchAnimDispatcher"},
         "description": "Add BenchAnimDispatcher event dispatcher to AnimInstance"},
        {"action": "validate_blueprint", "edit_domain": "compilation",
         "arguments": {"action": "validate_blueprint", "asset_path": "/Game/Benchmarks/ABP_TestAnim"},
         "description": "Validate AnimInstance blueprint (idempotent)"},
        {"action": "auto_layout", "edit_domain": "state",
         "arguments": {"action": "auto_layout", "asset_path": "/Game/Benchmarks/ABP_TestAnim",
                       "graph_name": "EventGraph"},
         "description": "Auto-layout AnimInstance EventGraph (idempotent)"},
        {"action": "compile_blueprint", "edit_domain": "compilation",
         "arguments": {"action": "compile_blueprint", "asset_path": "/Game/Benchmarks/ABP_TestAnim"},
         "description": "Compile AnimInstance blueprint"},
    ],
    "GameplayAbility": [
        {"action": "override_parent_function", "edit_domain": "function",
         "arguments": {"action": "override_parent_function", "asset_path": "/Game/Benchmarks/GA_TestAbility",
                       "parent_function_name": "K2_ActivateAbility"},
         "description": "Override K2_ActivateAbility (ActivateAbility BlueprintNativeEvent) on GameplayAbility"},
        {"action": "add_function", "edit_domain": "function",
         "arguments": {"action": "add_function", "asset_path": "/Game/Benchmarks/GA_TestAbility",
                       "function_name": "BenchAbilityCheck"},
         "description": "Add BenchAbilityCheck function graph to GameplayAbility"},
        {"action": "add_event_node", "edit_domain": "function",
         "arguments": {"action": "add_event_node", "asset_path": "/Game/Benchmarks/GA_TestAbility",
                       "event_name": "BenchAbilityOnActivated"},
         "description": "Add BenchAbilityOnActivated custom event to GameplayAbility EventGraph"},
        {"action": "add_variable", "edit_domain": "variable",
         "arguments": {"action": "add_variable", "asset_path": "/Game/Benchmarks/GA_TestAbility",
                       "name": "BenchAbilityLevel", "type": "int"},
         "description": "Add int BenchAbilityLevel variable to GameplayAbility"},
        {"action": "set_variable_defaults", "edit_domain": "variable",
         "arguments": {"action": "set_variable_defaults", "asset_path": "/Game/Benchmarks/GA_TestAbility",
                       "name": "AbilityCooldown", "default_value": "3.0"},
         "description": "Set AbilityCooldown fixture variable default to 3.0 (idempotent)"},
        {"action": "set_variable_defaults", "edit_domain": "variable",
         "arguments": {"action": "set_variable_defaults", "asset_path": "/Game/Benchmarks/GA_TestAbility",
                       "name": "AbilityCost", "default_value": "20.0"},
         "description": "Set AbilityCost fixture variable default to 20.0 (idempotent)"},
        {"action": "add_event_dispatcher", "edit_domain": "event",
         "arguments": {"action": "add_event_dispatcher", "asset_path": "/Game/Benchmarks/GA_TestAbility",
                       "name": "BenchAbilityOnEnd"},
         "description": "Add BenchAbilityOnEnd event dispatcher to GameplayAbility"},
        {"action": "validate_blueprint", "edit_domain": "compilation",
         "arguments": {"action": "validate_blueprint", "asset_path": "/Game/Benchmarks/GA_TestAbility"},
         "description": "Validate GameplayAbility blueprint (idempotent)"},
        {"action": "auto_layout", "edit_domain": "state",
         "arguments": {"action": "auto_layout", "asset_path": "/Game/Benchmarks/GA_TestAbility",
                       "graph_name": "EventGraph"},
         "description": "Auto-layout GameplayAbility EventGraph (idempotent)"},
        {"action": "compile_blueprint", "edit_domain": "compilation",
         "arguments": {"action": "compile_blueprint", "asset_path": "/Game/Benchmarks/GA_TestAbility"},
         "description": "Compile GameplayAbility blueprint"},
    ],
    "ActorComponent": [
        {"action": "add_function", "edit_domain": "function",
         "arguments": {"action": "add_function", "asset_path": "/Game/Benchmarks/BC_TestComponent",
                       "function_name": "BenchCompActivate"},
         "description": "Add BenchCompActivate function graph to ActorComponent"},
        {"action": "add_event_node", "edit_domain": "function",
         "arguments": {"action": "add_event_node", "asset_path": "/Game/Benchmarks/BC_TestComponent",
                       "event_name": "BenchCompOnActivated"},
         "description": "Add BenchCompOnActivated custom event to ActorComponent EventGraph"},
        {"action": "add_node", "edit_domain": "node",
         "arguments": {"action": "add_node", "asset_path": "/Game/Benchmarks/BC_TestComponent",
                       "graph_name": "EventGraph", "node_type": "CallFunction",
                       "function_name": "PrintString"},
         "description": "Add PrintString CallFunction node to ActorComponent EventGraph"},
        {"action": "add_variable", "edit_domain": "variable",
         "arguments": {"action": "add_variable", "asset_path": "/Game/Benchmarks/BC_TestComponent",
                       "name": "BenchCompLevel", "type": "int"},
         "description": "Add int BenchCompLevel variable to ActorComponent"},
        {"action": "set_variable_defaults", "edit_domain": "variable",
         "arguments": {"action": "set_variable_defaults", "asset_path": "/Game/Benchmarks/BC_TestComponent",
                       "name": "BenchCompLevel", "default_value": "0"},
         "description": "Set BenchCompLevel default to 0 (just created)"},
        {"action": "add_node", "edit_domain": "node",
         "arguments": {"action": "add_node", "asset_path": "/Game/Benchmarks/BC_TestComponent",
                       "graph_name": "EventGraph", "node_type": "VariableGet", "variable_name": "ComponentID"},
         "description": "Add ComponentID VariableGet node to ActorComponent EventGraph"},
        {"action": "add_event_dispatcher", "edit_domain": "event",
         "arguments": {"action": "add_event_dispatcher", "asset_path": "/Game/Benchmarks/BC_TestComponent",
                       "name": "BenchCompOnChanged"},
         "description": "Add BenchCompOnChanged event dispatcher to ActorComponent"},
        {"action": "validate_blueprint", "edit_domain": "compilation",
         "arguments": {"action": "validate_blueprint", "asset_path": "/Game/Benchmarks/BC_TestComponent"},
         "description": "Validate ActorComponent blueprint (idempotent)"},
        {"action": "auto_layout", "edit_domain": "state",
         "arguments": {"action": "auto_layout", "asset_path": "/Game/Benchmarks/BC_TestComponent",
                       "graph_name": "EventGraph"},
         "description": "Auto-layout ActorComponent EventGraph (idempotent)"},
        {"action": "compile_blueprint", "edit_domain": "compilation",
         "arguments": {"action": "compile_blueprint", "asset_path": "/Game/Benchmarks/BC_TestComponent"},
         "description": "Compile ActorComponent blueprint"},
    ],
    "Interface": [
        {"action": "add_function", "edit_domain": "function",
         "arguments": {"action": "add_function", "asset_path": "/Game/Benchmarks/BPI_TestInterface",
                       "function_name": "BenchIFaceGetHealth"},
         "description": "Add BenchIFaceGetHealth function stub to Interface"},
        {"action": "set_function_params", "edit_domain": "function",
         "arguments": {"action": "set_function_params", "asset_path": "/Game/Benchmarks/BPI_TestInterface",
                       "function_name": "BenchIFaceGetHealth",
                       "outputs": [{"name": "HealthValue", "type": "float"}]},
         "description": "Set typed float output on BenchIFaceGetHealth stub"},
        {"action": "add_function", "edit_domain": "function",
         "arguments": {"action": "add_function", "asset_path": "/Game/Benchmarks/BPI_TestInterface",
                       "function_name": "BenchIFaceGetSpeed"},
         "description": "Add BenchIFaceGetSpeed function stub to Interface"},
        {"action": "add_function", "edit_domain": "function",
         "arguments": {"action": "add_function", "asset_path": "/Game/Benchmarks/BPI_TestInterface",
                       "function_name": "BenchIFaceOnDamaged"},
         "description": "Add BenchIFaceOnDamaged function stub to Interface"},
        {"action": "set_function_params", "edit_domain": "function",
         "arguments": {"action": "set_function_params", "asset_path": "/Game/Benchmarks/BPI_TestInterface",
                       "function_name": "BenchIFaceOnDamaged",
                       "inputs": [{"name": "Amount", "type": "float"}]},
         "description": "Set typed float input on BenchIFaceOnDamaged stub"},
        {"action": "set_function_params", "edit_domain": "function",
         "arguments": {"action": "set_function_params", "asset_path": "/Game/Benchmarks/BPI_TestInterface",
                       "function_name": "BenchIFaceGetSpeed",
                       "outputs": [{"name": "SpeedValue", "type": "float"}]},
         "description": "Set typed float output on BenchIFaceGetSpeed stub"},
        {"action": "add_function", "edit_domain": "function",
         "arguments": {"action": "add_function", "asset_path": "/Game/Benchmarks/BPI_TestInterface",
                       "function_name": "BenchIFaceIsAlive"},
         "description": "Add BenchIFaceIsAlive function stub to Interface"},
        {"action": "rename_function", "edit_domain": "function",
         "arguments": {"action": "rename_function", "asset_path": "/Game/Benchmarks/BPI_TestInterface",
                       "old_name": "BenchIFaceIsAlive", "new_name": "BenchIFaceCheckAlive"},
         "description": "Rename BenchIFaceIsAlive to BenchIFaceCheckAlive"},
        {"action": "compile_blueprint", "edit_domain": "compilation",
         "arguments": {"action": "compile_blueprint", "asset_path": "/Game/Benchmarks/BPI_TestInterface"},
         "description": "Compile Interface blueprint"},
        {"action": "save_asset", "edit_domain": "compilation",
         "arguments": {"action": "save_asset", "asset_path": "/Game/Benchmarks/BPI_TestInterface"},
         "description": "Save Interface blueprint to disk"},
    ],
}

BLUEPRINT_ADDITIONAL_EDIT_EXECUTE_TASKS: List[Dict[str, Any]] = [
    # UMG widget graph/property/style coverage.
    {"blueprint_type": "Widget", "domain": "ui", "action": "add_variable", "edit_domain": "variable",
     "arguments": {"action": "add_variable", "asset_path": "/Game/Benchmarks/WBP_TestWidget",
                   "name": "BenchWidgetSubtitle", "type": "text"},
     "description": "Add FText BenchWidgetSubtitle style variable to Widget"},
    {"blueprint_type": "Widget", "domain": "ui", "action": "set_variable_defaults", "edit_domain": "variable",
     "arguments": {"action": "set_variable_defaults", "asset_path": "/Game/Benchmarks/WBP_TestWidget",
                   "name": "bIsVisible", "default_value": "true"},
     "description": "Set bIsVisible fixture variable default to true for Widget read-back"},
    {"blueprint_type": "Widget", "domain": "ui", "action": "add_comment_node", "edit_domain": "node",
     "arguments": {"action": "add_comment_node", "asset_path": "/Game/Benchmarks/WBP_TestWidget",
                   "graph_name": "EventGraph", "text": "BenchWidgetStyleBlock",
                   "position": [900, 220], "width": 480, "height": 180},
     "description": "Add BenchWidgetStyleBlock comment box to Widget EventGraph"},
    {"blueprint_type": "Widget", "domain": "ui", "action": "add_node", "edit_domain": "node",
     "arguments": {"action": "add_node", "asset_path": "/Game/Benchmarks/WBP_TestWidget",
                   "graph_name": "EventGraph", "node_type": "FormatText", "format": "Widget {Label}"},
     "description": "Add FormatText node for Widget label formatting"},
    {"blueprint_type": "Widget", "domain": "ui", "action": "add_function", "edit_domain": "function",
     "arguments": {"action": "add_function", "asset_path": "/Game/Benchmarks/WBP_TestWidget",
                   "function_name": "BenchWidgetOpenSettings", "category": "Benchmark|UI"},
     "description": "Add BenchWidgetOpenSettings function graph to Widget"},

    # AnimBP variables and graphs.
    {"blueprint_type": "AnimInstance", "domain": "animation", "action": "add_variable", "edit_domain": "variable",
     "arguments": {"action": "add_variable", "asset_path": "/Game/Benchmarks/ABP_TestAnim",
                   "name": "BenchAnimAcceleration", "type": "float"},
     "description": "Add float BenchAnimAcceleration variable to AnimInstance"},
    {"blueprint_type": "AnimInstance", "domain": "animation", "action": "set_variable_defaults", "edit_domain": "variable",
     "arguments": {"action": "set_variable_defaults", "asset_path": "/Game/Benchmarks/ABP_TestAnim",
                   "name": "bIsInAir", "default_value": "false"},
     "description": "Set bIsInAir fixture variable default to false for AnimInstance"},
    {"blueprint_type": "AnimInstance", "domain": "animation", "action": "add_function", "edit_domain": "function",
     "arguments": {"action": "add_function", "asset_path": "/Game/Benchmarks/ABP_TestAnim",
                   "function_name": "BenchAnimEvaluateTransitions"},
     "description": "Add BenchAnimEvaluateTransitions function graph to AnimInstance"},
    {"blueprint_type": "AnimInstance", "domain": "animation", "action": "set_function_thread_safe", "edit_domain": "function",
     "arguments": {"action": "set_function_thread_safe", "asset_path": "/Game/Benchmarks/ABP_TestAnim",
                   "function_name": "BenchAnimEvaluateTransitions", "thread_safe": True},
     "description": "Mark BenchAnimEvaluateTransitions as thread-safe on AnimInstance"},
    {"blueprint_type": "AnimInstance", "domain": "animation", "action": "add_node", "edit_domain": "node",
     "arguments": {"action": "add_node", "asset_path": "/Game/Benchmarks/ABP_TestAnim",
                   "graph_name": "BenchAnimEvaluateTransitions", "node_type": "VariableGet",
                   "variable_name": "Speed"},
     "description": "Add Speed VariableGet node to BenchAnimEvaluateTransitions graph"},

    # GAS ability Blueprint coverage.
    {"blueprint_type": "GameplayAbility", "domain": "ability", "action": "add_variable", "edit_domain": "variable",
     "arguments": {"action": "add_variable", "asset_path": "/Game/Benchmarks/GA_TestAbility",
                   "name": "BenchAbilityInputTag", "type": "name"},
     "description": "Add name BenchAbilityInputTag variable to GameplayAbility"},
    {"blueprint_type": "GameplayAbility", "domain": "ability", "action": "set_variable_defaults", "edit_domain": "variable",
     "arguments": {"action": "set_variable_defaults", "asset_path": "/Game/Benchmarks/GA_TestAbility",
                   "name": "AbilityCost", "default_value": "15.0"},
     "description": "Set AbilityCost fixture variable default to 15.0 for GameplayAbility"},
    {"blueprint_type": "GameplayAbility", "domain": "ability", "action": "add_function", "edit_domain": "function",
     "arguments": {"action": "add_function", "asset_path": "/Game/Benchmarks/GA_TestAbility",
                   "function_name": "BenchAbilityCommitCheck"},
     "description": "Add BenchAbilityCommitCheck function graph to GameplayAbility"},
    {"blueprint_type": "GameplayAbility", "domain": "ability", "action": "add_event_dispatcher", "edit_domain": "event",
     "arguments": {"action": "add_event_dispatcher", "asset_path": "/Game/Benchmarks/GA_TestAbility",
                   "name": "BenchAbilityOnCommitted"},
     "description": "Add BenchAbilityOnCommitted event dispatcher to GameplayAbility"},
    {"blueprint_type": "GameplayAbility", "domain": "ability", "action": "add_node", "edit_domain": "node",
     "arguments": {"action": "add_node", "asset_path": "/Game/Benchmarks/GA_TestAbility",
                   "graph_name": "EventGraph", "node_type": "CallFunction", "function_name": "PrintString"},
     "description": "Add PrintString node to GameplayAbility EventGraph for activation logging"},

    # ActorComponent authoring coverage.
    {"blueprint_type": "ActorComponent", "domain": "component", "action": "add_variable", "edit_domain": "variable",
     "arguments": {"action": "add_variable", "asset_path": "/Game/Benchmarks/BC_TestComponent",
                   "name": "BenchCompDebugName", "type": "string"},
     "description": "Add string BenchCompDebugName variable to ActorComponent"},
    {"blueprint_type": "ActorComponent", "domain": "component", "action": "set_variable_defaults", "edit_domain": "variable",
     "arguments": {"action": "set_variable_defaults", "asset_path": "/Game/Benchmarks/BC_TestComponent",
                   "name": "bComponentActive", "default_value": "true"},
     "description": "Set bComponentActive fixture variable default to true for ActorComponent"},
    {"blueprint_type": "ActorComponent", "domain": "component", "action": "add_function", "edit_domain": "function",
     "arguments": {"action": "add_function", "asset_path": "/Game/Benchmarks/BC_TestComponent",
                   "function_name": "BenchCompRefreshState"},
     "description": "Add BenchCompRefreshState function graph to ActorComponent"},
    {"blueprint_type": "ActorComponent", "domain": "component", "action": "add_node", "edit_domain": "node",
     "arguments": {"action": "add_node", "asset_path": "/Game/Benchmarks/BC_TestComponent",
                   "graph_name": "BenchCompRefreshState", "node_type": "VariableGet",
                   "variable_name": "ComponentID"},
     "description": "Add ComponentID VariableGet node to BenchCompRefreshState graph"},
    {"blueprint_type": "ActorComponent", "domain": "component", "action": "add_event_dispatcher", "edit_domain": "event",
     "arguments": {"action": "add_event_dispatcher", "asset_path": "/Game/Benchmarks/BC_TestComponent",
                   "name": "BenchCompOnRefresh"},
     "description": "Add BenchCompOnRefresh event dispatcher to ActorComponent"},

    # Interface function signature coverage.
    {"blueprint_type": "Interface", "domain": "interface", "action": "add_function", "edit_domain": "function",
     "arguments": {"action": "add_function", "asset_path": "/Game/Benchmarks/BPI_TestInterface",
                   "function_name": "BenchIFaceGetScore"},
     "description": "Add BenchIFaceGetScore function stub to Interface"},
    {"blueprint_type": "Interface", "domain": "interface", "action": "set_function_params", "edit_domain": "function",
     "arguments": {"action": "set_function_params", "asset_path": "/Game/Benchmarks/BPI_TestInterface",
                   "function_name": "BenchIFaceGetScore",
                   "outputs": [{"name": "Score", "type": "int"}]},
     "description": "Set int Score output on BenchIFaceGetScore Interface stub"},
    {"blueprint_type": "Interface", "domain": "interface", "action": "add_function", "edit_domain": "function",
     "arguments": {"action": "add_function", "asset_path": "/Game/Benchmarks/BPI_TestInterface",
                   "function_name": "BenchIFaceCanTarget"},
     "description": "Add BenchIFaceCanTarget function stub to Interface"},
    {"blueprint_type": "Interface", "domain": "interface", "action": "set_function_params", "edit_domain": "function",
     "arguments": {"action": "set_function_params", "asset_path": "/Game/Benchmarks/BPI_TestInterface",
                   "function_name": "BenchIFaceCanTarget",
                   "outputs": [{"name": "bCanTarget", "type": "bool"}]},
     "description": "Set bool bCanTarget output on BenchIFaceCanTarget Interface stub"},
    {"blueprint_type": "Interface", "domain": "interface", "action": "rename_function", "edit_domain": "function",
     "arguments": {"action": "rename_function", "asset_path": "/Game/Benchmarks/BPI_TestInterface",
                   "old_name": "BenchIFaceCanTarget", "new_name": "BenchIFaceCanTargetRenamed"},
     "description": "Rename BenchIFaceCanTarget to BenchIFaceCanTargetRenamed on Interface"},

    # Actor component/property edit coverage.
    {"blueprint_type": "Actor", "domain": "gameplay", "action": "add_component", "edit_domain": "component",
     "arguments": {"action": "add_component", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                   "component_class": "SpringArmComponent", "component_name": "BenchCameraBoom"},
     "description": "Add SpringArmComponent BenchCameraBoom to Actor"},
    {"blueprint_type": "Actor", "domain": "gameplay", "action": "set_component_property", "edit_domain": "component",
     "arguments": {"action": "set_component_property", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                   "component_name": "BenchCameraBoom", "property_name": "TargetArmLength", "value": "350.0"},
     "description": "Set TargetArmLength=350.0 on Actor BenchCameraBoom component"},
    {"blueprint_type": "Actor", "domain": "gameplay", "action": "duplicate_component", "edit_domain": "component",
     "arguments": {"action": "duplicate_component", "asset_path": "/Game/Benchmarks/BPB_TestActor",
                   "component_name": "BenchCameraBoom", "new_name": "BenchCameraBoomCopy"},
     "description": "Duplicate Actor BenchCameraBoom component to BenchCameraBoomCopy"},
]

ASSET_AUTHORING_ROOT = "/Game/Benchmarks/AssetAuthoring"
ASSET_AUTHORING_DELETE_PREFIXES = [ASSET_AUTHORING_ROOT]
ASSET_AUTHORING_BMP_2X2_B64 = (
    "Qk1GAAAAAAAAADYAAAAoAAAAAgAAAAIAAAABABgAAAAAABAAAAATCwAAEwsAAAAAAAAAAAAAAAD/AAD/AAAAAP8AAP8AAA=="
)
ASSET_AUTHORING_PNG_2X2_B64 = (
    "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAG0lEQVR4nGP476DwX6Hh/38GhRMJ///fcfgPAFcgCl5F2IZIAAAAAElFTkSuQmCC"
)
ASSET_AUTHORING_FILE_BMP_MARKER = "__ASSET_AUTHORING_FILE_BMP__"
ASSET_AUTHORING_INTERCHANGE_TEXTURE_BMP_MARKER = "__ASSET_AUTHORING_INTERCHANGE_TEXTURE_BMP__"
ASSET_AUTHORING_STRINGTABLE_CSV_MARKER = "__ASSET_AUTHORING_STRINGTABLE_CSV__"
ASSET_AUTHORING_STATIC_MESH_OBJ_MARKER = "__ASSET_AUTHORING_STATIC_MESH_OBJ__"
ASSET_AUTHORING_STATIC_MESH_LOD_OBJ_MARKER = "__ASSET_AUTHORING_STATIC_MESH_LOD_OBJ__"
ASSET_AUTHORING_MODELGEN_OBJ_MARKER = "__ASSET_AUTHORING_MODELGEN_OBJ__"
ASSET_AUTHORING_INTERCHANGE_MESH_OBJ_MARKER = "__ASSET_AUTHORING_INTERCHANGE_MESH_OBJ__"
ASSET_AUTHORING_INTERCHANGE_MESH_REIMPORT_OBJ_MARKER = "__ASSET_AUTHORING_INTERCHANGE_MESH_REIMPORT_OBJ__"
ASSET_AUTHORING_INTERCHANGE_MESH_EXPORT_OBJ_MARKER = "__ASSET_AUTHORING_INTERCHANGE_MESH_EXPORT_OBJ__"
ASSET_AUTHORING_INTERCHANGE_AUDIO_WAV_MARKER = "__ASSET_AUTHORING_INTERCHANGE_AUDIO_WAV__"
ASSET_AUTHORING_ENGINE_ROBOTO_MARKER = "__ENGINE_ROBOTO_REGULAR_TTF__"
_ASSET_AUTHORING_FIXTURE_LOCK = threading.Lock()

ASSET_AUTHORING_TASKS: List[Dict[str, Any]] = [
    {
        "name": "texture_import_rename_save",
        "domain": "asset",
        "edit_domain": "texture",
        "description": "Import and save a BMP Texture2D from bytes, rename it through AssetTools, and inspect the renamed asset",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/T_BenchIcon",
                                      f"{ASSET_AUTHORING_ROOT}/T_BenchIcon_Renamed"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "asset_query",
             "args": {"action": "import_texture_from_bytes",
                      "destination": f"{ASSET_AUTHORING_ROOT}/T_BenchIcon",
                      "bytes_b64": ASSET_AUTHORING_BMP_2X2_B64,
                      "format_hint": "bmp", "texture_role": "ui_icon",
                      "settings": {"srgb": True}, "save": True}},
            {"tool": "asset_query",
             "args": {"action": "batch_rename_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/T_BenchIcon"],
                      "add_suffix": "_Renamed"},
             "contains": ["status", "success", "T_BenchIcon_Renamed"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/T_BenchIcon_Renamed"}},
        ],
        "verify": [
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/T_BenchIcon_Renamed"},
             "contains": ["T_BenchIcon_Renamed", "Texture2D"]},
        ],
    },
    {
        "name": "texture_file_import_metadata_roundtrip",
        "domain": "asset",
        "edit_domain": "texture_file_import",
        "description": "Import a Texture2D from a real source file, assert source metadata is returned, save it, and validate the asset",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/T_BenchFileImport"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "asset_query",
             "args": {"action": "import_texture_from_file",
                      "source_path": ASSET_AUTHORING_FILE_BMP_MARKER,
                      "destination": f"{ASSET_AUTHORING_ROOT}/T_BenchFileImport",
                      "replace_existing": True,
                      "settings": {"srgb": True, "lod_group": "UI"}},
             "contains": ["TSF_BGRA8", "source_size_x", "source_size_y"],
             "not_contains": ["unknown"]},
            {"tool": "blueprint_query",
             "args": {"action": "set_cdo_property",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/T_BenchFileImport",
                      "property_name": "SRGB",
                      "value": False},
             "contains": ["SRGB"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/T_BenchFileImport"}},
        ],
        "verify": [
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/T_BenchFileImport"},
             "contains": ["T_BenchFileImport", "Texture2D", "source_width", "source_height"]},
            {"tool": "asset_query",
             "args": {"action": "validate_specialized_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/T_BenchFileImport"}},
            {"tool": "blueprint_query",
             "args": {"action": "get_cdo_properties",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/T_BenchFileImport"},
             "contains": ["SRGB", "false"]},
        ],
    },
    {
        "name": "imagegen_texture_provenance_roundtrip",
        "domain": "imagegen",
        "edit_domain": "generated_texture",
        "description": "Generate a deterministic Texture2D through ImageGen, save it, and verify redacted generation provenance",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/T_BenchGeneratedPrompt"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "imagegen_query",
             "args": {"action": "generate_image",
                      "prompt": "small benchmark checker icon texture, four colored quadrants",
                      "destination": f"{ASSET_AUTHORING_ROOT}/T_BenchGeneratedPrompt",
                      "overwrite_policy": "fail",
                      "texture_role": "ui_icon",
                      "save": True},
             "contains": ["T_BenchGeneratedPrompt", "local_deterministic", "ui_icon"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/T_BenchGeneratedPrompt"}},
        ],
        "verify": [
            {"tool": "imagegen_query",
             "args": {"action": "get_generated_asset_provenance",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/T_BenchGeneratedPrompt"},
             "contains": ["found", "true", "local_deterministic", "monolith/local-gradient-png-v1", "prompt_redacted"]},
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/T_BenchGeneratedPrompt"},
             "contains": ["T_BenchGeneratedPrompt", "Texture2D", "512x512", "TEXTUREGROUP_UI"]},
        ],
    },
    {
        "name": "imagegen_msdf_svg_roundtrip",
        "domain": "imagegen",
        "edit_domain": "msdf_texture",
        "description": "Validate an MSDF-ready SVG, bake an MSDF Texture2D plus preview material, save it, and inspect texture/provenance",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/T_BenchMsdfGlyph",
                                      f"{ASSET_AUTHORING_ROOT}/M_M_T_BenchMsdfGlyph"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "imagegen_query",
             "args": {"action": "validate_svg",
                      "svg_text": "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 64 64\"><rect x=\"12\" y=\"12\" width=\"40\" height=\"40\" fill=\"black\"/></svg>",
                      "profile": "msdf_source"},
             "contains": ["msdf_ready", "true", "geometry_valid", "true"]},
            {"tool": "imagegen_query",
             "args": {"action": "generate_msdf_from_svg",
                      "svg_text": "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 64 64\"><rect x=\"12\" y=\"12\" width=\"40\" height=\"40\" fill=\"black\"/></svg>",
                      "destination": f"{ASSET_AUTHORING_ROOT}/T_BenchMsdfGlyph",
                      "size": 128,
                      "pixel_range": 8,
                      "verify_samples": True,
                      "create_material": True,
                      "verify_material_render": False,
                      "overwrite_policy": "fail",
                      "save": True},
             "contains": ["T_BenchMsdfGlyph", "msdf_texture_asset_path", "channel_samples", "monolith_cpu_contour_msdf_v1"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/T_BenchMsdfGlyph"}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/M_M_T_BenchMsdfGlyph"}},
        ],
        "verify": [
            {"tool": "imagegen_query",
             "args": {"action": "get_generated_asset_provenance",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/T_BenchMsdfGlyph"},
             "contains": ["found", "true", "kind", "msdf", "monolith/local-msdf-cpu-v1"]},
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/T_BenchMsdfGlyph"},
             "contains": ["T_BenchMsdfGlyph", "Texture2D", "128x128", "TC_Masks", "SRGB", "False"]},
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/M_M_T_BenchMsdfGlyph"},
             "contains": ["M_M_T_BenchMsdfGlyph", "Material"]},
        ],
    },
    {
        "name": "interchange_texture_import_roundtrip",
        "domain": "interchange",
        "edit_domain": "interchange_texture_import",
        "description": "Import a Texture2D through the Interchange typed texture path, save it, and read source import metadata back",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/T_BenchInterchangeTexture"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "interchange_query",
             "args": {"action": "can_import",
                      "source_file": ASSET_AUTHORING_INTERCHANGE_TEXTURE_BMP_MARKER,
                      "destination_path": ASSET_AUTHORING_ROOT},
             "contains": ["can_import", "true", "BMP texture image"]},
            {"tool": "interchange_query",
             "args": {"action": "import_texture",
                      "source_file": ASSET_AUTHORING_INTERCHANGE_TEXTURE_BMP_MARKER,
                      "destination_path": ASSET_AUTHORING_ROOT,
                      "conflict_policy": "overwrite",
                      "confirm": True},
             "contains": ["status", "imported", "T_BenchInterchangeTexture", "Texture2D"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/T_BenchInterchangeTexture"}},
        ],
        "verify": [
            {"tool": "interchange_query",
             "args": {"action": "get_import_data",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/T_BenchInterchangeTexture"},
             "contains": ["has_import_data", "true", "T_BenchInterchangeTexture.bmp", "exists", "true"]},
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/T_BenchInterchangeTexture"},
             "contains": ["T_BenchInterchangeTexture", "Texture2D"]},
        ],
    },
    {
        "name": "interchange_static_mesh_reimport_export_roundtrip",
        "domain": "interchange",
        "edit_domain": "interchange_static_mesh_reimport_export",
        "description": "Import a StaticMesh through Interchange, edit its reimport source path, reimport, export, and verify the exported source",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/SM_BenchInterchangeCube"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "interchange_query",
             "args": {"action": "can_import",
                      "source_file": ASSET_AUTHORING_INTERCHANGE_MESH_OBJ_MARKER,
                      "destination_path": ASSET_AUTHORING_ROOT},
             "contains": ["can_import", "true", "Wavefront OBJ static mesh"]},
            {"tool": "interchange_query",
             "args": {"action": "import_mesh",
                      "source_file": ASSET_AUTHORING_INTERCHANGE_MESH_OBJ_MARKER,
                      "destination_path": ASSET_AUTHORING_ROOT,
                      "conflict_policy": "overwrite",
                      "confirm": True},
             "contains": ["status", "imported", "SM_BenchInterchangeCube", "StaticMesh"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SM_BenchInterchangeCube"}},
            {"tool": "interchange_query",
             "args": {"action": "can_reimport",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SM_BenchInterchangeCube"},
             "contains": ["can_reimport", "true", "SM_BenchInterchangeCube.obj"]},
            {"tool": "interchange_query",
             "args": {"action": "update_reimport_path",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SM_BenchInterchangeCube",
                      "source_file": ASSET_AUTHORING_INTERCHANGE_MESH_REIMPORT_OBJ_MARKER,
                      "confirm": True},
             "contains": ["updated_reimport_path", "SM_BenchInterchangeCube_Reimport.obj"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SM_BenchInterchangeCube"}},
            {"tool": "interchange_query",
             "args": {"action": "reimport_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SM_BenchInterchangeCube",
                      "confirm": True},
             "contains": ["status", "reimported", "SM_BenchInterchangeCube_Reimport.obj"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SM_BenchInterchangeCube"}},
            {"tool": "interchange_query",
             "args": {"action": "export_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SM_BenchInterchangeCube",
                      "file_path": ASSET_AUTHORING_INTERCHANGE_MESH_EXPORT_OBJ_MARKER,
                      "replace_existing": True,
                      "confirm": True},
             "contains": ["status", "exported", "file_size_bytes"]},
        ],
        "verify": [
            {"tool": "interchange_query",
             "args": {"action": "get_import_data",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SM_BenchInterchangeCube"},
             "contains": ["has_import_data", "true", "SM_BenchInterchangeCube_Reimport.obj", "exists", "true"]},
            {"tool": "interchange_query",
             "args": {"action": "can_import",
                      "source_file": ASSET_AUTHORING_INTERCHANGE_MESH_EXPORT_OBJ_MARKER,
                      "destination_path": ASSET_AUTHORING_ROOT},
             "contains": ["can_import", "true", "Wavefront OBJ static mesh"]},
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SM_BenchInterchangeCube"},
             "contains": ["SM_BenchInterchangeCube", "StaticMesh", "Triangles", "12",
                          "SM_BenchInterchangeCube_Reimport.obj"]},
        ],
    },
    {
        "name": "interchange_audio_import_roundtrip",
        "domain": "interchange",
        "edit_domain": "interchange_audio_import",
        "description": "Import a deterministic WAV through the Interchange typed audio path, save it, and read SoundWave import metadata back",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/SW_BenchInterchangeTone"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "interchange_query",
             "args": {"action": "can_import",
                      "source_file": ASSET_AUTHORING_INTERCHANGE_AUDIO_WAV_MARKER,
                      "destination_path": ASSET_AUTHORING_ROOT},
             "contains": ["can_import", "true"]},
            {"tool": "interchange_query",
             "args": {"action": "import_audio",
                      "source_file": ASSET_AUTHORING_INTERCHANGE_AUDIO_WAV_MARKER,
                      "destination_path": ASSET_AUTHORING_ROOT,
                      "conflict_policy": "overwrite",
                      "confirm": True},
             "contains": ["status", "imported", "SW_BenchInterchangeTone", "SoundWave"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SW_BenchInterchangeTone"}},
        ],
        "verify": [
            {"tool": "interchange_query",
             "args": {"action": "get_import_data",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SW_BenchInterchangeTone"},
             "contains": ["has_import_data", "true", "SW_BenchInterchangeTone.wav", "exists", "true"]},
            {"tool": "audio_query",
             "args": {"action": "get_sound_wave_info",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SW_BenchInterchangeTone"},
             "contains": ["SW_BenchInterchangeTone", "num_channels", "sample_rate"]},
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SW_BenchInterchangeTone"},
             "contains": ["SW_BenchInterchangeTone", "SoundWave"]},
        ],
    },
    {
        "name": "font_family_import_roundtrip",
        "domain": "asset",
        "edit_domain": "font",
        "description": "Resolve the project engine, import an engine TTF as UFont/UFontFace assets, save them, and inspect both outputs",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/Font_BenchRoboto",
                                      f"{ASSET_AUTHORING_ROOT}/F_Regular"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "asset_query",
             "args": {"action": "import_font_family",
                      "destination": ASSET_AUTHORING_ROOT,
                      "family_name": "Font_BenchRoboto",
                       "faces": [{"typeface": "Regular",
                                  "source_path": ASSET_AUTHORING_ENGINE_ROBOTO_MARKER}],
                       "loading_policy": "LazyLoad",
                       "hinting": "Default",
                       "save": True,
                       "allow_unique_names": False},
             "contains": ["Font_BenchRoboto", "F_Regular", "faces_imported"],
             "not_contains": ["Font_BenchRoboto1", "Font_BenchRoboto2", "F_Regular1", "F_Regular2"]},
            {"tool": "blueprint_query",
             "args": {"action": "set_cdo_property",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/Font_BenchRoboto",
                      "property_name": "LegacyFontSize",
                      "value": 18},
             "contains": ["LegacyFontSize"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/Font_BenchRoboto"}},
        ],
        "verify": [
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                       "asset_path": f"{ASSET_AUTHORING_ROOT}/Font_BenchRoboto"},
             "contains": ["Font_BenchRoboto", "Font"],
             "not_contains": ["Font_BenchRoboto1", "Font_BenchRoboto2"]},
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                       "asset_path": f"{ASSET_AUTHORING_ROOT}/F_Regular"},
             "contains": ["F_Regular", "FontFace"],
             "not_contains": ["F_Regular1", "F_Regular2"]},
            {"tool": "blueprint_query",
             "args": {"action": "get_cdo_properties",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/Font_BenchRoboto"},
             "contains": ["LegacyFontSize", "18"]},
        ],
    },
    {
        "name": "datatable_struct_row_roundtrip",
        "domain": "data",
        "edit_domain": "datatable",
        "description": "Create a UserDefinedStruct-backed DataTable, add and update a row, save it, and read back row values",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/DT_BenchRows",
                                      f"{ASSET_AUTHORING_ROOT}/S_BenchRow",
                                      f"{ASSET_AUTHORING_ROOT}/E_BenchState"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "blueprint_query",
             "args": {"action": "create_user_defined_struct",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/S_BenchRow",
                      "fields": [{"name": "DisplayName", "type": "text"},
                                  {"name": "Power", "type": "int", "default_value": "1"}]}},
            {"tool": "blueprint_query",
             "args": {"action": "create_user_defined_enum",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/E_BenchState",
                      "values": ["Idle", "Active", "Cooldown"]}},
            {"tool": "blueprint_query",
             "args": {"action": "create_data_table",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/DT_BenchRows",
                      "row_struct": f"{ASSET_AUTHORING_ROOT}/S_BenchRow.S_BenchRow"}},
            {"tool": "blueprint_query",
             "args": {"action": "add_data_table_row",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/DT_BenchRows",
                      "row_name": "Sword",
                      "values": {"DisplayName": "Iron Sword", "Power": 10}}},
            {"tool": "blueprint_query",
             "args": {"action": "update_data_table_row",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/DT_BenchRows",
                      "row_name": "Sword",
                      "confirm": True,
                      "values": {"DisplayName": "Silver Sword", "Power": 12}}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/DT_BenchRows"}},
        ],
        "verify": [
            {"tool": "blueprint_query",
             "args": {"action": "get_data_table_rows",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/DT_BenchRows"},
             "contains": ["Sword", "Silver Sword", "12"]},
            {"tool": "blueprint_query",
             "args": {"action": "describe_data_table_schema",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/DT_BenchRows"},
             "contains": ["DisplayName", "Power"]},
        ],
    },
    {
        "name": "stringtable_lifecycle",
        "domain": "localization",
        "edit_domain": "stringtable",
        "description": "Create a StringTable, write a localized entry and metadata, save it, and read the entry back",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/ST_BenchUI"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "localization_query",
             "args": {"action": "create_string_table",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/ST_BenchUI",
                      "namespace": "BenchUI", "confirm": True, "save": True}},
            {"tool": "localization_query",
             "args": {"action": "set_string_entry",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/ST_BenchUI",
                      "key": "StartButton", "source_string": "Start",
                      "metadata": {"Context": "MainMenu"}, "confirm": True, "save": True}},
            {"tool": "localization_query",
             "args": {"action": "set_string_metadata",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/ST_BenchUI",
                      "key": "StartButton", "metadata_key": "Screen",
                      "metadata_value": "Title", "confirm": True, "save": True}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/ST_BenchUI"}},
        ],
        "verify": [
            {"tool": "localization_query",
             "args": {"action": "get_string_table",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/ST_BenchUI",
                      "include_metadata": True},
             "contains": ["StartButton", "Start", "MainMenu", "Title"]},
            {"tool": "localization_query",
             "args": {"action": "validate_string_table",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/ST_BenchUI"}},
        ],
    },
    {
        "name": "stringtable_csv_import_roundtrip",
        "domain": "localization",
        "edit_domain": "stringtable_csv",
        "description": "Create a StringTable, import translator-style CSV rows with metadata, save it, and validate the imported entries",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/ST_BenchCsvImport"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "localization_query",
             "args": {"action": "create_string_table",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/ST_BenchCsvImport",
                      "namespace": "BenchCsvImport", "confirm": True, "save": True}},
            {"tool": "localization_query",
             "args": {"action": "import_string_table_csv",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/ST_BenchCsvImport",
                      "file_path": ASSET_AUTHORING_STRINGTABLE_CSV_MARKER,
                      "replace_existing": True,
                      "confirm": True,
                      "save": True}},
            {"tool": "localization_query",
             "args": {"action": "set_string_metadata",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/ST_BenchCsvImport",
                      "key": "ConfirmButton",
                      "metadata_key": "Screen",
                      "metadata_value": "ConfirmDialog",
                      "confirm": True,
                      "save": True}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/ST_BenchCsvImport"}},
        ],
        "verify": [
            {"tool": "localization_query",
             "args": {"action": "get_string_table",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/ST_BenchCsvImport",
                      "include_metadata": True},
             "contains": ["ConfirmButton", "Confirm", "metadata.Context", "Dialog", "ConfirmDialog"]},
            {"tool": "localization_query",
             "args": {"action": "validate_string_table",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/ST_BenchCsvImport"}},
        ],
    },
    {
        "name": "enhanced_input_mapping_roundtrip",
        "domain": "input",
        "edit_domain": "enhanced_input",
        "description": "Create an InputAction and InputMappingContext, map SpaceBar, validate mappings, and read the key binding back",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/IA_BenchJump",
                                      f"{ASSET_AUTHORING_ROOT}/IMC_BenchDefault"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "input_query",
             "args": {"action": "create_input_action",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/IA_BenchJump",
                      "value_type": "Boolean", "description": "Bench jump action",
                      "overwrite": True, "save": True}},
            {"tool": "input_query",
             "args": {"action": "set_input_action_properties",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/IA_BenchJump",
                      "description": "Bench jump action edited",
                      "trigger_when_paused": True, "save": True}},
            {"tool": "input_query",
             "args": {"action": "create_input_mapping_context",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/IMC_BenchDefault",
                      "description": "Bench mapping context", "overwrite": True, "save": True}},
            {"tool": "input_query", "allow_error": True,
             "args": {"action": "remove_input_mapping",
                      "context_path": f"{ASSET_AUTHORING_ROOT}/IMC_BenchDefault",
                      "action_path": f"{ASSET_AUTHORING_ROOT}/IA_BenchJump",
                      "key": "SpaceBar", "save": True}},
            {"tool": "input_query",
             "args": {"action": "add_input_mapping",
                      "context_path": f"{ASSET_AUTHORING_ROOT}/IMC_BenchDefault",
                      "action_path": f"{ASSET_AUTHORING_ROOT}/IA_BenchJump",
                      "key": "SpaceBar", "save": True}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/IA_BenchJump"}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/IMC_BenchDefault"}},
        ],
        "verify": [
            {"tool": "input_query",
             "args": {"action": "get_input_action",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/IA_BenchJump"},
             "contains": ["Boolean", "Bench jump action edited"]},
            {"tool": "input_query",
             "args": {"action": "get_input_mapping_context",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/IMC_BenchDefault"},
             "contains": ["IA_BenchJump", "SpaceBar"]},
            {"tool": "input_query",
             "args": {"action": "validate_input_mappings",
                      "context_paths": [f"{ASSET_AUTHORING_ROOT}/IMC_BenchDefault"]}},
        ],
    },
    {
        "name": "material_instance_save_roundtrip",
        "domain": "material",
        "edit_domain": "material",
        "description": "Create a Material, edit material properties, create a MIC, save both, and validate/read them back",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/MI_BenchAuthoring",
                                      f"{ASSET_AUTHORING_ROOT}/M_BenchAuthoring"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "material_query",
             "args": {"action": "create_material",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/M_BenchAuthoring",
                      "shading_model": "Unlit", "material_domain": "Surface"}},
            {"tool": "material_query",
             "args": {"action": "set_material_property",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/M_BenchAuthoring",
                      "two_sided": True, "shading_model": "Unlit"}},
            {"tool": "material_query",
             "args": {"action": "create_material_instance",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/MI_BenchAuthoring",
                      "parent_material": f"{ASSET_AUTHORING_ROOT}/M_BenchAuthoring"}},
            {"tool": "material_query",
             "args": {"action": "save_material",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/M_BenchAuthoring"}},
            {"tool": "material_query",
             "args": {"action": "save_material",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/MI_BenchAuthoring"}},
        ],
        "verify": [
            {"tool": "material_query",
             "args": {"action": "validate_material",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/M_BenchAuthoring"}},
            {"tool": "material_query",
             "args": {"action": "get_material_properties",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/M_BenchAuthoring"},
             "contains": ["two_sided", "Unlit"]},
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/MI_BenchAuthoring"},
             "contains": ["MI_BenchAuthoring", "MaterialInstance"]},
        ],
    },
    {
        "name": "material_function_roundtrip",
        "domain": "material",
        "edit_domain": "material_function",
        "description": "Create a Material Function, build function input/output expressions, save it, and inspect graph structure",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/MF_BenchScalarScale"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "material_query",
             "args": {"action": "create_material_function",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/MF_BenchScalarScale",
                      "description": "Benchmark scalar passthrough",
                      "expose_to_library": True,
                      "library_categories": ["Benchmark"],
                      "type": "MaterialFunction"},
             "contains": ["MF_BenchScalarScale", "MaterialFunction", "Benchmark scalar"]},
            {"tool": "material_query",
             "args": {"action": "build_function_graph",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/MF_BenchScalarScale",
                      "clear_existing": True,
                      "graph_spec": {
                          "inputs": [{"id": "BenchInput", "name": "BenchInput",
                                      "type": "float", "description": "Benchmark scalar input"}],
                          "outputs": [{"id": "BenchOutput", "name": "BenchOutput",
                                       "description": "Benchmark scalar output"}],
                          "connections": [{"from": "BenchInput", "to": "BenchOutput"}],
                      }},
             "contains": ["input_count", "output_count"]},
            {"tool": "material_query",
             "args": {"action": "save_material",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/MF_BenchScalarScale"}},
        ],
        "verify": [
            {"tool": "material_query",
             "args": {"action": "get_function_info",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/MF_BenchScalarScale"},
             "contains": ["BenchInput", "BenchOutput", "Benchmark scalar"]},
            {"tool": "material_query",
             "args": {"action": "export_function_graph",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/MF_BenchScalarScale"},
             "contains": ["BenchInput", "BenchOutput"]},
        ],
    },
    {
        "name": "material_graph_build_roundtrip",
        "domain": "material",
        "edit_domain": "material_graph",
        "description": "Create a base Material, build a graph, compile/save it, and read/export graph topology back",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/M_BenchGraphBuilt"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "material_query",
             "args": {"action": "create_material",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/M_BenchGraphBuilt",
                      "blend_mode": "Opaque",
                      "shading_model": "DefaultLit",
                      "material_domain": "Surface"},
             "contains": ["M_BenchGraphBuilt"]},
            {"tool": "material_query",
             "args": {"action": "build_material_graph",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/M_BenchGraphBuilt",
                      "clear_existing": True,
                      "graph_spec": {
                          "nodes": [
                              {"id": "BaseTint", "class": "VectorParameter",
                               "props": {"ParameterName": "BaseTint"},
                               "pos": [-420, -120]},
                              {"id": "RoughnessValue", "class": "ScalarParameter",
                               "props": {"ParameterName": "BenchRoughness"},
                               "pos": [-420, 120]},
                          ],
                          "outputs": [
                              {"from": "BaseTint", "from_pin": "RGB", "to_property": "BaseColor"},
                              {"from": "RoughnessValue", "to_property": "Roughness"},
                          ],
                      }}},
            {"tool": "material_query",
             "args": {"action": "recompile_material",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/M_BenchGraphBuilt",
                      "include_stats": True}},
            {"tool": "material_query",
             "args": {"action": "save_material",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/M_BenchGraphBuilt"}},
        ],
        "verify": [
            {"tool": "material_query",
             "args": {"action": "get_full_connection_graph",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/M_BenchGraphBuilt"},
             "contains": ["BaseColor", "Roughness"]},
            {"tool": "material_query",
             "args": {"action": "export_material_graph",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/M_BenchGraphBuilt",
                      "include_properties": True},
             "contains": ["BaseTint", "BenchRoughness"]},
            {"tool": "material_query",
             "args": {"action": "validate_material",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/M_BenchGraphBuilt"}},
            {"tool": "material_query",
             "args": {"action": "get_compilation_stats",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/M_BenchGraphBuilt"}},
        ],
    },
    {
        "name": "static_mesh_import_collision_roundtrip",
        "domain": "mesh",
        "edit_domain": "static_mesh_import",
        "description": "Import a local OBJ as a StaticMesh asset, edit simple collision, save it, and inspect mesh/collision data",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/SM_BenchImported"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "mesh_query",
             "args": {"action": "import_mesh",
                      "files": [ASSET_AUTHORING_STATIC_MESH_OBJ_MARKER],
                      "destination": ASSET_AUTHORING_ROOT,
                      "replace_existing": True,
                      "combine_meshes": True,
                      "auto_generate_collision": True,
                      "material_import": "skip"},
             "contains": ["SM_BenchImported", "StaticMesh", "triangle_count"]},
            {"tool": "mesh_query",
             "args": {"action": "set_mesh_collision",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SM_BenchImported",
                      "collision_type": "simple_box"},
             "contains": ["simple_box", "box_elements"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SM_BenchImported"}},
        ],
        "verify": [
            {"tool": "mesh_query",
             "args": {"action": "get_mesh_info",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SM_BenchImported"},
             "contains": ["SM_BenchImported", "StaticMesh", "triangles", "12"]},
            {"tool": "mesh_query",
             "args": {"action": "get_mesh_collision",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SM_BenchImported"},
             "contains": ["collision_type", "boxes"]},
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SM_BenchImported"},
             "contains": ["SM_BenchImported", "StaticMesh"]},
        ],
    },
    {
        "name": "static_mesh_lod_screen_quality_roundtrip",
        "domain": "mesh",
        "edit_domain": "static_mesh_lod_quality",
        "description": "Import a StaticMesh, edit its LOD screen-size metadata, save it, and run mesh quality/game-readiness read-backs",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/SM_BenchLodScreen"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "mesh_query",
             "args": {"action": "import_mesh",
                      "files": [ASSET_AUTHORING_STATIC_MESH_LOD_OBJ_MARKER],
                      "destination": ASSET_AUTHORING_ROOT,
                      "replace_existing": True,
                      "combine_meshes": True,
                      "auto_generate_collision": True,
                      "material_import": "skip"},
             "contains": ["SM_BenchLodScreen", "StaticMesh", "triangle_count"]},
            {"tool": "mesh_query",
             "args": {"action": "set_lod_screen_sizes",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SM_BenchLodScreen",
                      "screen_sizes": [0.95]},
             "contains": ["lod_count", "screen_sizes"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SM_BenchLodScreen"}},
        ],
        "verify": [
            {"tool": "mesh_query",
             "args": {"action": "get_mesh_lods",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SM_BenchLodScreen"},
             "contains": ["screen_size", "triangles", "12"]},
            {"tool": "mesh_query",
             "args": {"action": "analyze_mesh_quality",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SM_BenchLodScreen"},
             "contains": ["degenerate_triangles", "duplicate_vertices", "lod_count", "overall_score"]},
            {"tool": "mesh_query",
             "args": {"action": "validate_game_ready",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SM_BenchLodScreen"},
             "contains": ["collision", "lods"]},
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SM_BenchLodScreen"},
             "contains": ["SM_BenchLodScreen", "StaticMesh"]},
        ],
    },
    {
        "name": "modelgen_static_mesh_provenance_roundtrip",
        "domain": "modelgen",
        "edit_domain": "generated_static_mesh",
        "description": "Import a generated-model OBJ through ModelGen, save it as StaticMesh, and verify redacted generation provenance",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/SM_BenchGeneratedModel"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "modelgen_query",
             "args": {"action": "import_generated_model",
                      "file_path": ASSET_AUTHORING_MODELGEN_OBJ_MARKER,
                      "destination": ASSET_AUTHORING_ROOT,
                      "provider": "external",
                      "model": "benchmark/local-obj",
                      "prompt": "small generated benchmark cube mesh",
                      "replace_existing": True,
                      "material_import": "skip",
                      "save": True},
             "contains": ["SM_BenchGeneratedModel", "vertex_count", "triangle_count", "provenance"]},
            {"tool": "mesh_query",
             "args": {"action": "set_mesh_collision",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SM_BenchGeneratedModel",
                      "collision_type": "simple_box"},
             "contains": ["simple_box", "box_elements"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SM_BenchGeneratedModel"}},
        ],
        "verify": [
            {"tool": "modelgen_query",
             "args": {"action": "get_generated_model_provenance",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SM_BenchGeneratedModel"},
             "contains": ["found", "true", "kind", "model", "benchmark/local-obj", "prompt_redacted"]},
            {"tool": "mesh_query",
             "args": {"action": "get_mesh_collision",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SM_BenchGeneratedModel"},
             "contains": ["collision_type", "boxes"]},
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SM_BenchGeneratedModel"},
             "contains": ["SM_BenchGeneratedModel", "StaticMesh", "Triangles", "12"]},
        ],
    },
    {
        "name": "curvetable_key_roundtrip",
        "domain": "data",
        "edit_domain": "curvetable",
        "description": "Create a CurveTable UObject asset, write linear curve keys, save it, and read the curve row back",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/CT_BenchScaling"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "blueprint_query",
             "args": {"action": "create_data_asset",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/CT_BenchScaling",
                      "class_name": "CurveTable"}},
            {"tool": "blueprint_query",
             "args": {"action": "set_curve_table_keys",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/CT_BenchScaling",
                      "row_name": "DamageScale",
                      "keys": [{"time": 0.0, "value": 1.0},
                                {"time": 10.0, "value": 2.5}],
                      "interp_mode": "linear", "save": True}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/CT_BenchScaling"}},
        ],
        "verify": [
            {"tool": "blueprint_query",
             "args": {"action": "read_curve_table",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/CT_BenchScaling",
                      "row_name": "DamageScale"},
            "contains": ["DamageScale", "2.5", "simple"]},
        ],
    },
    {
        "name": "dataasset_reflection_roundtrip",
        "domain": "data",
        "edit_domain": "data_asset",
        "description": "Create a raw UObject asset, edit top-level properties through reflection, save it, and read values back",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/PM_BenchSurface"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "blueprint_query",
             "args": {"action": "create_data_asset",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/PM_BenchSurface",
                      "class_name": "PhysicalMaterial"}},
            {"tool": "blueprint_query",
             "args": {"action": "set_cdo_property",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/PM_BenchSurface",
                      "property_name": "Friction", "value": 0.73}},
            {"tool": "blueprint_query",
             "args": {"action": "set_cdo_property",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/PM_BenchSurface",
                      "property_name": "Restitution", "value": 0.11}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/PM_BenchSurface"}},
        ],
        "verify": [
            {"tool": "blueprint_query",
             "args": {"action": "get_cdo_properties",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/PM_BenchSurface",
                      "name_pattern": "Friction"},
             "contains": ["Friction", "0.73"]},
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/PM_BenchSurface"},
             "contains": ["PM_BenchSurface", "PhysicalMaterial"]},
        ],
    },
    {
        "name": "umg_widget_tree_roundtrip",
        "domain": "ui",
        "edit_domain": "widget_blueprint",
        "description": "Create a Widget Blueprint, add and style a TextBlock, compile/save it, and read the widget tree back",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/WBP_BenchAuthoring"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "ui_query",
             "args": {"action": "create_widget_blueprint",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/WBP_BenchAuthoring",
                      "root_widget": "CanvasPanel"}},
            {"tool": "ui_query",
             "args": {"action": "add_widget",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/WBP_BenchAuthoring",
                      "widget_class": "TextBlock", "widget_name": "BenchTitle",
                      "anchor_preset": "top_left",
                      "position": {"x": 32, "y": 24},
                      "size": {"x": 360, "y": 64},
                      "compile": False}},
            {"tool": "ui_query",
             "args": {"action": "set_text",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/WBP_BenchAuthoring",
                      "widget_name": "BenchTitle",
                      "text": "Benchmark Ready", "font_size": 24,
                      "compile": False}},
            {"tool": "ui_query",
             "args": {"action": "compile_widget",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/WBP_BenchAuthoring"}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/WBP_BenchAuthoring"}},
        ],
        "verify": [
            {"tool": "ui_query",
             "args": {"action": "get_widget_tree",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/WBP_BenchAuthoring"},
            "contains": ["BenchTitle", "TextBlock", "Benchmark Ready"]},
        ],
    },
    {
        "name": "commonui_text_block_roundtrip",
        "domain": "ui",
        "edit_domain": "commonui_text_block",
        "description": "Create a CommonActivatableWidget, convert/configure CommonTextBlock text, compile/save, and audit CommonUI wiring",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/WBP_BenchCommonPanel"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "ui_query",
             "args": {"action": "create_activatable_widget",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/WBP_BenchCommonPanel",
                      "root_widget": "CanvasPanel"},
             "contains": ["WBP_BenchCommonPanel", "CommonActivatableWidget"]},
            {"tool": "ui_query",
             "args": {"action": "add_widget",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/WBP_BenchCommonPanel",
                      "widget_class": "TextBlock", "widget_name": "BenchCommonLabel",
                      "anchor_preset": "center",
                      "position": {"x": 0, "y": 0},
                      "size": {"x": 420, "y": 64},
                      "compile": False},
             "contains": ["BenchCommonLabel", "TextBlock"]},
            {"tool": "ui_query",
             "args": {"action": "set_text",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/WBP_BenchCommonPanel",
                      "widget_name": "BenchCommonLabel",
                      "text": "CommonUI Bench", "font_size": 22,
                      "compile": False}},
            {"tool": "ui_query",
             "args": {"action": "convert_textblock_to_common",
                      "wbp_path": f"{ASSET_AUTHORING_ROOT}/WBP_BenchCommonPanel",
                      "widget_name": "BenchCommonLabel"},
             "contains": ["BenchCommonLabel", "CommonTextBlock"]},
            {"tool": "ui_query",
             "args": {"action": "configure_common_text",
                      "wbp_path": f"{ASSET_AUTHORING_ROOT}/WBP_BenchCommonPanel",
                      "widget_name": "BenchCommonLabel",
                      "wrap_text_width": 420,
                      "line_height_percentage": 1.1,
                      "mobile_font_size_multiplier": 1.2,
                      "text_case": "ToUpper"},
             "contains": ["BenchCommonLabel"]},
            {"tool": "ui_query",
             "args": {"action": "compile_widget",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/WBP_BenchCommonPanel"},
             "contains": ["compiled", "true", "error_count", "0"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/WBP_BenchCommonPanel"}},
        ],
        "verify": [
            {"tool": "ui_query",
             "args": {"action": "get_widget_tree",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/WBP_BenchCommonPanel"},
             "contains": ["BenchCommonLabel", "CommonTextBlock", "CommonUI Bench"]},
            {"tool": "ui_query",
             "args": {"action": "audit_commonui_widget",
                      "wbp_path": f"{ASSET_AUTHORING_ROOT}/WBP_BenchCommonPanel"},
             "contains": ["WBP_BenchCommonPanel", "issue_count"]},
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/WBP_BenchCommonPanel"},
             "contains": ["WBP_BenchCommonPanel", "WidgetBlueprint"]},
        ],
    },
    {
        "name": "animation_core_assets_roundtrip",
        "domain": "animation",
        "edit_domain": "animation_core",
        "description": "Create core skeletal animation assets from an engine skeleton, edit sequence/blendspace settings, save, and inspect sequence/blendspace/ABP metadata",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/A_BenchSkeletalCube",
                                      f"{ASSET_AUTHORING_ROOT}/BS_BenchSkeletalCube",
                                      f"{ASSET_AUTHORING_ROOT}/ABP_BenchSkeletalCube"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "animation_query",
             "args": {"action": "create_sequence",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/A_BenchSkeletalCube",
                      "skeleton_path": "/Engine/EngineMeshes/SkeletalCube_Skeleton"},
             "contains": ["A_BenchSkeletalCube", "SkeletalCube_Skeleton"]},
            {"tool": "animation_query",
             "args": {"action": "set_sequence_properties",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/A_BenchSkeletalCube",
                      "rate_scale": 1.25,
                      "loop": True},
             "contains": ["rate_scale", "1.25", "loop"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/A_BenchSkeletalCube"}},
            {"tool": "animation_query",
             "args": {"action": "create_blend_space",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BS_BenchSkeletalCube",
                      "skeleton_path": "/Engine/EngineMeshes/SkeletalCube_Skeleton",
                      "axis_x_name": "Speed",
                      "axis_x_min": 0,
                      "axis_x_max": 600,
                      "axis_y_name": "Direction",
                      "axis_y_min": -180,
                      "axis_y_max": 180},
             "contains": ["BS_BenchSkeletalCube", "SkeletalCube_Skeleton", "Speed", "Direction"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BS_BenchSkeletalCube"}},
            {"tool": "animation_query",
             "args": {"action": "create_anim_blueprint",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/ABP_BenchSkeletalCube",
                      "skeleton_path": "/Engine/EngineMeshes/SkeletalCube_Skeleton",
                      "parent_class": "AnimInstance"},
             "contains": ["ABP_BenchSkeletalCube", "SkeletalCube_Skeleton", "AnimInstance",
                          "generated_class"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/ABP_BenchSkeletalCube"}},
        ],
        "verify": [
            {"tool": "animation_query",
             "args": {"action": "get_sequence_info",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/A_BenchSkeletalCube"},
             "contains": ["A_BenchSkeletalCube", "SkeletalCube_Skeleton", "rate_scale",
                          "1.25", "is_looping", "true"]},
            {"tool": "animation_query",
             "args": {"action": "get_blend_space_info",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BS_BenchSkeletalCube"},
             "contains": ["BS_BenchSkeletalCube", "SkeletalCube_Skeleton", "Speed",
                          "Direction"]},
            {"tool": "animation_query",
             "args": {"action": "get_abp_info",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/ABP_BenchSkeletalCube"},
             "contains": ["ABP_BenchSkeletalCube", "SkeletalCube_Skeleton", "AnimGraph",
                          "EventGraph", "AnimInstance"]},
        ],
    },
    {
        "name": "animation_montage_sections_roundtrip",
        "domain": "animation",
        "edit_domain": "montage",
        "description": "Create an AnimSequence and Montage from the engine skeletal cube skeleton, add slot/sections/segment, save, and inspect montage layout",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/A_BenchMontageSource",
                                      f"{ASSET_AUTHORING_ROOT}/AM_BenchSkeletalCube"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "animation_query",
             "args": {"action": "create_sequence",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/A_BenchMontageSource",
                      "skeleton_path": "/Engine/EngineMeshes/SkeletalCube_Skeleton"},
             "contains": ["A_BenchMontageSource", "SkeletalCube_Skeleton"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/A_BenchMontageSource"}},
            {"tool": "animation_query",
             "args": {"action": "create_montage",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/AM_BenchSkeletalCube",
                      "skeleton_path": "/Engine/EngineMeshes/SkeletalCube_Skeleton"},
             "contains": ["AM_BenchSkeletalCube", "SkeletalCube_Skeleton"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/AM_BenchSkeletalCube"}},
            {"tool": "animation_query",
             "args": {"action": "add_montage_slot",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/AM_BenchSkeletalCube",
                      "slot_name": "BenchSlot"},
             "contains": ["BenchSlot"]},
            {"tool": "animation_query",
             "args": {"action": "add_montage_section",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/AM_BenchSkeletalCube",
                      "section_name": "Intro",
                      "start_time": 0.0},
             "contains": ["Intro"]},
            {"tool": "animation_query",
             "args": {"action": "add_montage_section",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/AM_BenchSkeletalCube",
                      "section_name": "Recover",
                      "start_time": 0.1},
             "contains": ["Recover"]},
            {"tool": "animation_query",
             "args": {"action": "add_montage_anim_segment",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/AM_BenchSkeletalCube",
                      "anim_path": f"{ASSET_AUTHORING_ROOT}/A_BenchMontageSource",
                      "slot_index": 1,
                      "start_pos": 0.0,
                      "anim_start_time": 0.0,
                      "anim_end_time": 0.1,
                      "play_rate": 1.0,
                      "looping_count": 1},
             "contains": ["A_BenchMontageSource"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/AM_BenchSkeletalCube"}},
        ],
        "verify": [
            {"tool": "animation_query",
             "args": {"action": "get_montage_info",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/AM_BenchSkeletalCube"},
             "contains": ["AM_BenchSkeletalCube", "BenchSlot", "Intro", "Recover"]},
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/AM_BenchSkeletalCube"},
             "contains": ["AM_BenchSkeletalCube", "AnimMontage"]},
        ],
    },
    {
        "name": "animation_sequence_curve_sync_metadata_roundtrip",
        "domain": "animation",
        "edit_domain": "animation_sequence_events",
        "description": "Create an AnimSequence, add curve and sync-marker metadata, save it, and read event metadata back",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/A_BenchEventMeta"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "animation_query",
             "args": {"action": "create_sequence",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/A_BenchEventMeta",
                      "skeleton_path": "/Engine/EngineMeshes/SkeletalCube_Skeleton"},
             "contains": ["A_BenchEventMeta", "SkeletalCube_Skeleton"]},
            {"tool": "animation_query",
             "args": {"action": "add_curve",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/A_BenchEventMeta.A_BenchEventMeta",
                      "curve_name": "BenchSpeedCurve",
                      "curve_type": "Float"},
             "contains": ["BenchSpeedCurve"]},
            {"tool": "animation_query",
             "args": {"action": "set_curve_keys",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/A_BenchEventMeta.A_BenchEventMeta",
                      "curve_name": "BenchSpeedCurve",
                      "keys_json": "[{\"time\":0.0,\"value\":0.25,\"interp\":\"linear\"},{\"time\":0.1,\"value\":1.0,\"interp\":\"linear\"}]"},
             "contains": ["BenchSpeedCurve", "num_keys", "2"]},
            {"tool": "animation_query",
             "args": {"action": "add_sync_marker",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/A_BenchEventMeta.A_BenchEventMeta",
                      "marker_name": "BenchPlant",
                      "time": 0.0,
                      "track_index": 0},
             "contains": ["BenchPlant"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/A_BenchEventMeta"}},
        ],
        "verify": [
            {"tool": "animation_query",
             "args": {"action": "get_curve_keys",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/A_BenchEventMeta.A_BenchEventMeta",
                      "curve_name": "BenchSpeedCurve"},
             "contains": ["BenchSpeedCurve", "0.25", "1"]},
            {"tool": "animation_query",
             "args": {"action": "get_sync_markers",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/A_BenchEventMeta.A_BenchEventMeta"},
             "contains": ["BenchPlant"]},
            {"tool": "animation_query",
             "args": {"action": "get_sequence_info",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/A_BenchEventMeta"},
             "contains": ["A_BenchEventMeta", "SkeletalCube_Skeleton"]},
        ],
    },
    {
        "name": "animation_ikrig_retargeter_metadata_roundtrip",
        "domain": "animation",
        "edit_domain": "ikrig_retargeter",
        "description": "Create source/target IK Rig assets and an IK Retargeter, assign rigs/preview meshes, save them, and inspect retarget metadata",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/IKR_BenchSource",
                                      f"{ASSET_AUTHORING_ROOT}/IKR_BenchTarget",
                                      f"{ASSET_AUTHORING_ROOT}/RTG_BenchSourceToTarget"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "animation_query",
             "args": {"action": "create_ik_rig",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/IKR_BenchSource",
                      "skeletal_mesh_path": "/Engine/EngineMeshes/SkeletalCube"},
             "contains": ["IKR_BenchSource", "SkeletalCube", "bone_count"]},
            {"tool": "animation_query",
             "args": {"action": "create_ik_rig",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/IKR_BenchTarget",
                      "skeletal_mesh_path": "/Engine/EngineMeshes/SkeletalCube"},
             "contains": ["IKR_BenchTarget", "SkeletalCube", "bone_count"]},
            {"tool": "animation_query",
             "args": {"action": "create_ik_retargeter",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/RTG_BenchSourceToTarget"},
             "contains": ["RTG_BenchSourceToTarget", "retarget_op_count"]},
            {"tool": "animation_query",
             "args": {"action": "set_retargeter_rigs",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/RTG_BenchSourceToTarget",
                      "source_ik_rig_path": f"{ASSET_AUTHORING_ROOT}/IKR_BenchSource",
                      "target_ik_rig_path": f"{ASSET_AUTHORING_ROOT}/IKR_BenchTarget",
                      "source_preview_mesh": "/Engine/EngineMeshes/SkeletalCube",
                      "target_preview_mesh": "/Engine/EngineMeshes/SkeletalCube",
                      "auto_map": "fuzzy"},
             "contains": ["IKR_BenchSource", "IKR_BenchTarget", "default_ops_seeded", "true"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/IKR_BenchSource"}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/IKR_BenchTarget"}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/RTG_BenchSourceToTarget"}},
        ],
        "verify": [
            {"tool": "animation_query",
             "args": {"action": "get_ikrig_info",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/IKR_BenchSource"},
             "contains": ["IKR_BenchSource", "SkeletalCube", "bone_count"]},
            {"tool": "animation_query",
             "args": {"action": "get_ikrig_info",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/IKR_BenchTarget"},
             "contains": ["IKR_BenchTarget", "SkeletalCube", "bone_count"]},
            {"tool": "animation_query",
             "args": {"action": "get_retargeter_info",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/RTG_BenchSourceToTarget"},
             "contains": ["IKR_BenchSource", "IKR_BenchTarget", "retarget_op_count",
                          "source_preview_mesh", "target_preview_mesh"]},
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/RTG_BenchSourceToTarget"},
             "contains": ["RTG_BenchSourceToTarget"]},
        ],
    },
    {
        "name": "audio_soundcue_roundtrip",
        "domain": "audio",
        "edit_domain": "sound_cue",
        "description": "Create a test SoundWave, SoundClass, and SoundCue, assign class metadata, save, validate, and read references back",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/SW_BenchTone",
                                      f"{ASSET_AUTHORING_ROOT}/SoundClass_BenchSFX",
                                      f"{ASSET_AUTHORING_ROOT}/SCue_BenchTone"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "audio_query",
             "args": {"action": "create_test_wave",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SW_BenchTone",
                      "frequency_hz": 440.0, "duration_seconds": 0.25,
                      "sample_rate": 44100, "amplitude": 0.4}},
            {"tool": "audio_query",
             "args": {"action": "create_sound_class",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SoundClass_BenchSFX",
                      "properties": {"Volume": 0.75}}},
            {"tool": "audio_query",
             "args": {"action": "set_sound_class_properties",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SoundClass_BenchSFX",
                      "properties": {"Volume": 0.5, "Pitch": 1.0}}},
            {"tool": "audio_query",
             "args": {"action": "batch_assign_sound_class",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/SW_BenchTone"],
                      "sound_class": f"{ASSET_AUTHORING_ROOT}/SoundClass_BenchSFX"}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SW_BenchTone"}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SoundClass_BenchSFX"}},
            {"tool": "audio_query",
             "args": {"action": "create_sound_cue",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SCue_BenchTone",
                      "sound_waves": [f"{ASSET_AUTHORING_ROOT}/SW_BenchTone"]}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SCue_BenchTone"}},
        ],
        "verify": [
            {"tool": "audio_query",
             "args": {"action": "get_sound_class_properties",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SoundClass_BenchSFX"},
             "contains": ["Volume", "0.5"]},
            {"tool": "audio_query",
             "args": {"action": "get_sound_wave_info",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SW_BenchTone"},
             "contains": ["SoundClass_BenchSFX"]},
            {"tool": "audio_query",
             "args": {"action": "find_sound_waves_in_cue",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SCue_BenchTone"},
             "contains": ["SW_BenchTone"]},
            {"tool": "audio_query",
             "args": {"action": "validate_sound_cue",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SCue_BenchTone"}},
        ],
    },
    {
        "name": "audio_routing_settings_roundtrip",
        "domain": "audio",
        "edit_domain": "audio_routing",
        "description": "Create attenuation, mix, concurrency, and submix assets, edit routing settings, save, and read each value back",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/SA_BenchSpatial",
                                      f"{ASSET_AUTHORING_ROOT}/SoundClass_BenchDialog",
                                      f"{ASSET_AUTHORING_ROOT}/Mix_BenchDialog",
                                      f"{ASSET_AUTHORING_ROOT}/Concurrency_BenchDialog",
                                      f"{ASSET_AUTHORING_ROOT}/Submix_BenchDialog"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "audio_query",
             "args": {"action": "create_sound_class",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SoundClass_BenchDialog",
                      "properties": {"Volume": 0.9}},
             "contains": ["SoundClass_BenchDialog", "Volume"]},
            {"tool": "audio_query",
             "args": {"action": "create_sound_attenuation",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SA_BenchSpatial",
                      "settings": {"bAttenuate": True, "FalloffDistance": 725.0}},
             "contains": ["SA_BenchSpatial", "FalloffDistance"]},
            {"tool": "audio_query",
             "args": {"action": "set_attenuation_settings",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SA_BenchSpatial",
                      "settings": {"bEnableOcclusion": True,
                                   "OcclusionVolumeAttenuation": 0.42}},
             "contains": ["bEnableOcclusion", "OcclusionVolumeAttenuation", "0.419"]},
            {"tool": "audio_query",
             "args": {"action": "create_sound_mix",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/Mix_BenchDialog"},
             "contains": ["Mix_BenchDialog"]},
            {"tool": "audio_query",
             "args": {"action": "create_sound_concurrency",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/Concurrency_BenchDialog",
                      "settings": {"MaxCount": 3, "VolumeScale": 0.6}},
             "contains": ["Concurrency_BenchDialog", "MaxCount"]},
            {"tool": "audio_query",
             "args": {"action": "set_concurrency_settings",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/Concurrency_BenchDialog",
                      "settings": {"MaxCount": 4, "VolumeScale": 0.55}},
             "contains": ["MaxCount", "4"]},
            {"tool": "audio_query",
             "args": {"action": "create_sound_submix",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/Submix_BenchDialog"},
             "contains": ["Submix_BenchDialog"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SA_BenchSpatial"}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SoundClass_BenchDialog"}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/Mix_BenchDialog"}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/Concurrency_BenchDialog.Concurrency_BenchDialog"}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/Submix_BenchDialog"}},
        ],
        "verify": [
            {"tool": "audio_query",
             "args": {"action": "get_attenuation_settings",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SA_BenchSpatial"},
             "contains": ["bEnableOcclusion", "OcclusionVolumeAttenuation", "0.419"]},
            {"tool": "audio_query",
             "args": {"action": "get_sound_mix_settings",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/Mix_BenchDialog"},
             "contains": ["Mix_BenchDialog", "duration"]},
            {"tool": "audio_query",
             "args": {"action": "get_concurrency_settings",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/Concurrency_BenchDialog"},
             "contains": ["MaxCount", "4"]},
            {"tool": "audio_query",
             "args": {"action": "get_submix_properties",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/Submix_BenchDialog"},
             "contains": ["Submix_BenchDialog", "output_volume_db"]},
        ],
    },
    {
        "name": "audio_soundcue_spec_roundtrip",
        "domain": "audio",
        "edit_domain": "sound_cue_spec",
        "description": "Build a SoundCue graph from a declarative spec with two WavePlayers feeding a Mixer, save it, and validate references",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/SW_BenchSpecLow",
                                      f"{ASSET_AUTHORING_ROOT}/SW_BenchSpecHigh",
                                      f"{ASSET_AUTHORING_ROOT}/SCue_BenchSpecMix"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "audio_query",
             "args": {"action": "create_test_wave",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SW_BenchSpecLow",
                      "frequency_hz": 330.0,
                      "duration_seconds": 0.2,
                      "sample_rate": 44100,
                      "amplitude": 0.35}},
            {"tool": "audio_query",
             "args": {"action": "create_test_wave",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SW_BenchSpecHigh",
                      "frequency_hz": 660.0,
                      "duration_seconds": 0.2,
                      "sample_rate": 44100,
                      "amplitude": 0.35}},
            {"tool": "audio_query",
             "args": {"action": "build_sound_cue_from_spec",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SCue_BenchSpecMix",
                      "spec": {
                          "nodes": [
                              {"id": "low", "type": "WavePlayer",
                               "properties": {"SoundWave": f"{ASSET_AUTHORING_ROOT}/SW_BenchSpecLow"}},
                              {"id": "high", "type": "WavePlayer",
                               "properties": {"SoundWave": f"{ASSET_AUTHORING_ROOT}/SW_BenchSpecHigh"}},
                              {"id": "mix", "type": "Mixer"},
                          ],
                          "connections": [
                              {"from": "low", "to": "mix", "child_index": 0},
                              {"from": "high", "to": "mix", "child_index": 1},
                          ],
                          "first_node": "mix",
                          "properties": {"VolumeMultiplier": 0.8, "PitchMultiplier": 1.0},
                      }},
             "contains": ["SCue_BenchSpecMix", "node_count", "3", "connection_count", "2"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SCue_BenchSpecMix"}},
        ],
        "verify": [
            {"tool": "audio_query",
             "args": {"action": "get_sound_cue_graph",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SCue_BenchSpecMix"},
             "contains": ["SoundNodeMixer", "SoundNodeWavePlayer", "SW_BenchSpecLow", "SW_BenchSpecHigh"]},
            {"tool": "audio_query",
             "args": {"action": "find_sound_waves_in_cue",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SCue_BenchSpecMix"},
             "contains": ["SW_BenchSpecLow", "SW_BenchSpecHigh"]},
            {"tool": "audio_query",
             "args": {"action": "validate_sound_cue",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SCue_BenchSpecMix"},
             "contains": ["valid", "true"]},
        ],
    },
    {
        "name": "audio_template_routing_roundtrip",
        "domain": "audio",
        "edit_domain": "audio_template_batch",
        "description": "Apply a batch audio template to a generated SoundWave, save it, and read back routing/compression/looping fields",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/SW_BenchTemplated",
                                      f"{ASSET_AUTHORING_ROOT}/SoundClass_BenchTemplate",
                                      f"{ASSET_AUTHORING_ROOT}/SA_BenchTemplate",
                                      f"{ASSET_AUTHORING_ROOT}/Concurrency_BenchTemplate",
                                      f"{ASSET_AUTHORING_ROOT}/Submix_BenchTemplate"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "audio_query",
             "args": {"action": "create_test_wave",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SW_BenchTemplated",
                      "frequency_hz": 550.0,
                      "duration_seconds": 0.2,
                      "sample_rate": 44100,
                      "amplitude": 0.3}},
            {"tool": "audio_query",
             "args": {"action": "create_sound_class",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SoundClass_BenchTemplate",
                      "properties": {"Volume": 0.67}}},
            {"tool": "audio_query",
             "args": {"action": "create_sound_attenuation",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SA_BenchTemplate",
                      "settings": {"bAttenuate": True, "FalloffDistance": 512.0}}},
            {"tool": "audio_query",
             "args": {"action": "create_sound_concurrency",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/Concurrency_BenchTemplate",
                      "settings": {"MaxCount": 2}}},
            {"tool": "audio_query",
             "args": {"action": "create_sound_submix",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/Submix_BenchTemplate"}},
            {"tool": "audio_query",
             "args": {"action": "apply_audio_template",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/SW_BenchTemplated"],
                      "template": {
                          "sound_class": f"{ASSET_AUTHORING_ROOT}/SoundClass_BenchTemplate",
                          "attenuation": f"{ASSET_AUTHORING_ROOT}/SA_BenchTemplate",
                          "submix": f"{ASSET_AUTHORING_ROOT}/Submix_BenchTemplate",
                          "concurrency": f"{ASSET_AUTHORING_ROOT}/Concurrency_BenchTemplate",
                          "compression": {"quality": 55, "type": "BinkAudio"},
                          "looping": True,
                          "virtualization": "PlayWhenSilent",
                      }},
             "contains": ["modified", "1", "sound_class", "attenuation", "submix", "concurrency",
                          "compression", "looping", "virtualization"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SW_BenchTemplated"}},
        ],
        "verify": [
            {"tool": "audio_query",
             "args": {"action": "get_sound_wave_info",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SW_BenchTemplated"},
             "contains": ["SW_BenchTemplated", "SoundClass_BenchTemplate", "SA_BenchTemplate",
                          "Submix_BenchTemplate", "compression_quality", "55", "looping", "true"]},
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/SW_BenchTemplated"},
             "contains": ["SW_BenchTemplated", "SoundWave"]},
        ],
    },
    {
        "name": "niagara_system_roundtrip",
        "domain": "niagara",
        "edit_domain": "niagara_system",
        "description": "Create a Niagara System, add an emitter, edit warmup/timing/fixed bounds, save, validate, and read back system data",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/NS_BenchBurst"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "niagara_query",
             "args": {"action": "create_system",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/NS_BenchBurst"}},
            {"tool": "niagara_query",
             "args": {"action": "create_emitter",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/NS_BenchBurst",
                      "name": "BenchEmitter", "sim_target": "cpu"}},
            {"tool": "niagara_query",
             "args": {"action": "set_system_property",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/NS_BenchBurst",
                      "property": "WarmupTime", "value": "0.2"}},
            {"tool": "niagara_query",
             "args": {"action": "set_emitter_loop_profile",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/NS_BenchBurst",
                      "emitter": "BenchEmitter",
                      "loop_behavior": "Once", "loop_duration": 0.4}},
            {"tool": "niagara_query",
             "args": {"action": "set_fixed_bounds",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/NS_BenchBurst",
                      "min": [-100.0, -100.0, -50.0],
                      "max": [100.0, 100.0, 250.0]},
             "contains": ["success", "system"]},
            {"tool": "niagara_query",
             "args": {"action": "save_system",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/NS_BenchBurst"}},
        ],
        "verify": [
            {"tool": "niagara_query",
             "args": {"action": "list_emitters",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/NS_BenchBurst"},
             "contains": ["BenchEmitter"]},
            {"tool": "niagara_query",
             "args": {"action": "get_system_summary",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/NS_BenchBurst"},
             "contains": ["BenchEmitter", "fixed_bounds", "true"]},
            {"tool": "niagara_query",
             "args": {"action": "get_emitter_timing_summary",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/NS_BenchBurst",
                      "emitter": "BenchEmitter"},
             "contains": ["BenchEmitter", "0.4"]},
            {"tool": "niagara_query",
             "args": {"action": "get_system_property",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/NS_BenchBurst",
                      "property": "WarmupTime"},
             "contains": ["0.2"]},
            {"tool": "niagara_query",
             "args": {"action": "validate_system",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/NS_BenchBurst"}},
        ],
    },
    {
        "name": "niagara_sidecar_assets_roundtrip",
        "domain": "niagara",
        "edit_domain": "niagara_sidecar_assets",
        "description": "Create Niagara Parameter Collection and EffectType sidecar assets, edit defaults/scalability, save, and read them back",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/NPC_BenchGlobals",
                                      f"{ASSET_AUTHORING_ROOT}/NE_BenchBudget"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "niagara_query",
             "args": {"action": "create_npc",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/NPC_BenchGlobals",
                      "namespace": "BenchGlobals"},
             "contains": ["NPC_BenchGlobals", "BenchGlobals"]},
            {"tool": "niagara_query",
             "args": {"action": "add_npc_parameter",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/NPC_BenchGlobals",
                      "name": "BenchSpawnScale",
                      "type": "float"},
             "contains": ["BenchSpawnScale", "Float"]},
            {"tool": "niagara_query",
             "args": {"action": "set_npc_default",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/NPC_BenchGlobals",
                      "name": "BenchSpawnScale",
                      "value": 0.85},
             "contains": ["BenchSpawnScale"]},
            {"tool": "niagara_query",
             "args": {"action": "create_effect_type",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/NE_BenchBudget",
                      "cull_reaction": "DeactivateImmediate",
                      "update_frequency": "Low",
                      "max_distance": 2500.0},
             "contains": ["NE_BenchBudget"]},
            {"tool": "niagara_query",
             "args": {"action": "set_effect_type_property",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/NE_BenchBudget",
                      "property": "CullReaction",
                      "value": "PauseResume"},
             "contains": ["CullReaction"]},
            {"tool": "niagara_query",
             "args": {"action": "save_system",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/NPC_BenchGlobals"}},
            {"tool": "niagara_query",
             "args": {"action": "save_system",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/NE_BenchBudget"}},
        ],
        "verify": [
            {"tool": "niagara_query",
             "args": {"action": "get_npc",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/NPC_BenchGlobals"},
             "contains": ["BenchGlobals", "BenchSpawnScale", "0.85"]},
            {"tool": "niagara_query",
             "args": {"action": "get_effect_type",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/NE_BenchBudget"},
             "contains": ["CullReaction", "PauseResume"]},
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/NE_BenchBudget"},
            "contains": ["NE_BenchBudget", "NiagaraEffectType"]},
        ],
    },
    {
        "name": "niagara_hlsl_module_roundtrip",
        "domain": "niagara",
        "edit_domain": "niagara_module_script",
        "description": "Create a Niagara module script from HLSL, attach it to a new emitter stack, save, and inspect module graph/order",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/NS_BenchModuleStack",
                                      f"{ASSET_AUTHORING_ROOT}/NM_BenchScaleFloat"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "niagara_query",
             "args": {"action": "create_module_from_hlsl",
                      "name": "NM_BenchScaleFloat",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/NM_BenchScaleFloat",
                      "hlsl": "OutValue = InValue * Scale;",
                      "inputs": [{"name": "InValue", "type": "float"},
                                  {"name": "Scale", "type": "float"}],
                      "outputs": [{"name": "OutValue", "type": "float"}],
                      "description": "Benchmark scalar multiply module"},
             "contains": ["NM_BenchScaleFloat", "InValue", "OutValue"]},
            {"tool": "niagara_query",
             "args": {"action": "create_system",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/NS_BenchModuleStack"},
             "contains": ["NS_BenchModuleStack"]},
            {"tool": "niagara_query",
             "args": {"action": "create_emitter",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/NS_BenchModuleStack",
                      "name": "BenchEmitter", "sim_target": "cpu"},
             "contains": ["BenchEmitter"]},
            {"tool": "niagara_query",
             "args": {"action": "request_compile",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/NS_BenchModuleStack",
                      "force": True,
                      "synchronous": True},
             "contains": ["NS_BenchModuleStack"]},
            {"tool": "niagara_query",
             "args": {"action": "add_module",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/NS_BenchModuleStack",
                      "emitter": "BenchEmitter",
                      "usage": "particle_update",
                      "module_script": f"{ASSET_AUTHORING_ROOT}/NM_BenchScaleFloat.NM_BenchScaleFloat"},
             "contains": ["node_guid"]},
            {"tool": "niagara_query",
             "args": {"action": "save_system",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/NS_BenchModuleStack"}},
        ],
        "verify": [
            {"tool": "niagara_query",
             "args": {"action": "get_ordered_modules",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/NS_BenchModuleStack",
                      "emitter": "BenchEmitter"},
             "contains": ["NM_BenchScaleFloat", "particle_update"]},
            {"tool": "niagara_query",
             "args": {"action": "get_module_graph",
                      "script_path": f"{ASSET_AUTHORING_ROOT}/NM_BenchScaleFloat"},
             "contains": ["InValue", "Scale", "OutValue"]},
            {"tool": "niagara_query",
             "args": {"action": "get_system_summary",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/NS_BenchModuleStack"},
             "contains": ["BenchEmitter"]},
        ],
    },
    {
        "name": "ai_blackboard_behavior_tree_roundtrip",
        "domain": "ai",
        "edit_domain": "behavior_tree",
        "description": "Create a Blackboard and BehaviorTree, edit Blackboard keys, link the tree to the Blackboard, save, and inspect both",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/BB_BenchDecision",
                                      f"{ASSET_AUTHORING_ROOT}/BT_BenchDecision"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "ai_query",
             "args": {"action": "create_blackboard",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/BB_BenchDecision"}},
            {"tool": "ai_query",
             "args": {"action": "add_bb_key",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BB_BenchDecision",
                      "key_name": "TargetActor", "key_type": "Object",
                      "base_class": "Actor"}},
            {"tool": "ai_query",
             "args": {"action": "add_bb_key",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BB_BenchDecision",
                      "key_name": "PatrolIndex", "key_type": "Int"}},
            {"tool": "ai_query",
             "args": {"action": "rename_bb_key",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BB_BenchDecision",
                      "old_name": "PatrolIndex", "new_name": "PatrolPointIndex"}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BB_BenchDecision"}},
            {"tool": "ai_query",
             "args": {"action": "create_behavior_tree",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/BT_BenchDecision",
                      "blackboard_path": f"{ASSET_AUTHORING_ROOT}/BB_BenchDecision"}},
            {"tool": "ai_query",
             "args": {"action": "set_bt_blackboard",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BT_BenchDecision",
                      "blackboard_path": f"{ASSET_AUTHORING_ROOT}/BB_BenchDecision"}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BB_BenchDecision"}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BT_BenchDecision"}},
        ],
        "verify": [
            {"tool": "ai_query",
             "args": {"action": "get_blackboard",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BB_BenchDecision"},
             "contains": ["TargetActor", "PatrolPointIndex"]},
            {"tool": "ai_query",
             "args": {"action": "get_behavior_tree",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BT_BenchDecision"},
             "contains": ["BB_BenchDecision"]},
        ],
    },
    {
        "name": "ai_eqs_query_roundtrip",
        "domain": "ai",
        "edit_domain": "eqs_query",
        "description": "Create an EQS query, add a generator and scored/filtering test, save it, and inspect the query options",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/EQS_BenchCover"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "ai_query",
             "args": {"action": "create_eqs_query",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/EQS_BenchCover"},
             "contains": ["EQS_BenchCover"]},
            {"tool": "ai_query",
             "args": {"action": "add_eqs_generator",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/EQS_BenchCover",
                      "generator_class": "EnvQueryGenerator_SimpleGrid",
                      "properties": {"GridSize": 400.0, "SpaceBetween": 100.0}},
             "contains": ["EnvQueryGenerator_SimpleGrid", "option_index"]},
            {"tool": "ai_query",
             "args": {"action": "add_eqs_test",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/EQS_BenchCover",
                      "option_index": 0,
                      "test_class": "EnvQueryTest_Distance"},
             "contains": ["EnvQueryTest_Distance", "test_index"]},
            {"tool": "ai_query",
             "args": {"action": "configure_eqs_scoring",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/EQS_BenchCover",
                      "option_index": 0,
                      "test_index": 0,
                      "purpose": "FilterAndScore",
                      "equation": "InverseLinear",
                      "factor": 2.0},
             "contains": ["InverseLinear", "scoring_factor", "2"]},
            {"tool": "ai_query",
             "args": {"action": "configure_eqs_filter",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/EQS_BenchCover",
                      "option_index": 0,
                      "test_index": 0,
                      "filter_type": "Range",
                      "min": 100.0,
                      "max": 1200.0},
             "contains": ["Range", "float_value_min", "100", "float_value_max", "1200"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/EQS_BenchCover"}},
        ],
        "verify": [
            {"tool": "ai_query",
             "args": {"action": "get_eqs_query",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/EQS_BenchCover"},
             "contains": ["EnvQueryGenerator_SimpleGrid", "EnvQueryTest_Distance", "InverseLinear", "1200"]},
            {"tool": "ai_query",
             "args": {"action": "validate_eqs_query",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/EQS_BenchCover"}},
        ],
    },
    {
        "name": "ai_behavior_tree_spec_roundtrip",
        "domain": "ai",
        "edit_domain": "behavior_tree_spec",
        "description": "Build a BehaviorTree from a declarative spec, save it, and inspect graph/task layout",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/BT_BenchSpecPatrol"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "ai_query",
             "args": {"action": "build_behavior_tree_from_spec",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/BT_BenchSpecPatrol",
                      "strict_mode": True,
                      "spec": {
                          "root": {
                              "type": "Selector",
                              "name": "BenchRootSelector",
                              "children": [
                                  {"type": "BTTask_Wait",
                                   "name": "BenchWait",
                                   "properties": {"WaitTime": 0.1}},
                              ],
                          },
                      }},
             "contains": ["BT_BenchSpecPatrol", "node_count"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BT_BenchSpecPatrol"}},
        ],
        "verify": [
            {"tool": "ai_query",
             "args": {"action": "get_behavior_tree",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BT_BenchSpecPatrol"},
             "contains": ["Selector", "BTTask_Wait", "BenchWait"]},
            {"tool": "ai_query",
             "args": {"action": "get_bt_graph",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BT_BenchSpecPatrol"},
             "contains": ["Selector", "BTTask_Wait"]},
            {"tool": "ai_query",
             "args": {"action": "validate_behavior_tree",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BT_BenchSpecPatrol"}},
        ],
    },
    {
        "name": "ai_behavior_tree_node_blueprints_roundtrip",
        "domain": "ai",
        "edit_domain": "behavior_tree_node_blueprints",
        "description": "Create BehaviorTree Task/Decorator/Service Blueprint assets, save them, and verify their parent classes",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/BTT_BenchScan",
                                      f"{ASSET_AUTHORING_ROOT}/BTD_BenchCanSee",
                                      f"{ASSET_AUTHORING_ROOT}/BTS_BenchSense"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "ai_query",
             "args": {"action": "create_bt_task_blueprint",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/BTT_BenchScan",
                      "name": "BTT_BenchScan"},
             "contains": ["BTT_BenchScan", "BTTask_BlueprintBase"]},
            {"tool": "ai_query",
             "args": {"action": "create_bt_decorator_blueprint",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/BTD_BenchCanSee",
                      "name": "BTD_BenchCanSee"},
             "contains": ["BTD_BenchCanSee", "BTDecorator_BlueprintBase"]},
            {"tool": "ai_query",
             "args": {"action": "create_bt_service_blueprint",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/BTS_BenchSense",
                      "name": "BTS_BenchSense"},
             "contains": ["BTS_BenchSense", "BTService_BlueprintBase"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BTT_BenchScan"}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BTD_BenchCanSee"}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BTS_BenchSense"}},
        ],
        "verify": [
            {"tool": "blueprint_query",
             "args": {"action": "get_parent_class",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BTT_BenchScan"},
             "contains": ["BTTask_BlueprintBase"]},
            {"tool": "blueprint_query",
             "args": {"action": "get_parent_class",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BTD_BenchCanSee"},
             "contains": ["BTDecorator_BlueprintBase"]},
            {"tool": "blueprint_query",
             "args": {"action": "get_parent_class",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BTS_BenchSense"},
             "contains": ["BTService_BlueprintBase"]},
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BTT_BenchScan"},
             "contains": ["BTT_BenchScan", "Blueprint"]},
        ],
    },
    {
        "name": "gas_gameplay_effect_roundtrip",
        "domain": "gas",
        "edit_domain": "gameplay_effect",
        "description": "Create a GameplayEffect, edit duration policy/magnitude through GAS persistence, validate, and read the effect back",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/GE_BenchSlow"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "gas_query",
             "args": {"action": "create_gameplay_effect",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchSlow",
                      "duration_policy": "has_duration"}},
            {"tool": "gas_query",
             "args": {"action": "set_duration",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchSlow",
                      "duration_policy": "has_duration",
                      "duration_magnitude": 4.0}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchSlow"}},
        ],
        "verify": [
            {"tool": "gas_query",
             "args": {"action": "get_gameplay_effect",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchSlow"},
             "contains": ["has_duration", "4"]},
            {"tool": "gas_query",
             "args": {"action": "validate_effect",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchSlow"}},
        ],
    },
    {
        "name": "gas_ability_blueprint_roundtrip",
        "domain": "gas",
        "edit_domain": "gameplay_ability",
        "description": "Create a GameplayAbility Blueprint, bind cooldown GE, edit policies/flags, scaffold commit flow, save, and inspect",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/GE_BenchDashCooldown",
                                      f"{ASSET_AUTHORING_ROOT}/GA_BenchDash"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "gas_query",
             "args": {"action": "create_gameplay_effect",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchDashCooldown",
                      "duration_policy": "has_duration"},
             "contains": ["GE_BenchDashCooldown"]},
            {"tool": "gas_query",
             "args": {"action": "set_duration",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchDashCooldown",
                      "duration_policy": "has_duration",
                      "duration_magnitude": 1.25},
             "contains": ["1.25"]},
            {"tool": "gas_query",
             "args": {"action": "add_ge_component",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchDashCooldown",
                      "component_type": "target_tags",
                      "config": {"tags": ["Cooldown.Skill.001"]}},
             "contains": ["target_tags", "component_count"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchDashCooldown"}},
            {"tool": "gas_query",
             "args": {"action": "create_ability",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/GA_BenchDash",
                      "parent_class": "GameplayAbility",
                      "display_name": "Bench Dash"},
             "contains": ["GA_BenchDash", "GameplayAbility", "Bench Dash"]},
            {"tool": "gas_query",
             "args": {"action": "set_ability_policy",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GA_BenchDash",
                      "instancing_policy": "InstancedPerActor",
                      "net_execution_policy": "LocalPredicted",
                      "net_security_policy": "ClientOrServer"},
             "contains": ["InstancedPerActor", "LocalPredicted", "ClientOrServer"]},
            {"tool": "gas_query",
             "args": {"action": "set_ability_cooldown",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GA_BenchDash",
                      "cooldown_effect_class": f"{ASSET_AUTHORING_ROOT}/GE_BenchDashCooldown"},
             "contains": ["GE_BenchDashCooldown"]},
            {"tool": "gas_query",
             "args": {"action": "add_commit_and_end_flow",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GA_BenchDash",
                      "position": {"x": 240, "y": 0}},
             "contains": ["CommitAbility", "EndAbility"]},
            {"tool": "gas_query",
             "args": {"action": "compile_ability",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GA_BenchDash"},
             "contains": ["success", "error_count", "0"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GA_BenchDash"}},
        ],
        "verify": [
            {"tool": "gas_query",
             "args": {"action": "get_ability_info",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GA_BenchDash"},
             "contains": ["InstancedPerActor", "LocalPredicted", "GE_BenchDashCooldown"]},
            {"tool": "gas_query",
             "args": {"action": "get_ability_graph_flow",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GA_BenchDash"},
             "contains": ["instant", "has_commit_ability", "has_end_ability"]},
            {"tool": "gas_query",
             "args": {"action": "validate_ability_blueprint",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GA_BenchDash"}},
        ],
    },
    {
        "name": "gas_input_binding_roundtrip",
        "domain": "gas",
        "edit_domain": "ability_input_binding",
        "description": "Create an ASC actor, GameplayAbility, and Enhanced Input action, bind them through GAS input metadata, save, and read back binding state",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/BP_BenchGASInputActor",
                                      f"{ASSET_AUTHORING_ROOT}/GA_BenchInputDash",
                                      f"{ASSET_AUTHORING_ROOT}/IA_BenchDashInput"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "blueprint_query",
             "args": {"action": "create_blueprint",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/BP_BenchGASInputActor",
                      "parent_class": "Actor",
                      "blueprint_type": "Normal"},
             "contains": ["BP_BenchGASInputActor", "Actor"]},
            {"tool": "gas_query",
             "args": {"action": "add_asc_to_actor",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BP_BenchGASInputActor",
                      "asc_class": "AbilitySystemComponent",
                      "location": "self"},
             "contains": ["AbilitySystemComponent", "self"]},
            {"tool": "gas_query",
             "args": {"action": "create_ability",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/GA_BenchInputDash",
                      "parent_class": "GameplayAbility",
                      "display_name": "Bench Input Dash"},
             "contains": ["GA_BenchInputDash", "GameplayAbility", "Bench Input Dash"]},
            {"tool": "input_query",
             "args": {"action": "create_input_action",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/IA_BenchDashInput",
                      "value_type": "Boolean",
                      "description": "Bench dash input",
                      "overwrite": True,
                      "save": True},
             "contains": ["IA_BenchDashInput", "Boolean"]},
            {"tool": "gas_query",
             "args": {"action": "setup_ability_input_binding",
                      "actor_path": f"{ASSET_AUTHORING_ROOT}/BP_BenchGASInputActor",
                      "binding_mode": "enhanced_input"},
             "contains": ["enhanced_input", "has_asc", "true"]},
            {"tool": "gas_query",
             "args": {"action": "bind_ability_to_input",
                      "actor_path": f"{ASSET_AUTHORING_ROOT}/BP_BenchGASInputActor",
                      "ability_class": f"{ASSET_AUTHORING_ROOT}/GA_BenchInputDash",
                      "input_action": f"{ASSET_AUTHORING_ROOT}/IA_BenchDashInput",
                      "trigger_event": "triggered"},
             "contains": ["GA_BenchInputDash", "IA_BenchDashInput", "triggered", "enhanced_input"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BP_BenchGASInputActor"}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GA_BenchInputDash"}},
        ],
        "verify": [
            {"tool": "gas_query",
             "args": {"action": "get_ability_input_bindings",
                      "actor_path": f"{ASSET_AUTHORING_ROOT}/BP_BenchGASInputActor"},
             "contains": ["has_asc", "true", "AbilitySystemComponent"]},
            {"tool": "blueprint_query",
             "args": {"action": "get_components",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BP_BenchGASInputActor"},
             "contains": ["AbilitySystemComponent"]},
            {"tool": "gas_query",
             "args": {"action": "get_ability_info",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GA_BenchInputDash"},
             "contains": ["GA_BenchInputDash", "GameplayAbility"]},
            {"tool": "input_query",
             "args": {"action": "get_input_action",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/IA_BenchDashInput"},
             "contains": ["IA_BenchDashInput", "Boolean"]},
        ],
    },
    {
        "name": "gas_gameplay_cue_notify_roundtrip",
        "domain": "gas",
        "edit_domain": "gameplay_cue_notify",
        "description": "Create a looping GameplayCue Notify Blueprint, edit cue actor auto-destroy behavior, save it, and inspect cue metadata",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/GCN_BenchImpact"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "gas_query",
             "args": {"action": "create_gameplay_cue_notify",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/GCN_BenchImpact",
                      "cue_tag": "GameplayCue.Skill.001",
                      "type": "looping"},
             "contains": ["GCN_BenchImpact", "GameplayCue.Skill.001", "looping"]},
            {"tool": "blueprint_query",
             "args": {"action": "set_cdo_property",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GCN_BenchImpact",
                      "property_name": "bAutoDestroyOnRemove",
                      "value": True},
             "contains": ["bAutoDestroyOnRemove"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GCN_BenchImpact"}},
        ],
        "verify": [
            {"tool": "gas_query",
             "args": {"action": "get_cue_info",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GCN_BenchImpact"},
             "contains": ["GCN_BenchImpact", "looping", "GameplayCueNotify_Actor",
                          "GameplayCue.Skill.001", "bAutoDestroyOnRemove", "true"]},
            {"tool": "gas_query",
             "args": {"action": "list_gameplay_cues",
                      "type_filter": "looping"},
             "contains": ["GCN_BenchImpact"]},
        ],
    },
    {
        "name": "chooser_table_roundtrip",
        "domain": "animation",
        "edit_domain": "chooser_table",
        "description": "Create a Chooser Table, add a bool predicate column, select a Texture2D result row, edit the cell, save, and inspect rows/cells",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/CHT_BenchTextureChoice",
                                      f"{ASSET_AUTHORING_ROOT}/T_BenchChooserResult"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "asset_query",
             "args": {"action": "import_texture_from_bytes",
                      "destination": f"{ASSET_AUTHORING_ROOT}/T_BenchChooserResult",
                      "bytes_b64": ASSET_AUTHORING_BMP_2X2_B64,
                      "format_hint": "bmp",
                      "texture_role": "ui_icon",
                      "overwrite": True,
                      "save": True},
             "contains": ["T_BenchChooserResult"]},
            {"tool": "chooser_query",
             "args": {"action": "create_chooser_table",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/CHT_BenchTextureChoice",
                      "output_type": "ObjectResult",
                      "output_class": "/Script/Engine.Texture2D",
                      "context_class": "/Script/CoreUObject.Object"},
             "contains": ["CHT_BenchTextureChoice", "ObjectResult", "Texture2D"]},
            {"tool": "chooser_query",
             "args": {"action": "add_chooser_column",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/CHT_BenchTextureChoice",
                      "column_kind": "Bool"},
             "contains": ["column_count", "Bool"]},
            {"tool": "chooser_query",
             "args": {"action": "add_chooser_row",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/CHT_BenchTextureChoice",
                      "cells": [True],
                      "output_psd": f"{ASSET_AUTHORING_ROOT}/T_BenchChooserResult"},
             "contains": ["row_count", "T_BenchChooserResult"]},
            {"tool": "chooser_query",
             "args": {"action": "set_chooser_cell",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/CHT_BenchTextureChoice",
                      "column_index": 0,
                      "row_index": 0,
                      "bool_value": False},
             "contains": ["column_index", "row_index"]},
            {"tool": "chooser_query",
             "args": {"action": "validate_chooser",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/CHT_BenchTextureChoice",
                      "expected_result_type": "ObjectResult"},
             "contains": ["valid", "true", "ObjectResult"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/CHT_BenchTextureChoice"},
             "contains": ["saved", "CHT_BenchTextureChoice"]},
        ],
        "verify": [
            {"tool": "chooser_query",
             "args": {"action": "inspect_chooser",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/CHT_BenchTextureChoice",
                      "include_cells": True,
                      "recursive": True},
             "contains": ["CHT_BenchTextureChoice", "ObjectResult", "BoolColumn",
                          "T_BenchChooserResult", "false", "compiled"]},
        ],
    },
    {
        "name": "gas_target_actor_roundtrip",
        "domain": "gas",
        "edit_domain": "target_actor",
        "description": "Create a GameplayAbilityTargetActor Blueprint, edit radius/range server targeting flags, save, and inspect parent class",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/TA_BenchSphere"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "gas_query",
             "args": {"action": "create_target_actor",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/TA_BenchSphere",
                      "targeting_type": "sphere",
                      "radius": 200.0,
                      "max_range": 1200.0},
             "contains": ["TA_BenchSphere", "sphere", "GameplayAbilityTargetActor_Radius"]},
            {"tool": "gas_query",
             "args": {"action": "configure_target_actor",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/TA_BenchSphere",
                      "radius": 275.0,
                      "max_range": 1500.0,
                      "should_produce_target_data_on_server": True,
                      "debug_draw": True},
             "contains": ["Radius = 275"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/TA_BenchSphere"}},
        ],
        "verify": [
            {"tool": "blueprint_query",
             "args": {"action": "get_parent_class",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/TA_BenchSphere"},
             "contains": ["GameplayAbilityTargetActor_Radius"]},
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/TA_BenchSphere"},
             "contains": ["TA_BenchSphere", "Blueprint"]},
        ],
    },
    {
        "name": "material_instance_parameter_overrides_roundtrip",
        "domain": "material",
        "edit_domain": "material_instance_parameters",
        "description": "Create a parameterized Material and MIC, batch-set scalar/vector overrides, save, and read the overrides back",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/M_BenchParamBase",
                                      f"{ASSET_AUTHORING_ROOT}/MI_BenchParamOverrides"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "material_query",
             "args": {"action": "create_material",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/M_BenchParamBase",
                      "blend_mode": "Opaque",
                      "shading_model": "DefaultLit",
                      "material_domain": "Surface"},
             "contains": ["M_BenchParamBase"]},
            {"tool": "material_query",
             "args": {"action": "build_material_graph",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/M_BenchParamBase",
                      "clear_existing": True,
                      "graph_spec": {
                          "nodes": [
                              {"id": "BenchTint", "class": "VectorParameter",
                               "props": {"ParameterName": "BenchTint"},
                               "pos": [-360, -80]},
                              {"id": "BenchRoughness", "class": "ScalarParameter",
                               "props": {"ParameterName": "BenchRoughness"},
                               "pos": [-360, 120]},
                          ],
                          "outputs": [
                              {"from": "BenchTint", "from_pin": "RGB", "to_property": "BaseColor"},
                              {"from": "BenchRoughness", "to_property": "Roughness"},
                          ],
                      }},
             "contains": ["M_BenchParamBase"]},
            {"tool": "material_query",
             "args": {"action": "create_material_instance",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/MI_BenchParamOverrides",
                      "parent_material": f"{ASSET_AUTHORING_ROOT}/M_BenchParamBase"},
             "contains": ["MI_BenchParamOverrides"]},
            {"tool": "material_query",
             "args": {"action": "set_instance_parameters",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/MI_BenchParamOverrides",
                      "parameters": [
                          {"name": "BenchRoughness", "type": "scalar", "value": 0.42},
                          {"name": "BenchTint", "type": "vector",
                           "value": {"R": 0.1, "G": 0.7, "B": 0.3, "A": 1.0}},
                      ]},
             "contains": ["BenchRoughness", "BenchTint"]},
            {"tool": "material_query",
             "args": {"action": "save_material",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/M_BenchParamBase"}},
            {"tool": "material_query",
             "args": {"action": "save_material",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/MI_BenchParamOverrides"}},
        ],
        "verify": [
            {"tool": "material_query",
             "args": {"action": "get_instance_parameters",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/MI_BenchParamOverrides"},
             "contains": ["BenchRoughness", "BenchTint", "is_overridden"]},
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/MI_BenchParamOverrides"},
             "contains": ["MI_BenchParamOverrides", "MaterialInstance"]},
        ],
    },
    {
        "name": "commonui_input_action_datatable_roundtrip",
        "domain": "ui",
        "edit_domain": "commonui_input_action_datatable",
        "description": "Create a CommonUI input-action DataTable, add keyboard/gamepad glyph rows, save, and read the rows back",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/DT_BenchCommonActions"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "ui_query",
             "args": {"action": "create_input_action_data_table",
                      "package_path": ASSET_AUTHORING_ROOT,
                      "asset_name": "DT_BenchCommonActions"},
             "contains": ["DT_BenchCommonActions"]},
            {"tool": "ui_query",
             "args": {"action": "add_input_action_row",
                      "table_path": f"{ASSET_AUTHORING_ROOT}/DT_BenchCommonActions",
                      "row_name": "BenchAccept",
                      "display_name": "Bench Accept",
                      "hold_display_name": "Hold Bench Accept",
                      "nav_bar_priority": 10,
                      "keyboard_key": "(Key=(KeyName=E))",
                      "gamepad_key": "(Key=(KeyName=Gamepad_FaceButton_Bottom))"},
             "contains": ["BenchAccept"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/DT_BenchCommonActions"}},
        ],
        "verify": [
            {"tool": "blueprint_query",
             "args": {"action": "get_data_table_rows",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/DT_BenchCommonActions"},
             "contains": ["BenchAccept", "Bench Accept", "DefaultGamepadInputTypeInfo"]},
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/DT_BenchCommonActions"},
             "contains": ["DT_BenchCommonActions", "DataTable"]},
        ],
    },
    {
        "name": "gas_effect_template_duplicate_roundtrip",
        "domain": "gas",
        "edit_domain": "gameplay_effect_template",
        "description": "Create a GameplayEffect from a template, edit stacking/period settings, duplicate it with overrides, save, and validate both effects",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/GE_BenchCooldownTemplate",
                                      f"{ASSET_AUTHORING_ROOT}/GE_BenchCooldownClone"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "gas_query",
             "args": {"action": "create_effect_from_template",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchCooldownTemplate",
                      "template": "cooldown",
                      "overrides": {"duration": 2.5,
                                    "tags": {"target_tags": ["Cooldown.Bench.Template"]}}},
             "contains": ["GE_BenchCooldownTemplate", "cooldown"]},
            {"tool": "gas_query",
             "args": {"action": "set_effect_stacking",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchCooldownTemplate",
                      "stacking_type": "aggregate_by_target",
                      "stack_limit": 2,
                      "stack_duration_refresh_policy": "RefreshOnSuccessfulApplication",
                      "stack_period_reset_policy": "ResetOnSuccessfulApplication",
                      "stack_expiration_policy": "ClearEntireStack"},
             "contains": ["aggregate_by_target", "2"]},
            {"tool": "gas_query",
             "args": {"action": "set_period",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchCooldownTemplate",
                      "period": 0.5,
                      "execute_on_application": True},
             "contains": ["0.5"]},
            {"tool": "gas_query",
             "args": {"action": "duplicate_gameplay_effect",
                      "source_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchCooldownTemplate",
                      "dest_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchCooldownClone",
                      "overrides": {"duration_policy": "has_duration"}},
             "contains": ["GE_BenchCooldownClone"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchCooldownTemplate"}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchCooldownClone"}},
        ],
        "verify": [
            {"tool": "gas_query",
             "args": {"action": "get_gameplay_effect",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchCooldownTemplate"},
             "contains": ["GE_BenchCooldownTemplate", "has_duration", "0.5"]},
            {"tool": "gas_query",
             "args": {"action": "get_gameplay_effect",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchCooldownClone"},
             "contains": ["GE_BenchCooldownClone", "has_duration"]},
            {"tool": "gas_query",
             "args": {"action": "validate_effect",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchCooldownTemplate"}},
        ],
    },
    {
        "name": "ai_controller_perception_roundtrip",
        "domain": "ai",
        "edit_domain": "ai_controller_perception",
        "description": "Create Blackboard/BehaviorTree/AIController assets, set controller team/flags, add sight perception, save, and inspect controller config",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/BB_BenchController",
                                      f"{ASSET_AUTHORING_ROOT}/BT_BenchController",
                                      f"{ASSET_AUTHORING_ROOT}/AIC_BenchSight"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "ai_query",
             "args": {"action": "create_blackboard",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/BB_BenchController"},
             "contains": ["BB_BenchController"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BB_BenchController"}},
            {"tool": "ai_query",
             "args": {"action": "add_bb_key",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BB_BenchController",
                      "key_name": "TargetActor", "key_type": "Object",
                      "base_class": "Actor"},
             "contains": ["TargetActor"]},
            {"tool": "ai_query",
             "args": {"action": "create_behavior_tree",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/BT_BenchController",
                      "blackboard_path": f"{ASSET_AUTHORING_ROOT}/BB_BenchController"},
             "contains": ["BT_BenchController"]},
            {"tool": "ai_query",
             "args": {"action": "create_ai_controller",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/AIC_BenchSight",
                      "bt_path": f"{ASSET_AUTHORING_ROOT}/BT_BenchController",
                      "bb_path": f"{ASSET_AUTHORING_ROOT}/BB_BenchController"},
             "contains": ["AIC_BenchSight"]},
            {"tool": "ai_query",
             "args": {"action": "set_ai_team",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/AIC_BenchSight",
                      "team_id": 7},
             "contains": ["7"]},
            {"tool": "ai_query",
             "args": {"action": "set_ai_controller_flags",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/AIC_BenchSight",
                      "wants_player_state": True,
                      "start_ai_on_possess": True,
                      "allow_strafe": True},
             "contains": ["AIC_BenchSight"]},
            {"tool": "ai_query",
             "args": {"action": "add_perception_component",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/AIC_BenchSight",
                      "dominant_sense": "Sight"},
             "contains": ["AIPerceptionComponent"]},
            {"tool": "ai_query",
             "args": {"action": "configure_sight_sense",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/AIC_BenchSight",
                      "radius": 1800.0,
                      "lose_radius": 2200.0,
                      "peripheral_angle": 70.0,
                      "affiliation": {"enemies": True, "neutrals": True, "friendlies": False},
                      "max_age": 3.0},
             "contains": ["Sight", "1800"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/AIC_BenchSight"}},
        ],
        "verify": [
            {"tool": "ai_query",
             "args": {"action": "get_ai_controller",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/AIC_BenchSight"},
             "contains": ["AIC_BenchSight", "AIController", "allow_strafe", "true"]},
            {"tool": "ai_query",
             "args": {"action": "get_perception_config",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/AIC_BenchSight"},
             "contains": ["Sight", "1800", "70"]},
            {"tool": "ai_query",
             "args": {"action": "validate_ai_controller",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/AIC_BenchSight"}},
        ],
    },
    {
        "name": "hlod_layer_config_roundtrip",
        "domain": "hlod",
        "edit_domain": "hlod_layer",
        "description": "Create a World Partition HLODLayer asset, reconfigure cell/loading settings, save, and inspect the reflected HLOD layer",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/HLODLayer_BenchWorld"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "hlod_query",
             "args": {"action": "create_hlod_layer",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/HLODLayer_BenchWorld",
                      "layer_type": "MeshSimplify",
                      "cell_size": 12000,
                      "loading_range": 1.5},
             "contains": ["HLODLayer_BenchWorld"]},
            {"tool": "hlod_query",
             "args": {"action": "configure_hlod_layer",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/HLODLayer_BenchWorld",
                      "layer_type": "MeshMerge",
                      "cell_size": 24000,
                      "loading_range": 2.5},
             "contains": ["HLODLayer_BenchWorld"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/HLODLayer_BenchWorld"}},
        ],
        "verify": [
            {"tool": "hlod_query",
             "args": {"action": "get_hlod_layer",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/HLODLayer_BenchWorld"},
             "contains": ["HLODLayer_BenchWorld", "HLODLayer"]},
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/HLODLayer_BenchWorld"},
             "contains": ["HLODLayer_BenchWorld"]},
        ],
    },
    {
        "name": "collection_static_membership_roundtrip",
        "domain": "collection",
        "edit_domain": "content_browser_collection",
        "description": "Create a local Content Browser collection, add a generated Texture2D member, color it, and verify membership",
        "chain": [
            {"tool": "collection_query", "allow_error": True,
             "args": {"action": "delete_collection",
                      "name": "BenchAssetAuthoringCollection",
                      "share_type": "local",
                      "force": True}},
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/T_BenchCollectionIcon"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "asset_query",
             "args": {"action": "import_texture_from_bytes",
                      "destination": f"{ASSET_AUTHORING_ROOT}/T_BenchCollectionIcon",
                      "bytes_b64": ASSET_AUTHORING_BMP_2X2_B64,
                      "format_hint": "bmp",
                      "texture_role": "ui_icon",
                      "save": True},
             "contains": ["T_BenchCollectionIcon"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/T_BenchCollectionIcon"}},
            {"tool": "collection_query",
             "args": {"action": "create_collection",
                      "name": "BenchAssetAuthoringCollection",
                      "share_type": "local",
                      "storage_mode": "static"},
             "contains": ["BenchAssetAuthoringCollection"]},
            {"tool": "collection_query",
             "args": {"action": "add_assets",
                      "name": "BenchAssetAuthoringCollection",
                      "share_type": "local",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/T_BenchCollectionIcon"]},
             "contains": ["added", "1"]},
            {"tool": "collection_query",
             "args": {"action": "set_collection_color",
                      "name": "BenchAssetAuthoringCollection",
                      "share_type": "local",
                      "color": {"r": 0.1, "g": 0.45, "b": 0.9, "a": 1.0}},
             "contains": ["BenchAssetAuthoringCollection"]},
        ],
        "verify": [
            {"tool": "collection_query",
             "args": {"action": "contains_asset",
                      "name": "BenchAssetAuthoringCollection",
                      "share_type": "local",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/T_BenchCollectionIcon"},
             "contains": ["true"]},
            {"tool": "collection_query",
             "args": {"action": "list_assets",
                      "name": "BenchAssetAuthoringCollection",
                      "share_type": "local"},
             "contains": ["T_BenchCollectionIcon"]},
            {"tool": "collection_query",
             "args": {"action": "get_collection",
                      "name": "BenchAssetAuthoringCollection",
                      "share_type": "local"},
            "contains": ["BenchAssetAuthoringCollection"]},
        ],
    },
    {
        "name": "ui_animation_v2_roundtrip",
        "domain": "ui",
        "edit_domain": "widget_animation_v2",
        "description": "Create a Widget Blueprint, add a TextBlock, author a multi-key UWidgetAnimation, save, and read animation tracks back",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/WBP_BenchAnimatedPanel"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "ui_query",
             "args": {"action": "create_widget_blueprint",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/WBP_BenchAnimatedPanel",
                      "root_widget": "CanvasPanel"},
             "contains": ["WBP_BenchAnimatedPanel"]},
            {"tool": "ui_query",
             "args": {"action": "add_widget",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/WBP_BenchAnimatedPanel",
                      "widget_class": "TextBlock",
                      "widget_name": "BenchAnimatedLabel",
                      "anchor_preset": "center",
                      "position": {"x": 0, "y": 0},
                      "size": {"x": 360, "y": 72},
                      "compile": False},
             "contains": ["BenchAnimatedLabel"]},
            {"tool": "ui_query",
             "args": {"action": "set_text",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/WBP_BenchAnimatedPanel",
                      "widget_name": "BenchAnimatedLabel",
                      "text": "Animated Benchmark",
                      "font_size": 26,
                      "compile": False}},
            {"tool": "ui_query",
             "args": {"action": "create_animation_v2",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/WBP_BenchAnimatedPanel",
                      "animation_name": "IntroFade",
                      "duration_sec": 0.75,
                      "tracks": [
                          {"widget_name": "BenchAnimatedLabel",
                           "property": "RenderOpacity",
                           "keys": [
                               {"time": 0.0, "value": 0.0, "interp": "linear"},
                               {"time": 0.25, "value": 0.45, "interp": "linear"},
                               {"time": 0.75, "value": 1.0, "interp": "linear"},
                           ]},
                      ],
                      "compile_once": True},
             "contains": ["IntroFade"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/WBP_BenchAnimatedPanel"}},
        ],
        "verify": [
            {"tool": "ui_query",
             "args": {"action": "list_animations",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/WBP_BenchAnimatedPanel"},
             "contains": ["IntroFade"]},
            {"tool": "ui_query",
             "args": {"action": "get_animation_details",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/WBP_BenchAnimatedPanel",
                      "animation_name": "IntroFade"},
             "contains": ["BenchAnimatedLabel", "MovieSceneFloatTrack", "keyframe_count", "3", "0.75"]},
        ],
    },
    {
        "name": "imagegen_external_png_import_roundtrip",
        "domain": "imagegen",
        "edit_domain": "generated_image_import",
        "description": "Import externally generated PNG bytes as a Texture2D through ImageGen provenance, save, and inspect provenance/read-back",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/T_BenchExternalGenerated"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "imagegen_query",
             "args": {"action": "import_generated_image",
                      "bytes_b64": ASSET_AUTHORING_PNG_2X2_B64,
                      "format_hint": "png",
                      "prompt": "benchmark external generated png fixture",
                      "provider": "benchmark-external",
                      "model": "fixture-png-2x2",
                      "destination": f"{ASSET_AUTHORING_ROOT}/T_BenchExternalGenerated",
                      "overwrite_policy": "fail",
                      "texture_role": "ui_icon",
                      "save": True,
                      "save_source_png": False},
             "contains": ["T_BenchExternalGenerated"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/T_BenchExternalGenerated"}},
        ],
        "verify": [
            {"tool": "imagegen_query",
             "args": {"action": "get_generated_asset_provenance",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/T_BenchExternalGenerated"},
             "contains": ["benchmark-external", "fixture-png-2x2"]},
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/T_BenchExternalGenerated"},
             "contains": ["T_BenchExternalGenerated", "Texture2D"]},
        ],
    },
    {
        "name": "worldgen_blockout_blueprint_roundtrip",
        "domain": "worldgen",
        "edit_domain": "blockout_volume_blueprint",
        "description": "Create the reusable WorldGen blockout-volume Blueprint asset, save it, and verify its authoring variables",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/BP_BenchBlockoutVolume"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "worldgen_query",
             "args": {"action": "create_blockout_blueprint",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/BP_BenchBlockoutVolume",
                      "force": True},
             "contains": ["BP_BenchBlockoutVolume"]},
            {"tool": "blueprint_query",
             "args": {"action": "compile_blueprint",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BP_BenchBlockoutVolume"},
             "contains": ["error_count", "0"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BP_BenchBlockoutVolume"}},
        ],
        "verify": [
            {"tool": "blueprint_query",
             "args": {"action": "get_variables",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BP_BenchBlockoutVolume"},
             "contains": ["RoomType", "BlockoutTags", "Density"]},
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BP_BenchBlockoutVolume"},
             "contains": ["BP_BenchBlockoutVolume", "Blueprint"]},
        ],
    },
    {
        "name": "collection_dynamic_query_roundtrip",
        "domain": "collection",
        "edit_domain": "content_browser_dynamic_collection",
        "description": "Create a dynamic Content Browser collection, edit its project asset query, color it, and read the query back",
        "chain": [
            {"tool": "collection_query", "allow_error": True,
             "args": {"action": "delete_collection",
                      "name": "BenchAssetAuthoringDynamicCollection",
                      "share_type": "local",
                      "force": True}},
            {"tool": "collection_query",
             "args": {"action": "create_collection",
                      "name": "BenchAssetAuthoringDynamicCollection",
                      "share_type": "local",
                      "storage_mode": "dynamic"},
             "contains": ["BenchAssetAuthoringDynamicCollection"]},
            {"tool": "collection_query",
             "args": {"action": "set_dynamic_query",
                      "name": "BenchAssetAuthoringDynamicCollection",
                      "share_type": "local",
                      "query_text": "Path:/Game/Benchmarks/AssetAuthoring"},
             "contains": ["BenchAssetAuthoringDynamicCollection"]},
            {"tool": "collection_query",
             "args": {"action": "set_collection_color",
                      "name": "BenchAssetAuthoringDynamicCollection",
                      "share_type": "local",
                      "color": {"r": 0.8, "g": 0.3, "b": 0.15, "a": 1.0}},
             "contains": ["BenchAssetAuthoringDynamicCollection"]},
        ],
        "verify": [
            {"tool": "collection_query",
             "args": {"action": "get_dynamic_query",
                      "name": "BenchAssetAuthoringDynamicCollection",
                      "share_type": "local"},
             "contains": ["BenchAssetAuthoringDynamicCollection", "Path:/Game/Benchmarks/AssetAuthoring"]},
            {"tool": "collection_query",
             "args": {"action": "get_collection",
                      "name": "BenchAssetAuthoringDynamicCollection",
                      "share_type": "local"},
             "contains": ["BenchAssetAuthoringDynamicCollection", "dynamic", "0.800000"]},
        ],
    },
    {
        "name": "blueprint_spec_builder_roundtrip",
        "domain": "blueprint",
        "edit_domain": "blueprint_spec_builder",
        "description": "Create an Actor Blueprint, build variables/components/nodes from a declarative spec, save, and read the generated graph back",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/BP_BenchSpecActor"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "blueprint_query",
             "args": {"action": "create_blueprint",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/BP_BenchSpecActor",
                      "parent_class": "Actor",
                      "blueprint_type": "Normal"},
             "contains": ["BP_BenchSpecActor", "Actor"]},
            {"tool": "blueprint_query",
             "args": {"action": "build_blueprint_from_spec",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BP_BenchSpecActor",
                      "graph_name": "EventGraph",
                      "variables": [
                          {"name": "BenchSpecHealth", "type": "float",
                           "default_value": "125.0", "category": "Benchmark",
                           "instance_editable": True},
                      ],
                      "components": [
                          {"name": "BenchSpecSphere", "class": "SphereComponent"},
                      ],
                      "nodes": [
                          {"id": "evt", "type": "CustomEvent",
                           "event_name": "BenchSpecRun", "position": [0, 0]},
                          {"id": "print", "type": "CallFunction",
                           "function_name": "PrintString", "position": [320, 0]},
                      ],
                      "connections": [
                          {"source": "evt", "source_pin": "Then",
                           "target": "print", "target_pin": "execute"},
                      ],
                      "pin_defaults": [
                          {"node_id": "print", "pin_name": "InString",
                           "value": "BenchSpecBuilt"},
                      ],
                      "auto_compile": True},
             "contains": ["variables_created", "components_created", "nodes_created", "connections_made"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BP_BenchSpecActor"}},
        ],
        "verify": [
            {"tool": "blueprint_query",
             "args": {"action": "get_variables",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BP_BenchSpecActor"},
             "contains": ["BenchSpecHealth", "125"]},
            {"tool": "blueprint_query",
             "args": {"action": "get_components",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BP_BenchSpecActor"},
             "contains": ["BenchSpecSphere", "SphereComponent"]},
            {"tool": "blueprint_query",
             "args": {"action": "get_graph_data",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BP_BenchSpecActor",
                      "graph_name": "EventGraph"},
             "contains": ["BenchSpecRun", "PrintString"]},
            {"tool": "blueprint_query",
             "args": {"action": "compile_blueprint",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BP_BenchSpecActor"},
             "contains": ["error_count", "0"]},
        ],
    },
    {
        "name": "datatable_bulk_import_export_roundtrip",
        "domain": "data",
        "edit_domain": "datatable_bulk_import_export",
        "description": "Bulk-edit DataTable rows, rename/duplicate/remove rows, export them, replace by CSV import, save, and read back the replacement row set",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/S_BenchBulkRow",
                                      f"{ASSET_AUTHORING_ROOT}/DT_BenchBulkRows"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "blueprint_query",
             "args": {"action": "create_user_defined_struct",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/S_BenchBulkRow",
                      "fields": [{"name": "DisplayName", "type": "text"},
                                  {"name": "Power", "type": "int", "default_value": "0"}]},
             "contains": ["S_BenchBulkRow", "DisplayName", "Power"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/S_BenchBulkRow"}},
            {"tool": "blueprint_query",
             "args": {"action": "create_data_table",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/DT_BenchBulkRows",
                      "row_struct": f"{ASSET_AUTHORING_ROOT}/S_BenchBulkRow.S_BenchBulkRow"},
             "contains": ["DT_BenchBulkRows"]},
            {"tool": "blueprint_query",
             "args": {"action": "set_data_table_rows",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/DT_BenchBulkRows",
                      "rows": [{"row_name": "Alpha",
                                "values": {"DisplayName": "Alpha Sword", "Power": 11}},
                               {"row_name": "Beta",
                                "values": {"DisplayName": "Beta Axe", "Power": 17}}],
                      "strict": True,
                      "save": False},
             "contains": ["Alpha", "Beta", "Power"]},
            {"tool": "blueprint_query",
             "args": {"action": "rename_data_table_row",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/DT_BenchBulkRows",
                      "old_name": "Alpha",
                      "new_name": "AlphaRenamed",
                      "save": False},
             "contains": ["AlphaRenamed"]},
            {"tool": "blueprint_query",
             "args": {"action": "duplicate_data_table_row",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/DT_BenchBulkRows",
                      "source_row": "Beta",
                      "new_name": "BetaCopy",
                      "save": False},
             "contains": ["BetaCopy"]},
            {"tool": "blueprint_query",
             "args": {"action": "remove_data_table_row",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/DT_BenchBulkRows",
                      "row_name": "BetaCopy",
                      "confirm": True,
                      "save": False},
             "contains": ["BetaCopy"]},
            {"tool": "blueprint_query",
             "args": {"action": "export_data_table",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/DT_BenchBulkRows",
                      "format": "json",
                      "simple_text": True},
             "contains": ["AlphaRenamed", "Beta", "total_rows"]},
            {"tool": "blueprint_query",
             "args": {"action": "import_data_table",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/DT_BenchBulkRows",
                      "format": "csv",
                      "mode": "replace",
                      "text": "Name,DisplayName,Power\nImportedAlpha,Imported Sword,21\nImportedBeta,Imported Shield,34\n",
                      "save": True},
             "contains": ["rows_written", "2", "success", "true"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/S_BenchBulkRow"}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/DT_BenchBulkRows"}},
        ],
        "verify": [
            {"tool": "blueprint_query",
             "args": {"action": "get_data_table_rows",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/DT_BenchBulkRows"},
             "contains": ["ImportedAlpha", "Imported Sword", "21", "ImportedBeta", "34"],
             "not_contains": ["AlphaRenamed", "BetaCopy"]},
            {"tool": "asset_query",
             "args": {"action": "inspect_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/DT_BenchBulkRows"},
             "contains": ["DT_BenchBulkRows", "DataTable"]},
        ],
    },
    {
        "name": "ai_blackboard_inheritance_duplicate_roundtrip",
        "domain": "ai",
        "edit_domain": "blackboard_inheritance_duplicate",
        "description": "Create parent/child Blackboards, inherit parent keys, duplicate the child Blackboard, save, and read inherited/local keys back",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/BB_BenchParent",
                                      f"{ASSET_AUTHORING_ROOT}/BB_BenchChild",
                                      f"{ASSET_AUTHORING_ROOT}/BB_BenchChildCopy"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "ai_query",
             "args": {"action": "create_blackboard",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/BB_BenchParent"},
             "contains": ["BB_BenchParent"]},
            {"tool": "ai_query",
             "args": {"action": "batch_add_bb_keys",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BB_BenchParent",
                      "keys": [{"name": "TargetActor", "type": "Object",
                                "base_class": "/Script/Engine.Actor",
                                "description": "Inherited target"},
                               {"name": "Alertness", "type": "Float",
                                "description": "Inherited alertness"}]},
             "contains": ["TargetActor", "Alertness"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BB_BenchParent"}},
            {"tool": "ai_query",
             "args": {"action": "create_blackboard",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/BB_BenchChild",
                      "parent_bb": f"{ASSET_AUTHORING_ROOT}/BB_BenchParent"},
             "contains": ["BB_BenchChild"]},
            {"tool": "ai_query",
             "args": {"action": "add_bb_key",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BB_BenchChild",
                      "key_name": "LocalRoute",
                      "key_type": "Name",
                      "description": "Child route key"},
             "contains": ["LocalRoute"]},
            {"tool": "ai_query",
             "args": {"action": "duplicate_blackboard",
                      "source_path": f"{ASSET_AUTHORING_ROOT}/BB_BenchChild",
                      "dest_path": f"{ASSET_AUTHORING_ROOT}/BB_BenchChildCopy"},
             "contains": ["BB_BenchChildCopy"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BB_BenchChild"}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BB_BenchChildCopy"}},
        ],
        "verify": [
            {"tool": "ai_query",
             "args": {"action": "get_blackboard",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BB_BenchChild"},
             "contains": ["TargetActor", "Alertness", "LocalRoute", "inherited"]},
            {"tool": "ai_query",
             "args": {"action": "get_blackboard",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BB_BenchChildCopy"},
             "contains": ["TargetActor", "Alertness", "LocalRoute"]},
            {"tool": "ai_query",
             "args": {"action": "compare_blackboards",
                      "path_a": f"{ASSET_AUTHORING_ROOT}/BB_BenchChild",
                      "path_b": f"{ASSET_AUTHORING_ROOT}/BB_BenchChildCopy"},
             "contains": ["only_in_a", "only_in_b", "type_changed", "same"]},
        ],
    },
    {
        "name": "ai_eqs_template_roundtrip",
        "domain": "ai",
        "edit_domain": "eqs_template",
        "description": "Create an EQS query from the find_cover preset template with overrides, save it, and inspect template-generated options/tests",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/EQS_BenchTemplateCover"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "ai_query",
             "args": {"action": "create_eqs_from_template",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/EQS_BenchTemplateCover",
                      "template": "find_cover",
                      "properties": {"GridSize": 1600, "SpaceBetween": 250}},
             "contains": ["EQS_BenchTemplateCover", "find_cover"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/EQS_BenchTemplateCover"}},
        ],
        "verify": [
            {"tool": "ai_query",
             "args": {"action": "get_eqs_query",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/EQS_BenchTemplateCover"},
             "contains": ["EnvQueryGenerator", "EnvQueryTest", "1600"]},
            {"tool": "ai_query",
             "args": {"action": "validate_eqs_query",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/EQS_BenchTemplateCover"}},
        ],
    },
    {
        "name": "ai_behavior_tree_template_export_roundtrip",
        "domain": "ai",
        "edit_domain": "behavior_tree_template_export",
        "description": "Create a BehaviorTree from the patrol template, save its paired Blackboard, export the spec, and validate the generated tree",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/BT_BenchTemplatePatrol",
                                      f"{ASSET_AUTHORING_ROOT}/BB_BenchTemplatePatrol"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "ai_query",
             "args": {"action": "create_bt_from_template",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/BT_BenchTemplatePatrol",
                      "template": "patrol"},
             "contains": ["BT_BenchTemplatePatrol", "patrol", "BB_BenchTemplatePatrol"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BT_BenchTemplatePatrol"}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BB_BenchTemplatePatrol"}},
        ],
        "verify": [
            {"tool": "ai_query",
             "args": {"action": "get_behavior_tree",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BT_BenchTemplatePatrol"},
             "contains": ["BT_BenchTemplatePatrol", "BB_BenchTemplatePatrol"]},
            {"tool": "ai_query",
             "args": {"action": "export_bt_spec",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BT_BenchTemplatePatrol"},
             "contains": ["blackboard_path", "root", "patrol"]},
            {"tool": "ai_query",
             "args": {"action": "validate_behavior_tree",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/BT_BenchTemplatePatrol"}},
        ],
    },
    {
        "name": "gas_effect_spec_builder_roundtrip",
        "domain": "gas",
        "edit_domain": "gameplay_effect_spec_builder",
        "description": "Build a GameplayEffect from a declarative spec, duplicate it with overrides, save, validate, and read both effects back",
        "chain": [
            {"tool": "asset_query", "allow_error": True,
             "args": {"action": "delete_assets",
                      "asset_paths": [f"{ASSET_AUTHORING_ROOT}/GE_BenchSpecAura",
                                      f"{ASSET_AUTHORING_ROOT}/GE_BenchSpecAuraClone"],
                      "allowed_prefixes": ASSET_AUTHORING_DELETE_PREFIXES, "force": True}},
            {"tool": "gas_query",
             "args": {"action": "build_effect_from_spec",
                      "save_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchSpecAura",
                      "spec": {"duration_policy": "has_duration",
                               "duration_magnitude": 3.0,
                               "period": 0.5,
                               "execute_on_application": True,
                               "stacking": {"type": "AggregateByTarget",
                                            "limit": 2}}},
             "contains": ["GE_BenchSpecAura", "has_duration"]},
            {"tool": "gas_query",
             "args": {"action": "duplicate_gameplay_effect",
                      "source_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchSpecAura",
                      "dest_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchSpecAuraClone",
                      "overrides": {"duration_policy": "has_duration"}},
             "contains": ["GE_BenchSpecAuraClone"]},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchSpecAura"}},
            {"tool": "asset_query",
             "args": {"action": "save_asset",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchSpecAuraClone"}},
        ],
        "verify": [
            {"tool": "gas_query",
             "args": {"action": "get_gameplay_effect",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchSpecAura"},
             "contains": ["GE_BenchSpecAura", "has_duration"]},
            {"tool": "gas_query",
             "args": {"action": "get_gameplay_effect",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchSpecAuraClone"},
             "contains": ["GE_BenchSpecAuraClone", "has_duration"]},
            {"tool": "gas_query",
             "args": {"action": "validate_effect",
                      "asset_path": f"{ASSET_AUTHORING_ROOT}/GE_BenchSpecAura"}},
        ],
    },
]


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


def project_root() -> pathlib.Path:
    """Return the Unreal project root without relying on the current working directory."""
    return resolve_plugin_path(".").parent.parent


def asset_authoring_fixture_dir() -> pathlib.Path:
    return project_root() / "Saved" / "Monolith" / "Benchmarks" / "BlueprintEditing" / "AssetAuthoringFixtures"


def ensure_asset_authoring_local_fixtures() -> Dict[str, pathlib.Path]:
    """Materialize local source files required by asset-authoring import tasks."""
    with _ASSET_AUTHORING_FIXTURE_LOCK:
        fixture_dir = asset_authoring_fixture_dir()
        fixture_dir.mkdir(parents=True, exist_ok=True)

        texture_file = fixture_dir / "T_BenchFileImport.bmp"
        texture_file.write_bytes(base64.b64decode(ASSET_AUTHORING_BMP_2X2_B64))

        interchange_texture_file = fixture_dir / "T_BenchInterchangeTexture.bmp"
        interchange_texture_file.write_bytes(base64.b64decode(ASSET_AUTHORING_BMP_2X2_B64))

        stringtable_csv = fixture_dir / "ST_BenchCsvImport.csv"
        stringtable_csv.write_text(
            "key,source_string,metadata.Context,metadata.Screen\n"
            "ConfirmButton,Confirm,Dialog,Main\n"
            "CancelButton,Cancel,Dialog,Main\n",
            encoding="utf-8",
            newline="\n",
        )

        static_mesh_obj = fixture_dir / "SM_BenchImported.obj"
        static_mesh_obj.write_text(
            "o SM_BenchImported\n"
            "v -50 -50 0\n"
            "v 50 -50 0\n"
            "v 50 50 0\n"
            "v -50 50 0\n"
            "v -50 -50 100\n"
            "v 50 -50 100\n"
            "v 50 50 100\n"
            "v -50 50 100\n"
            "vt 0 0\n"
            "vt 1 0\n"
            "vt 1 1\n"
            "vt 0 1\n"
            "vn 0 0 -1\n"
            "vn 0 0 1\n"
            "vn 0 -1 0\n"
            "vn 1 0 0\n"
            "vn 0 1 0\n"
            "vn -1 0 0\n"
            "f 1/1/1 3/3/1 2/2/1\n"
            "f 1/1/1 4/4/1 3/3/1\n"
            "f 5/1/2 6/2/2 7/3/2\n"
            "f 5/1/2 7/3/2 8/4/2\n"
            "f 1/1/3 2/2/3 6/3/3\n"
            "f 1/1/3 6/3/3 5/4/3\n"
            "f 2/1/4 3/2/4 7/3/4\n"
            "f 2/1/4 7/3/4 6/4/4\n"
            "f 3/1/5 4/2/5 8/3/5\n"
            "f 3/1/5 8/3/5 7/4/5\n"
            "f 4/1/6 1/2/6 5/3/6\n"
            "f 4/1/6 5/3/6 8/4/6\n",
            encoding="ascii",
            newline="\n",
        )

        static_mesh_lod_obj = fixture_dir / "SM_BenchLodScreen.obj"
        static_mesh_lod_obj.write_text(
            static_mesh_obj.read_text(encoding="ascii").replace(
                "o SM_BenchImported", "o SM_BenchLodScreen", 1
            ),
            encoding="ascii",
            newline="\n",
        )

        modelgen_obj = fixture_dir / "SM_BenchGeneratedModel.obj"
        modelgen_obj.write_text(
            static_mesh_obj.read_text(encoding="ascii").replace(
                "o SM_BenchImported", "o SM_BenchGeneratedModel", 1
            ),
            encoding="ascii",
            newline="\n",
        )

        interchange_mesh_obj = fixture_dir / "SM_BenchInterchangeCube.obj"
        interchange_mesh_obj.write_text(
            static_mesh_obj.read_text(encoding="ascii").replace(
                "o SM_BenchImported", "o SM_BenchInterchangeCube", 1
            ),
            encoding="ascii",
            newline="\n",
        )

        interchange_mesh_reimport_obj = fixture_dir / "SM_BenchInterchangeCube_Reimport.obj"
        interchange_mesh_reimport_obj.write_text(
            static_mesh_obj.read_text(encoding="ascii").replace(
                "o SM_BenchImported", "o SM_BenchInterchangeCube_Reimport", 1
            ),
            encoding="ascii",
            newline="\n",
        )

        interchange_mesh_export_obj = fixture_dir / "SM_BenchInterchangeCube_Export.obj"

        interchange_audio_wav = fixture_dir / "SW_BenchInterchangeTone.wav"
        sample_rate = 22050
        duration_seconds = 0.18
        frames = bytearray()
        for index in range(int(sample_rate * duration_seconds)):
            sample = int(32767 * 0.28 * math.sin(2.0 * math.pi * 440.0 * index / sample_rate))
            frames.extend(struct.pack("<h", sample))
        with wave.open(str(interchange_audio_wav), "wb") as handle:
            handle.setnchannels(1)
            handle.setsampwidth(2)
            handle.setframerate(sample_rate)
            handle.writeframes(frames)

    return {
        ASSET_AUTHORING_FILE_BMP_MARKER: texture_file,
        ASSET_AUTHORING_INTERCHANGE_TEXTURE_BMP_MARKER: interchange_texture_file,
        ASSET_AUTHORING_STRINGTABLE_CSV_MARKER: stringtable_csv,
        ASSET_AUTHORING_STATIC_MESH_OBJ_MARKER: static_mesh_obj,
        ASSET_AUTHORING_STATIC_MESH_LOD_OBJ_MARKER: static_mesh_lod_obj,
        ASSET_AUTHORING_MODELGEN_OBJ_MARKER: modelgen_obj,
        ASSET_AUTHORING_INTERCHANGE_MESH_OBJ_MARKER: interchange_mesh_obj,
        ASSET_AUTHORING_INTERCHANGE_MESH_REIMPORT_OBJ_MARKER: interchange_mesh_reimport_obj,
        ASSET_AUTHORING_INTERCHANGE_MESH_EXPORT_OBJ_MARKER: interchange_mesh_export_obj,
        ASSET_AUTHORING_INTERCHANGE_AUDIO_WAV_MARKER: interchange_audio_wav,
    }


_ENGINE_ROBOTO_CACHE: Optional[pathlib.Path] = None


def resolve_engine_roboto_regular_ttf() -> pathlib.Path:
    """Resolve the UE root from GO.uproject EngineAssociation and return Roboto-Regular.ttf."""
    global _ENGINE_ROBOTO_CACHE
    if _ENGINE_ROBOTO_CACHE is not None:
        return _ENGINE_ROBOTO_CACHE

    root = project_root()
    resolver = root / "BatchFiles" / "Script" / "ResolveUnrealEngine.ps1"
    uproject = root / "GO.uproject"
    if not resolver.is_file():
        raise RuntimeError(f"Unreal engine resolver not found: {resolver}")
    if not uproject.is_file():
        raise RuntimeError(f"GO.uproject not found: {uproject}")

    engine_root_text = subprocess.check_output(
        [
            "powershell",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(resolver),
            "-Project",
            str(uproject),
            "-Output",
            "Root",
        ],
        cwd=str(root),
        stderr=subprocess.STDOUT,
        text=True,
        timeout=60,
    ).strip()
    engine_root = pathlib.Path(engine_root_text)
    candidate = engine_root / "Engine" / "Content" / "Slate" / "Fonts" / "Roboto-Regular.ttf"
    if not candidate.is_file():
        raise RuntimeError(f"Roboto-Regular.ttf not found under resolved engine root: {candidate}")

    _ENGINE_ROBOTO_CACHE = candidate
    return candidate


def resolve_asset_authoring_placeholders(value: Any) -> Any:
    """Resolve portable benchmark placeholder tokens to local absolute source files."""
    if isinstance(value, str):
        if value in (ASSET_AUTHORING_FILE_BMP_MARKER, ASSET_AUTHORING_INTERCHANGE_TEXTURE_BMP_MARKER,
                     ASSET_AUTHORING_STRINGTABLE_CSV_MARKER, ASSET_AUTHORING_STATIC_MESH_OBJ_MARKER,
                     ASSET_AUTHORING_STATIC_MESH_LOD_OBJ_MARKER, ASSET_AUTHORING_MODELGEN_OBJ_MARKER,
                     ASSET_AUTHORING_INTERCHANGE_MESH_OBJ_MARKER,
                     ASSET_AUTHORING_INTERCHANGE_MESH_REIMPORT_OBJ_MARKER,
                     ASSET_AUTHORING_INTERCHANGE_MESH_EXPORT_OBJ_MARKER,
                     ASSET_AUTHORING_INTERCHANGE_AUDIO_WAV_MARKER):
            return ensure_asset_authoring_local_fixtures()[value].as_posix()
        if value == ASSET_AUTHORING_ENGINE_ROBOTO_MARKER:
            return resolve_engine_roboto_regular_ttf().as_posix()
        return value
    if isinstance(value, list):
        return [resolve_asset_authoring_placeholders(item) for item in value]
    if isinstance(value, dict):
        return {key: resolve_asset_authoring_placeholders(item) for key, item in value.items()}
    return value


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


def project_results(data: Dict[str, Any]) -> List[Dict[str, Any]]:
    for key in ("results", "hits", "items", "assets"):
        value = data.get(key)
        if isinstance(value, list):
            return [r for r in value if isinstance(r, dict)]
    return []


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


def _response_text_contains_any(response: Dict[str, Any], tokens: List[str]) -> bool:
    """True if any non-empty token from tokens appears in the response text."""
    text = result_text(response)
    return any(tok and tok in text for tok in tokens)


def _response_text_contains_all(response: Dict[str, Any], tokens: List[str]) -> bool:
    text = result_text(response)
    return all(tok and tok in text for tok in tokens)


# ---------------------------------------------------------------------------
# Read-back / round-trip helpers (v5)
# ---------------------------------------------------------------------------

def _compile_is_clean(response: Dict[str, Any]) -> Tuple[bool, Dict[str, Any]]:
    """True if a compile_blueprint / validate_blueprint response reports ZERO errors.

    Inspects the structured payload (``error_count`` / ``errors`` / ``status``) rather than
    trusting the non-error envelope: a handler can return a non-error envelope that DESCRIBES
    a failed compile. Verified live: compile_blueprint returns
    {success, status:"UpToDate", errors:[], warnings:[], error_count:0, warning_count:0}.
    """
    if response.get("transport_error") or response.get("parse_error"):
        return False, {"reason": "transport_or_parse_error"}
    if _is_error(response):
        return False, {"reason": "isError"}
    data = result_data(response)
    error_count = data.get("error_count")
    errors = data.get("errors")
    status = str(data.get("status", "")).lower()
    # validate_blueprint has a DIFFERENT shape from compile_blueprint: it returns a structured
    # lint report (node_errors / unimplemented_interface_functions / duplicate_custom_events as
    # lists) with NO error_count. Verified live: validate_blueprint returns
    # {unused_variables, disconnected_nodes, node_errors, unimplemented_interface_functions,
    #  duplicate_custom_events, total_graphs, total_nodes}. unused_variables and disconnected_nodes
    # are warnings (a blueprint with them still compiles), so clean = the three HARD-error lists
    # are all empty. This must be checked BEFORE the text fallback, which would otherwise see the
    # substring "error" in "node_errors" and wrongly fail every clean validate.
    validate_keys = ("node_errors", "unimplemented_interface_functions", "duplicate_custom_events")
    if error_count is None and any(k in data for k in validate_keys):
        hard = {k: data.get(k) for k in validate_keys}
        clean = all(not (isinstance(v, list) and len(v) > 0) for v in hard.values())
        return clean, {"validate_report": {k: (len(v) if isinstance(v, list) else v)
                                            for k, v in hard.items()},
                       "status": status}
    if isinstance(error_count, int):
        clean = error_count == 0
    elif isinstance(errors, list):
        clean = len(errors) == 0
    elif status:
        clean = status in ("uptodate", "success", "ok", "compiled")
    else:
        # No structured compile signal — documented text-scan fallback only.
        text = result_text(response).lower()
        clean = "0 error" in text or "error" not in text
    return clean, {"error_count": error_count, "status": status,
                   "errors_len": len(errors) if isinstance(errors, list) else None}


def _compile_has_errors(response: Dict[str, Any]) -> Tuple[bool, Dict[str, Any]]:
    """Inverse of _compile_is_clean for the negative_compile category: True iff the response
    reports a REAL compile failure (error_count > 0, a non-empty errors list, an error status,
    or a non-empty validate hard-error list). A transport/parse error or a structured isError is
    NOT a compile failure signal (the asset failed to load, not to compile) and returns False, so
    a server that simply errors on everything cannot pass negative_compile."""
    if response.get("transport_error") or response.get("parse_error") or _is_error(response):
        return False, {"reason": "transport_parse_or_iserror_not_a_compile_signal"}
    clean, detail = _compile_is_clean(response)
    return (not clean), detail


def _extract_node_id(response: Dict[str, Any]) -> Optional[str]:
    """Pull the node id from an add_node response. Verified live: add_node returns the new
    node at top-level key ``id`` (e.g. 'K2Node_CallFunction_0')."""
    data = result_data(response)
    if not isinstance(data, dict):
        return None
    for key in ("id", "node_id", "nodeId", "new_node_id"):
        val = data.get(key)
        if isinstance(val, str) and val:
            return val
    return None


def _subst_ids(value: Any, captured: Dict[str, str]) -> Any:
    """Recursively replace ${label} placeholders with captured node ids."""
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


def _norm_value(x: Any) -> str:
    """Normalize a scalar for value-equality read-backs so '350.0'=='350', 'True'=='true'=='1'."""
    if isinstance(x, bool):
        return "true" if x else "false"
    s = str(x).strip()
    low = s.lower()
    if low in ("true", "false"):
        return low
    try:
        return repr(float(s))
    except (TypeError, ValueError):
        return low


def _collect_names(obj: Any) -> set:
    """Recursively collect every ``name`` field value (entities, nested SCS component trees,
    function/variable lists). Used by exact-membership ``absent`` checks where a substring
    ``not_contains`` gives a false fail (e.g. old name is a prefix of the new name)."""
    names: set = set()
    if isinstance(obj, dict):
        nm = obj.get("name")
        if isinstance(nm, str):
            names.add(nm)
        for v in obj.values():
            names |= _collect_names(v)
    elif isinstance(obj, list):
        for v in obj:
            names |= _collect_names(v)
    return names


def _find_component_node(obj: Any, name: str) -> Optional[Dict[str, Any]]:
    """Find a component node by name in the (possibly nested) get_components SCS tree."""
    if isinstance(obj, dict):
        if obj.get("name") == name:
            return obj
        for v in obj.values():
            found = _find_component_node(v, name)
            if found is not None:
                return found
    elif isinstance(obj, list):
        for v in obj:
            found = _find_component_node(v, name)
            if found is not None:
                return found
    return None


def _verify_readback(url: str, asset_path: str, verify: Dict[str, Any], timeout_s: float) -> Tuple[bool, Dict[str, Any]]:
    """Run a read action and assert the edit is actually observable.

    verify verbs (all optional; all that are present must hold):
      read_action     : the read action to call (get_variables / get_components / get_functions / ...)
      contains        : tokens that must ALL appear in the read response text (presence)
      not_contains    : tokens that must NOT appear (text-level absence; delete/disconnect proof)
      absent          : entity names that must NOT exist as an exact ``name`` (parsed; survives
                        prefix collisions where old_name is a substring of new_name)
      var_default     : {name, value} — a variable must exist with that default_value (parsed)
      prop_value      : {name, value} — get_component_details ``properties`` must carry name=value
      var_replicated  : a variable must exist with replicated==True (parsed)
      var_type        : {name, type} — a variable must exist with that type (parsed)
      component_parent: {child, parent} — child must be in parent's children[] (SCS tree walk)
      pos_equals      : {node_id, x, y} — a node must report that position (parsed)
    """
    read_action = verify.get("read_action")
    if not read_action:
        return True, {"skipped": "no_read_action"}
    read_call = {"action": read_action, "asset_path": asset_path}
    read_call.update(verify.get("read_args", {}))
    resp = mcp_call(url, "blueprint_query", read_call, timeout_s=timeout_s)
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

    var_default = verify.get("var_default")
    if ok and isinstance(var_default, dict):
        variables = data.get("variables", []) if isinstance(data, dict) else []
        match = next((v for v in variables if isinstance(v, dict)
                      and v.get("name") == var_default.get("name")), None)
        got = match.get("default_value") if isinstance(match, dict) else None
        ok = got is not None and _norm_value(got) == _norm_value(var_default.get("value"))
        detail["var_default"] = {"name": var_default.get("name"),
                                 "want": str(var_default.get("value")), "got": got, "ok": ok}

    prop_value = verify.get("prop_value")
    if ok and isinstance(prop_value, dict):
        props = data.get("properties", []) if isinstance(data, dict) else []
        match = next((p for p in props if isinstance(p, dict)
                      and p.get("name") == prop_value.get("name")), None)
        got = match.get("value") if isinstance(match, dict) else None
        ok = got is not None and _norm_value(got) == _norm_value(prop_value.get("value"))
        detail["prop_value"] = {"name": prop_value.get("name"),
                                "want": str(prop_value.get("value")), "got": got, "ok": ok}

    var_replicated = verify.get("var_replicated")
    if ok and var_replicated:
        variables = data.get("variables", []) if isinstance(data, dict) else []
        match = next((v for v in variables if isinstance(v, dict)
                      and v.get("name") == var_replicated), None)
        ok = bool(match) and match.get("replicated") is True
        detail["var_replicated"] = {"name": var_replicated,
                                    "got": match.get("replicated") if match else None, "ok": ok}

    var_type = verify.get("var_type")
    if ok and isinstance(var_type, dict):
        variables = data.get("variables", []) if isinstance(data, dict) else []
        match = next((v for v in variables if isinstance(v, dict)
                      and v.get("name") == var_type.get("name")), None)
        got = str(match.get("type")) if isinstance(match, dict) else ""
        want = str(var_type.get("type"))
        ok = bool(match) and (want.lower() in got.lower() or got.lower() in want.lower())
        detail["var_type"] = {"name": var_type.get("name"), "want": want, "got": got, "ok": ok}

    component_parent = verify.get("component_parent")
    if ok and isinstance(component_parent, dict):
        parent_node = _find_component_node(data, component_parent.get("parent"))
        children = _collect_names(parent_node.get("children")) if isinstance(parent_node, dict) else set()
        ok = component_parent.get("child") in children
        detail["component_parent"] = {**component_parent, "ok": ok}

    pos_equals = verify.get("pos_equals")
    if ok and isinstance(pos_equals, dict):
        node = _find_node_by_id(data, str(pos_equals.get("node_id")))
        pos = node.get("pos") if isinstance(node, dict) else None
        ok = (isinstance(pos, list) and len(pos) >= 2
              and int(pos[0]) == int(pos_equals.get("x")) and int(pos[1]) == int(pos_equals.get("y")))
        detail["pos_equals"] = {**pos_equals, "got": pos, "ok": ok}

    return ok, detail


def _find_node_by_id(obj: Any, node_id: str) -> Optional[Dict[str, Any]]:
    """Find a node dict by its ``id`` in a get_graph_data / get_node_details payload."""
    if isinstance(obj, dict):
        if obj.get("id") == node_id:
            return obj
        for v in obj.values():
            found = _find_node_by_id(v, node_id)
            if found is not None:
                return found
    elif isinstance(obj, list):
        for v in obj:
            found = _find_node_by_id(v, node_id)
            if found is not None:
                return found
    return None


def _score_op_chain(url: str, task: Dict[str, Any], timeout_s: float) -> Dict[str, Any]:
    """Score a multi-step executed chain (edit_execute op_chain or workflow_execute).

    Runs each step in order, capturing add_node ids into a local label table and
    substituting ${label} placeholders into later steps' arguments — so connect_pins wires
    REAL nodes by their returned ids. Compile steps must report a clean compile. After the
    chain a read-back must observe the end state: an actual wired connection
    (source pin's connected_to contains "<target_id>.<pin>") and/or contains-tokens.
    """
    tool = str(task["tool"])
    asset_path = task.get("asset_path", "")
    captured: Dict[str, str] = {}
    steps_evidence: List[Dict[str, Any]] = []
    ok = True

    for step in task.get("chain", []):
        args = _subst_ids(dict(step.get("args", {})), captured)
        resp = mcp_call(url, tool, args, timeout_s=timeout_s)
        action = str(args.get("action", ""))
        step_ok = not resp.get("transport_error") and not resp.get("parse_error")
        is_err = _is_error(resp)
        resp_text_l = result_text(resp).lower()
        # "already exists" / "already implemented" / "already in use" are all idempotent-success
        # signals (the entity is present); the final read-back still gates correctness, so a
        # silent no-op cannot pass on the strength of this tolerance alone.
        already = is_err and ("exist" in resp_text_l or "already" in resp_text_l)
        # A leading remove_*/delete_* in a delete-first chain legitimately reports "not found" on a
        # fresh run (nothing to delete yet) — tolerate it so the subsequent create still runs.
        remove_missing = (is_err and action.startswith(("remove_", "delete_"))
                          and any(t in resp_text_l for t in ("not found", "does not exist", "no such")))
        if action in ("compile_blueprint", "validate_blueprint"):
            clean, cdet = _compile_is_clean(resp)
            step_ok = step_ok and clean
            steps_evidence.append({"action": action, "compile": cdet, "ok": step_ok})
        else:
            step_ok = step_ok and (not is_err or already or remove_missing)
            steps_evidence.append({"action": action, "is_error": is_err, "already": already,
                                   "remove_missing": remove_missing,
                                   "ok": step_ok, "snippet": result_text(resp)[:120]})
        if "capture" in step:
            nid = _extract_node_id(resp)
            if nid:
                captured[step["capture"]] = nid
            else:
                step_ok = False
                steps_evidence[-1]["no_node_id"] = True
        if not step_ok:
            ok = False
            break

    verify_detail: Dict[str, Any] = {}
    if ok:
        def _pin_connected(node_label_key: str, from_pin_key: str, to_label_key: str,
                           to_pin_key: str, spec: Dict[str, Any]) -> Tuple[Optional[bool], Dict[str, Any]]:
            src = captured.get(spec.get(node_label_key, ""))
            dst = captured.get(spec.get(to_label_key, ""))
            want = "%s.%s" % (dst, spec.get(to_pin_key, "")) if dst else None
            if not (src and want):
                return None, {"missing_capture": True}
            det = mcp_call(url, tool, {"action": "get_node_details", "asset_path": asset_path,
                                       "node_id": src, "graph_name": task.get("graph_name", "")},
                           timeout_s=timeout_s)
            payload = result_data(det)
            pins = payload.get("pins", []) if isinstance(payload, dict) else []
            pin = next((p for p in pins if isinstance(p, dict)
                        and p.get("name") == spec.get(from_pin_key)), None)
            wired = bool(pin) and want in (pin.get("connected_to") or [])
            return wired, {"from": src, "pin": spec.get(from_pin_key), "want": want, "wired": wired}

        conn = task.get("verify_connection")
        if isinstance(conn, dict):
            wired, cdet = _pin_connected("from", "from_pin", "to", "to_pin", conn)
            verify_detail["connection"] = cdet
            ok = ok and (wired is True)

        disc = task.get("verify_disconnection")
        if ok and isinstance(disc, dict):
            # Inverse of verify_connection: the named pin must NOT carry the connection any more.
            wired, ddet = _pin_connected("from", "from_pin", "to", "to_pin", disc)
            verify_detail["disconnection"] = ddet
            ok = ok and (wired is False)

        verify = task.get("verify")
        if ok and isinstance(verify, dict):
            # Substitute captured node ids into the verify block so ${label} reaches read_args
            # (needed by per-node read-backs: set_pin_default, remove_node, set_node_position).
            verify = _subst_ids(verify, captured)
            v_ok, v_det = _verify_readback(url, asset_path, verify, timeout_s)
            verify_detail["readback"] = v_det
            ok = ok and v_ok

    return {
        "task_id": task.get("id"),
        "category": task.get("category", ""),
        "namespace": task.get("namespace"),
        "action": task.get("action"),
        "blueprint_type": task.get("blueprint_type", ""),
        "domain": task.get("domain", ""),
        "edit_domain": task.get("edit_domain", ""),
        "workflow": task.get("workflow", ""),
        "direct_success": ok,
        "planning_signals": False,
        "evidence": {"steps": steps_evidence, "captured": captured, "verify": verify_detail},
        "transport_error": False,
        "transport_error_raw": "",
        "response_is_error": not ok,
        "response_text": json.dumps(verify_detail)[:500],
    }


def _score_negative_compile(url: str, task: Dict[str, Any], timeout_s: float) -> Dict[str, Any]:
    """Score a negative_compile task: run a setup chain that DELIBERATELY breaks a scratch
    blueprint, then compile and require a REAL compile failure (error_count>0). Proves the
    compiler actually runs — a stub returning error_count:0 for everything fails here. The setup
    steps are run tolerantly (their job is to construct the break); the scored signal is the final
    compile response. Captured add_node ids thread into later steps (${label})."""
    tool = str(task["tool"])
    asset_path = task.get("asset_path", "")
    captured: Dict[str, str] = {}
    steps_evidence: List[Dict[str, Any]] = []

    for step in task.get("setup_chain", []):
        args = _subst_ids(dict(step.get("args", {})), captured)
        step_tool = str(step.get("tool", tool))
        resp = mcp_call(url, step_tool, args, timeout_s=timeout_s)
        action = str(args.get("action", ""))
        if "capture" in step:
            nid = _extract_node_id(resp)
            if nid:
                captured[step["capture"]] = nid
        steps_evidence.append({"action": action, "is_error": _is_error(resp),
                               "snippet": result_text(resp)[:100]})

    compile_args = _subst_ids(dict(task.get("compile_args",
                              {"action": "compile_blueprint", "asset_path": asset_path})), captured)
    compile_resp = mcp_call(url, tool, compile_args, timeout_s=timeout_s)
    has_err, cdet = _compile_has_errors(compile_resp)
    error_tokens = task.get("expected", {}).get("error_tokens", [])
    text_l = result_text(compile_resp).lower()
    token_ok = (not error_tokens) or any(str(t).lower() in text_l for t in error_tokens)
    direct_success = has_err and token_ok

    # Cleanup: repair the deliberate break and save, so the scratch blueprint is NOT left in a
    # corrupt (invalid-type) state on disk — a persisted corrupt asset crashes the headless
    # -nullrhi editor on the next boot's asset scan. Run tolerantly; does not affect the score.
    for step in task.get("cleanup_chain", []):
        try:
            step_tool = str(step.get("tool", tool))
            mcp_call(url, step_tool, _subst_ids(dict(step.get("args", {})), captured), timeout_s=timeout_s)
        except Exception:
            pass

    return {
        "task_id": task.get("id"),
        "category": "negative_compile",
        "namespace": task.get("namespace"),
        "action": task.get("action"),
        "blueprint_type": task.get("blueprint_type", ""),
        "domain": task.get("domain", ""),
        "edit_domain": task.get("edit_domain", ""),
        "workflow": "",
        "direct_success": direct_success,
        "planning_signals": False,
        "evidence": {"setup_steps": steps_evidence, "compile_has_errors": has_err,
                     "compile_detail": cdet, "token_ok": token_ok,
                     "compile_snippet": result_text(compile_resp)[:300]},
        "transport_error": bool(compile_resp.get("transport_error")),
        "transport_error_raw": str(compile_resp.get("raw", ""))[:300] if compile_resp.get("transport_error") else "",
        "response_is_error": not direct_success,
        "response_text": result_text(compile_resp)[:500],
    }


# ---------------------------------------------------------------------------
# Task scoring
# ---------------------------------------------------------------------------

def _score_asset_authoring_task(url: str, task: Dict[str, Any], timeout_s: float) -> Dict[str, Any]:
    """Score cross-namespace asset-authoring chains.

    These tasks intentionally step outside the blueprint graph namespace to cover common UE asset
    lifecycles that agents frequently author: Texture2D ingest, DataTables, StringTables, Enhanced
    Input, Materials/MICs, and CurveTables. Each chain must mutate/save through the owning
    namespace and then prove the result with one or more read-back calls.
    """
    steps_evidence: List[Dict[str, Any]] = []
    verify_evidence: List[Dict[str, Any]] = []
    ok = True

    for step in task.get("chain", []):
        tool = str(step.get("tool", task.get("tool", "")))
        args = resolve_asset_authoring_placeholders(dict(step.get("args", {})))
        resp = mcp_call(url, tool, args, timeout_s=timeout_s)
        text = result_text(resp)
        is_err = _is_error(resp)
        transport_or_parse = bool(resp.get("transport_error") or resp.get("parse_error"))
        allow_error = bool(step.get("allow_error"))
        contains = step.get("contains") or []
        not_contains = step.get("not_contains") or []
        contains_ok = all(str(tok) in text for tok in contains)
        not_contains_ok = all(str(tok) not in text for tok in not_contains)
        step_ok = not transport_or_parse and (not is_err or allow_error) and contains_ok and not_contains_ok
        steps_evidence.append({
            "tool": tool,
            "action": args.get("action", ""),
            "is_error": is_err,
            "transport_error": bool(resp.get("transport_error")),
            "parse_error": bool(resp.get("parse_error")),
            "allow_error": allow_error,
            "contains": contains,
            "contains_ok": contains_ok,
            "not_contains": not_contains,
            "not_contains_ok": not_contains_ok,
            "ok": step_ok,
            "snippet": text[:180],
            "raw_error": str(resp.get("raw", ""))[:220] if resp.get("transport_error") else "",
        })
        if not step_ok:
            ok = False
            break

    if ok:
        for verify in task.get("verify", []):
            if not isinstance(verify, dict):
                continue
            tool = str(verify.get("tool", task.get("tool", "")))
            args = resolve_asset_authoring_placeholders(dict(verify.get("args", {})))
            resp = mcp_call(url, tool, args, timeout_s=timeout_s)
            text = result_text(resp)
            contains = verify.get("contains") or []
            not_contains = verify.get("not_contains") or []
            server_ok = not resp.get("transport_error") and not resp.get("parse_error") and not _is_error(resp)
            contains_ok = all(str(tok) in text for tok in contains)
            not_contains_ok = all(str(tok) not in text for tok in not_contains)
            verify_ok = server_ok and contains_ok and not_contains_ok
            verify_evidence.append({
                "tool": tool,
                "action": args.get("action", ""),
                "server_ok": server_ok,
                "transport_error": bool(resp.get("transport_error")),
                "parse_error": bool(resp.get("parse_error")),
                "contains": contains,
                "contains_ok": contains_ok,
                "not_contains": not_contains,
                "not_contains_ok": not_contains_ok,
                "ok": verify_ok,
                "snippet": text[:220],
                "raw_error": str(resp.get("raw", ""))[:220] if resp.get("transport_error") else "",
            })
            ok = ok and verify_ok
            if not verify_ok:
                break

    return {
        "task_id": task.get("id"),
        "category": "asset_authoring",
        "namespace": task.get("namespace"),
        "action": task.get("action"),
        "blueprint_type": task.get("blueprint_type", ""),
        "domain": task.get("domain", ""),
        "edit_domain": task.get("edit_domain", ""),
        "workflow": task.get("workflow", task.get("name", "")),
        "direct_success": ok,
        "planning_signals": False,
        "evidence": {"steps": steps_evidence, "verify": verify_evidence},
        "transport_error": False,
        "transport_error_raw": "",
        "response_is_error": not ok,
        "response_text": json.dumps(verify_evidence)[:500],
    }


def _score_duplicate_reject(url: str, task: Dict[str, Any], timeout_s: float) -> Dict[str, Any]:
    """Score a duplicate_reject task.

    Calls the create action TWICE. The first call ensures the entity exists
    (it may succeed, or already exist from a prior run). The SECOND identical
    call must be refused with a structured ``isError`` — that is the only proof
    the action has an effective duplicate-name guard rather than silently
    creating a suffixed copy (which edit_execute cannot detect).
    """
    tool = str(task["tool"])
    args = dict(task.get("arguments", {}))

    # setup_arguments may be a single dict or a LIST of setup calls. The list form is used to
    # (1) DELETE the fixed-name entity from a prior run so the first create is a clean success,
    # and (2) create any host entity (e.g. the function a local variable lives in). A leading
    # delete that returns "not found" on run 1 is harmless (it precedes the create).
    setup_ok = True
    setup_args = task.get("setup_arguments")
    setup_list: List[Dict[str, Any]] = []
    if isinstance(setup_args, dict):
        setup_list = [setup_args]
    elif isinstance(setup_args, list):
        setup_list = [s for s in setup_args if isinstance(s, dict)]
    for s in setup_list:
        s_resp = mcp_call(url, tool, dict(s), timeout_s=timeout_s)
        s_action = str(s.get("action", ""))
        s_err = _is_error(s_resp)
        # Deletes may legitimately report "not found" (nothing to remove on a fresh run); host
        # creates may report "already exists". Both are acceptable; transport/parse failures are not.
        s_tolerable = (s_action.startswith("remove_") or s_action.startswith("delete_")
                       or "exist" in result_text(s_resp).lower()
                       or "not found" in result_text(s_resp).lower())
        setup_ok = setup_ok and (not s_resp.get("transport_error") and not s_resp.get("parse_error")
                                 and (not s_err or s_tolerable))

    first = mcp_call(url, tool, dict(args), timeout_s=timeout_s)
    second = mcp_call(url, tool, dict(args), timeout_s=timeout_s)

    transport_error = bool(second.get("transport_error"))
    parse_error = bool(second.get("parse_error"))
    server_handled = not transport_error and not parse_error
    second_is_error = _is_error(second)

    # The first call must CLEANLY create the entity. After a leading delete (see setup_arguments)
    # the create is guaranteed fresh, so a clean non-error success is required — a server that
    # errors on EVERY call (and thus fakes a guard) now fails first_ok. The one exception is
    # add_event_node, which has no per-name remove to reset it; that task sets
    # allow_existing_first so an "already exists" first call is tolerated for it only.
    first_is_error = _is_error(first)
    allow_existing_first = bool(task.get("allow_existing_first"))
    first_ok = (not first.get("transport_error") and not first.get("parse_error")
                and (not first_is_error
                     or (allow_existing_first and "exist" in result_text(first).lower())))
    # The second call must be refused with a DUPLICATE-specific error, not just any isError.
    second_text = result_text(second).lower()
    second_is_duplicate = second_is_error and any(
        tok in second_text for tok in ("already", "exist", "duplicate", "in use", "taken"))

    direct_success = setup_ok and first_ok and server_handled and second_is_duplicate

    return {
        "task_id": task.get("id"),
        "category": "duplicate_reject",
        "namespace": task.get("namespace"),
        "action": task.get("action"),
        "blueprint_type": task.get("blueprint_type", ""),
        "domain": task.get("domain", ""),
        "edit_domain": task.get("edit_domain", ""),
        "workflow": "",
        "direct_success": direct_success,
        "planning_signals": False,
        "evidence": {
            "setup_ok": setup_ok,
            "first_call_ok": first_ok,
            "first_call_is_error": first_is_error,
            "second_call_handled": server_handled,
            "second_call_is_error": second_is_error,
            "second_is_duplicate": second_is_duplicate,
            "edit_domain": task.get("edit_domain", ""),
            "response_snippet": result_text(second)[:200],
        },
        "transport_error": transport_error,
        "transport_error_raw": str(second.get("raw", ""))[:300] if transport_error else "",
        "response_is_error": second_is_error,
        "response_text": result_text(second)[:500],
    }


def score_task(url: str, task: Dict[str, Any], timeout_s: float) -> Dict[str, Any]:
    category = task.get("category", "")
    if category == "asset_authoring":
        return _score_asset_authoring_task(url, task, timeout_s)
    if category == "duplicate_reject":
        return _score_duplicate_reject(url, task, timeout_s)
    if category == "negative_compile":
        return _score_negative_compile(url, task, timeout_s)
    # Executed multi-step chains (real node-wiring or end-to-end workflows) are scored by
    # threading add_node ids and reading the result back.
    if category == "workflow_execute" or task.get("action") == "op_chain":
        return _score_op_chain(url, task, timeout_s)

    response = mcp_call(url, str(task["tool"]), dict(task.get("arguments", {})), timeout_s=timeout_s)
    data = result_data(response)

    transport_error = bool(response.get("transport_error"))
    parse_error = bool(response.get("parse_error"))
    server_is_error = _is_error(response)
    server_handled = not transport_error and not parse_error

    direct_success = False
    planning_signals = False
    evidence: Dict[str, Any] = {}

    if category == "type_discovery":
        valid_resp = is_valid_non_error_response(data, response)
        results = project_results(data)
        # Prefix queries require ≥1 result; text-based queries accept 0 results (project-specific).
        require_results = task.get("require_results", True)
        direct_success = valid_resp and (len(results) > 0 if require_results else True)
        evidence = {
            "valid_response": valid_resp,
            "results_count": len(results),
            "require_results": require_results,
        }

    elif category in ("graph_read", "variable_read"):
        # Strict: server must handle the request AND not return isError.
        server_ok = server_handled and not server_is_error
        # Content shape check: verify the response has meaningful content.
        # For get_variables: check that at least one fixture variable name appears in response text.
        # For list_graphs: check that the expected graph name appears.
        # Skipped when no fixture contract is specified (Interface no-graph_name, empty fixture_vars).
        content_ok = True
        content_check_applied = False
        if server_ok:
            action = task.get("action", "")
            fixture_vars = task.get("expected", {}).get("fixture_vars", [])
            expected_graph = task.get("expected", {}).get("expected_graph", "")
            expected_functions = task.get("expected", {}).get("expected_functions", [])
            if action == "get_variables" and fixture_vars:
                content_ok = _response_text_contains_any(response, fixture_vars)
                content_check_applied = True
            elif action == "list_graphs" and expected_graph:
                content_ok = _response_text_contains_any(response, [expected_graph])
                content_check_applied = True
            elif action == "get_functions" and expected_functions:
                # Interface fixture: at least one declared interface stub must appear.
                content_ok = _response_text_contains_any(response, expected_functions)
                content_check_applied = True
            expected_contains = task.get("expected", {}).get("contains", [])
            if content_ok and expected_contains:
                content_ok = all(str(tok) in result_text(response) for tok in expected_contains)
                content_check_applied = True
        direct_success = server_ok and content_ok
        evidence = {
            "server_handled": server_handled,
            "is_error": server_is_error,
            "content_ok": content_ok,
            "content_check_applied": content_check_applied,
            "response_snippet": result_text(response)[:200],
        }

    elif category == "read_schema":
        # Lenient: only require planning_signals + skill metadata.
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

    elif category == "edit_schema":
        # Strict: isError (e.g., unknown action) fails even if text fields are present.
        schema = data.get("schema") if isinstance(data, dict) else None
        if not isinstance(schema, dict):
            schema = data
        has_signals = False if server_is_error else schema_has_planning_signals(schema)
        has_skill = False if server_is_error else schema_has_skill(schema)
        planning_signals = has_signals
        direct_success = bool(has_signals and has_skill and not server_is_error)
        evidence = {
            "has_planning_signals": has_signals,
            "has_skill": has_skill,
            "is_error": server_is_error,
            "edit_domain": task.get("edit_domain", ""),
        }

    elif category == "edit_execute":
        # Strict + read-back verified. A non-error envelope is necessary but NOT sufficient:
        #   - compile/validate actions must report a CLEAN compile (error_count == 0), inspected
        #     from the payload, not just a non-error envelope.
        #   - actions with a ``verify`` block must have their mutation observable via a follow-up
        #     read (e.g. after add_variable, get_variables must list the new name).
        # "already exists" still counts (idempotent across re-runs) but the read-back must still
        # confirm the entity is present, so a silent no-op cannot pass.
        action = task.get("action", "")
        if action in ("compile_blueprint", "validate_blueprint"):
            clean, cdet = _compile_is_clean(response)
            direct_success = clean
            evidence = {"compile_clean": clean, "compile_detail": cdet,
                        "edit_domain": task.get("edit_domain", "")}
        else:
            already_exists = server_is_error and "exist" in result_text(response).lower()
            base_ok = server_handled and (not server_is_error or already_exists)
            verify = task.get("verify")
            verify_ok = True
            verify_detail: Dict[str, Any] = {}
            if base_ok and isinstance(verify, dict):
                asset_path = dict(task.get("arguments", {})).get("asset_path", "")
                # Substitute ${self} in the verify block with the node id this call returned, so
                # add_node read-backs assert the EXACT new node id appears in get_graph_data
                # (per-call precise; not satisfiable by a same-named leftover node from a prior run,
                # and robust for FormatText where the format literal is not in get_graph_data).
                self_id = _extract_node_id(response)
                if self_id:
                    verify = _subst_ids(verify, {"self": self_id})
                verify_ok, verify_detail = _verify_readback(url, asset_path, verify, timeout_s)
            direct_success = base_ok and verify_ok
            evidence = {
                "server_handled": server_handled,
                "is_error": server_is_error,
                "already_exists": already_exists,
                "verify_ok": verify_ok,
                "verify": verify_detail,
                "edit_domain": task.get("edit_domain", ""),
                "response_snippet": result_text(response)[:300],
            }

    elif category == "error_path":
        # Inverted + input-SPECIFIC: pass = a structured isError whose message references the
        # OFFENDING IDENTIFIER (the unique NONEXISTENT_*/INVALID_* token), not just a generic
        # English word. Scoring only on the specific tier denies credit to a reject-everything
        # server that returns a canned "could not find variable/function/component" for ALL
        # inputs — such a message contains the generic words but never the bad identifier. The
        # generic error_tokens are kept for diagnostics (generic_only) but no longer grant a pass.
        reason_tokens = task.get("expected", {}).get("error_tokens", [])
        specific_tokens = task.get("expected", {}).get("specific_tokens", [])
        text_l = result_text(response).lower()
        reason_ok = (not specific_tokens) or any(str(t).lower() in text_l for t in specific_tokens)
        generic_only = (bool(reason_tokens) and not reason_ok
                        and any(str(t).lower() in text_l for t in reason_tokens))
        direct_success = server_handled and server_is_error and reason_ok
        evidence = {
            "server_handled": server_handled,
            "returned_is_error": server_is_error,
            "reason_ok": reason_ok,
            "generic_only": generic_only,
            "specific_tokens": specific_tokens,
            "response_snippet": result_text(response)[:200],
        }

    else:
        evidence = {"unsupported_category": category}

    return {
        "task_id": task.get("id"),
        "category": category,
        "namespace": task.get("namespace"),
        "action": task.get("action"),
        "blueprint_type": task.get("blueprint_type", ""),
        "domain": task.get("domain", ""),
        "edit_domain": task.get("edit_domain", ""),
        "workflow": task.get("workflow", ""),
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

def aggregate(
    label: str,
    status: Dict[str, Any],
    tasks: List[Dict[str, Any]],
    rows: List[Dict[str, Any]],
    *,
    warn_missing_categories: bool = True,
) -> Dict[str, Any]:
    def rate(cat: str) -> float:
        cat_rows = [r for r in rows if r["category"] == cat]
        return avg([1.0 if r["direct_success"] else 0.0 for r in cat_rows])

    # P0-7: a weighted category with ZERO rows scores 0.0 via avg([]) — indistinguishable from
    # "all failed", and it silently caps the composite while the sum-to-1.0 assert still passes.
    # Surface it loudly so a dropped/empty category can never quietly corrupt a frozen baseline.
    # (The hard guard lives in build_static_tasks; this protects partial/ad-hoc task files at run time.)
    missing_categories = [name for name in WEIGHTS
                          if not any(r["category"] == name for r in rows)]
    if warn_missing_categories:
        for name in missing_categories:
            print(f"WARNING: weighted category '{name}' has 0 rows; its dimension is zeroed "
                  f"(-{WEIGHTS[name]} max score). The composite is not comparable to a full run.",
                  flush=True)

    # rates[name] for every weighted dimension, keyed by the WEIGHTS category names.
    rates: Dict[str, float] = {name: rate(name) for name in WEIGHTS}
    blueprint_editing_score = sum(WEIGHTS[name] * rates[name] for name in WEIGHTS)

    edit_schema_rows = [r for r in rows if r["category"] == "edit_schema"]
    edit_domain_breakdown: Dict[str, Dict[str, Any]] = {}
    for domain in sorted({r.get("edit_domain") or "" for r in edit_schema_rows} - {""}):
        d_rows = [r for r in edit_schema_rows if r.get("edit_domain") == domain]
        edit_domain_breakdown[domain] = {
            "count": len(d_rows),
            "rate": round(avg([1.0 if r["direct_success"] else 0.0 for r in d_rows]), 6),
        }

    gv_rows = [r for r in rows if r["category"] in ("graph_read", "variable_read")]
    bp_type_breakdown: Dict[str, Dict[str, Any]] = {}
    for bp_type in sorted({r.get("blueprint_type") or "" for r in gv_rows} - {""}):
        t_rows = [r for r in gv_rows if r.get("blueprint_type") == bp_type]
        bp_type_breakdown[bp_type] = {
            "count": len(t_rows),
            "rate": round(avg([1.0 if r["direct_success"] else 0.0 for r in t_rows]), 6),
        }

    ee_rows = [r for r in rows if r["category"] == "edit_execute"]
    edit_execute_type_breakdown: Dict[str, Dict[str, Any]] = {}
    for bp_type in sorted({r.get("blueprint_type") or "" for r in ee_rows} - {""}):
        t_rows = [r for r in ee_rows if r.get("blueprint_type") == bp_type]
        edit_execute_type_breakdown[bp_type] = {
            "count": len(t_rows),
            "rate": round(avg([1.0 if r["direct_success"] else 0.0 for r in t_rows]), 6),
        }

    error_count = sum(1 for r in rows if not r.get("direct_success", False))

    return {
        "label": label,
        "created_at": utc_now(),
        "mcp_status": status,
        "task_count": len(rows),
        "error_count": error_count,
        "category_counts": count_by(tasks, "category"),
        "metrics": {
            "blueprint_editing_score": round(blueprint_editing_score, 6),
            **{f"{name}_rate": round(rates[name], 6) for name in WEIGHTS},
            "task_count": len(rows),
            "error_count": error_count,
        },
        "missing_categories": missing_categories,
        "edit_domain_breakdown": edit_domain_breakdown,
        "blueprint_type_breakdown": bp_type_breakdown,
        "edit_execute_type_breakdown": edit_execute_type_breakdown,
    }


# ---------------------------------------------------------------------------
# Generate
# ---------------------------------------------------------------------------

def _edit_execute_verify(action: str, args: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    """Derive a read-back ``verify`` block for an edit_execute task, so a non-error envelope
    alone cannot pass: the mutation must be observable via a follow-up read. Returns None for
    actions with no cheap single-read read-back (they still require a clean non-error result;
    compile/validate are checked separately via _compile_is_clean)."""
    if action == "add_variable":
        name = args.get("name") or args.get("variable_name")
        return {"read_action": "get_variables", "contains": [name]} if name else None
    if action == "add_replicated_variable":
        # P0-3: assert the replication FLAG, not just the name — a plain (non-replicated) leftover
        # variable of the same name must NOT pass.
        name = args.get("variable_name") or args.get("name")
        return {"read_action": "get_variables", "var_replicated": name} if name else None
    if action == "set_variable_type":
        name, ty = args.get("name") or args.get("variable_name"), args.get("type")
        return ({"read_action": "get_variables", "var_type": {"name": name, "type": ty}}
                if name and ty else None)
    if action == "set_variable_defaults":
        name, val = args.get("name"), args.get("default_value")
        return ({"read_action": "get_variables", "var_default": {"name": name, "value": val}}
                if name is not None and val is not None else None)
    if action == "add_function":
        fn = args.get("function_name")
        return {"read_action": "get_functions", "contains": [fn]} if fn else None
    if action == "set_function_params":
        fn = args.get("function_name")
        tokens: List[str] = [str(fn)] if fn else []
        for key in ("inputs", "outputs"):
            values = args.get(key)
            if isinstance(values, list):
                tokens.extend(str(v.get("name")) for v in values
                              if isinstance(v, dict) and v.get("name"))
        return ({"read_action": "get_function_signature",
                 "read_args": {"function_name": fn}, "contains": tokens}
                if fn and tokens else None)
    if action == "set_function_thread_safe":
        fn = args.get("function_name")
        return ({"read_action": "get_function_signature",
                 "read_args": {"function_name": fn}, "contains": [fn]}
                if fn else None)
    if action == "rename_function":
        nn = args.get("new_name")
        return {"read_action": "get_functions", "contains": [nn]} if nn else None
    if action == "add_event_node":
        ev = args.get("event_name")
        return ({"read_action": "get_graph_data",
                 "read_args": {"graph_name": args.get("graph_name", "EventGraph")},
                 "contains": [ev]} if ev else None)
    if action == "add_node":
        # Per-call precise: assert the EXACT node id this call returned (substituted as ${self} by
        # the edit_execute scorer) appears in get_graph_data. This proves THIS call created the
        # node — not satisfiable by a same-typed leftover node from a prior run — and is robust for
        # FormatText, whose `format` literal is not surfaced by get_graph_data (fixes BEB-244).
        gn = args.get("graph_name", "EventGraph")
        return {"read_action": "get_graph_data", "read_args": {"graph_name": gn},
                "contains": ["${self}"]}
    if action == "add_comment_node":
        text, gn = args.get("text"), args.get("graph_name", "EventGraph")
        return ({"read_action": "get_graph_data", "read_args": {"graph_name": gn},
                 "contains": [text]} if text else None)
    if action == "add_component":
        cn = args.get("component_name")
        return {"read_action": "get_components", "contains": [cn]} if cn else None
    if action in ("rename_component", "duplicate_component"):
        cn = args.get("new_name")
        return {"read_action": "get_components", "contains": [cn]} if cn else None
    if action == "set_component_property":
        # P1-1: read back the VALUE, not just the component name — a no-op set must fail.
        cn, pn, val = args.get("component_name"), args.get("property_name"), args.get("value")
        if cn and pn is not None and val is not None:
            return {"read_action": "get_component_details",
                    "read_args": {"component_name": cn},
                    "prop_value": {"name": pn, "value": val}}
        return {"read_action": "get_component_details",
                "read_args": {"component_name": cn}, "contains": [cn]} if cn else None
    if action == "set_cdo_property":
        # P0-4: derive contains from the property + value actually set; an empty contains verifies
        # nothing. get_cdo_properties emits JSON booleans, so values normalize via the read-back.
        pn, val = args.get("property_name"), args.get("value")
        toks = [str(pn)] + ([str(val)] if val is not None else [])
        return {"read_action": "get_cdo_properties", "contains": toks} if pn else None
    if action == "set_cdo_properties":
        props = args.get("properties")
        toks = []
        if isinstance(props, dict):
            for k, v in props.items():
                toks.append(str(k))
        return {"read_action": "get_cdo_properties", "contains": toks} if toks else None
    if action == "duplicate_graph":
        nn = args.get("new_name")
        return {"read_action": "get_functions", "contains": [nn]} if nn else None
    if action == "add_event_dispatcher":
        nm = args.get("name")
        return {"read_action": "get_event_dispatchers", "contains": [nm]} if nm else None
    return None


# P0-2: create actions that have a clean inverse remove. Wrapping these as delete-first op_chains
# makes the read-back prove THIS run created the entity (a same-named leftover from a prior run is
# deleted first), closing the leftover-state no-op pass on the dominant edit_execute category.
def _create_inverse(action: str, args: Dict[str, Any], asset_path: str) -> Optional[Dict[str, Any]]:
    if action == "add_variable":
        nm = args.get("name") or args.get("variable_name")
        return {"action": "remove_variable", "asset_path": asset_path, "name": nm} if nm else None
    if action == "add_replicated_variable":
        nm = args.get("variable_name") or args.get("name")
        return {"action": "remove_variable", "asset_path": asset_path, "name": nm} if nm else None
    if action == "add_function":
        fn = args.get("function_name")
        return {"action": "remove_function", "asset_path": asset_path, "name": fn} if fn else None
    if action == "add_component":
        cn = args.get("component_name")
        return {"action": "remove_component", "asset_path": asset_path, "component_name": cn} if cn else None
    if action == "add_event_dispatcher":
        # Handler API inconsistency (verified live): add_event_dispatcher takes `name`, but
        # remove_event_dispatcher requires `dispatcher_name`.
        nm = args.get("name")
        return {"action": "remove_event_dispatcher", "asset_path": asset_path, "dispatcher_name": nm} if nm else None
    return None


def _build_edit_execute_task(spec: Dict[str, Any], bp_type: str, domain: str) -> Dict[str, Any]:
    """Build one edit_execute task. Create actions with a clean inverse are emitted as delete-first
    op_chains (the read-back then proves THIS run created the entity); everything else stays flat
    with a derived read-back verify."""
    action = spec["action"]
    args = spec["arguments"]
    asset_path = args.get("asset_path", "")
    verify = _edit_execute_verify(action, args)
    inverse = _create_inverse(action, args, asset_path)
    base = {
        "category": "edit_execute", "namespace": "blueprint", "tool": "blueprint_query",
        "expected": {"direct_success": True}, "safety": "mutating_fixture",
        "edit_domain": spec["edit_domain"], "blueprint_type": bp_type, "domain": domain,
        "description": spec["description"],
    }
    if inverse is not None and verify is not None:
        return {**base, "action": "op_chain", "asset_path": asset_path,
                "graph_name": args.get("graph_name", "EventGraph"),
                "chain": [{"op": inverse["action"], "args": inverse},
                          {"op": action, "args": args}],
                "verify": verify}
    task = {**base, "action": action, "arguments": args}
    if verify is not None:
        task["verify"] = verify
    return task


def build_wiring_chains() -> List[Dict[str, Any]]:
    """One executed node-wiring chain per EventGraph-bearing fixture: add two PrintString
    CallFunction nodes, capture their returned ids, wire the first node's ``then`` exec output
    to the second node's ``execute`` exec input for real, and read back that the connection
    exists. This is the core of practical Blueprint editing (exec/data flow), which the old
    benchmark never executed (add_node responses were discarded). CallFunction nodes duplicate
    freely, so the chain is idempotent across re-runs (no unique-name collision)."""
    chains: List[Dict[str, Any]] = []
    for bp in BP_TYPES:
        if bp["type"] == "Interface":
            continue  # interfaces have no EventGraph
        path, t = bp["path"], bp["type"]
        chains.append({
            "name": f"wire_{t.lower()}",
            "blueprint_type": t, "domain": bp["domain"], "edit_domain": "node",
            "asset_path": path, "graph_name": "EventGraph",
            "description": f"Wire PrintString.then -> PrintString.execute on {t} EventGraph (executed connect_pins + read-back)",
            "chain": [
                {"op": "add_node", "capture": "a",
                 "args": {"action": "add_node", "asset_path": path, "graph_name": "EventGraph",
                          "node_type": "CallFunction", "function_name": "PrintString",
                          "position": [0, 0]}},
                {"op": "add_node", "capture": "b",
                 "args": {"action": "add_node", "asset_path": path, "graph_name": "EventGraph",
                          "node_type": "CallFunction", "function_name": "PrintString",
                          "position": [420, 0]}},
                {"op": "connect_pins",
                 "args": {"action": "connect_pins", "asset_path": path, "graph_name": "EventGraph",
                          "source_node": "${a}", "source_pin": "then",
                          "target_node": "${b}", "target_pin": "execute"}},
            ],
            "verify_connection": {"from": "a", "from_pin": "then", "to": "b", "to_pin": "execute"},
        })

    # P1-2: DATA-pin (typed value) wiring — wire a float variable getter into PrintString's
    # float Duration input. Exec-pin wiring (above) is necessary but the single most common act
    # after node placement is wiring DATA flow; a server that handles exec but mishandles typed
    # data pins now fails. Source pin of a VariableGet is named after the variable. The float→float
    # connection keeps the compile clean (no autocast ambiguity). One per fixture with a float var.
    data_pin_fixtures = [
        ("/Game/Benchmarks/BPB_TestActor", "Actor", "gameplay", "Health"),
        ("/Game/Benchmarks/BPB_TestCharacter", "Character", "gameplay", "MoveSpeed"),
        ("/Game/Benchmarks/ABP_TestAnim", "AnimInstance", "animation", "Speed"),
        ("/Game/Benchmarks/GA_TestAbility", "GameplayAbility", "ability", "AbilityCooldown"),
    ]
    for path, t, dom, var in data_pin_fixtures:
        chains.append({
            "name": f"datawire_{t.lower()}",
            "blueprint_type": t, "domain": dom, "edit_domain": "node",
            "asset_path": path, "graph_name": "EventGraph",
            "description": f"Wire {var} (float) -> PrintString.Duration on {t} EventGraph (executed data-pin connect_pins + read-back)",
            "chain": [
                {"op": "add_node", "capture": "g",
                 "args": {"action": "add_node", "asset_path": path, "graph_name": "EventGraph",
                          "node_type": "VariableGet", "variable_name": var}},
                {"op": "add_node", "capture": "p",
                 "args": {"action": "add_node", "asset_path": path, "graph_name": "EventGraph",
                          "node_type": "CallFunction", "function_name": "PrintString"}},
                {"op": "connect_pins",
                 "args": {"action": "connect_pins", "asset_path": path, "graph_name": "EventGraph",
                          "source_node": "${g}", "source_pin": var,
                          "target_node": "${p}", "target_pin": "Duration"}},
                {"op": "compile_blueprint",
                 "args": {"action": "compile_blueprint", "asset_path": path}},
            ],
            "verify_connection": {"from": "g", "from_pin": var, "to": "p", "to_pin": "Duration"},
        })
    return chains


def build_delete_roundtrip_tasks() -> List[Dict[str, Any]]:
    """P1-4: add -> remove -> read-back-GONE round-trips. Measures the destructive half of
    authoring and catches a dangerous silent-no-op remove. Self-cleaning (add then remove) so
    idempotent across re-runs. Uses not_contains/absent (Group 0 verify verbs)."""
    A = "/Game/Benchmarks/BPB_TestActor"
    tasks: List[Dict[str, Any]] = [
        {"name": "delete_variable", "blueprint_type": "Actor", "domain": "gameplay",
         "edit_domain": "variable", "asset_path": A,
         "description": "Add then remove a variable; get_variables must no longer list it",
         "chain": [
             {"op": "add_variable", "args": {"action": "add_variable", "asset_path": A,
                                             "name": "BenchDelVar", "type": "int"}},
             {"op": "remove_variable", "args": {"action": "remove_variable", "asset_path": A,
                                                "name": "BenchDelVar"}}],
         "verify": {"read_action": "get_variables", "not_contains": ["BenchDelVar"]}},
        {"name": "delete_function", "blueprint_type": "Actor", "domain": "gameplay",
         "edit_domain": "function", "asset_path": A,
         "description": "Add then remove a function; get_functions must no longer list it",
         "chain": [
             {"op": "add_function", "args": {"action": "add_function", "asset_path": A,
                                             "function_name": "BenchDelFunc"}},
             {"op": "remove_function", "args": {"action": "remove_function", "asset_path": A,
                                                "name": "BenchDelFunc"}}],  # remove_function takes `name`
         "verify": {"read_action": "get_functions", "not_contains": ["BenchDelFunc"]}},
        {"name": "delete_component", "blueprint_type": "Actor", "domain": "gameplay",
         "edit_domain": "component", "asset_path": A,
         "description": "Add then remove a component; get_components must no longer list it",
         "chain": [
             {"op": "add_component", "args": {"action": "add_component", "asset_path": A,
                                              "component_class": "StaticMeshComponent",
                                              "component_name": "BenchDelComp"}},
             {"op": "remove_component", "args": {"action": "remove_component", "asset_path": A,
                                                 "component_name": "BenchDelComp"}}],
         "verify": {"read_action": "get_components", "absent": ["BenchDelComp"]}},
        {"name": "delete_node", "blueprint_type": "Actor", "domain": "gameplay",
         "edit_domain": "node", "asset_path": A, "graph_name": "EventGraph",
         "description": "Add then remove a node; get_graph_data must no longer contain its id",
         "chain": [
             {"op": "add_node", "capture": "n",
              "args": {"action": "add_node", "asset_path": A, "graph_name": "EventGraph",
                       "node_type": "CallFunction", "function_name": "PrintString"}},
             {"op": "remove_node", "args": {"action": "remove_node", "asset_path": A,
                                            "graph_name": "EventGraph", "node_id": "${n}"}}],
         "verify": {"read_action": "get_graph_data", "read_args": {"graph_name": "EventGraph"},
                    "not_contains": ["${n}"]}},
    ]
    return tasks


def build_set_pin_default_tasks() -> List[Dict[str, Any]]:
    """P1-3: set a pin literal on a freshly added node and read it back via get_node_details.
    The most frequent literal-editing op, schema-only before. Idempotent (PrintString duplicates)."""
    A = "/Game/Benchmarks/BPB_TestActor"
    return [{
        "name": "set_pin_default", "blueprint_type": "Actor", "domain": "gameplay",
        "edit_domain": "node", "asset_path": A, "graph_name": "EventGraph",
        "description": "Set a PrintString InString pin literal and read it back",
        "chain": [
            {"op": "add_node", "capture": "n",
             "args": {"action": "add_node", "asset_path": A, "graph_name": "EventGraph",
                      "node_type": "CallFunction", "function_name": "PrintString"}},
            {"op": "set_pin_default",
             "args": {"action": "set_pin_default", "asset_path": A, "graph_name": "EventGraph",
                      "node_id": "${n}", "pin_name": "InString", "value": "BenchPinLiteral"}}],
        "verify": {"read_action": "get_node_details", "read_args": {"node_id": "${n}",
                   "graph_name": "EventGraph"}, "contains": ["BenchPinLiteral"]},
    }]


def build_negative_compile_tasks() -> List[Dict[str, Any]]:
    """P1-5: the only test that makes "compile is clean" falsifiable. Deliberately break a scratch
    blueprint and require the server to REPORT a real compile failure (error_count>0). Isolated on
    a dedicated scratch BP so it never poisons the 14 clean-compile fixtures, and reset per run
    (remove+add the break variable) so it is idempotent.

    Break mechanism (validated against the live UE 5.8 editor): create a function and set one
    input to a non-existent struct type. compile_blueprint then returns status:"Error" with a
    real compiler diagnostic: "bad or unknown type (Structure)". Invalid member-variable types
    and dangling VariableGet nodes are silently tolerated/normalized by UE 5.8, so they are not
    used for this falsification test."""
    SC = "/Game/Benchmarks/BPB_CompileFailScratch"
    return [{
        "id": None, "category": "negative_compile", "namespace": "blueprint",
        "action": "compile_blueprint", "tool": "blueprint_query",
        "asset_path": SC, "graph_name": "EventGraph",
        "blueprint_type": "Actor", "domain": "gameplay", "edit_domain": "compilation",
        "safety": "mutating_fixture",
        "description": "Deliberately break a scratch blueprint function signature with an invalid struct type and require a reported compile error",
        "setup_chain": [
            {"op": "create_blueprint",
             "args": {"action": "create_blueprint", "save_path": SC,
                      "parent_class": "Actor", "blueprint_type": "Normal"}},
            {"op": "remove_function",
             "args": {"action": "remove_function", "asset_path": SC, "name": "BenchBadSignature"}},
            {"op": "add_function",
             "args": {"action": "add_function", "asset_path": SC,
                      "function_name": "BenchBadSignature"}},
            {"op": "set_function_params",
             "args": {"action": "set_function_params", "asset_path": SC,
                      "function_name": "BenchBadSignature",
                      "inputs": [{"name": "BadStruct",
                                  "type": "struct:MonolithBenchNoSuchStructZZZ"}],
                      "outputs": []}},
        ],
        "compile_args": {"action": "compile_blueprint", "asset_path": SC},
        # Repair the break and save a CLEAN scratch BP so a corrupt (invalid-type) asset is never
        # persisted — that would crash the -nullrhi editor on the next boot's asset scan.
        "cleanup_chain": [
            {"args": {"action": "remove_function", "asset_path": SC, "name": "BenchBadSignature"}},
            {"args": {"action": "compile_blueprint", "asset_path": SC}},
            {"tool": "asset_query", "args": {"action": "save_asset", "asset_path": SC}},
        ],
        "expected": {"is_compile_error": True, "error_tokens": ["bad or unknown type", "unknown type"]},
    }]


def fixture_asset_name(bp: Dict[str, Any]) -> str:
    return str(bp["path"]).rsplit("/", 1)[-1]


def build_additional_graph_read_tasks() -> List[Dict[str, Any]]:
    tasks: List[Dict[str, Any]] = []
    for bp in BP_TYPES:
        asset_name = fixture_asset_name(bp)
        parent_class_name = FIXTURE_CREATE_PARAMS[bp["type"]]["parent_class"]
        tasks.append({
            "category": "graph_read",
            "namespace": "blueprint",
            "action": "get_blueprint_info",
            "tool": "blueprint_query",
            "arguments": {"action": "get_blueprint_info", "asset_path": bp["path"]},
            "expected": {"server_handled": True, "contains": [asset_name]},
            "safety": "read_only",
            "blueprint_type": bp["type"],
            "domain": bp["domain"],
            "description": f"Read blueprint info for {bp['type']} fixture {asset_name}",
        })
        tasks.append({
            "category": "graph_read",
            "namespace": "blueprint",
            "action": "get_parent_class",
            "tool": "blueprint_query",
            "arguments": {"action": "get_parent_class", "asset_path": bp["path"]},
            "expected": {"server_handled": True, "contains": [parent_class_name]},
            "safety": "read_only",
            "blueprint_type": bp["type"],
            "domain": bp["domain"],
            "description": f"Read parent class for {bp['type']} fixture {asset_name}",
        })
    return tasks


def build_additional_variable_read_tasks() -> List[Dict[str, Any]]:
    tasks: List[Dict[str, Any]] = []
    for bp in BP_TYPES:
        funcs = FIXTURE_FUNCTIONS_BY_TYPE.get(bp["type"], [])
        # get_interface_functions requires `interface_class` (it lists the functions of an
        # interface CLASS, not an asset's own graph functions). Pass the interface fixture's full
        # /Game path — the resolver loads the Blueprint Interface asset and uses its generated
        # class. (Verified live BEB-083: with only asset_path the handler returns
        # "Missing required param(s): [interface_class]".)
        is_iface = bp["type"] == "Interface"
        read_action = "get_interface_functions" if is_iface else "get_functions"
        read_args: Dict[str, Any] = {"action": read_action, "asset_path": bp["path"]}
        if is_iface:
            read_args["interface_class"] = bp["path"]
        tasks.append({
            "category": "variable_read",
            "namespace": "blueprint",
            "action": read_action,
            "tool": "blueprint_query",
            "arguments": read_args,
            "expected": {"server_handled": True, "expected_functions": funcs, "contains": funcs},
            "safety": "read_only",
            "blueprint_type": bp["type"],
            "domain": bp["domain"],
            "description": f"Read setup-created function stubs for {bp['type']} fixture",
        })
        if bp["type"] == "Interface":
            tasks.append({
                "category": "variable_read",
                "namespace": "blueprint",
                "action": "get_functions",
                "tool": "blueprint_query",
                "arguments": {"action": "get_functions", "asset_path": bp["path"]},
                "expected": {"server_handled": True, "expected_functions": funcs, "contains": funcs},
                "safety": "read_only",
                "blueprint_type": bp["type"],
                "domain": bp["domain"],
                "description": "Read Interface function stubs through generic get_functions",
            })
        else:
            tasks.append({
                "category": "variable_read",
                "namespace": "blueprint",
                "action": "get_variables",
                "tool": "blueprint_query",
                "arguments": {"action": "get_variables", "asset_path": bp["path"],
                              "include_inherited": False},
                "expected": {"server_handled": True, "fixture_vars": bp["fixture_vars"],
                             "contains": bp["fixture_vars"]},
                "safety": "read_only",
                "blueprint_type": bp["type"],
                "domain": bp["domain"],
                "description": f"Read declared fixture variables only for {bp['type']}",
            })
    return tasks


# A verify block with read_action set must carry at least one MEANINGFUL assertion verb — never a
# bare read_action or an empty `contains:[]` (which the scorer short-circuits to "ok=True",
# re-introducing a content-free no-op read-back). Enforced by validate_task_integrity (P0-4).
_VERIFY_ASSERT_VERBS = ("contains", "not_contains", "absent", "var_default", "prop_value",
                        "var_replicated", "var_type", "component_parent", "pos_equals")


def _verify_is_meaningful(verify: Any) -> bool:
    if not isinstance(verify, dict) or not verify.get("read_action"):
        return True  # no read_action -> nothing to assert against (compile/connection-only tasks)
    for verb in _VERIFY_ASSERT_VERBS:
        v = verify.get(verb)
        if isinstance(v, list):
            if len(v) > 0:
                return True
        elif v is not None:
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
        if not _verify_is_meaningful(task.get("verify")):
            raise RuntimeError(f"{task_id} has a read_action verify with no assertion verb "
                               f"(empty contains/no verb = content-free no-op read-back): {task.get('verify')}")


def build_static_tasks() -> List[Dict[str, Any]]:
    tasks: List[Dict[str, Any]] = []

    def next_id() -> str:
        return f"BEB-{len(tasks) + 1:03d}"

    # --- type_discovery (21) ---
    for query, btype, domain, require_results in TYPE_SEARCH_QUERIES:
        tasks.append({
            "id": next_id(),
            "category": "type_discovery",
            "namespace": "project",
            "action": "search",
            "tool": "project_query",
            "arguments": {"action": "search", "query": query, "include_content": False},
            "expected": {"valid_response": True, "min_results": 1 if require_results else 0},
            "require_results": require_results,
            "safety": "read_only",
            "blueprint_type": btype,
            "domain": domain,
            "description": f"Discover {btype} fixture with project.search query '{query}'",
        })

    # --- graph_read (35 = base 21 + info/parent read-backs for each type) ---
    for bp in BP_TYPES:
        tasks.append({
            "id": next_id(),
            "category": "graph_read",
            "namespace": "blueprint",
            "action": "list_graphs",
            "tool": "blueprint_query",
            "arguments": {"action": "list_graphs", "asset_path": bp["path"]},
            "expected": {"server_handled": True,
                         "expected_graph": bp["default_graph"] or ""},
            "safety": "read_only",
            "blueprint_type": bp["type"],
            "domain": bp["domain"],
            "description": f"List graphs for {bp['type']} fixture",
        })
        if bp["type"] == "Interface":
            # Interface BPs have no event graphs; use get_blueprint_info instead.
            tasks.append({
                "id": next_id(),
                "category": "graph_read",
                "namespace": "blueprint",
                "action": "get_blueprint_info",
                "tool": "blueprint_query",
                "arguments": {"action": "get_blueprint_info", "asset_path": bp["path"]},
                "expected": {"server_handled": True},
                "safety": "read_only",
                "blueprint_type": bp["type"],
                "domain": bp["domain"],
                "description": "Read blueprint info for Interface fixture without EventGraph",
            })
        else:
            gd_args: Dict[str, Any] = {"action": "get_graph_data", "asset_path": bp["path"]}
            if bp["default_graph"] is not None:
                gd_args["graph_name"] = bp["default_graph"]
            tasks.append({
                "id": next_id(),
                "category": "graph_read",
                "namespace": "blueprint",
                "action": "get_graph_data",
                "tool": "blueprint_query",
                "arguments": gd_args,
                "expected": {"server_handled": True},
                "safety": "read_only",
                "blueprint_type": bp["type"],
                "domain": bp["domain"],
                "description": f"Read {bp['type']} fixture default graph data",
            })
        tasks.append({
            "id": next_id(),
            "category": "graph_read",
            "namespace": "blueprint",
            "action": "get_graph_summary",
            "tool": "blueprint_query",
            "arguments": {"action": "get_graph_summary", "asset_path": bp["path"]},
            "expected": {"server_handled": True},
            "safety": "read_only",
            "blueprint_type": bp["type"],
            "domain": bp["domain"],
            "description": f"Read graph summary for {bp['type']} fixture",
        })
    for spec in build_additional_graph_read_tasks():
        spec["id"] = next_id()
        tasks.append(spec)

    # --- variable_read (28 = base 14 + function/declared-variable read-backs) ---
    for bp in BP_TYPES:
        tasks.append({
            "id": next_id(),
            "category": "variable_read",
            "namespace": "blueprint",
            "action": "get_variables",
            "tool": "blueprint_query",
            "arguments": {"action": "get_variables", "asset_path": bp["path"]},
            "expected": {"server_handled": True,
                         "fixture_vars": bp["fixture_vars"]},
            "safety": "read_only",
            "blueprint_type": bp["type"],
            "domain": bp["domain"],
            "description": f"Read variables for {bp['type']} fixture",
        })
        if bp["type"] == "Interface":
            # Interface has no class-level variables; test get_functions instead.
            tasks.append({
                "id": next_id(),
                "category": "variable_read",
                "namespace": "blueprint",
                "action": "get_functions",
                "tool": "blueprint_query",
                "arguments": {"action": "get_functions", "asset_path": bp["path"]},
                "expected": {"server_handled": True,
                             "expected_functions": INTERFACE_FIXTURE_FUNCTIONS},
                "safety": "read_only",
                "blueprint_type": bp["type"],
                "domain": bp["domain"],
                "note": "Interface has no variables; get_functions tests the function-stub surface",
                "description": "Read Interface fixture functions as variable_read substitute",
            })
        else:
            tasks.append({
                "id": next_id(),
                "category": "variable_read",
                "namespace": "blueprint",
                "action": "get_variables",
                "tool": "blueprint_query",
                "arguments": {"action": "get_variables", "asset_path": bp["path"],
                              "include_inherited": True},
                "expected": {"server_handled": True,
                             "fixture_vars": bp["fixture_vars"]},
                "safety": "read_only",
                "blueprint_type": bp["type"],
                "domain": bp["domain"],
                "description": f"Read inherited variables for {bp['type']} fixture",
            })
    for spec in build_additional_variable_read_tasks():
        spec["id"] = next_id()
        tasks.append(spec)

    # --- read_schema (23) ---
    for action in BLUEPRINT_READ_ACTIONS:
        tasks.append({
            "id": next_id(),
            "category": "read_schema",
            "namespace": "blueprint",
            "action": action,
            "tool": "monolith_discover",
            "arguments": {"action": action, "mode": "schema", "namespace": "blueprint"},
            "expected": {"requires_planning_signals": True},
            "safety": "read_only_discovery",
            "description": f"Discover read-action schema for blueprint.{action}",
        })

    # --- edit_schema (46) ---
    for action, edit_domain in BLUEPRINT_EDIT_ACTIONS:
        tasks.append({
            "id": next_id(),
            "category": "edit_schema",
            "namespace": "blueprint",
            "action": action,
            "tool": "monolith_discover",
            "arguments": {"action": action, "mode": "schema", "namespace": "blueprint"},
            "expected": {"requires_planning_signals": True},
            "safety": "read_only_discovery",
            "edit_domain": edit_domain,
            "description": f"Discover edit-action schema for blueprint.{action}",
        })

    # --- workflow_execute (11 executed end-to-end chains) ---
    for wf in BLUEPRINT_WORKFLOW_EXECUTE:
        tasks.append({
            "id": next_id(),
            "category": "workflow_execute",
            "namespace": "blueprint",
            "action": "op_chain",
            "tool": "blueprint_query",
            "asset_path": wf["asset_path"],
            "graph_name": wf.get("graph_name", "EventGraph"),
            "chain": wf["chain"],
            **({"verify_connection": wf["verify_connection"]} if "verify_connection" in wf else {}),
            **({"verify": wf["verify"]} if "verify" in wf else {}),
            "expected": {"direct_success": True},
            "safety": "mutating_fixture",
            "edit_domain": wf.get("edit_domain", ""),
            "blueprint_type": wf.get("blueprint_type", ""),
            "domain": wf.get("domain", ""),
            "workflow": wf["name"],
            "description": wf["description"],
        })

    # --- edit_execute: per-type single-edit tasks, each read-back verified (creates are
    #     delete-first op_chains so the read-back proves THIS run made the edit) ---
    for bp in BP_TYPES:
        bp_type = bp["type"]
        type_tasks = BLUEPRINT_EDIT_EXECUTE_TASKS_BY_TYPE.get(bp_type, [])
        for spec in type_tasks:
            task = _build_edit_execute_task(spec, bp_type, bp["domain"])
            task["id"] = next_id()
            tasks.append(task)

    # --- edit_execute: executed node-wiring chains (real connect_pins, 6 = non-Interface types) ---
    for chain in build_wiring_chains():
        tasks.append({
            "id": next_id(),
            "category": "edit_execute",
            "namespace": "blueprint",
            "action": "op_chain",
            "tool": "blueprint_query",
            "asset_path": chain["asset_path"],
            "graph_name": chain.get("graph_name", "EventGraph"),
            "chain": chain["chain"],
            "verify_connection": chain["verify_connection"],
            "expected": {"direct_success": True},
            "safety": "mutating_fixture",
            "edit_domain": chain["edit_domain"],
            "blueprint_type": chain["blueprint_type"],
            "domain": chain["domain"],
            "description": chain["description"],
        })

    # --- edit_execute: practical v5 expansion tasks (creates delete-first as above) ---
    for spec in BLUEPRINT_ADDITIONAL_EDIT_EXECUTE_TASKS:
        task = _build_edit_execute_task(spec, spec.get("blueprint_type", ""), spec.get("domain", ""))
        task["id"] = next_id()
        tasks.append(task)

    # --- edit_execute: delete round-trips + set_pin_default (executed op_chains) ---
    for spec in build_delete_roundtrip_tasks() + build_set_pin_default_tasks():
        tasks.append({
            "id": next_id(),
            "category": "edit_execute",
            "namespace": "blueprint",
            "action": "op_chain",
            "tool": "blueprint_query",
            "asset_path": spec["asset_path"],
            "graph_name": spec.get("graph_name", "EventGraph"),
            "chain": spec["chain"],
            "verify": spec["verify"],
            "expected": {"direct_success": True},
            "safety": "mutating_fixture",
            "edit_domain": spec["edit_domain"],
            "blueprint_type": spec["blueprint_type"],
            "domain": spec["domain"],
            "workflow": spec["name"],
            "description": spec["description"],
        })

    # --- asset_authoring: high-ROI UE asset lifecycle tasks across common authoring namespaces ---
    for spec in ASSET_AUTHORING_TASKS:
        tasks.append({
            "id": next_id(),
            "category": "asset_authoring",
            "namespace": "mixed",
            "action": "op_chain",
            "tool": "monolith_query",
            "asset_path": ASSET_AUTHORING_ROOT,
            "chain": spec["chain"],
            "verify": spec["verify"],
            "expected": {"direct_success": True},
            "safety": "mutating_benchmark_assets",
            "edit_domain": spec.get("edit_domain", ""),
            "blueprint_type": "",
            "domain": spec.get("domain", ""),
            "workflow": spec["name"],
            "description": spec["description"],
        })

    # --- negative_compile (the only test that makes "compile is clean" falsifiable) ---
    for spec in build_negative_compile_tasks():
        spec["id"] = next_id()
        tasks.append(spec)

    # --- error_path (16) ---
    for spec in BLUEPRINT_ERROR_PATH_TASKS:
        err_tokens = spec.get("error_tokens", [])
        # P0-1: score on the SPECIFIC offending identifier (the unique NONEXISTENT_*/INVALID_*
        # token, always authored first in each error_tokens list), not the generic English words.
        # specific_tokens may be overridden per-spec; otherwise the first token is the identifier.
        specific = spec.get("specific_tokens") or ([err_tokens[0]] if err_tokens else [])
        tasks.append({
            "id": next_id(),
            "category": "error_path",
            "namespace": "blueprint",
            "action": spec["action"],
            "tool": "blueprint_query",
            "arguments": spec["arguments"],
            "expected": {"is_error": True, "error_tokens": err_tokens, "specific_tokens": specific},
            "safety": "read_only_invalid",
            "description": spec["description"],
        })

    # --- duplicate_reject (11) ---
    for spec in BLUEPRINT_DUPLICATE_REJECT_TASKS:
        action = spec["action"]
        a = spec["arguments"]
        ap = a.get("asset_path", "")
        # P0-5: the first create must be a CLEAN success, so DELETE the fixed-name entity first to
        # reset prior-run state (a leading delete reporting "not found" is tolerated). add_event_node
        # has no per-name remove -> allow_existing_first instead. The existing host-function setup
        # for add_local_variable is preserved and ordered before the local-variable delete.
        setup_list: List[Dict[str, Any]] = []
        if "setup_arguments" in spec:
            setup_list.append(spec["setup_arguments"])
        allow_existing_first = False
        if action == "add_variable":
            setup_list.append({"action": "remove_variable", "asset_path": ap, "name": a.get("name")})
        elif action == "add_function":
            setup_list.append({"action": "remove_function", "asset_path": ap, "name": a.get("function_name")})
        elif action == "add_component":
            setup_list.append({"action": "remove_component", "asset_path": ap, "component_name": a.get("component_name")})
        elif action == "add_event_dispatcher":
            setup_list.append({"action": "remove_event_dispatcher", "asset_path": ap, "dispatcher_name": a.get("name")})
        elif action == "add_local_variable":
            setup_list.append({"action": "remove_local_variable", "asset_path": ap,
                               "function_name": a.get("function_name"), "name": a.get("name")})
        elif action == "add_event_node":
            allow_existing_first = True  # no per-name remove for a custom event node
        task: Dict[str, Any] = {
            "id": next_id(),
            "category": "duplicate_reject",
            "namespace": "blueprint",
            "action": action,
            "tool": "blueprint_query",
            "arguments": a,
            "expected": {"is_error": True},
            "safety": "mutating_idempotency",
            "edit_domain": spec.get("edit_domain", ""),
            "blueprint_type": spec.get("blueprint_type", "Actor"),
            "domain": spec.get("domain", "gameplay"),
            "description": spec["description"],
        }
        if setup_list:
            task["setup_arguments"] = setup_list
        if allow_existing_first:
            task["allow_existing_first"] = True
        tasks.append(task)

    for i, task in enumerate(tasks, 1):
        task["id"] = f"BEB-{i:03d}"

    validate_task_integrity(tasks)
    # P0-7 hard guard: every weighted category MUST have at least one task, else adding it to
    # WEIGHTS would silently cap the composite (avg([])->0) while the sum-to-1.0 assert still passes.
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

    domain_counts: Dict[str, int] = {}
    for _, dom in BLUEPRINT_EDIT_ACTIONS:
        domain_counts[dom] = domain_counts.get(dom, 0) + 1
    edit_execute_counts = count_by((t for t in tasks if t.get("category") == "edit_execute"), "blueprint_type")

    manifest = {
        "benchmark": "BlueprintEditing",
        "description": (
            "Measures blueprint editing capability: type discovery, graph/variable reads, "
            "edit action schemas, read-back-verified edit execution, executed end-to-end "
            "workflows, high-ROI cross-domain asset authoring, graceful input-specific error "
            "handling, duplicate-name rejection, and explicit fixture lifecycle preflight"
        ),
        "primary_score": "blueprint_editing_score",
        "expected_namespace": "blueprint-plus-asset-authoring",
        "generated_at": utc_now(),
        "task_count": len(tasks),
        "category_counts": count_by(tasks, "category"),
        "edit_schema_domains": domain_counts,
        "blueprint_types_tested": [bp["type"] for bp in BP_TYPES],
        "workflows_tested": [wf["name"] for wf in BLUEPRINT_WORKFLOW_EXECUTE],
        "score_formula": score_formula_string(),
        "weights": dict(WEIGHTS),
        "base_edit_execute_tasks_per_type": 10,
        "additional_edit_execute_tasks": len(BLUEPRINT_ADDITIONAL_EDIT_EXECUTE_TASKS),
        "asset_authoring_tasks": len(ASSET_AUTHORING_TASKS),
        "edit_execute_tasks_per_type": edit_execute_counts,
        "fixture_paths": {bp["type"]: bp["path"] for bp in BP_TYPES},
        "preflight_command": "python Scripts/blueprint_editing_benchmark.py preflight --mcp-url http://localhost:9316/mcp",
        "setup_fixtures_command": "python Scripts/blueprint_editing_benchmark.py setup_fixtures --mcp-url http://localhost:9316/mcp",
        "score_dimensions": list(SCORE_DIMENSIONS),
        "catalog_version_verified": "v0.20.3-blueprint-139-actions-plus-asset-authoring",
        "task_file": display_path(tasks_path),
    }
    write_json(manifest_path, manifest)
    return manifest


# ---------------------------------------------------------------------------
# Fixture lifecycle preflight
# ---------------------------------------------------------------------------

def endpoint_preflight(url: str, timeout_s: float) -> Dict[str, Any]:
    response = mcp_call(url, "monolith_status", {}, timeout_s=timeout_s)
    failure_kind = classify_mcp_failure(response)
    ok = not failure_kind
    return {
        "ok": ok,
        "phase": "endpoint",
        "failure_kind": failure_kind,
        "message": "MCP endpoint reachable" if ok else "MCP endpoint did not return a usable monolith_status response",
        "status": result_data(response) if ok else None,
        "raw": str(response.get("raw", ""))[:300] if response.get("transport_error") or response.get("parse_error") else result_text(response)[:300],
    }


def project_index_preflight(url: str, timeout_s: float) -> Dict[str, Any]:
    response = mcp_call(url, "project_query", {
        "action": "search",
        "query": "BPB_TestActor",
        "include_content": False,
        "limit": 1,
    }, timeout_s=timeout_s)
    failure_kind = classify_mcp_failure(response)
    data = result_data(response) if not failure_kind else {}
    results = project_results(data)
    ok = (not failure_kind) and len(results) >= 1
    return {
        "ok": ok,
        "phase": "project_index",
        "failure_kind": failure_kind or ("" if ok else "project_index_empty"),
        "message": "ProjectIndex project.search is ready" if ok else "ProjectIndex project.search is not ready for type_discovery tasks",
        "query": "BPB_TestActor",
        "results_count": len(results),
        "snippet": str(response.get("raw", ""))[:300] if response.get("transport_error") else result_text(response)[:300],
    }


def fixture_readiness_preflight(url: str, timeout_s: float, require_fixtures: bool = True) -> Dict[str, Any]:
    endpoint = endpoint_preflight(url, timeout_s)
    if not endpoint["ok"] or not require_fixtures:
        return {
            "ok": endpoint["ok"],
            "phase": endpoint["phase"],
            "failure_kind": endpoint["failure_kind"],
            "message": endpoint["message"],
            "endpoint": endpoint,
            "project_index": None,
            "fixtures": [],
        }

    project_index = project_index_preflight(url, timeout_s)
    if not project_index["ok"]:
        return {
            "ok": False,
            "phase": project_index["phase"],
            "failure_kind": project_index["failure_kind"],
            "message": project_index["message"],
            "endpoint": endpoint,
            "project_index": project_index,
            "fixtures": [],
            "first_failure": project_index,
        }

    fixtures: List[Dict[str, Any]] = []
    first_failure: Optional[Dict[str, Any]] = None

    for bp in BP_TYPES:
        bp_type = bp["type"]
        asset_path = bp["path"]
        checks: List[Dict[str, Any]] = []

        info_resp = mcp_call(url, "blueprint_query", {
            "action": "get_blueprint_info", "asset_path": asset_path,
        }, timeout_s=timeout_s)
        info_failure = classify_mcp_failure(info_resp)
        if info_failure:
            kind = "transport_error" if info_failure == "transport_error" else "fixture_missing_or_invalid"
            checks.append({
                "name": "get_blueprint_info",
                "ok": False,
                "failure_kind": kind,
                "snippet": str(info_resp.get("raw", ""))[:200] if info_failure == "transport_error" else result_text(info_resp)[:200],
            })
            fixture_ok = False
        else:
            checks.append({"name": "get_blueprint_info", "ok": True})
            fixture_ok = True

        if fixture_ok and bp_type != "Interface":
            vars_resp = mcp_call(url, "blueprint_query", {
                "action": "get_variables", "asset_path": asset_path,
            }, timeout_s=timeout_s)
            var_failure = classify_mcp_failure(vars_resp)
            vars_ok = (not var_failure) and _response_text_contains_any(vars_resp, bp["fixture_vars"])
            checks.append({
                "name": "get_variables",
                "ok": vars_ok,
                "failure_kind": var_failure or ("" if vars_ok else "fixture_contract_missing"),
                "expected_any": bp["fixture_vars"],
                "snippet": result_text(vars_resp)[:200],
            })
            fixture_ok = fixture_ok and vars_ok

        if fixture_ok:
            funcs = FIXTURE_FUNCTIONS_BY_TYPE.get(bp_type, [])
            if funcs:
                funcs_resp = mcp_call(url, "blueprint_query", {
                    "action": "get_functions", "asset_path": asset_path,
                }, timeout_s=timeout_s)
                func_failure = classify_mcp_failure(funcs_resp)
                funcs_ok = (not func_failure) and _response_text_contains_any(funcs_resp, funcs)
                checks.append({
                    "name": "get_functions",
                    "ok": funcs_ok,
                    "failure_kind": func_failure or ("" if funcs_ok else "fixture_contract_missing"),
                    "expected_any": funcs,
                    "snippet": result_text(funcs_resp)[:200],
                })
                fixture_ok = fixture_ok and funcs_ok

        fixture = {
            "blueprint_type": bp_type,
            "asset_path": asset_path,
            "ok": fixture_ok,
            "checks": checks,
        }
        fixtures.append(fixture)
        if not fixture_ok and first_failure is None:
            failed_check = next((c for c in checks if not c.get("ok")), {})
            first_failure = {
                "phase": "fixtures",
                "failure_kind": failed_check.get("failure_kind") or "fixture_readiness_failed",
                "message": f"Fixture preflight failed for {bp_type} at {asset_path}",
                "fixture": fixture,
            }

    ok = first_failure is None
    return {
        "ok": ok,
        "phase": "ready" if ok else "fixtures",
        "failure_kind": "" if ok else str(first_failure.get("failure_kind", "fixture_readiness_failed")),
        "message": "All BlueprintEditing fixtures are ready" if ok else str(first_failure.get("message", "Fixture preflight failed")),
        "endpoint": endpoint,
        "project_index": project_index,
        "fixtures": fixtures,
        "first_failure": first_failure,
    }


def print_preflight_summary(preflight: Dict[str, Any]) -> None:
    status = "ok" if preflight.get("ok") else "FAILED"
    print(f"preflight: {status} phase={preflight.get('phase')} failure_kind={preflight.get('failure_kind', '')}", flush=True)
    if preflight.get("message"):
        print(f"  {preflight['message']}", flush=True)
    project_index = preflight.get("project_index")
    if isinstance(project_index, dict):
        p_status = "ok" if project_index.get("ok") else "FAILED"
        print(f"  [{p_status}] project.search {project_index.get('query')} results={project_index.get('results_count', 0)}", flush=True)
    for fixture in preflight.get("fixtures", []):
        f_status = "ok" if fixture.get("ok") else "FAILED"
        print(f"  [{f_status}] {fixture.get('blueprint_type')} {fixture.get('asset_path')}", flush=True)
        for check in fixture.get("checks", []):
            c_status = "ok" if check.get("ok") else "FAIL"
            kind = f" ({check.get('failure_kind')})" if check.get("failure_kind") else ""
            print(f"       {c_status} {check.get('name')}{kind}", flush=True)


# ---------------------------------------------------------------------------
# Setup Fixtures
# ---------------------------------------------------------------------------

def setup_fixtures(url: str, timeout_s: float) -> Dict[str, Any]:
    """Create or verify benchmark fixture blueprints at /Game/Benchmarks/.

    Each fixture blueprint is created with ``create_blueprint``, then populated
    with the fixture variables defined in ``BP_TYPES``, and compiled.

    Safe to run multiple times: if a fixture already exists the step is skipped
    and reported as ``already_exists`` rather than failing.
    """
    preflight = fixture_readiness_preflight(url, timeout_s, require_fixtures=False)
    print_preflight_summary(preflight)
    if not preflight["ok"]:
        return {
            "preflight": preflight,
            "fixtures": [],
            "total": len(BP_TYPES),
            "succeeded": 0,
            "ready": False,
            "failure_kind": preflight.get("failure_kind", "transport_error"),
        }

    results: List[Dict[str, Any]] = []

    for bp in BP_TYPES:
        bp_type = bp["type"]
        asset_path = bp["path"]
        create_params = FIXTURE_CREATE_PARAMS[bp_type]
        steps: List[Dict[str, Any]] = []

        # 1. create_blueprint
        create_resp = mcp_call(url, "blueprint_query", {
            "action": "create_blueprint",
            "save_path": asset_path,
            "parent_class": create_params["parent_class"],
            "blueprint_type": create_params["blueprint_type"],
        }, timeout_s=timeout_s)
        create_data = result_data(create_resp)
        create_is_error = _is_error(create_resp)
        already_exists = create_is_error and "already exists" in result_text(create_resp).lower()
        create_ok = not create_resp.get("transport_error") and not create_resp.get("parse_error")
        steps.append({
            "action": "create_blueprint",
            "success": create_ok and (not create_is_error or already_exists),
            "already_exists": already_exists,
            "is_error": create_is_error,
            "failure_kind": classify_mcp_failure(create_resp),
            "snippet": result_text(create_resp)[:200],
        })

        # 2. Add fixture variables (skip for Interface which has no class variables).
        if bp_type != "Interface":
            # Explicit per-variable types (FIXTURE_VAR_TYPES) match the test_blueprints.md
            # contract exactly (ActorTag=FName/name, DisplayText=FText/text, ...).
            for var_name, var_type in FIXTURE_VAR_TYPES.get(bp_type, {}).items():
                var_resp = mcp_call(url, "blueprint_query", {
                    "action": "add_variable",
                    "asset_path": asset_path,
                    "name": var_name,
                    "type": var_type,
                }, timeout_s=timeout_s)
                var_is_error = _is_error(var_resp)
                var_already = var_is_error and "already" in result_text(var_resp).lower()
                steps.append({
                    "action": f"add_variable:{var_name}:{var_type}",
                    "success": not var_resp.get("transport_error") and (not var_is_error or var_already),
                    "already_exists": var_already,
                    "is_error": var_is_error,
                    "failure_kind": classify_mcp_failure(var_resp),
                    "snippet": result_text(var_resp)[:100],
                })

        # 3. Add fixture function stubs for read/preflight contract checks.
        for fn_name in FIXTURE_FUNCTIONS_BY_TYPE.get(bp_type, []):
            fn_resp = mcp_call(url, "blueprint_query", {
                "action": "add_function", "asset_path": asset_path, "function_name": fn_name,
            }, timeout_s=timeout_s)
            fn_is_error = _is_error(fn_resp)
            fn_already = fn_is_error and "exist" in result_text(fn_resp).lower()
            steps.append({
                "action": f"add_function:{fn_name}",
                "success": not fn_resp.get("transport_error") and (not fn_is_error or fn_already),
                "already_exists": fn_already,
                "is_error": fn_is_error,
                "failure_kind": classify_mcp_failure(fn_resp),
                "snippet": result_text(fn_resp)[:100],
            })

        # 4. compile_blueprint — must be a CLEAN compile (0 errors), inspected from the payload.
        compile_resp = mcp_call(url, "blueprint_query", {
            "action": "compile_blueprint", "asset_path": asset_path,
        }, timeout_s=timeout_s)
        compile_clean, compile_detail = _compile_is_clean(compile_resp)
        steps.append({
            "action": "compile_blueprint",
            "success": compile_clean,
            "compile_detail": compile_detail,
            "failure_kind": classify_mcp_failure(compile_resp),
            "snippet": result_text(compile_resp)[:150],
        })

        # 5. save_asset — PERSIST the fixture edits to disk. Without this, newly-added variables/
        # functions live only in the editor's memory; if the headless editor crashes and reboots
        # before the scored run (a known -nullrhi instability), the fixtures revert to their
        # on-disk state and read/edit tasks against the new entities fail. save_asset is idempotent.
        save_resp = mcp_call(url, "blueprint_query", {
            "action": "save_asset", "asset_path": asset_path,
        }, timeout_s=timeout_s)
        steps.append({
            "action": "save_asset",
            "success": not save_resp.get("transport_error") and not save_resp.get("parse_error")
                       and not _is_error(save_resp),
            "failure_kind": classify_mcp_failure(save_resp),
            "snippet": result_text(save_resp)[:120],
        })

        overall_ok = all(s["success"] for s in steps)
        results.append({
            "blueprint_type": bp_type,
            "asset_path": asset_path,
            "overall_success": overall_ok,
            "steps": steps,
        })
        status = "ok" if overall_ok else "FAILED"
        print(f"  [{status}] {bp_type} → {asset_path}", flush=True)
        for s in steps:
            flag = "ok" if s["success"] else "FAIL"
            extra = " (already existed)" if s.get("already_exists") else ""
            kind = f" [{s.get('failure_kind')}]" if s.get("failure_kind") else ""
            print(f"       {flag} {s['action']}{extra}{kind}", flush=True)

    total_ok = sum(1 for r in results if r["overall_success"])
    readiness = fixture_readiness_preflight(url, timeout_s, require_fixtures=True)
    print_preflight_summary(readiness)
    print(f"\nsetup_fixtures: {total_ok}/{len(results)} fixtures ready", flush=True)
    return {
        "preflight": preflight,
        "post_setup_readiness": readiness,
        "fixtures": results,
        "total": len(results),
        "succeeded": total_ok,
        "ready": total_ok == len(results) and readiness.get("ok", False),
        "failure_kind": "" if total_ok == len(results) and readiness.get("ok", False) else readiness.get("failure_kind", "fixture_edit_failure"),
    }


# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------

def _normalize_resource_path(value: str) -> Optional[str]:
    if not value.startswith("/Game/"):
        return None
    # Object paths can include a subobject/export suffix; the package path is the lock boundary.
    return value.split(".", 1)[0]


def _collect_resource_paths(value: Any, *, skip_keys: Optional[Set[str]] = None) -> Set[str]:
    skip_keys = skip_keys or set()
    paths: Set[str] = set()
    if isinstance(value, str):
        normalized = _normalize_resource_path(value)
        if normalized:
            paths.add(normalized)
    elif isinstance(value, list):
        for item in value:
            paths.update(_collect_resource_paths(item, skip_keys=skip_keys))
    elif isinstance(value, dict):
        for key, item in value.items():
            if str(key) in skip_keys:
                continue
            paths.update(_collect_resource_paths(item, skip_keys=skip_keys))
    return paths


GLOBAL_TOOL_LOCKS = {
    # These namespaces share editor graph loading and ProjectIndex state. They need the same lock:
    # separate per-tool locks still let project.search overlap Blueprint graph reads, which is not
    # reentrant in the headless editor.
    "blueprint_query": "tool:editor_graph_index",
    "project_query": "tool:editor_graph_index",
}
ASSET_AUTHORING_GLOBAL_LOCK = "tool:editor_graph_index"


def _collect_tool_names(task: Dict[str, Any]) -> Set[str]:
    tools: Set[str] = set()
    if task.get("tool"):
        tools.add(str(task["tool"]))
    for block_name in ("chain", "verify", "setup_chain", "cleanup_chain"):
        for step in task.get(block_name, []) or []:
            if isinstance(step, dict) and step.get("tool"):
                tools.add(str(step["tool"]))
    return tools


def task_resource_keys(task: Dict[str, Any]) -> List[str]:
    """Return package-level locks needed to run a task safely with other tasks.

    Read/schema/catalog tasks stay lock-free. Mutating Blueprint and asset-authoring tasks lock the
    concrete packages they touch, so a parallel run can make progress without racing two edits on the
    same fixture or deleting an asset while another task is verifying it.
    """
    category = str(task.get("category", ""))
    paths: Set[str] = set()
    skip_keys = {"allowed_prefixes"}
    lock_keys: Set[str] = {
        GLOBAL_TOOL_LOCKS[tool] for tool in _collect_tool_names(task)
        if tool in GLOBAL_TOOL_LOCKS
    }

    if category in {"read_schema", "edit_schema"}:
        return sorted(lock_keys)

    if category == "asset_authoring":
        # Live editor asset creation/save/delete touches global package, AssetRegistry, source-control,
        # graph compilation, and factory state across namespaces. Per-package locks are insufficient:
        # authoring chains must not overlap Blueprint/project graph reads or other editor mutations.
        lock_keys.add(ASSET_AUTHORING_GLOBAL_LOCK)
        for block_name in ("chain", "verify"):
            for step in task.get(block_name, []) or []:
                if isinstance(step, dict):
                    paths.update(_collect_resource_paths(step.get("args", {}), skip_keys=skip_keys))
    else:
        if task.get("asset_path"):
            paths.update(_collect_resource_paths(task.get("asset_path"), skip_keys=skip_keys))
        for key in ("arguments", "setup_arguments", "setup_chain", "cleanup_chain",
                    "compile_args", "verify", "verify_connection", "verify_disconnection"):
            paths.update(_collect_resource_paths(task.get(key), skip_keys=skip_keys))

    return sorted(lock_keys | paths)


def _score_task_with_locks(
    url: str,
    task: Dict[str, Any],
    timeout_s: float,
    locks_by_key: Dict[str, threading.Lock],
    resource_keys: Optional[List[str]] = None,
) -> Tuple[Dict[str, Any], List[str]]:
    keys = resource_keys if resource_keys is not None else task_resource_keys(task)
    acquired: List[threading.Lock] = []
    try:
        for key in keys:
            lock = locks_by_key[key]
            lock.acquire()
            acquired.append(lock)
        return score_task(url, task, timeout_s), keys
    finally:
        for lock in reversed(acquired):
            lock.release()


def run_benchmark(
    url: str,
    tasks_path: pathlib.Path,
    output_dir: pathlib.Path,
    label: str,
    timeout_s: float,
    jobs: int = 1,
) -> Dict[str, Any]:
    tasks_path = resolve_plugin_path(tasks_path)
    tasks = load_jsonl(tasks_path)
    output_dir.mkdir(parents=True, exist_ok=True)
    status_response = mcp_call(url, "monolith_status", {}, timeout_s=timeout_s)
    if status_response.get("transport_error") or status_response.get("parse_error") or _is_error(status_response):
        raise RuntimeError(
            "monolith_status failed before benchmark execution: "
            f"{str(status_response.get('raw') or result_text(status_response))[:300]}"
        )
    status = result_data(status_response)
    benchmark_inputs = build_benchmark_inputs("BlueprintEditing", tasks_path=tasks_path, mcp_status=status)

    rows: List[Dict[str, Any]] = []
    per_task_jsonl = output_dir / "per_task.jsonl"
    if per_task_jsonl.exists():
        per_task_jsonl.unlink()

    jobs = max(1, int(jobs))
    if jobs == 1:
        for index, task in enumerate(tasks, 1):
            row = score_task(url, task, timeout_s)
            row["task_index"] = index
            rows.append(row)
            with per_task_jsonl.open("a", encoding="utf-8", newline="\n") as handle:
                handle.write(json.dumps(row, ensure_ascii=False, sort_keys=True))
                handle.write("\n")
            print(f"[{index}/{len(tasks)}] {row['task_id']} category={row['category']} success={row['direct_success']}", flush=True)
            if index == 1 or index == len(tasks) or index % 10 == 0:
                partial = aggregate(label, status, tasks[:index], rows, warn_missing_categories=False)
                partial["completed_task_count"] = index
                partial["total_task_count"] = len(tasks)
                partial["execution"] = {"mode": "sequential", "jobs": 1}
                attach_benchmark_inputs(partial, benchmark_inputs)
                write_json(output_dir / "partial_summary.json", partial)
    else:
        task_keys_by_index = [task_resource_keys(task) for task in tasks]
        locks_by_key: Dict[str, threading.Lock] = {
            key: threading.Lock()
            for key in sorted({key for keys in task_keys_by_index for key in keys})
        }
        rows_by_index: List[Optional[Dict[str, Any]]] = [None] * len(tasks)
        completed = 0
        print(f"Running {len(tasks)} tasks with jobs={jobs} and package-level resource locks", flush=True)
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
            future_to_index = {
                executor.submit(
                    _score_task_with_locks,
                    url,
                    task,
                    timeout_s,
                    locks_by_key,
                    task_keys_by_index[index - 1],
                ): index
                for index, task in enumerate(tasks, 1)
            }
            for future in concurrent.futures.as_completed(future_to_index):
                index = future_to_index[future]
                try:
                    row, resource_keys = future.result()
                except Exception as exc:  # noqa: BLE001 - benchmark rows should record runner faults.
                    task = tasks[index - 1]
                    row = {
                        "task_id": task.get("id"),
                        "category": task.get("category"),
                        "namespace": task.get("namespace"),
                        "action": task.get("action"),
                        "blueprint_type": task.get("blueprint_type", ""),
                        "domain": task.get("domain", ""),
                        "edit_domain": task.get("edit_domain", ""),
                        "workflow": task.get("workflow", ""),
                        "direct_success": False,
                        "planning_signals": False,
                        "evidence": {"runner_exception": type(exc).__name__, "message": str(exc)},
                        "transport_error": False,
                        "transport_error_raw": "",
                        "response_is_error": True,
                        "response_text": str(exc)[:500],
                    }
                    resource_keys = task_resource_keys(task)
                row["task_index"] = index
                row["resource_keys"] = resource_keys
                rows_by_index[index - 1] = row
                completed += 1
                with per_task_jsonl.open("a", encoding="utf-8", newline="\n") as handle:
                    handle.write(json.dumps(row, ensure_ascii=False, sort_keys=True))
                    handle.write("\n")
                print(f"[{completed}/{len(tasks)}] #{index} {row['task_id']} category={row['category']} success={row['direct_success']}", flush=True)
                if completed == 1 or completed == len(tasks) or completed % 10 == 0:
                    partial_rows = [r for r in rows_by_index if r is not None]
                    partial_tasks = [tasks[i] for i, r in enumerate(rows_by_index) if r is not None]
                    partial = aggregate(label, status, partial_tasks, partial_rows, warn_missing_categories=False)
                    partial["completed_task_count"] = completed
                    partial["total_task_count"] = len(tasks)
                    partial["execution"] = {
                        "mode": "parallel",
                        "jobs": jobs,
                        "resource_lock_count": len(locks_by_key),
                    }
                    attach_benchmark_inputs(partial, benchmark_inputs)
                    write_json(output_dir / "partial_summary.json", partial)
        rows = [r for r in rows_by_index if r is not None]

    summary = aggregate(label, status, tasks, rows)
    summary["execution"] = {
        "mode": "parallel" if jobs > 1 else "sequential",
        "jobs": jobs,
    }
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
    # Built from SCORE_DIMENSIONS so every scored dimension (incl. duplicate_reject_rate and
    # workflow_execute_rate) always renders — a scored dimension can never be silently omitted.
    # Union with any extra rate keys present in the data keeps old/renamed baselines visible.
    metrics = ["blueprint_editing_score"] + list(SCORE_DIMENSIONS)
    for extra in list(current.get("metrics", {})) + list(baseline.get("metrics", {})):
        if extra.endswith("_rate") and extra not in metrics:
            metrics.append(extra)
    lines = [
        "# Monolith BlueprintEditing Benchmark Comparison",
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
    lines.append("Higher is better for all metrics.")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)

    gen = sub.add_parser("generate", help="Generate static task fixtures")
    gen.add_argument("--tasks", type=pathlib.Path, default=DEFAULT_TASKS)
    gen.add_argument("--manifest", type=pathlib.Path, default=DEFAULT_MANIFEST)

    sf_cmd = sub.add_parser("setup_fixtures",
                             help="Create benchmark fixture blueprints at /Game/Benchmarks/ via MCP")
    sf_cmd.add_argument("--mcp-url", default=DEFAULT_MCP_URL)
    sf_cmd.add_argument("--request-timeout-s", type=float, default=30.0)

    pf_cmd = sub.add_parser("preflight",
                             help="Check MCP endpoint and BlueprintEditing fixture readiness before setup/run")
    pf_cmd.add_argument("--mcp-url", default=DEFAULT_MCP_URL)
    pf_cmd.add_argument("--request-timeout-s", type=float, default=12.0)
    pf_cmd.add_argument("--endpoint-only", action="store_true",
                        help="Only check monolith_status transport; do not require fixture assets")

    run_cmd = sub.add_parser("run", help="Run tasks against a live MCP endpoint and score results")
    run_cmd.add_argument("--mcp-url", default=DEFAULT_MCP_URL)
    run_cmd.add_argument("--tasks", type=pathlib.Path, default=DEFAULT_TASKS)
    run_cmd.add_argument("--output-dir", type=pathlib.Path, required=True)
    run_cmd.add_argument("--label", required=True)
    run_cmd.add_argument("--request-timeout-s", type=float, default=12.0)
    run_cmd.add_argument("--jobs", type=int, default=1,
                         help="Number of parallel task workers; package-level locks serialize tasks that touch the same /Game asset")
    run_cmd.add_argument("--skip-preflight", action="store_true",
                         help="Compatibility escape hatch: run without fixture readiness preflight")

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
        print("Creating benchmark fixture blueprints...", flush=True)
        result = setup_fixtures(args.mcp_url, args.request_timeout_s)
        sys.stdout.buffer.write((json.dumps(result, indent=2, ensure_ascii=False) + "\n").encode("utf-8"))
        return 0 if result.get("ready") else 1

    if args.cmd == "preflight":
        result = fixture_readiness_preflight(
            args.mcp_url,
            args.request_timeout_s,
            require_fixtures=not args.endpoint_only,
        )
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
        summary = run_benchmark(args.mcp_url, args.tasks, args.output_dir, args.label, args.request_timeout_s, args.jobs)
        sys.stdout.buffer.write((json.dumps(summary, indent=2, ensure_ascii=False) + "\n").encode("utf-8"))
        return 0

    if args.cmd == "compare":
        comparison = compare_runs(args.baseline, args.current, args.output_dir)
        print(json.dumps({"output_dir": str(args.output_dir), "deltas": comparison["deltas"]}, indent=2))
        return 0

    return 2


if __name__ == "__main__":
    raise SystemExit(main())
