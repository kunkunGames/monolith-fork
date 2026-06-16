#!/usr/bin/env python3
"""
Monolith MCP BlueprintEditing benchmark.

Measures the MCP server's capability to support blueprint editing workflows
across 7 Blueprint types: Actor, Character, Widget, AnimInstance, GameplayAbility,
ActorComponent, and Interface.

Eight task categories (124 tasks total):
  type_discovery        - project.search for BP type prefixes and class names
  graph_read            - blueprint.list_graphs / get_graph_data / get_graph_summary
  variable_read         - blueprint.get_variables with options
  read_schema           - monolith_discover schema for 15 read actions (lenient)
  edit_schema           - monolith_discover schema for 38 edit actions (strict: isError fails)
  workflow_completeness - verify workflow steps present in catalog actions[].action
  edit_execute          - call real edit actions against fixture assets (strict: requires success)
  error_path            - send invalid inputs and verify structured isError response

Scoring formula (weights sum to 1.0):
  blueprint_editing_score =
    0.25 * edit_execute_rate
    + 0.20 * edit_schema_rate
    + 0.15 * graph_read_rate
    + 0.15 * variable_read_rate
    + 0.10 * error_path_rate
    + 0.07 * read_schema_rate
    + 0.05 * type_discovery_rate
    + 0.03 * workflow_completeness_rate

Action names verified against live blueprint namespace catalog (138 actions, v0.20.2).
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


DEFAULT_MCP_URL = "http://localhost:9316/mcp"
DEFAULT_TASKS = pathlib.Path("Benchmarks/BlueprintEditing/tasks.jsonl")
DEFAULT_MANIFEST = pathlib.Path("Benchmarks/BlueprintEditing/manifest.json")
DEFAULT_RESULTS_ROOT = pathlib.Path("Saved/Monolith/Benchmarks/BlueprintEditing")

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
     "default_graph": "EventGraph", "fixture_vars": ["ComponentID", "bIsActive"],
     "fixture_graphs": ["EventGraph"]},
    {"type": "Interface", "domain": "interface", "path": "/Game/Benchmarks/BPI_TestInterface",
     "default_graph": None, "fixture_vars": [], "fixture_graphs": []},
]

# (query, bp_type, domain, require_results)
# require_results=True: task fails if response has 0 results (prefix searches that almost always match).
# require_results=False: any non-error response passes (text searches that may find 0 in some projects).
TYPE_SEARCH_QUERIES: List[Tuple[str, str, str, bool]] = [
    ("BP_", "Actor", "gameplay", True),
    ("WBP_", "Widget", "ui", True),
    ("ABP_", "AnimInstance", "animation", True),
    ("GA_", "GameplayAbility", "ability", True),
    ("BPI_", "Interface", "interface", False),        # project may have 0 BPI_ assets
    ("Actor Blueprint", "Actor", "gameplay", True),
    ("Character Blueprint", "Character", "gameplay", True),
    ("UserWidget Blueprint", "Widget", "ui", False),  # description text; may not match
    ("AnimInstance Blueprint", "AnimInstance", "animation", False),
    ("GameplayAbility Blueprint", "GameplayAbility", "ability", False),
    ("ActorComponent Blueprint", "ActorComponent", "component", False),
    ("Blueprint Pawn", "Pawn", "gameplay", False),
    ("AI Controller Blueprint", "AIController", "ai", True),
    ("Blueprint Interface", "Interface", "interface", False),
]

# All 15 names verified against live blueprint namespace catalog (monolith_discover mode=actions).
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
]

# All 38 names verified against live blueprint namespace catalog.
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
]

# Verified action names from the live blueprint namespace catalog.
# requires_actions must match catalog actions[].action values exactly.
BLUEPRINT_WORKFLOWS = [
    {
        "name": "create_function",
        "description": "Create a new function, add nodes, connect pins, compile",
        "requires_actions": ["add_function", "add_node", "connect_pins", "compile_blueprint"],
    },
    {
        "name": "add_variable",
        "description": "Add a variable, set its type and default value",
        "requires_actions": ["add_variable", "set_variable_type", "set_variable_defaults"],
    },
    {
        "name": "add_component",
        "description": "Add a component, configure properties, reparent to hierarchy",
        "requires_actions": ["add_component", "set_component_property", "reparent_component", "compile_blueprint"],
    },
    {
        "name": "implement_interface",
        "description": "Implement interface, add required functions, compile",
        "requires_actions": ["implement_interface", "add_function", "compile_blueprint"],
    },
    {
        "name": "event_dispatcher_setup",
        "description": "Create event dispatcher, configure its params",
        "requires_actions": ["add_event_dispatcher", "set_event_dispatcher_params", "compile_blueprint"],
    },
]


# Inputs designed to provoke structured isError responses. Scored inverted:
# pass = server returns isError (graceful rejection), fail = transport error (crash).
# All action names use real catalog names so the test exercises actual error handling,
# not "unknown action" rejection.
BLUEPRINT_ERROR_PATH_TASKS: List[Dict[str, Any]] = [
    {
        "action": "get_graph_data", "description": "get_graph_data on non-existent asset",
        "arguments": {"action": "get_graph_data",
                      "asset_path": "/Game/Benchmarks/NONEXISTENT_BenchBP_ZZZZZ"},
    },
    {
        "action": "compile_blueprint", "description": "compile_blueprint on non-existent asset",
        "arguments": {"action": "compile_blueprint",
                      "asset_path": "/Game/Benchmarks/NONEXISTENT_BenchBP_ZZZZZ"},
    },
    {
        "action": "get_variables", "description": "get_variables on non-existent asset",
        "arguments": {"action": "get_variables",
                      "asset_path": "/Game/Benchmarks/NONEXISTENT_BenchBP_ZZZZZ"},
    },
    {
        "action": "list_graphs", "description": "list_graphs on non-existent asset",
        "arguments": {"action": "list_graphs",
                      "asset_path": "/Game/Benchmarks/NONEXISTENT_BenchBP_ZZZZZ"},
    },
    {
        "action": "connect_pins", "description": "connect_pins with invalid node IDs",
        "arguments": {"action": "connect_pins",
                      "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "graph_name": "EventGraph",
                      "source_node_id": "INVALID_NODE_AAAA_ZZZZ",
                      "source_pin": "exec",
                      "target_node_id": "INVALID_NODE_BBBB_ZZZZ",
                      "target_pin": "execute"},
    },
    {
        "action": "remove_variable", "description": "remove non-existent variable",
        "arguments": {"action": "remove_variable",
                      "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "name": "NONEXISTENT_VAR_BENCH_ZZZZ"},
    },
    {
        "action": "rename_function", "description": "rename non-existent function",
        "arguments": {"action": "rename_function",
                      "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "old_name": "NONEXISTENT_FUNC_BENCH_ZZZZ",
                      "new_name": "AnotherName"},
    },
    {
        "action": "remove_component", "description": "remove non-existent component",
        "arguments": {"action": "remove_component",
                      "asset_path": "/Game/Benchmarks/BPB_TestActor",
                      "component_name": "NONEXISTENT_COMP_BENCH_ZZZZ"},
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

# 10 type-specific edit_execute tasks per Blueprint type (70 total).
# All action names verified against live blueprint namespace catalog v0.20.2.
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
        {"action": "find_variable_references", "edit_domain": "variable",
         "arguments": {"action": "find_variable_references", "asset_path": "/Game/Benchmarks/ABP_TestAnim",
                       "variable_name": "Speed"},
         "description": "Find all graph references to Speed fixture variable (read, requires fixture)"},
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
        {"action": "find_variable_references", "edit_domain": "variable",
         "arguments": {"action": "find_variable_references", "asset_path": "/Game/Benchmarks/BC_TestComponent",
                       "variable_name": "ComponentID"},
         "description": "Find all graph references to ComponentID fixture variable (read, requires fixture)"},
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
        {"action": "get_function_signature", "edit_domain": "function",
         "arguments": {"action": "get_function_signature", "asset_path": "/Game/Benchmarks/BPI_TestInterface",
                       "function_name": "BenchIFaceGetHealth"},
         "description": "Read BenchIFaceGetHealth signature (read-back, requires fixture+function)"},
        {"action": "add_function", "edit_domain": "function",
         "arguments": {"action": "add_function", "asset_path": "/Game/Benchmarks/BPI_TestInterface",
                       "function_name": "BenchIFaceOnDamaged"},
         "description": "Add BenchIFaceOnDamaged function stub to Interface"},
        {"action": "set_function_params", "edit_domain": "function",
         "arguments": {"action": "set_function_params", "asset_path": "/Game/Benchmarks/BPI_TestInterface",
                       "function_name": "BenchIFaceOnDamaged",
                       "inputs": [{"name": "Amount", "type": "float"}]},
         "description": "Set typed float input on BenchIFaceOnDamaged stub"},
        {"action": "get_functions", "edit_domain": "function",
         "arguments": {"action": "get_functions", "asset_path": "/Game/Benchmarks/BPI_TestInterface"},
         "description": "Read all function stubs defined in BPI_TestInterface (read-back)"},
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


def project_results(data: Dict[str, Any]) -> List[Dict[str, Any]]:
    for key in ("results", "hits", "items", "assets"):
        value = data.get(key)
        if isinstance(value, list):
            return [r for r in value if isinstance(r, dict)]
    return []


def _is_error(response: Dict[str, Any]) -> bool:
    """True if the MCP result carries isError=True (structured server-side error)."""
    return bool(result_payload(response).get("isError"))


def _response_text_contains_any(response: Dict[str, Any], tokens: List[str]) -> bool:
    """True if any non-empty token from tokens appears in the response text."""
    text = result_text(response)
    return any(tok and tok in text for tok in tokens)


# ---------------------------------------------------------------------------
# Task scoring
# ---------------------------------------------------------------------------

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

    setup_args = task.get("setup_arguments")
    if isinstance(setup_args, dict):
        mcp_call(url, tool, dict(setup_args), timeout_s=timeout_s)

    first = mcp_call(url, tool, dict(args), timeout_s=timeout_s)
    second = mcp_call(url, tool, dict(args), timeout_s=timeout_s)

    transport_error = bool(second.get("transport_error"))
    parse_error = bool(second.get("parse_error"))
    server_handled = not transport_error and not parse_error
    second_is_error = _is_error(second)
    # Pass = the second identical creation is gracefully refused with isError.
    direct_success = server_handled and second_is_error

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
            "first_call_is_error": _is_error(first),
            "second_call_handled": server_handled,
            "second_call_is_error": second_is_error,
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
    if category == "duplicate_reject":
        return _score_duplicate_reject(url, task, timeout_s)

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
            if action == "get_variables" and fixture_vars:
                content_ok = _response_text_contains_any(response, fixture_vars)
                content_check_applied = True
            elif action == "list_graphs" and expected_graph:
                content_ok = _response_text_contains_any(response, [expected_graph])
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

    elif category == "workflow_completeness":
        requires = task.get("expected", {}).get("requires_actions", [])
        # The live catalog returns actions[].action (not actions[].name).
        action_list = data.get("actions", []) if isinstance(data, dict) else []
        registered = {a.get("action", "") for a in action_list if isinstance(a, dict)}
        registered.discard("")  # remove empty-string sentinel from entries missing "action" key
        if registered:
            missing = [a for a in requires if a not in registered]
            used_fallback = False
        else:
            # Fallback: substring search in response text (less precise, flagged in evidence).
            response_text = result_text(response)
            missing = [a for a in requires if a not in response_text]
            used_fallback = True
        direct_success = len(missing) == 0 and not transport_error
        evidence = {
            "workflow": task.get("workflow", ""),
            "requires_actions": requires,
            "missing_actions": missing,
            "registered_count": len(registered),
            "used_text_fallback": used_fallback,
        }

    elif category == "edit_execute":
        # Strict: requires actual execution success (no isError, no transport error).
        # Fails if fixture assets do not exist — this is intentional: real capability check.
        # "already exists" / "already" responses count as success: the item IS present,
        # which means the server capability works (idempotent across repeated benchmark runs).
        already_exists = server_is_error and "already" in result_text(response).lower()
        direct_success = server_handled and (not server_is_error or already_exists)
        evidence = {
            "server_handled": server_handled,
            "is_error": server_is_error,
            "already_exists": already_exists,
            "edit_domain": task.get("edit_domain", ""),
            "response_snippet": result_text(response)[:300],
        }

    elif category == "error_path":
        # Inverted: pass = server returned a structured isError (graceful rejection).
        # fail = transport error (crash/unreachable) or silent success on invalid input.
        direct_success = server_handled and server_is_error
        evidence = {
            "server_handled": server_handled,
            "returned_is_error": server_is_error,
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

def aggregate(label: str, status: Dict[str, Any], tasks: List[Dict[str, Any]], rows: List[Dict[str, Any]]) -> Dict[str, Any]:
    def rate(cat: str) -> float:
        cat_rows = [r for r in rows if r["category"] == cat]
        return avg([1.0 if r["direct_success"] else 0.0 for r in cat_rows])

    type_discovery_rate = rate("type_discovery")
    graph_read_rate = rate("graph_read")
    variable_read_rate = rate("variable_read")
    read_schema_rate = rate("read_schema")
    edit_schema_rate = rate("edit_schema")
    workflow_completeness_rate = rate("workflow_completeness")
    edit_execute_rate = rate("edit_execute")
    error_path_rate = rate("error_path")
    duplicate_reject_rate = rate("duplicate_reject")

    blueprint_editing_score = (
        0.22 * edit_execute_rate
        + 0.18 * edit_schema_rate
        + 0.14 * graph_read_rate
        + 0.14 * variable_read_rate
        + 0.10 * error_path_rate
        + 0.08 * duplicate_reject_rate
        + 0.07 * read_schema_rate
        + 0.04 * type_discovery_rate
        + 0.03 * workflow_completeness_rate
    )

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

    error_count = sum(1 for r in rows if r.get("transport_error") or r.get("response_is_error"))

    return {
        "label": label,
        "created_at": utc_now(),
        "mcp_status": status,
        "task_count": len(rows),
        "error_count": error_count,
        "category_counts": count_by(tasks, "category"),
        "metrics": {
            "blueprint_editing_score": round(blueprint_editing_score, 6),
            "edit_execute_rate": round(edit_execute_rate, 6),
            "edit_schema_rate": round(edit_schema_rate, 6),
            "graph_read_rate": round(graph_read_rate, 6),
            "variable_read_rate": round(variable_read_rate, 6),
            "error_path_rate": round(error_path_rate, 6),
            "duplicate_reject_rate": round(duplicate_reject_rate, 6),
            "read_schema_rate": round(read_schema_rate, 6),
            "type_discovery_rate": round(type_discovery_rate, 6),
            "workflow_completeness_rate": round(workflow_completeness_rate, 6),
            "task_count": len(rows),
            "error_count": error_count,
        },
        "edit_domain_breakdown": edit_domain_breakdown,
        "blueprint_type_breakdown": bp_type_breakdown,
        "edit_execute_type_breakdown": edit_execute_type_breakdown,
    }


# ---------------------------------------------------------------------------
# Generate
# ---------------------------------------------------------------------------

def build_static_tasks() -> List[Dict[str, Any]]:
    tasks: List[Dict[str, Any]] = []

    def next_id() -> str:
        return f"BEB-{len(tasks) + 1:03d}"

    # --- type_discovery (14) ---
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
        })

    # --- graph_read (21 = 7 types x 3 ops) ---
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
        })

    # --- variable_read (14 = 7 types x 2 ops) ---
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
                "expected": {"server_handled": True},
                "safety": "read_only",
                "blueprint_type": bp["type"],
                "domain": bp["domain"],
                "note": "Interface has no variables; get_functions tests the function-stub surface",
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
            })

    # --- read_schema (15) ---
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
        })

    # --- edit_schema (38) ---
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
        })

    # --- workflow_completeness (5) ---
    for wf in BLUEPRINT_WORKFLOWS:
        tasks.append({
            "id": next_id(),
            "category": "workflow_completeness",
            "namespace": "blueprint",
            "tool": "monolith_discover",
            "arguments": {"namespace": "blueprint"},
            "expected": {"requires_actions": wf["requires_actions"]},
            "safety": "read_only_discovery",
            "workflow": wf["name"],
            "workflow_description": wf["description"],
        })

    # --- edit_execute (70 = 7 types x 10) ---
    for bp in BP_TYPES:
        bp_type = bp["type"]
        type_tasks = BLUEPRINT_EDIT_EXECUTE_TASKS_BY_TYPE.get(bp_type, [])
        for spec in type_tasks:
            tasks.append({
                "id": next_id(),
                "category": "edit_execute",
                "namespace": "blueprint",
                "action": spec["action"],
                "tool": "blueprint_query",
                "arguments": spec["arguments"],
                "expected": {"direct_success": True},
                "safety": "mutating_fixture",
                "edit_domain": spec["edit_domain"],
                "blueprint_type": bp_type,
                "domain": bp["domain"],
                "description": spec["description"],
            })

    # --- error_path (8) ---
    for spec in BLUEPRINT_ERROR_PATH_TASKS:
        tasks.append({
            "id": next_id(),
            "category": "error_path",
            "namespace": "blueprint",
            "action": spec["action"],
            "tool": "blueprint_query",
            "arguments": spec["arguments"],
            "expected": {"is_error": True},
            "safety": "read_only_invalid",
            "description": spec["description"],
        })

    # --- duplicate_reject (6) ---
    for spec in BLUEPRINT_DUPLICATE_REJECT_TASKS:
        task: Dict[str, Any] = {
            "id": next_id(),
            "category": "duplicate_reject",
            "namespace": "blueprint",
            "action": spec["action"],
            "tool": "blueprint_query",
            "arguments": spec["arguments"],
            "expected": {"is_error": True},
            "safety": "mutating_idempotency",
            "edit_domain": spec.get("edit_domain", ""),
            "blueprint_type": "Actor",
            "domain": "gameplay",
            "description": spec["description"],
        }
        if "setup_arguments" in spec:
            task["setup_arguments"] = spec["setup_arguments"]
        tasks.append(task)

    for i, task in enumerate(tasks, 1):
        task["id"] = f"BEB-{i:03d}"

    return tasks


def generate_tasks(tasks_path: pathlib.Path, manifest_path: pathlib.Path) -> Dict[str, Any]:
    tasks = build_static_tasks()
    write_jsonl(tasks_path, tasks)

    domain_counts: Dict[str, int] = {}
    for _, dom in BLUEPRINT_EDIT_ACTIONS:
        domain_counts[dom] = domain_counts.get(dom, 0) + 1

    manifest = {
        "benchmark": "BlueprintEditing",
        "description": (
            "Measures blueprint editing capability: type discovery, graph/variable reads, "
            "edit action schemas, edit execution, graceful error handling, workflow completeness"
        ),
        "primary_score": "blueprint_editing_score",
        "expected_namespace": "blueprint",
        "generated_at": utc_now(),
        "task_count": len(tasks),
        "category_counts": count_by(tasks, "category"),
        "edit_schema_domains": domain_counts,
        "blueprint_types_tested": [bp["type"] for bp in BP_TYPES],
        "workflows_tested": [wf["name"] for wf in BLUEPRINT_WORKFLOWS],
        "score_formula": (
            "0.22*edit_execute_rate + 0.18*edit_schema_rate + 0.14*graph_read_rate "
            "+ 0.14*variable_read_rate + 0.10*error_path_rate + 0.08*duplicate_reject_rate "
            "+ 0.07*read_schema_rate + 0.04*type_discovery_rate + 0.03*workflow_completeness_rate"
        ),
        "edit_execute_tasks_per_type": 10,
        "fixture_paths": {bp["type"]: bp["path"] for bp in BP_TYPES},
        "setup_fixtures_command": "python Scripts/blueprint_editing_benchmark.py setup_fixtures --mcp-url http://localhost:9316/mcp",
        "score_dimensions": [
            "edit_execute_rate",
            "edit_schema_rate",
            "graph_read_rate",
            "variable_read_rate",
            "error_path_rate",
            "duplicate_reject_rate",
            "read_schema_rate",
            "type_discovery_rate",
            "workflow_completeness_rate",
        ],
        "catalog_version_verified": "v0.20.2-blueprint-138-actions",
        "task_file": "Benchmarks/BlueprintEditing/tasks.jsonl",
    }
    write_json(manifest_path, manifest)
    return manifest


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
            "snippet": result_text(create_resp)[:200],
        })

        # 2. Add fixture variables (skip for Interface which has no class variables).
        if bp_type != "Interface":
            for var_name in bp["fixture_vars"]:
                var_type = "float" if var_name not in ("ActorTag", "CharacterName", "bIsSprinting",
                                                        "bIsInAir", "bIsActive", "ComponentID") else "string"
                if var_name.startswith("b") and len(var_name) > 1 and var_name[1].isupper():
                    var_type = "bool"
                if var_name in ("ComponentID",):
                    var_type = "int"
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
                    "snippet": result_text(var_resp)[:100],
                })

        # 3. compile_blueprint
        compile_resp = mcp_call(url, "blueprint_query", {
            "action": "compile_blueprint", "asset_path": asset_path,
        }, timeout_s=timeout_s)
        compile_ok = not compile_resp.get("transport_error") and not _is_error(compile_resp)
        steps.append({
            "action": "compile_blueprint",
            "success": compile_ok,
            "snippet": result_text(compile_resp)[:150],
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
            print(f"       {flag} {s['action']}{extra}", flush=True)

    total_ok = sum(1 for r in results if r["overall_success"])
    print(f"\nsetup_fixtures: {total_ok}/{len(results)} fixtures ready", flush=True)
    return {"fixtures": results, "total": len(results), "succeeded": total_ok}


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
    tasks = load_jsonl(tasks_path)
    output_dir.mkdir(parents=True, exist_ok=True)
    status_response = mcp_call(url, "monolith_status", {}, timeout_s=timeout_s)
    status = result_data(status_response)

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
            write_json(output_dir / "partial_summary.json", partial)

    summary = aggregate(label, status, tasks, rows)
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
        "blueprint_editing_score",
        "edit_execute_rate",
        "edit_schema_rate",
        "graph_read_rate",
        "variable_read_rate",
        "error_path_rate",
        "read_schema_rate",
        "type_discovery_rate",
        "workflow_completeness_rate",
    ]
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
        sys.stdout.buffer.write((json.dumps(manifest, indent=2, ensure_ascii=False) + "\n").encode("utf-8"))
        return 0

    if args.cmd == "setup_fixtures":
        print("Creating benchmark fixture blueprints...", flush=True)
        result = setup_fixtures(args.mcp_url, args.request_timeout_s)
        sys.stdout.buffer.write((json.dumps(result, indent=2, ensure_ascii=False) + "\n").encode("utf-8"))
        return 0 if result["succeeded"] == result["total"] else 1

    if args.cmd == "run":
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
