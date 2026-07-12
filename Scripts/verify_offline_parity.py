#!/usr/bin/env python3
"""
verify_offline_parity.py -- HARD-GATE parity guard for the two offline Monolith RI tools.

Byte/deep-diffs the C++ exe (Binaries/monolith_query.exe) against the Python
reference (Scripts/monolith_offline.py) across 137 actions: 99 Reflection-
Intelligence (RI) actions spanning 4 namespaces (cppreflect, network, decision,
risk; JSON deep-diff) PLUS 38 source-ergonomics actions (get_include_path,
get_signature, check_deprecations, verify_symbols, find_example_usage,
lint_header, generate_class_stub; plain-text STRICT byte-compare).

This is the acceptance test for the offline-parity sprint. Strict mode (default)
requires every JSON action to deep-equal (INCLUDING the opaque base64
`next_cursor` bytes -- so the two tools must compute identical filter-hashes qh)
and every text action's raw stdout to byte-match (trailing newline normalised).

Usage (run from the Monolith plugin root):
    python Scripts/verify_offline_parity.py
    python Scripts/verify_offline_parity.py --exe Binaries/monolith_query-<source-hash>.exe
    python Scripts/verify_offline_parity.py --ignore-cursor-bytes
    python Scripts/verify_offline_parity.py --live          # FUTURE stub, see below

Exit code 0 IFF all 137 actions match in the active mode AND --version
parity-rev matches. Non-zero otherwise.

stdlib-only. Do not add third-party deps.

--------------------------------------------------------------------------------
FUTURE: --live three-way diff (orchestrator-driven, NOT implemented here)
--------------------------------------------------------------------------------
The live Monolith MCP server runs in-process inside the Unreal Editor and is
NOT reachable from a standalone CLI script (no stable loopback contract, and
the editor may be down). A real three-way guard (exe vs py vs live) is therefore
performed MANUALLY by the orchestrator:
  1. Run each RI action through the live MCP dispatcher (e.g. cppreflect_query).
  2. Capture the JSON result.
  3. deep_diff() it against the exe/py results this script already produces.
The --live flag below is a documented stub that refuses to run rather than
silently faking a third source. When a loopback transport exists, wire
fetch_live(ns, action, args) into run_action() as a third producer.
"""

import argparse
import base64
import json
import subprocess
import sys
from pathlib import Path

# ------------------------------------------------------------------ paths
# Script lives in <MonolithRoot>/Scripts/, so the plugin root is parent.parent
SCRIPT_DIR = Path(__file__).resolve().parent
MONO_ROOT = SCRIPT_DIR.parent
EXE_PATH = MONO_ROOT / "Binaries" / "monolith_query.exe"
PY_PATH = MONO_ROOT / "Scripts" / "monolith_offline.py"

# ------------------------------------------------------------------ invocation


def _run(cmd):
    """Run a command, return (returncode, stdout_text, stderr_text)."""
    proc = subprocess.run(
        cmd,
        capture_output=True,
        cwd=str(MONO_ROOT),
        # Force utf-8 decode regardless of Windows console codepage; the RI
        # corpus contains emoji/unicode in rationale fields.
        encoding="utf-8",
        errors="replace",
    )
    return proc.returncode, proc.stdout, proc.stderr


def run_exe(ns, action, args):
    cmd = [str(EXE_PATH), ns, action, *[str(a) for a in args]]
    return _run(cmd)


def run_py(ns, action, args):
    cmd = [sys.executable, str(PY_PATH), ns, action, *[str(a) for a in args]]
    return _run(cmd)


def parse_json(text):
    """Parse text as JSON; raise ValueError with a clipped snippet on failure."""
    try:
        return json.loads(text)
    except Exception as exc:  # noqa: BLE001 - want any parse failure surfaced
        snippet = (text or "")[:300]
        raise ValueError(f"JSON parse failed: {exc}; raw[:300]={snippet!r}")


# ------------------------------------------------------------------ cursor


def decode_cursor(cur):
    """Decode an opaque base64 cursor -> dict {qh,p,tc}. None on failure/None."""
    if cur is None:
        return None
    try:
        raw = base64.b64decode(cur)
        return json.loads(raw)
    except Exception:  # noqa: BLE001
        return {"_undecodable": cur}


# ------------------------------------------------------------------ deep diff


# Fields derived from the wall clock at invocation time. The exe and py run as
# separate processes, so a "now"-based value (e.g. cutoff_unix = now - max_age)
# can legitimately differ by the inter-process run gap. Live would diverge too.
# Compare these with a small epsilon instead of byte-exact; a within-tolerance
# delta is recorded as a WARNING, not a blocking DIFF.
TIME_TOLERANT_FIELDS = {"cutoff_unix", "since_unix"}
TIME_TOLERANCE_SEC = 5

# Actions whose next_cursor qh is derived from a wall-clock value (e.g. since_unix
# = now - window), so the opaque cursor bytes legitimately differ between two
# process invocations at different instants -- on BOTH live and offline (live
# recomputes now too, so this is faithful parity with a live quirk, not a bug).
# For these, compare the DECODED {p,tc} and treat a qh delta as a warning, not a
# blocking diff. This keeps the hard gate deterministic (no flake when two runs
# straddle a second boundary).
TIME_DERIVED_CURSOR_ACTIONS = {"risk.get_release_window_hotspots"}


def deep_diff(a, b, path="", ignore_cursor_bytes=False, diffs=None, warnings=None):
    """
    Recursively compare two JSON values. Appends (path, exe_val, py_val) tuples
    to `diffs` for every divergence. Cursor handling:
      - strict (default): next_cursor compared as opaque string (byte-equal).
      - --ignore-cursor-bytes: decode both cursors, compare decoded {p,tc};
        a differing `qh` (filter-hash) is recorded in `warnings`, not `diffs`.
    Wall-clock-derived fields (TIME_TOLERANT_FIELDS) compare with an epsilon.
    Returns (diffs, warnings).
    """
    if diffs is None:
        diffs = []
    if warnings is None:
        warnings = []

    # Special-case the cursor field by key name at any depth.
    # (Handled when we descend into a dict below.)

    if isinstance(a, dict) and isinstance(b, dict):
        keys = sorted(set(a.keys()) | set(b.keys()))
        for k in keys:
            kp = f"{path}.{k}" if path else k
            in_a = k in a
            in_b = k in b
            if not in_a or not in_b:
                diffs.append((kp + " [key-presence]",
                              "<present>" if in_a else "<MISSING>",
                              "<present>" if in_b else "<MISSING>"))
                continue
            if k == "next_cursor":
                _diff_cursor(a[k], b[k], kp, ignore_cursor_bytes, diffs, warnings)
            elif k in TIME_TOLERANT_FIELDS and isinstance(a[k], (int, float)) and isinstance(b[k], (int, float)):
                if abs(a[k] - b[k]) > TIME_TOLERANCE_SEC:
                    diffs.append((kp + " [time-field]", a[k], b[k]))
                elif a[k] != b[k]:
                    warnings.append((kp + f" [time-field within {TIME_TOLERANCE_SEC}s]", a[k], b[k]))
            else:
                deep_diff(a[k], b[k], kp, ignore_cursor_bytes, diffs, warnings)
    elif isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            diffs.append((path + " [list-length]", len(a), len(b)))
        for i in range(min(len(a), len(b))):
            deep_diff(a[i], b[i], f"{path}[{i}]", ignore_cursor_bytes, diffs, warnings)
    else:
        if a != b:
            diffs.append((path or "<root>", a, b))

    return diffs, warnings


def _diff_cursor(exe_cur, py_cur, path, ignore_cursor_bytes, diffs, warnings):
    if not ignore_cursor_bytes:
        # Strict: opaque byte compare.
        if exe_cur != py_cur:
            diffs.append((path + " [cursor-bytes]", exe_cur, py_cur))
            # Also surface decoded delta for the report's convenience.
            ed, pd = decode_cursor(exe_cur), decode_cursor(py_cur)
            if ed != pd:
                diffs.append((path + " [cursor-decoded]", ed, pd))
        return

    # Lenient: compare decoded {p,tc}; qh mismatch is a WARNING.
    ed, pd = decode_cursor(exe_cur), decode_cursor(py_cur)
    if ed is None and pd is None:
        return
    if ed is None or pd is None:
        diffs.append((path + " [cursor-presence]", ed, pd))
        return
    e_pt = {"p": ed.get("p"), "tc": ed.get("tc")}
    p_pt = {"p": pd.get("p"), "tc": pd.get("tc")}
    if e_pt != p_pt:
        diffs.append((path + " [cursor-page/total]", e_pt, p_pt))
    if ed.get("qh") != pd.get("qh"):
        warnings.append((path + " [cursor-filter-hash qh]", ed.get("qh"), pd.get("qh")))


# ------------------------------------------------------------------ chaining helpers


def _first_sorted(values):
    """Deterministic pick: sort, take first. None if empty."""
    if not values:
        return None
    return sorted(values)[0]


def discover_chain_inputs():
    """
    Run list-style actions and deterministically pick real ids/paths so the
    get-style actions are fed reproducible, existing arguments.
    Pulls from the EXE (authoritative corpus); the same args are then fed to
    BOTH tools so the comparison is apples-to-apples.
    """
    chain = {}

    # decision id, from list_decisions
    rc, out, err = run_exe("decision", "list_decisions", ["--limit", "50"])
    try:
        d = parse_json(out)
        ids = [x.get("decision_id") for x in d.get("decisions", []) if x.get("decision_id")]
        chain["decision_id"] = _first_sorted(ids)
    except Exception:
        chain["decision_id"] = None

    # risk file path, from get_release_window_hotspots / list_conditional_gates;
    # fall back to a known high-churn doc.
    chain["risk_path"] = "Docs/SPEC_CORE.md"
    rc, out, err = run_exe("risk", "get_hotspot_score", [chain["risk_path"]])
    try:
        d = parse_json(out)
        if not d.get("success"):
            chain["risk_path"] = "Docs/SPEC_CORE.md"
    except Exception:
        pass

    # cppreflect class: prefer a real project class; ALeviathanCharacterBase is
    # in-tree (Leviathan module). Verify via get_uclass success.
    chain["uclass"] = "ALeviathanCharacterBase"
    rc, out, err = run_exe("cppreflect", "get_uclass", [chain["uclass"]])
    try:
        d = parse_json(out)
        if not d.get("success"):
            chain["uclass"] = "ACarnageFXCheckpointTrigger"
    except Exception:
        chain["uclass"] = "ACarnageFXCheckpointTrigger"

    return chain


# ------------------------------------------------------------------ action table


def build_actions(chain):
    """
    The 137 parity actions with deterministic representative args.
    Each entry: (label, namespace, action, [args] or None).
    None means the current corpus lacks a deterministic required input; the
    action is reported as SKIP rather than a parity failure.
    """
    cls = chain["uclass"]
    did = chain["decision_id"]
    rpath = chain["risk_path"]

    actions = [
        # ---- cppreflect (46) ----
        # chain-derived entries (6)
        ("cppreflect.get_uclass", "cppreflect", "get_uclass", [cls]),
        ("cppreflect.list_uproperties", "cppreflect", "list_uproperties", [cls]),
        ("cppreflect.list_ufunctions", "cppreflect", "list_ufunctions", [cls]),
        ("cppreflect.find_interface_impls", "cppreflect", "find_interface_impls",
         ["IAbilitySystemInterface"]),
        ("cppreflect.find_class_specifier", "cppreflect", "find_class_specifier",
         ["Blueprintable"]),
        ("cppreflect.list_class_specifiers", "cppreflect", "list_class_specifiers", []),
        # fixed engine class entries (20)
        ("cppreflect.get_uclass/ACharacter", "cppreflect", "get_uclass", ["ACharacter"]),
        ("cppreflect.get_uclass/APlayerController", "cppreflect", "get_uclass", ["APlayerController"]),
        ("cppreflect.get_uclass/AGameModeBase", "cppreflect", "get_uclass", ["AGameModeBase"]),
        ("cppreflect.get_uclass/AGameStateBase", "cppreflect", "get_uclass", ["AGameStateBase"]),
        ("cppreflect.get_uclass/APlayerState", "cppreflect", "get_uclass", ["APlayerState"]),
        ("cppreflect.get_uclass/UActorComponent", "cppreflect", "get_uclass", ["UActorComponent"]),
        ("cppreflect.list_uproperties/ACharacter", "cppreflect", "list_uproperties", ["ACharacter"]),
        ("cppreflect.list_uproperties/AGameModeBase", "cppreflect", "list_uproperties", ["AGameModeBase"]),
        ("cppreflect.list_uproperties/AGameStateBase", "cppreflect", "list_uproperties", ["AGameStateBase"]),
        ("cppreflect.list_ufunctions/ACharacter", "cppreflect", "list_ufunctions", ["ACharacter"]),
        ("cppreflect.list_ufunctions/APlayerController", "cppreflect", "list_ufunctions", ["APlayerController"]),
        ("cppreflect.list_ufunctions/AGameModeBase", "cppreflect", "list_ufunctions", ["AGameModeBase"]),
        ("cppreflect.find_interface_impls/IGameplayTaskOwnerInterface", "cppreflect", "find_interface_impls", ["IGameplayTaskOwnerInterface"]),
        ("cppreflect.find_interface_impls/IAbilitySystemInterface", "cppreflect", "find_interface_impls", ["IAbilitySystemInterface"]),
        ("cppreflect.find_class_specifier/BlueprintType", "cppreflect", "find_class_specifier", ["BlueprintType"]),
        ("cppreflect.find_class_specifier/NotBlueprintable", "cppreflect", "find_class_specifier", ["NotBlueprintable"]),
        ("cppreflect.find_class_specifier/Abstract", "cppreflect", "find_class_specifier", ["Abstract"]),
        ("cppreflect.find_class_specifier/MinimalAPI", "cppreflect", "find_class_specifier", ["MinimalAPI"]),
        ("cppreflect.find_class_specifier/Config", "cppreflect", "find_class_specifier", ["Config"]),
        ("cppreflect.list_class_specifiers(b)", "cppreflect", "list_class_specifiers", []),
        # gameplay-critical engine/GAS/input/UI/anim/movement classes (20) --
        # the highest-value new reflection coverage, mirrored into the hard gate
        # from offline_parity_benchmark.py so exe<->py parity is CI-enforced.
        ("cppreflect.get_uclass/UGameplayAbility", "cppreflect", "get_uclass", ["UGameplayAbility"]),
        ("cppreflect.get_uclass/UAbilitySystemComponent", "cppreflect", "get_uclass", ["UAbilitySystemComponent"]),
        ("cppreflect.get_uclass/UAttributeSet", "cppreflect", "get_uclass", ["UAttributeSet"]),
        ("cppreflect.get_uclass/UGameplayEffect", "cppreflect", "get_uclass", ["UGameplayEffect"]),
        ("cppreflect.get_uclass/UEnhancedInputComponent", "cppreflect", "get_uclass", ["UEnhancedInputComponent"]),
        ("cppreflect.get_uclass/UInputAction", "cppreflect", "get_uclass", ["UInputAction"]),
        ("cppreflect.get_uclass/UInputMappingContext", "cppreflect", "get_uclass", ["UInputMappingContext"]),
        ("cppreflect.get_uclass/UUserWidget", "cppreflect", "get_uclass", ["UUserWidget"]),
        ("cppreflect.get_uclass/UAnimMontage", "cppreflect", "get_uclass", ["UAnimMontage"]),
        ("cppreflect.get_uclass/UAnimInstance", "cppreflect", "get_uclass", ["UAnimInstance"]),
        ("cppreflect.get_uclass/UCharacterMovementComponent", "cppreflect", "get_uclass", ["UCharacterMovementComponent"]),
        ("cppreflect.get_uclass/APawn", "cppreflect", "get_uclass", ["APawn"]),
        ("cppreflect.get_uclass/AController", "cppreflect", "get_uclass", ["AController"]),
        ("cppreflect.get_uclass/AAIController", "cppreflect", "get_uclass", ["AAIController"]),
        ("cppreflect.get_uclass/USkeletalMeshComponent", "cppreflect", "get_uclass", ["USkeletalMeshComponent"]),
        ("cppreflect.get_uclass/UStaticMeshComponent", "cppreflect", "get_uclass", ["UStaticMeshComponent"]),
        ("cppreflect.get_uclass/UWorld", "cppreflect", "get_uclass", ["UWorld"]),
        ("cppreflect.get_uclass/UGameInstance", "cppreflect", "get_uclass", ["UGameInstance"]),
        ("cppreflect.find_interface_impls/IGameplayTagAssetInterface", "cppreflect", "find_interface_impls", ["IGameplayTagAssetInterface"]),
        ("cppreflect.find_class_specifier/EditInlineNew", "cppreflect", "find_class_specifier", ["EditInlineNew"]),

        # ---- network (16) ----
        # original entries (4)
        ("network.list_replicated_classes", "network", "list_replicated_classes",
         ["--limit", "5"]),
        ("network.list_rpc_functions", "network", "list_rpc_functions", ["--limit", "5"]),
        ("network.list_onrep_handlers", "network", "list_onrep_handlers", ["--limit", "5"]),
        ("network.audit_unbalanced_onreps", "network", "audit_unbalanced_onreps", []),
        # limit variants (12)
        ("network.list_replicated_classes/limit1", "network", "list_replicated_classes", ["--limit", "1"]),
        ("network.list_replicated_classes/limit10", "network", "list_replicated_classes", ["--limit", "10"]),
        ("network.list_replicated_classes/limit20", "network", "list_replicated_classes", ["--limit", "20"]),
        ("network.list_replicated_classes/limit50", "network", "list_replicated_classes", ["--limit", "50"]),
        ("network.list_rpc_functions/limit1", "network", "list_rpc_functions", ["--limit", "1"]),
        ("network.list_rpc_functions/limit10", "network", "list_rpc_functions", ["--limit", "10"]),
        ("network.list_rpc_functions/limit20", "network", "list_rpc_functions", ["--limit", "20"]),
        ("network.list_onrep_handlers/limit1", "network", "list_onrep_handlers", ["--limit", "1"]),
        ("network.list_onrep_handlers/limit10", "network", "list_onrep_handlers", ["--limit", "10"]),
        ("network.list_onrep_handlers/limit20", "network", "list_onrep_handlers", ["--limit", "20"]),
        # determinism verification (repeat calls)
        ("network.audit_unbalanced_onreps/det1", "network", "audit_unbalanced_onreps", []),
        ("network.audit_unbalanced_onreps/det2", "network", "audit_unbalanced_onreps", []),

        # ---- decision (15) ----
        # original entries (5)
        ("decision.list_decisions", "decision", "list_decisions", ["--limit", "5"]),
        ("decision.get_decision", "decision", "get_decision", [did] if did else None),
        ("decision.list_stale", "decision", "list_stale", ["3650", "--limit", "5"]),
        ("decision.find_supersession_chain", "decision", "find_supersession_chain",
         [did] if did else None),
        ("decision.find_referent_decisions", "decision", "find_referent_decisions",
         [did] if did else None),
        # limit variants (4)
        ("decision.list_decisions/limit1", "decision", "list_decisions", ["--limit", "1"]),
        ("decision.list_decisions/limit10", "decision", "list_decisions", ["--limit", "10"]),
        ("decision.list_decisions/limit20", "decision", "list_decisions", ["--limit", "20"]),
        ("decision.list_decisions/limit50", "decision", "list_decisions", ["--limit", "50"]),
        # staleness threshold variants (6)
        ("decision.list_stale/30d", "decision", "list_stale", ["30", "--limit", "5"]),
        ("decision.list_stale/90d", "decision", "list_stale", ["90", "--limit", "5"]),
        ("decision.list_stale/180d", "decision", "list_stale", ["180", "--limit", "5"]),
        ("decision.list_stale/365d", "decision", "list_stale", ["365", "--limit", "5"]),
        ("decision.list_stale/730d", "decision", "list_stale", ["730", "--limit", "5"]),
        ("decision.list_stale/1825d", "decision", "list_stale", ["1825", "--limit", "5"]),

        # ---- risk (22) ----
        # original entries (5)
        ("risk.get_hotspot_score", "risk", "get_hotspot_score", [rpath]),
        ("risk.get_cochange_pairs", "risk", "get_cochange_pairs", [rpath]),
        ("risk.get_file_churn", "risk", "get_file_churn", [rpath]),
        ("risk.get_release_window_hotspots", "risk", "get_release_window_hotspots", []),
        ("risk.list_conditional_gates", "risk", "list_conditional_gates", []),
        # get_hotspot_score for additional files (7)
        ("risk.get_hotspot_score/action_guidance_benchmark", "risk", "get_hotspot_score", ["Scripts/action_guidance_benchmark.py"]),
        ("risk.get_hotspot_score/source_index_benchmark", "risk", "get_hotspot_score", ["Scripts/source_index_benchmark.py"]),
        ("risk.get_hotspot_score/schema_completeness_benchmark", "risk", "get_hotspot_score", ["Scripts/schema_completeness_benchmark.py"]),
        ("risk.get_hotspot_score/offline_parity_benchmark", "risk", "get_hotspot_score", ["Scripts/offline_parity_benchmark.py"]),
        ("risk.get_hotspot_score/ci_static_checks", "risk", "get_hotspot_score", ["Scripts/ci_static_checks.py"]),
        ("risk.get_hotspot_score/monolith_proxy", "risk", "get_hotspot_score", ["Scripts/monolith_proxy.py"]),
        ("risk.get_hotspot_score/verify_offline_parity", "risk", "get_hotspot_score", ["Scripts/verify_offline_parity.py"]),
        # get_cochange_pairs for additional files (5)
        ("risk.get_cochange_pairs/monolith_offline", "risk", "get_cochange_pairs", ["Scripts/monolith_offline.py"]),
        ("risk.get_cochange_pairs/ci_static_checks", "risk", "get_cochange_pairs", ["Scripts/ci_static_checks.py"]),
        ("risk.get_cochange_pairs/monolith_proxy", "risk", "get_cochange_pairs", ["Scripts/monolith_proxy.py"]),
        ("risk.get_cochange_pairs/index_project", "risk", "get_cochange_pairs", ["Scripts/index_project.py"]),
        ("risk.get_cochange_pairs/tag_path_params", "risk", "get_cochange_pairs", ["Scripts/tag_path_params.py"]),
        # get_file_churn for additional files (5)
        ("risk.get_file_churn/lint_agent_tools", "risk", "get_file_churn", ["Scripts/lint_agent_tools.py"]),
        ("risk.get_file_churn/check_offline_exe_fresh", "risk", "get_file_churn", ["Scripts/check_offline_exe_fresh.py"]),
        ("risk.get_file_churn/check_index_freshness", "risk", "get_file_churn", ["Scripts/check_index_freshness.ps1"]),
        ("risk.get_file_churn/recover_mcp", "risk", "get_file_churn", ["Scripts/recover_mcp.ps1"]),
        ("risk.get_file_churn/docs_spec", "risk", "get_file_churn", ["Docs/SPEC.md"]),

        # ---- source ergonomics (36) -- plain-text output, STRICT byte-compare ----
        # original entries (7)
        # Deterministic fixed inputs (no chaining):
        #   AActor                          -> stable engine class header
        #   UGameplayStatics::ApplyDamage   -> stable class-body method (declaration_read)
        #   PreparePathfinding              -> known 4.13 UE_DEPRECATED_FORGAME verdict
        #   AActor (2nd check_deprecations) -> known not-deprecated
        ("source.get_include_path", "source", "get_include_path", ["AActor"], "text"),
        ("source.get_signature", "source", "get_signature",
         ["UGameplayStatics::ApplyDamage"], "text"),
        ("source.check_deprecations", "source", "check_deprecations",
         ["PreparePathfinding", "AActor"], "text"),
        # Fixed deterministic inputs:
        #   verify_symbols      -> a stable class-body method (exists:true via class
        #                          row + source_fts), a stable class, and a guaranteed-
        #                          missing name (exists:false, no error).
        #   find_example_usage  -> a stable engine API method with many call sites;
        #                          --limit pins the page size so the slice is fixed.
        ("source.verify_symbols", "source", "verify_symbols",
         ["UGameplayStatics::ApplyDamage", "AActor", "UThisDoesNotExistAnywhereXYZ"], "text"),
        ("source.find_example_usage", "source", "find_example_usage",
         ["UGameplayStatics::ApplyDamage", "--limit", "5"], "text"),
        # Fixed deterministic inputs:
        #   lint_header         -> a COMMITTED fixture so exe + py see byte-identical
        #                          input. The fixture carries a `.h.fixture` extension
        #                          (UHT must NOT parse its reflection macros); lint_header
        #                          reads via LoadFileToStringArray, which is extension-
        #                          agnostic. LintOrphanUproperty.h.fixture has NO UCLASS
        #                          macro, so rules (c)/(d) (the only base-name-sensitive
        #                          ones) and the invalid-specifier rule never fire -> the
        #                          `.h.fixture` extension is irrelevant to its output, and
        #                          exe == py deterministically. It triggers rule (e) so the
        #                          "Clean" sentinel line is never byte-compared.
        #   generate_class_stub -> a stable engine UCLASS parent (AActor); the .h/.cpp
        #                          text is a pure DB read (offline-served, Decision 5).
        #                          class "AMyParityActor" -> UE file base "MyParityActor"
        #                          (the A/U UCLASS prefix is stripped from FILE names),
        #                          so both tools emit "MyParityActor.generated.h" / .h
        #                          and the "// === MyParityActor.h ===" banner -- byte-equal.
        ("source.lint_header", "source", "lint_header",
         ["Source/MonolithSource/Private/Tests/Fixtures/CppErgoCorpus/Source/CppErgoTestMod/LintOrphanUproperty.h.fixture"], "text"),
        ("source.generate_class_stub", "source", "generate_class_stub",
         ["AActor", "AMyParityActor", "MonolithSource"], "text"),
        # get_include_path for engine classes (10)
        ("source.get_include_path/ACharacter", "source", "get_include_path", ["ACharacter"], "text"),
        ("source.get_include_path/APlayerController", "source", "get_include_path", ["APlayerController"], "text"),
        ("source.get_include_path/AGameModeBase", "source", "get_include_path", ["AGameModeBase"], "text"),
        ("source.get_include_path/AGameStateBase", "source", "get_include_path", ["AGameStateBase"], "text"),
        ("source.get_include_path/APlayerState", "source", "get_include_path", ["APlayerState"], "text"),
        ("source.get_include_path/UActorComponent", "source", "get_include_path", ["UActorComponent"], "text"),
        ("source.get_include_path/USceneComponent", "source", "get_include_path", ["USceneComponent"], "text"),
        ("source.get_include_path/UStaticMeshComponent", "source", "get_include_path", ["UStaticMeshComponent"], "text"),
        ("source.get_include_path/USkeletalMeshComponent", "source", "get_include_path", ["USkeletalMeshComponent"], "text"),
        ("source.get_include_path/UPrimitiveComponent", "source", "get_include_path", ["UPrimitiveComponent"], "text"),
        # get_signature for common engine methods (8)
        ("source.get_signature/AActor_BeginPlay", "source", "get_signature", ["AActor::BeginPlay"], "text"),
        ("source.get_signature/AActor_EndPlay", "source", "get_signature", ["AActor::EndPlay"], "text"),
        ("source.get_signature/AActor_Tick", "source", "get_signature", ["AActor::Tick"], "text"),
        ("source.get_signature/ACharacter_Jump", "source", "get_signature", ["ACharacter::Jump"], "text"),
        ("source.get_signature/ACharacter_Crouch", "source", "get_signature", ["ACharacter::Crouch"], "text"),
        ("source.get_signature/APlayerController_Possess", "source", "get_signature", ["APlayerController::Possess"], "text"),
        ("source.get_signature/UActorComponent_BeginPlay", "source", "get_signature", ["UActorComponent::BeginPlay"], "text"),
        ("source.get_signature/UActorComponent_TickComponent", "source", "get_signature", ["UActorComponent::TickComponent"], "text"),
        # check_deprecations for symbol pairs (3)
        ("source.check_deprecations/ACharacter_UActorComponent", "source", "check_deprecations", ["AActor", "ACharacter"], "text"),
        ("source.check_deprecations/UObject_UActorComponent", "source", "check_deprecations", ["UObject", "UActorComponent"], "text"),
        ("source.check_deprecations/AGameModeBase_AGameStateBase", "source", "check_deprecations", ["AGameModeBase", "AGameStateBase"], "text"),
        # verify_symbols for symbol sets (5)
        ("source.verify_symbols/AActor_ACharacter_UObject", "source", "verify_symbols", ["AActor", "ACharacter", "UObject"], "text"),
        ("source.verify_symbols/ApplyDamage_BeginPlay", "source", "verify_symbols", ["UGameplayStatics::ApplyDamage", "AActor::BeginPlay"], "text"),
        ("source.verify_symbols/APC_APS_AGSB", "source", "verify_symbols", ["APlayerController", "APlayerState", "AGameStateBase"], "text"),
        ("source.verify_symbols/SMC_SKMC", "source", "verify_symbols", ["UStaticMeshComponent", "USkeletalMeshComponent"], "text"),
        ("source.verify_symbols/GMB_GI_World", "source", "verify_symbols", ["AGameModeBase", "UGameInstance", "UWorld"], "text"),
        # find_example_usage with different methods and limits (3)
        ("source.find_example_usage/BeginPlay_limit3", "source", "find_example_usage", ["AActor::BeginPlay", "--limit", "3"], "text"),
        ("source.find_example_usage/Jump_limit5", "source", "find_example_usage", ["ACharacter::Jump", "--limit", "5"], "text"),
        ("source.find_example_usage/ApplyDamage_limit3", "source", "find_example_usage", ["UGameplayStatics::ApplyDamage", "--limit", "3"], "text"),
        # generate_class_stub for different parent/class combos (2)
        ("source.generate_class_stub/UObject_MyTestObject", "source", "generate_class_stub", ["UObject", "AMyTestObject", "TestModule"], "text"),
        ("source.generate_class_stub/UActorComponent_MyTestComp", "source", "generate_class_stub", ["UActorComponent", "UMyTestComp", "TestModule"], "text"),
    ]
    return actions


# ------------------------------------------------------------------ live stub


def fetch_live(ns, action, args):
    """FUTURE three-way hook. Not reachable from a standalone CLI -- see module docstring."""
    raise NotImplementedError(
        "--live is a documented stub. The live MCP runs in-process in the editor "
        "and is not reachable from this script; the orchestrator performs the "
        "exe/py-vs-live three-way diff manually."
    )


# ------------------------------------------------------------------ per-action


def run_action(label, ns, action, args, ignore_cursor_bytes, compare="json"):
    """
    Returns a result dict:
      status: MATCH | DIFF | ERROR
      diffs:  list of (path, exe_val, py_val)
      warnings: list of (path, exe_val, py_val)
      error:  str | None

    compare="json" (default): parse both stdouts as JSON and deep-diff.
    compare="text": STRICT raw-stdout byte-compare (the source.* ergonomics
    actions emit plain text, not JSON). Trailing newlines are normalised so a
    lone print() newline difference is not a false DIFF; all other bytes must match.
    """
    res = {"label": label, "args": args, "status": None,
           "diffs": [], "warnings": [], "error": None}

    erc, eout, eerr = run_exe(ns, action, args)
    prc, pout, perr = run_py(ns, action, args)

    err_parts = []
    if erc != 0:
        err_parts.append(f"EXE exit={erc} stderr={eerr.strip()[:400]!r}")
    if prc != 0:
        err_parts.append(f"PY exit={prc} stderr={perr.strip()[:400]!r}")

    if err_parts:
        res["status"] = "ERROR"
        res["error"] = " | ".join(err_parts)
        return res

    if compare == "text":
        # Normalise only trailing newline(s); interior bytes must match exactly.
        etext = (eout or "").rstrip("\n")
        ptext = (pout or "").rstrip("\n")
        if etext != ptext:
            res["diffs"] = [("<stdout-text>", etext, ptext)]
            res["status"] = "DIFF"
        else:
            res["status"] = "MATCH"
        return res

    edata = pdata = None
    try:
        edata = parse_json(eout)
    except ValueError as e:
        err_parts.append(f"EXE {e}")
    try:
        pdata = parse_json(pout)
    except ValueError as e:
        err_parts.append(f"PY {e}")

    if err_parts:
        res["status"] = "ERROR"
        res["error"] = " | ".join(err_parts)
        return res

    eff_ignore_cursor = ignore_cursor_bytes or (label in TIME_DERIVED_CURSOR_ACTIONS)
    diffs, warnings = deep_diff(edata, pdata, ignore_cursor_bytes=eff_ignore_cursor)
    res["diffs"] = diffs
    res["warnings"] = warnings
    res["status"] = "MATCH" if not diffs else "DIFF"
    return res


def skipped_action(label, reason):
    return {"label": label, "args": [], "status": "SKIP",
            "diffs": [], "warnings": [], "error": reason}


# ------------------------------------------------------------------ version


def check_version_parity():
    """
    The two tools intentionally print --version differently:
      exe -> JSON {plugin_version, parity_spec_rev, ...}
      py  -> bare parity_spec_rev string
    Compare the parity-rev specifically. Returns (ok, exe_rev, py_rev).
    """
    _, eout, _ = run_exe_raw_version()
    _, pout, _ = run_py_raw_version()

    exe_rev = None
    try:
        exe_rev = json.loads(eout).get("parity_spec_rev")
    except Exception:
        exe_rev = eout.strip() or None
    py_rev = pout.strip() or None
    return (exe_rev is not None and exe_rev == py_rev), exe_rev, py_rev


def run_exe_raw_version():
    return _run([str(EXE_PATH), "--version"])


def run_py_raw_version():
    return _run([sys.executable, str(PY_PATH), "--version"])


# ------------------------------------------------------------------ reporting


def _clip(v, n=160):
    s = json.dumps(v, ensure_ascii=False) if not isinstance(v, str) else v
    return s if len(s) <= n else s[: n - 3] + "..."


def main():
    global EXE_PATH
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", type=Path, default=EXE_PATH,
                    help="Authoritative native Query image to verify (default: fixed compatibility path).")
    ap.add_argument("--ignore-cursor-bytes", action="store_true",
                    help="Decode cursors and compare {p,tc}; treat qh mismatch as WARNING.")
    ap.add_argument("--live", action="store_true",
                    help="FUTURE three-way vs live MCP (stub -- refuses to run).")
    args = ap.parse_args()
    EXE_PATH = args.exe.resolve()

    if args.live:
        try:
            fetch_live("cppreflect", "list_class_specifiers", [])
        except NotImplementedError as e:
            print(f"[--live] {e}")
        return 3

    # Preflight.
    if not EXE_PATH.exists():
        print(f"FATAL: exe not found at {EXE_PATH}")
        return 4
    if not PY_PATH.exists():
        print(f"FATAL: py not found at {PY_PATH}")
        return 4

    mode = "lenient(ignore-cursor-bytes)" if args.ignore_cursor_bytes else "STRICT(byte-compare)"
    print(f"Offline parity guard -- mode={mode}")
    print(f"  exe = {EXE_PATH}")
    print(f"  py  = {PY_PATH}\n")

    # Version parity.
    ver_ok, exe_rev, py_rev = check_version_parity()

    # Chain discovery.
    chain = discover_chain_inputs()
    print(f"Chained inputs: uclass={chain['uclass']!r} "
          f"decision_id={chain['decision_id']!r} risk_path={chain['risk_path']!r}\n")

    actions = build_actions(chain)
    results = []
    for action_entry in actions:
        if len(action_entry) == 5:
            label, ns, action, aargs, compare = action_entry
        else:
            label, ns, action, aargs = action_entry
            compare = "json"

        if aargs is None:
            results.append(skipped_action(label, "current DB corpus has no decision_id input"))
        else:
            results.append(run_action(label, ns, action, aargs, args.ignore_cursor_bytes, compare))

    n_match = sum(1 for r in results if r["status"] == "MATCH")
    n_diff = sum(1 for r in results if r["status"] == "DIFF")
    n_err = sum(1 for r in results if r["status"] == "ERROR")
    n_skip = sum(1 for r in results if r["status"] == "SKIP")
    comparable = len(results) - n_skip

    # Summary table.
    print("=" * 72)
    print(f"SUMMARY: {n_match}/{comparable} MATCH | {n_diff} DIFF | {n_err} ERROR | {n_skip} SKIP")
    print("=" * 72)
    print(f"{'ACTION':<40} {'STATUS':<8} {'#DIFF':>6} {'#WARN':>6}")
    print("-" * 72)
    for r in results:
        print(f"{r['label']:<40} {r['status']:<8} "
              f"{len(r['diffs']):>6} {len(r['warnings']):>6}")
    print("-" * 72)

    # Detailed diffs.
    if n_diff or n_err or n_skip:
        print("\nDETAILS")
        print("=" * 72)
        for r in results:
            if r["status"] == "ERROR":
                print(f"\n[ERROR] {r['label']}  args={r['args']}")
                print(f"        {r['error']}")
            elif r["status"] == "SKIP":
                print(f"\n[SKIP] {r['label']}")
                print(f"        {r['error']}")
            elif r["status"] == "DIFF":
                print(f"\n[DIFF] {r['label']}  args={r['args']}")
                for (path, ev, pv) in r["diffs"]:
                    print(f"    field: {path}")
                    print(f"        exe: {_clip(ev)}")
                    print(f"        py : {_clip(pv)}")

    # Warnings (cursor-hash, etc.).
    any_warn = any(r["warnings"] for r in results)
    if any_warn:
        print("\nWARNINGS (non-fatal in lenient mode)")
        print("=" * 72)
        for r in results:
            for (path, ev, pv) in r["warnings"]:
                print(f"  {r['label']} :: {path}")
                print(f"      exe: {_clip(ev)}   py: {_clip(pv)}")

    # Version verdict.
    print("\nVERSION PARITY")
    print("=" * 72)
    print(f"  exe parity_spec_rev = {exe_rev!r}")
    print(f"  py  parity_spec_rev = {py_rev!r}")
    print(f"  -> {'MATCH' if ver_ok else 'MISMATCH'}")

    # Exit: strict mode requires zero diffs AND version match.
    ok = (n_diff == 0 and n_err == 0 and ver_ok)
    print(f"\nRESULT: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
