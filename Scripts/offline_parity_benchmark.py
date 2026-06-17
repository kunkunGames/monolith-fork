#!/usr/bin/env python3
"""
offline_parity_benchmark.py -- Scored benchmark for Monolith offline parity.

Elevates verify_offline_parity.py (the CI hard-gate) into a tracked, scored
benchmark that produces summary.json, per_action.jsonl, and comparison output
for regression detection across builds.

The hard-gate (verify_offline_parity.py) remains the CI pass/fail signal.
This benchmark tracks the *quality* of exe-vs-py parity over time via a
composite score called offline_parity_score.

Score formula:
    offline_parity_score = (
        0.50 * match_rate           # fraction of non-skipped actions with status=MATCH
      + 0.20 * (1 - diff_rate)      # 1 - fraction of non-skipped actions with status=DIFF
      + 0.20 * (1 - error_rate)     # 1 - fraction of non-skipped actions with status=ERROR
      + 0.10 * version_parity_score # 1.0 if exe parity_spec_rev == py parity_spec_rev, else 0.0
    )
Note: match_rate + diff_rate + error_rate = 1.0 for non-skipped actions.
So the formula simplifies to:
    offline_parity_score = 0.70 * match_rate + 0.20 * (1 - error_rate) + 0.10 * version_parity_score
If comparable_actions == 0, offline_parity_score is forced to 0.0.

Expected-error negative cases keep status=MATCH when both tools fail as
expected, and are exposed through expected_error/error_kind diagnostics instead
of being mixed with real tool failures.

Subcommands:
    run     -- executes the offline parity check and produces scored summary.json
    compare -- diffs two summary.json files
    report  -- prints a human-readable summary of a summary.json

stdlib-only. Do not add third-party deps.

Usage (run from the Monolith plugin root):
    python Scripts/offline_parity_benchmark.py run --output-dir Saved/Monolith/Benchmarks/OfflineParity/current --label current
    python Scripts/offline_parity_benchmark.py compare --baseline .../baseline/summary.json --current .../current/summary.json --output-dir .../comparison
    python Scripts/offline_parity_benchmark.py report --summary .../current/summary.json
"""

from __future__ import annotations

import argparse
import base64
import datetime as _dt
import json
import pathlib
import subprocess
import sys
from typing import Any, Dict, Iterable, List, Optional, Tuple

from benchmark_common import attach_benchmark_inputs, build_benchmark_inputs

# ------------------------------------------------------------------ paths

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
MONO_ROOT = SCRIPT_DIR.parent

# Default tool paths (resolved relative to plugin root)
DEFAULT_EXE_PATH = MONO_ROOT / "Binaries" / "monolith_query.exe"
DEFAULT_PY_PATH = MONO_ROOT / "Scripts" / "monolith_offline.py"

# ------------------------------------------------------------------ time helpers


def utc_now() -> str:
    return _dt.datetime.now(_dt.timezone.utc).isoformat()


# ------------------------------------------------------------------ file I/O helpers


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


# ------------------------------------------------------------------ invocation helpers
# (inlined from verify_offline_parity.py; do not import that module directly)


def _run(cmd: List[str], cwd: pathlib.Path) -> Tuple[int, str, str]:
    """Run a command, return (returncode, stdout_text, stderr_text)."""
    proc = subprocess.run(
        cmd,
        capture_output=True,
        cwd=str(cwd),
        encoding="utf-8",
        errors="replace",
    )
    return proc.returncode, proc.stdout, proc.stderr


def run_exe(
    ns: str,
    action: str,
    args: List[str],
    exe_path: pathlib.Path,
    mono_root: pathlib.Path,
) -> Tuple[int, str, str]:
    cmd = [str(exe_path), ns, action, *[str(a) for a in args]]
    return _run(cmd, mono_root)


def run_py(
    ns: str,
    action: str,
    args: List[str],
    py_path: pathlib.Path,
    mono_root: pathlib.Path,
) -> Tuple[int, str, str]:
    cmd = [sys.executable, str(py_path), ns, action, *[str(a) for a in args]]
    return _run(cmd, mono_root)


def parse_json(text: str) -> Any:
    """Parse text as JSON; raise ValueError with a clipped snippet on failure."""
    try:
        return json.loads(text)
    except Exception as exc:  # noqa: BLE001
        snippet = (text or "")[:300]
        raise ValueError(f"JSON parse failed: {exc}; raw[:300]={snippet!r}")


# ------------------------------------------------------------------ cursor helpers
# (inlined from verify_offline_parity.py)


def decode_cursor(cur: Optional[str]) -> Optional[Dict[str, Any]]:
    """Decode an opaque base64 cursor -> dict {qh,p,tc}. None on failure/None."""
    if cur is None:
        return None
    try:
        raw = base64.b64decode(cur)
        return json.loads(raw)
    except Exception:  # noqa: BLE001
        return {"_undecodable": cur}


# ------------------------------------------------------------------ deep diff
# (inlined from verify_offline_parity.py)

# Fields derived from the wall clock at invocation time. Compare with epsilon.
TIME_TOLERANT_FIELDS = {"cutoff_unix", "since_unix"}
TIME_TOLERANCE_SEC = 5

# Actions whose next_cursor qh is derived from a wall-clock value.
TIME_DERIVED_CURSOR_ACTIONS = {"risk.get_release_window_hotspots"}

EXPECTED_ACTION_COUNT = 317
BENCHMARK_EXTENSION_ACTION_COUNT = 100


def _clip_text(text: str, limit: int = 400) -> str:
    stripped = (text or "").strip()
    return stripped if len(stripped) <= limit else stripped[: limit - 3] + "..."


def _process_diagnostics(returncode: int, stdout: str, stderr: str) -> Dict[str, Any]:
    return {
        "exit_code": returncode,
        "stdout": _clip_text(stdout),
        "stderr": _clip_text(stderr),
    }


def is_time_derived_cursor_action(label: str, args: List[str]) -> bool:
    if label in TIME_DERIVED_CURSOR_ACTIONS:
        return True
    if not label.startswith("risk.get_release_window_hotspots"):
        return False
    return "--since_unix" not in args


def deep_diff(
    a: Any,
    b: Any,
    path: str = "",
    ignore_cursor_bytes: bool = False,
    diffs: Optional[List[Tuple[str, Any, Any]]] = None,
    warnings: Optional[List[Tuple[str, Any, Any]]] = None,
) -> Tuple[List[Tuple[str, Any, Any]], List[Tuple[str, Any, Any]]]:
    """
    Recursively compare two JSON values. Appends (path, exe_val, py_val) tuples
    to `diffs` for every divergence. Returns (diffs, warnings).
    """
    if diffs is None:
        diffs = []
    if warnings is None:
        warnings = []

    if isinstance(a, dict) and isinstance(b, dict):
        keys = sorted(set(a.keys()) | set(b.keys()))
        for k in keys:
            kp = f"{path}.{k}" if path else k
            in_a = k in a
            in_b = k in b
            if not in_a or not in_b:
                diffs.append((
                    kp + " [key-presence]",
                    "<present>" if in_a else "<MISSING>",
                    "<present>" if in_b else "<MISSING>",
                ))
                continue
            if k == "next_cursor":
                _diff_cursor(a[k], b[k], kp, ignore_cursor_bytes, diffs, warnings)
            elif (
                k in TIME_TOLERANT_FIELDS
                and isinstance(a[k], (int, float))
                and isinstance(b[k], (int, float))
            ):
                if abs(a[k] - b[k]) > TIME_TOLERANCE_SEC:
                    diffs.append((kp + " [time-field]", a[k], b[k]))
                elif a[k] != b[k]:
                    warnings.append((
                        kp + f" [time-field within {TIME_TOLERANCE_SEC}s]",
                        a[k],
                        b[k],
                    ))
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


def _diff_cursor(
    exe_cur: Optional[str],
    py_cur: Optional[str],
    path: str,
    ignore_cursor_bytes: bool,
    diffs: List[Tuple[str, Any, Any]],
    warnings: List[Tuple[str, Any, Any]],
) -> None:
    if not ignore_cursor_bytes:
        if exe_cur != py_cur:
            diffs.append((path + " [cursor-bytes]", exe_cur, py_cur))
            ed, pd = decode_cursor(exe_cur), decode_cursor(py_cur)
            if ed != pd:
                diffs.append((path + " [cursor-decoded]", ed, pd))
        return

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
# (inlined from verify_offline_parity.py)


def _first_sorted(values: List[Any]) -> Optional[Any]:
    if not values:
        return None
    return sorted(values)[0]


def discover_chain_inputs(
    exe_path: pathlib.Path,
    mono_root: pathlib.Path,
) -> Dict[str, Any]:
    """
    Run list-style actions and deterministically pick real ids/paths so that
    get-style actions receive reproducible, existing arguments.
    Pulls from the EXE (authoritative corpus); the same args are fed to both tools.
    """
    chain: Dict[str, Any] = {}

    # decision id
    rc, out, err = run_exe("decision", "list_decisions", ["--limit", "50"], exe_path, mono_root)
    try:
        d = parse_json(out)
        ids = [x.get("decision_id") for x in d.get("decisions", []) if x.get("decision_id")]
        chain["decision_id"] = _first_sorted(ids)
    except Exception:  # noqa: BLE001
        chain["decision_id"] = None

    # risk file path
    chain["risk_path"] = "Docs/SPEC_CORE.md"
    rc, out, err = run_exe("risk", "get_hotspot_score", [chain["risk_path"]], exe_path, mono_root)
    try:
        d = parse_json(out)
        if not d.get("success"):
            chain["risk_path"] = "Docs/SPEC_CORE.md"
    except Exception:  # noqa: BLE001
        pass

    # cppreflect class
    chain["uclass"] = "ALeviathanCharacterBase"
    rc, out, err = run_exe("cppreflect", "get_uclass", [chain["uclass"]], exe_path, mono_root)
    try:
        d = parse_json(out)
        if not d.get("success"):
            chain["uclass"] = "ACarnageFXCheckpointTrigger"
    except Exception:  # noqa: BLE001
        chain["uclass"] = "ACarnageFXCheckpointTrigger"

    return chain


# ------------------------------------------------------------------ action table
# (inlined from verify_offline_parity.py)


def build_actions(chain: Dict[str, Any]) -> List[tuple]:
    """
    The 317 parity actions with deterministic representative args.
    Each entry: (label, namespace, action, args_or_None) or
                (label, namespace, action, args_or_None, compare_mode)
                (label, namespace, action, args_or_None, compare_mode, metadata)
    where compare_mode defaults to "json"; "text" uses strict byte-compare.
    metadata currently supports expected_error=True for negative source cases.
    None args means the action is reported as SKIP rather than a parity failure.
    """
    cls = chain["uclass"]
    did = chain["decision_id"]
    rpath = chain["risk_path"]

    actions = [
        # ---- cppreflect (26) ----
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
        ("source.get_include_path", "source", "get_include_path", ["AActor"], "text"),
        ("source.get_signature", "source", "get_signature",
         ["UGameplayStatics::ApplyDamage"], "text"),
        ("source.check_deprecations", "source", "check_deprecations",
         ["PreparePathfinding", "AActor"], "text"),
        ("source.verify_symbols", "source", "verify_symbols",
         ["UGameplayStatics::ApplyDamage", "AActor", "UThisDoesNotExistAnywhereXYZ"], "text"),
        ("source.find_example_usage", "source", "find_example_usage",
         ["UGameplayStatics::ApplyDamage", "--limit", "5"], "text"),
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

        # ---- cppreflect (40 new) -- engine/GAS/input/UI/anim/movement reflection bundles ----
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
        ("cppreflect.get_uclass/UPawnMovementComponent", "cppreflect", "get_uclass", ["UPawnMovementComponent"]),
        ("cppreflect.get_uclass/APawn", "cppreflect", "get_uclass", ["APawn"]),
        ("cppreflect.get_uclass/ADefaultPawn", "cppreflect", "get_uclass", ["ADefaultPawn"]),
        ("cppreflect.get_uclass/ASpectatorPawn", "cppreflect", "get_uclass", ["ASpectatorPawn"]),
        ("cppreflect.get_uclass/AHUD", "cppreflect", "get_uclass", ["AHUD"]),
        ("cppreflect.get_uclass/AInfo", "cppreflect", "get_uclass", ["AInfo"]),
        ("cppreflect.get_uclass/AController", "cppreflect", "get_uclass", ["AController"]),
        ("cppreflect.get_uclass/AAIController", "cppreflect", "get_uclass", ["AAIController"]),
        ("cppreflect.get_uclass/USkeletalMeshComponent", "cppreflect", "get_uclass", ["USkeletalMeshComponent"]),
        ("cppreflect.get_uclass/UStaticMeshComponent", "cppreflect", "get_uclass", ["UStaticMeshComponent"]),
        ("cppreflect.get_uclass/USceneComponent", "cppreflect", "get_uclass", ["USceneComponent"]),
        ("cppreflect.get_uclass/UPrimitiveComponent", "cppreflect", "get_uclass", ["UPrimitiveComponent"]),
        ("cppreflect.get_uclass/UWorld", "cppreflect", "get_uclass", ["UWorld"]),
        ("cppreflect.get_uclass/UGameInstance", "cppreflect", "get_uclass", ["UGameInstance"]),
        ("cppreflect.get_uclass/ULevel", "cppreflect", "get_uclass", ["ULevel"]),
        ("cppreflect.get_uclass/UGameViewportClient", "cppreflect", "get_uclass", ["UGameViewportClient"]),
        ("cppreflect.list_uproperties/UActorComponent", "cppreflect", "list_uproperties", ["UActorComponent"]),
        ("cppreflect.list_uproperties/APawn", "cppreflect", "list_uproperties", ["APawn"]),
        ("cppreflect.list_uproperties/UWorld", "cppreflect", "list_uproperties", ["UWorld"]),
        ("cppreflect.list_uproperties/UGameInstance", "cppreflect", "list_uproperties", ["UGameInstance"]),
        ("cppreflect.list_ufunctions/UActorComponent", "cppreflect", "list_ufunctions", ["UActorComponent"]),
        ("cppreflect.list_ufunctions/APawn", "cppreflect", "list_ufunctions", ["APawn"]),
        ("cppreflect.list_ufunctions/UWorld", "cppreflect", "list_ufunctions", ["UWorld"]),
        ("cppreflect.find_interface_impls/IGameplayTagAssetInterface", "cppreflect", "find_interface_impls", ["IGameplayTagAssetInterface"]),
        ("cppreflect.find_interface_impls/IGenericTeamAgentInterface", "cppreflect", "find_interface_impls", ["IGenericTeamAgentInterface"]),
        ("cppreflect.find_class_specifier/DefaultToInstanced", "cppreflect", "find_class_specifier", ["DefaultToInstanced"]),
        ("cppreflect.find_class_specifier/EditInlineNew", "cppreflect", "find_class_specifier", ["EditInlineNew"]),
        ("cppreflect.find_class_specifier/Transient", "cppreflect", "find_class_specifier", ["Transient"]),
        ("cppreflect.find_class_specifier/HideDropdown", "cppreflect", "find_class_specifier", ["HideDropdown"]),

        # ---- network (20 new) -- additional limit variants + determinism repeats ----
        ("network.list_replicated_classes/limit2", "network", "list_replicated_classes", ["--limit", "2"]),
        ("network.list_replicated_classes/limit3", "network", "list_replicated_classes", ["--limit", "3"]),
        ("network.list_replicated_classes/limit4", "network", "list_replicated_classes", ["--limit", "4"]),
        ("network.list_replicated_classes/limit6", "network", "list_replicated_classes", ["--limit", "6"]),
        ("network.list_replicated_classes/limit7", "network", "list_replicated_classes", ["--limit", "7"]),
        ("network.list_replicated_classes/limit8", "network", "list_replicated_classes", ["--limit", "8"]),
        ("network.list_replicated_classes/limit15", "network", "list_replicated_classes", ["--limit", "15"]),
        ("network.list_replicated_classes/limit25", "network", "list_replicated_classes", ["--limit", "25"]),
        ("network.list_replicated_classes/limit30", "network", "list_replicated_classes", ["--limit", "30"]),
        ("network.list_replicated_classes/limit100", "network", "list_replicated_classes", ["--limit", "100"]),
        ("network.list_rpc_functions/limit2", "network", "list_rpc_functions", ["--limit", "2"]),
        ("network.list_rpc_functions/limit3", "network", "list_rpc_functions", ["--limit", "3"]),
        ("network.list_rpc_functions/limit30", "network", "list_rpc_functions", ["--limit", "30"]),
        ("network.list_rpc_functions/limit50", "network", "list_rpc_functions", ["--limit", "50"]),
        ("network.list_onrep_handlers/limit2", "network", "list_onrep_handlers", ["--limit", "2"]),
        ("network.list_onrep_handlers/limit3", "network", "list_onrep_handlers", ["--limit", "3"]),
        ("network.list_onrep_handlers/limit30", "network", "list_onrep_handlers", ["--limit", "30"]),
        ("network.list_onrep_handlers/limit50", "network", "list_onrep_handlers", ["--limit", "50"]),
        ("network.audit_unbalanced_onreps/det3", "network", "audit_unbalanced_onreps", []),
        ("network.audit_unbalanced_onreps/det4", "network", "audit_unbalanced_onreps", []),

        # ---- decision (20 new) -- additional limit + staleness-window variants ----
        ("decision.list_decisions/limit2", "decision", "list_decisions", ["--limit", "2"]),
        ("decision.list_decisions/limit3", "decision", "list_decisions", ["--limit", "3"]),
        ("decision.list_decisions/limit4", "decision", "list_decisions", ["--limit", "4"]),
        ("decision.list_decisions/limit6", "decision", "list_decisions", ["--limit", "6"]),
        ("decision.list_decisions/limit7", "decision", "list_decisions", ["--limit", "7"]),
        ("decision.list_decisions/limit8", "decision", "list_decisions", ["--limit", "8"]),
        ("decision.list_decisions/limit15", "decision", "list_decisions", ["--limit", "15"]),
        ("decision.list_decisions/limit25", "decision", "list_decisions", ["--limit", "25"]),
        ("decision.list_decisions/limit30", "decision", "list_decisions", ["--limit", "30"]),
        ("decision.list_decisions/limit100", "decision", "list_decisions", ["--limit", "100"]),
        ("decision.list_stale/14d", "decision", "list_stale", ["14", "--limit", "5"]),
        ("decision.list_stale/45d", "decision", "list_stale", ["45", "--limit", "5"]),
        ("decision.list_stale/60d", "decision", "list_stale", ["60", "--limit", "5"]),
        ("decision.list_stale/120d", "decision", "list_stale", ["120", "--limit", "5"]),
        ("decision.list_stale/270d", "decision", "list_stale", ["270", "--limit", "5"]),
        ("decision.list_stale/545d", "decision", "list_stale", ["545", "--limit", "5"]),
        ("decision.list_stale/1095d", "decision", "list_stale", ["1095", "--limit", "5"]),
        ("decision.list_stale/2920d", "decision", "list_stale", ["2920", "--limit", "5"]),
        ("decision.list_stale/5475d", "decision", "list_stale", ["5475", "--limit", "5"]),
        ("decision.list_stale/7300d", "decision", "list_stale", ["7300", "--limit", "5"]),

        # ---- risk (20 new) -- additional Scripts/Docs file hotspot + churn targets ----
        ("risk.get_hotspot_score/index_project", "risk", "get_hotspot_score", ["Scripts/index_project.py"]),
        ("risk.get_hotspot_score/tag_path_params", "risk", "get_hotspot_score", ["Scripts/tag_path_params.py"]),
        ("risk.get_hotspot_score/lint_agent_tools", "risk", "get_hotspot_score", ["Scripts/lint_agent_tools.py"]),
        ("risk.get_hotspot_score/check_offline_exe_fresh", "risk", "get_hotspot_score", ["Scripts/check_offline_exe_fresh.py"]),
        ("risk.get_hotspot_score/check_index_freshness", "risk", "get_hotspot_score", ["Scripts/check_index_freshness.ps1"]),
        ("risk.get_hotspot_score/recover_mcp", "risk", "get_hotspot_score", ["Scripts/recover_mcp.ps1"]),
        ("risk.get_hotspot_score/prune_invocation_logs", "risk", "get_hotspot_score", ["Scripts/prune_invocation_logs.ps1"]),
        ("risk.get_hotspot_score/onboard_monolith", "risk", "get_hotspot_score", ["Scripts/onboard_monolith.ps1"]),
        ("risk.get_hotspot_score/install_monolith_skills", "risk", "get_hotspot_score", ["Scripts/install_monolith_skills.ps1"]),
        ("risk.get_hotspot_score/validate_monolith_skills", "risk", "get_hotspot_score", ["Scripts/validate_monolith_skills.ps1"]),
        ("risk.get_hotspot_score/SPEC", "risk", "get_hotspot_score", ["Docs/SPEC.md"]),
        ("risk.get_hotspot_score/API_REFERENCE", "risk", "get_hotspot_score", ["Docs/API_REFERENCE.md"]),
        ("risk.get_hotspot_score/TODO", "risk", "get_hotspot_score", ["Docs/TODO.md"]),
        ("risk.get_hotspot_score/README", "risk", "get_hotspot_score", ["README.md"]),
        ("risk.get_hotspot_score/Monolith", "risk", "get_hotspot_score", ["Monolith.uplugin"]),
        ("risk.get_file_churn/index_project", "risk", "get_file_churn", ["Scripts/index_project.py"]),
        ("risk.get_file_churn/SPEC", "risk", "get_file_churn", ["Docs/SPEC.md"]),
        ("risk.get_file_churn/README", "risk", "get_file_churn", ["README.md"]),
        ("risk.get_file_churn/Monolith", "risk", "get_file_churn", ["Monolith.uplugin"]),
        ("risk.get_file_churn/tag_path_params", "risk", "get_file_churn", ["Scripts/tag_path_params.py"]),
    ]

    benchmark_extension = [
        # ---- cppreflect (30 benchmark-only) -- gameplay, UI, data, networking classes ----
        ("cppreflect.get_uclass/AGameSession", "cppreflect", "get_uclass", ["AGameSession"]),
        ("cppreflect.get_uclass/AGameNetworkManager", "cppreflect", "get_uclass", ["AGameNetworkManager"]),
        ("cppreflect.get_uclass/APlayerCameraManager", "cppreflect", "get_uclass", ["APlayerCameraManager"]),
        ("cppreflect.get_uclass/UPlayer", "cppreflect", "get_uclass", ["UPlayer"]),
        ("cppreflect.get_uclass/ULocalPlayer", "cppreflect", "get_uclass", ["ULocalPlayer"]),
        ("cppreflect.get_uclass/UNetDriver", "cppreflect", "get_uclass", ["UNetDriver"]),
        ("cppreflect.get_uclass/UGameUserSettings", "cppreflect", "get_uclass", ["UGameUserSettings"]),
        ("cppreflect.get_uclass/UCanvas", "cppreflect", "get_uclass", ["UCanvas"]),
        ("cppreflect.get_uclass/UDamageType", "cppreflect", "get_uclass", ["UDamageType"]),
        ("cppreflect.get_uclass/UGameplayStatics", "cppreflect", "get_uclass", ["UGameplayStatics"]),
        ("cppreflect.get_uclass/UBlueprintFunctionLibrary", "cppreflect", "get_uclass", ["UBlueprintFunctionLibrary"]),
        ("cppreflect.get_uclass/UDataAsset", "cppreflect", "get_uclass", ["UDataAsset"]),
        ("cppreflect.get_uclass/UPrimaryDataAsset", "cppreflect", "get_uclass", ["UPrimaryDataAsset"]),
        ("cppreflect.get_uclass/UCurveFloat", "cppreflect", "get_uclass", ["UCurveFloat"]),
        ("cppreflect.get_uclass/UDataTable", "cppreflect", "get_uclass", ["UDataTable"]),
        ("cppreflect.list_uproperties/APlayerController", "cppreflect", "list_uproperties", ["APlayerController"]),
        ("cppreflect.list_uproperties/APlayerState", "cppreflect", "list_uproperties", ["APlayerState"]),
        ("cppreflect.list_uproperties/UPrimitiveComponent", "cppreflect", "list_uproperties", ["UPrimitiveComponent"]),
        ("cppreflect.list_uproperties/USkeletalMeshComponent", "cppreflect", "list_uproperties", ["USkeletalMeshComponent"]),
        ("cppreflect.list_uproperties/UStaticMeshComponent", "cppreflect", "list_uproperties", ["UStaticMeshComponent"]),
        ("cppreflect.list_uproperties/UUserWidget", "cppreflect", "list_uproperties", ["UUserWidget"]),
        ("cppreflect.list_ufunctions/UGameInstance", "cppreflect", "list_ufunctions", ["UGameInstance"]),
        ("cppreflect.list_ufunctions/AController", "cppreflect", "list_ufunctions", ["AController"]),
        ("cppreflect.list_ufunctions/AAIController", "cppreflect", "list_ufunctions", ["AAIController"]),
        ("cppreflect.list_ufunctions/UUserWidget", "cppreflect", "list_ufunctions", ["UUserWidget"]),
        ("cppreflect.find_interface_impls/INavAgentInterface", "cppreflect", "find_interface_impls", ["INavAgentInterface"]),
        ("cppreflect.find_interface_impls/IInterface_AssetUserData", "cppreflect", "find_interface_impls", ["IInterface_AssetUserData"]),
        ("cppreflect.find_class_specifier/BlueprintSpawnableComponent", "cppreflect", "find_class_specifier", ["BlueprintSpawnableComponent"]),
        ("cppreflect.find_class_specifier/BlueprintAuthorityOnly", "cppreflect", "find_class_specifier", ["BlueprintAuthorityOnly"]),
        ("cppreflect.find_class_specifier/Within", "cppreflect", "find_class_specifier", ["Within"]),

        # ---- network (10 benchmark-only) -- filtered RPC/OnRep and pagination checks ----
        ("network.list_rpc_functions/class_APlayerController", "network", "list_rpc_functions", ["--class_name", "APlayerController", "--limit", "10"]),
        ("network.list_rpc_functions/class_ACharacter", "network", "list_rpc_functions", ["--class_name", "ACharacter", "--limit", "10"]),
        ("network.list_rpc_functions/rpc_Server", "network", "list_rpc_functions", ["--rpc_kind", "Server", "--limit", "10"]),
        ("network.list_rpc_functions/rpc_Client", "network", "list_rpc_functions", ["--rpc_kind", "Client", "--limit", "10"]),
        ("network.list_rpc_functions/rpc_Multicast", "network", "list_rpc_functions", ["--rpc_kind", "Multicast", "--limit", "10"]),
        ("network.list_onrep_handlers/class_APlayerState", "network", "list_onrep_handlers", ["--class_name", "APlayerState", "--limit", "10"]),
        ("network.list_onrep_handlers/class_AGameStateBase", "network", "list_onrep_handlers", ["--class_name", "AGameStateBase", "--limit", "10"]),
        ("network.audit_unbalanced_onreps/limit1", "network", "audit_unbalanced_onreps", ["--limit", "1"]),
        ("network.audit_unbalanced_onreps/limit10", "network", "audit_unbalanced_onreps", ["--limit", "10"]),
        ("network.list_replicated_classes/limit75", "network", "list_replicated_classes", ["--limit", "75"]),

        # ---- decision (15 benchmark-only) -- agent decision-record filters and chain depths ----
        ("decision.list_decisions/path_docs", "decision", "list_decisions", ["--path_filter", "Docs", "--limit", "10"]),
        ("decision.list_decisions/path_source", "decision", "list_decisions", ["--path_filter", "Source", "--limit", "10"]),
        ("decision.list_decisions/status_accepted", "decision", "list_decisions", ["--status", "accepted", "--limit", "10"]),
        ("decision.list_decisions/status_superseded", "decision", "list_decisions", ["--status", "superseded", "--limit", "10"]),
        ("decision.list_decisions/min_confidence_0_5", "decision", "list_decisions", ["--min_confidence", "0.5", "--limit", "10"]),
        ("decision.list_decisions/min_confidence_0_8", "decision", "list_decisions", ["--min_confidence", "0.8", "--limit", "10"]),
        ("decision.list_decisions/min_confidence_0_95", "decision", "list_decisions", ["--min_confidence", "0.95", "--limit", "10"]),
        ("decision.list_decisions/path_scripts_min_confidence_0_5", "decision", "list_decisions", ["--path_filter", "Scripts", "--min_confidence", "0.5", "--limit", "10"]),
        ("decision.list_stale/365d_docs", "decision", "list_stale", ["365", "--path_filter", "Docs", "--limit", "10"]),
        ("decision.list_stale/3650d_scripts", "decision", "list_stale", ["3650", "--path_filter", "Scripts", "--limit", "10"]),
        ("decision.list_stale/0d", "decision", "list_stale", ["0", "--limit", "10"]),
        ("decision.list_stale/9999d_limit10", "decision", "list_stale", ["9999", "--limit", "10"]),
        ("decision.find_supersession_chain/depth1", "decision", "find_supersession_chain", [did, "--depth", "1"] if did else None),
        ("decision.find_supersession_chain/depth3", "decision", "find_supersession_chain", [did, "--depth", "3"] if did else None),
        ("decision.get_decision/repeat", "decision", "get_decision", [did] if did else None),

        # ---- risk (20 benchmark-only) -- benchmark/script/doc hotspots and report filters ----
        ("risk.get_cochange_pairs/offline_parity_benchmark", "risk", "get_cochange_pairs", ["Scripts/offline_parity_benchmark.py"]),
        ("risk.get_cochange_pairs/verify_offline_parity", "risk", "get_cochange_pairs", ["Scripts/verify_offline_parity.py"]),
        ("risk.get_cochange_pairs/benchmark_readme", "risk", "get_cochange_pairs", ["Benchmarks/OfflineParity/README.md"]),
        ("risk.get_cochange_pairs/benchmark_metrics", "risk", "get_cochange_pairs", ["Benchmarks/OfflineParity/METRICS.md"]),
        ("risk.get_cochange_pairs/benchmark_results", "risk", "get_cochange_pairs", ["Benchmarks/OfflineParity/RESULTS.md"]),
        ("risk.get_cochange_pairs/SPEC_CORE", "risk", "get_cochange_pairs", ["Docs/SPEC_CORE.md"]),
        ("risk.get_cochange_pairs/API_REFERENCE", "risk", "get_cochange_pairs", ["Docs/API_REFERENCE.md"]),
        ("risk.get_cochange_pairs/TODO", "risk", "get_cochange_pairs", ["Docs/TODO.md"]),
        ("risk.get_cochange_pairs/README", "risk", "get_cochange_pairs", ["README.md"]),
        ("risk.get_cochange_pairs/Monolith", "risk", "get_cochange_pairs", ["Monolith.uplugin"]),
        ("risk.list_conditional_gates/WITH_EDITOR", "risk", "list_conditional_gates", ["--macro_filter", "WITH_EDITOR", "--limit", "10"]),
        ("risk.list_conditional_gates/WITH_EDITORONLY_DATA", "risk", "list_conditional_gates", ["--macro_filter", "WITH_EDITORONLY_DATA", "--limit", "10"]),
        ("risk.list_conditional_gates/UE_BUILD_SHIPPING", "risk", "list_conditional_gates", ["--macro_filter", "UE_BUILD_SHIPPING", "--limit", "10"]),
        ("risk.list_conditional_gates/path_monolith_source", "risk", "list_conditional_gates", ["--path_filter", "Source/MonolithSource%", "--limit", "10"]),
        ("risk.list_conditional_gates/path_reflection_intel", "risk", "list_conditional_gates", ["--path_filter", "Source/MonolithReflectionIntel%", "--limit", "10"]),
        ("risk.get_release_window_hotspots/limit1", "risk", "get_release_window_hotspots", ["--limit", "1"]),
        ("risk.get_release_window_hotspots/limit10", "risk", "get_release_window_hotspots", ["--limit", "10"]),
        ("risk.get_release_window_hotspots/since0_limit5", "risk", "get_release_window_hotspots", ["--since_unix", "0", "--limit", "5"]),
        ("risk.get_release_window_hotspots/since1700000000_limit5", "risk", "get_release_window_hotspots", ["--since_unix", "1700000000", "--limit", "5"]),
        ("risk.get_release_window_hotspots/since1600000000_limit20", "risk", "get_release_window_hotspots", ["--since_unix", "1600000000", "--limit", "20"]),

        # ---- source ergonomics (25 benchmark-only) -- API lookup and expected-error parity ----
        ("source.get_include_path/UGameInstance", "source", "get_include_path", ["UGameInstance"], "text"),
        ("source.get_include_path/UWorld", "source", "get_include_path", ["UWorld"], "text"),
        ("source.get_include_path/UUserWidget", "source", "get_include_path", ["UUserWidget"], "text"),
        ("source.get_include_path/UGameplayStatics", "source", "get_include_path", ["UGameplayStatics"], "text"),
        ("source.get_include_path/UDataTable", "source", "get_include_path", ["UDataTable"], "text"),
        ("source.get_signature/UGameplayStatics_OpenLevel", "source", "get_signature", ["UGameplayStatics::OpenLevel"], "text"),
        ("source.get_signature/UGameplayStatics_SpawnEmitter", "source", "get_signature", ["UGameplayStatics::SpawnEmitterAtLocation"], "text"),
        ("source.get_signature/UActorComponent_Activate", "source", "get_signature", ["UActorComponent::Activate"], "text"),
        ("source.get_signature/APlayerController_ClientTravel", "source", "get_signature", ["APlayerController::ClientTravel"], "text"),
        ("source.get_signature/AController_SetPawn", "source", "get_signature", ["AController::SetPawn"], "text"),
        ("source.check_deprecations/AController_APawn", "source", "check_deprecations", ["AController", "APawn"], "text"),
        ("source.check_deprecations/UWorld_UGameInstance", "source", "check_deprecations", ["UWorld", "UGameInstance"], "text"),
        ("source.check_deprecations/UDataTable_UDataAsset", "source", "check_deprecations", ["UDataTable", "UDataAsset"], "text"),
        ("source.verify_symbols/Input_UI", "source", "verify_symbols", ["UInputAction", "UInputMappingContext", "UUserWidget"], "text"),
        ("source.verify_symbols/GameFrameworkTravel", "source", "verify_symbols", ["APlayerController::ClientTravel", "AController::SetPawn", "UGameInstance"], "text"),
        ("source.verify_symbols/GAS", "source", "verify_symbols", ["UAbilitySystemComponent", "UGameplayAbility", "UGameplayEffect"], "text"),
        ("source.verify_symbols/DataAssets", "source", "verify_symbols", ["UDataAsset", "UPrimaryDataAsset", "UDataTable"], "text"),
        ("source.find_example_usage/OpenLevel_limit3", "source", "find_example_usage", ["UGameplayStatics::OpenLevel", "--limit", "3"], "text"),
        ("source.find_example_usage/ClientTravel_limit3", "source", "find_example_usage", ["APlayerController::ClientTravel", "--limit", "3"], "text"),
        ("source.find_example_usage/SpawnEmitter_limit2", "source", "find_example_usage", ["UGameplayStatics::SpawnEmitterAtLocation", "--limit", "2"], "text"),
        ("source.get_signature/missing_symbol(expected-error)", "source", "get_signature", ["UThisDoesNotExistAnywhereXYZ::Nope"], "text", {"expected_error": True}),
        ("source.get_include_path/missing_symbol(expected-error)", "source", "get_include_path", ["UThisDoesNotExistAnywhereXYZ"], "text", {"expected_error": True}),
        ("source.generate_class_stub/missing_parent(expected-error)", "source", "generate_class_stub", ["UThisDoesNotExistAnywhereXYZ", "AMyBadActor", "TestModule"], "text", {"expected_error": True}),
        ("source.lint_header/missing_file(expected-error)", "source", "lint_header", ["Source/DoesNotExist/Missing.h"], "text", {"expected_error": True}),
        ("source.read_file/missing_file(expected-error)", "source", "read_file", ["Source/DoesNotExist/Missing.h"], "text", {"expected_error": True}),
    ]
    if len(benchmark_extension) != BENCHMARK_EXTENSION_ACTION_COUNT:
        raise AssertionError(
            f"benchmark extension must contain {BENCHMARK_EXTENSION_ACTION_COUNT} actions; "
            f"got {len(benchmark_extension)}"
        )
    actions.extend(benchmark_extension)
    if len(actions) != EXPECTED_ACTION_COUNT:
        raise AssertionError(f"offline parity action table must contain {EXPECTED_ACTION_COUNT} actions; got {len(actions)}")
    return actions


# ------------------------------------------------------------------ per-action runner
# (inlined from verify_offline_parity.py, with benchmark diagnostics)


def unpack_action_entry(action_entry: tuple) -> Tuple[str, str, str, Optional[List[str]], str, Dict[str, Any]]:
    if len(action_entry) == 4:
        lbl, ns, action, aargs = action_entry
        return lbl, ns, action, aargs, "json", {}
    if len(action_entry) == 5:
        lbl, ns, action, aargs, extra = action_entry
        if isinstance(extra, dict):
            return lbl, ns, action, aargs, str(extra.get("compare", "json")), dict(extra)
        return lbl, ns, action, aargs, str(extra), {}
    if len(action_entry) == 6:
        lbl, ns, action, aargs, compare, metadata = action_entry
        if not isinstance(metadata, dict):
            raise TypeError(f"action metadata must be a dict for {lbl}")
        return lbl, ns, action, aargs, str(compare), dict(metadata)
    raise ValueError(f"unsupported action entry shape: {action_entry!r}")


def run_action(
    label: str,
    ns: str,
    action: str,
    args: List[str],
    ignore_cursor_bytes: bool,
    exe_path: pathlib.Path,
    py_path: pathlib.Path,
    mono_root: pathlib.Path,
    compare: str = "json",
    expected_error: bool = False,
) -> Dict[str, Any]:
    """
    Returns a result dict:
      status: MATCH | DIFF | ERROR
      diffs:  list of (path, exe_val, py_val)
      warnings: list of (path, exe_val, py_val)
      error:  str | None
      error_kind: none | expected | expected_missing | expected_mismatch | real
    """
    res: Dict[str, Any] = {
        "label": label,
        "args": args,
        "status": None,
        "diffs": [],
        "warnings": [],
        "error": None,
        "expected_error": expected_error,
        "error_kind": "none",
        "exe_exit_code": None,
        "py_exit_code": None,
        "error_sources": {},
    }

    erc, eout, eerr = run_exe(ns, action, args, exe_path, mono_root)
    prc, pout, perr = run_py(ns, action, args, py_path, mono_root)
    res["exe_exit_code"] = erc
    res["py_exit_code"] = prc

    if expected_error:
        res["error_sources"] = {
            "exe": _process_diagnostics(erc, eout, eerr),
            "py": _process_diagnostics(prc, pout, perr),
        }
        exe_failed = erc != 0
        py_failed = prc != 0
        if exe_failed and py_failed:
            res["status"] = "MATCH"
            res["error_kind"] = "expected"
            if erc != prc:
                res["warnings"] = [("expected_error.exit_code", erc, prc)]
            return res
        if exe_failed != py_failed:
            res["status"] = "DIFF"
            res["error_kind"] = "expected_mismatch"
            res["error"] = "expected error mismatch: only one tool exited non-zero"
            res["diffs"] = [("expected_error.nonzero", exe_failed, py_failed)]
            return res
        res["status"] = "DIFF"
        res["error_kind"] = "expected_missing"
        res["error"] = "expected error was not observed; both tools exited 0"
        res["diffs"] = [("expected_error.nonzero", "both tools should exit non-zero", "both tools exited 0")]
        return res

    err_parts = []
    if erc != 0:
        err_parts.append(f"EXE exit={erc} stderr={eerr.strip()[:400]!r}")
    if prc != 0:
        err_parts.append(f"PY exit={prc} stderr={perr.strip()[:400]!r}")

    if err_parts:
        res["status"] = "ERROR"
        res["error_kind"] = "real"
        res["error_sources"] = {
            "exe": _process_diagnostics(erc, eout, eerr),
            "py": _process_diagnostics(prc, pout, perr),
        }
        res["error"] = " | ".join(err_parts)
        return res

    if compare == "text":
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
        res["error_kind"] = "real"
        res["error_sources"] = {
            "exe": _process_diagnostics(erc, eout, eerr),
            "py": _process_diagnostics(prc, pout, perr),
        }
        res["error"] = " | ".join(err_parts)
        return res

    eff_ignore_cursor = ignore_cursor_bytes or is_time_derived_cursor_action(label, args)
    diffs, warnings = deep_diff(edata, pdata, ignore_cursor_bytes=eff_ignore_cursor)
    res["diffs"] = diffs
    res["warnings"] = warnings
    res["status"] = "MATCH" if not diffs else "DIFF"
    return res


def skipped_action(label: str, reason: str) -> Dict[str, Any]:
    return {
        "label": label,
        "args": [],
        "status": "SKIP",
        "diffs": [],
        "warnings": [],
        "error": reason,
        "expected_error": False,
        "error_kind": "skip",
        "exe_exit_code": None,
        "py_exit_code": None,
        "error_sources": {},
    }


# ------------------------------------------------------------------ version parity
# (inlined from verify_offline_parity.py)


def check_version_parity(
    exe_path: pathlib.Path,
    py_path: pathlib.Path,
    mono_root: pathlib.Path,
) -> Tuple[bool, Optional[str], Optional[str]]:
    """Compare parity_spec_rev between exe and py. Returns (ok, exe_rev, py_rev)."""
    _, eout, _ = _run([str(exe_path), "--version"], mono_root)
    _, pout, _ = _run([sys.executable, str(py_path), "--version"], mono_root)

    exe_rev = None
    try:
        exe_rev = json.loads(eout).get("parity_spec_rev")
    except Exception:  # noqa: BLE001
        exe_rev = eout.strip() or None
    py_rev = pout.strip() or None
    return (exe_rev is not None and exe_rev == py_rev), exe_rev, py_rev


# ------------------------------------------------------------------ scoring


def _build_category_breakdown(results: List[Dict[str, Any]]) -> Dict[str, Any]:
    """
    Group per-action results by namespace (the label prefix before the first '.').
    Returns a dict keyed by namespace with match/diff/error/skip counts and
    action_count (non-skipped) for each namespace.
    """
    from collections import defaultdict

    groups: Dict[str, List[Dict[str, Any]]] = defaultdict(list)
    for r in results:
        ns = r["label"].split(".")[0]
        groups[ns].append(r)

    breakdown: Dict[str, Any] = {}
    for ns, rows in sorted(groups.items()):
        statuses = [r["status"] for r in rows]
        n_match = statuses.count("MATCH")
        n_diff = statuses.count("DIFF")
        n_err = statuses.count("ERROR")
        n_skip = statuses.count("SKIP")
        n_expected = sum(1 for r in rows if r.get("error_kind") == "expected")
        n_expected_problem = sum(
            1 for r in rows
            if r.get("error_kind") in ("expected_missing", "expected_mismatch")
        )
        n_real_error = sum(1 for r in rows if r.get("error_kind") == "real")
        action_count = len(statuses) - n_skip
        breakdown[ns] = {
            "match": n_match,
            "diff": n_diff,
            "error": n_err,
            "skip": n_skip,
            "action_count": action_count,
            "expected_error": n_expected,
            "expected_error_problem": n_expected_problem,
            "real_error": n_real_error,
        }
    return breakdown


def build_error_diagnostics(results: List[Dict[str, Any]]) -> Dict[str, Any]:
    expected_matches = [r["label"] for r in results if r.get("error_kind") == "expected"]
    expected_problems = [
        r["label"] for r in results
        if r.get("error_kind") in ("expected_missing", "expected_mismatch")
    ]
    real_errors = [r["label"] for r in results if r.get("error_kind") == "real"]
    return {
        "expected_error": {
            "match_count": len(expected_matches),
            "problem_count": len(expected_problems),
            "labels": expected_matches,
            "problem_labels": expected_problems,
        },
        "real_error": {
            "count": len(real_errors),
            "labels": real_errors,
        },
    }


def compute_metrics(
    results: List[Dict[str, Any]],
    version_parity_ok: bool,
) -> Dict[str, Any]:
    """
    Compute aggregate metrics from per-action results.

    Score formula:
        offline_parity_score = (
            0.50 * match_rate           # fraction of non-skipped with status=MATCH
          + 0.20 * (1 - diff_rate)      # 1 - fraction of non-skipped with status=DIFF
          + 0.20 * (1 - error_rate)     # 1 - fraction of non-skipped with status=ERROR
          + 0.10 * version_parity_score # 1.0 if parity_spec_rev matches, else 0.0
        )
    Note: match_rate + diff_rate + error_rate = 1.0 for non-skipped actions.
    So the formula simplifies to:
        offline_parity_score = 0.70 * match_rate + 0.20 * (1 - error_rate) + 0.10 * version_parity_score
    """
    total = len(results)
    n_match = sum(1 for r in results if r["status"] == "MATCH")
    n_diff = sum(1 for r in results if r["status"] == "DIFF")
    n_err = sum(1 for r in results if r["status"] == "ERROR")
    n_skip = sum(1 for r in results if r["status"] == "SKIP")
    comparable = total - n_skip
    n_expected_error = sum(1 for r in results if r.get("error_kind") == "expected")
    n_expected_error_problem = sum(
        1 for r in results
        if r.get("error_kind") in ("expected_missing", "expected_mismatch")
    )
    n_real_error = sum(1 for r in results if r.get("error_kind") == "real")

    match_rate = n_match / comparable if comparable > 0 else 0.0
    diff_rate = n_diff / comparable if comparable > 0 else 0.0
    error_rate = n_err / comparable if comparable > 0 else 0.0
    skip_rate = n_skip / total if total > 0 else 0.0
    expected_error_rate = n_expected_error / comparable if comparable > 0 else 0.0
    real_error_rate = n_real_error / comparable if comparable > 0 else 0.0
    version_parity_score = 1.0 if version_parity_ok else 0.0

    if comparable == 0:
        offline_parity_score = 0.0
    else:
        offline_parity_score = (
            0.50 * match_rate
            + 0.20 * (1.0 - diff_rate)
            + 0.20 * (1.0 - error_rate)
            + 0.10 * version_parity_score
        )

    diff_action_results = [r for r in results if r["status"] == "DIFF"]
    diff_counts = [len(r["diffs"]) for r in diff_action_results]
    mean_diffs = sum(diff_counts) / len(diff_counts) if diff_counts else 0.0

    return {
        "offline_parity_score": round(offline_parity_score, 6),
        "match_rate": round(match_rate, 6),
        "diff_rate": round(diff_rate, 6),
        "error_rate": round(error_rate, 6),
        "skip_rate": round(skip_rate, 6),
        "expected_error_rate": round(expected_error_rate, 6),
        "real_error_rate": round(real_error_rate, 6),
        "version_parity_score": round(version_parity_score, 6),
        "total_actions": total,
        "comparable_actions": comparable,
        "action_count": comparable,
        "expected_error_count": n_expected_error,
        "expected_error_problem_count": n_expected_error_problem,
        "real_error_count": n_real_error,
        "mean_diffs_per_diff_action": round(mean_diffs, 6),
        "category_breakdown": _build_category_breakdown(results),
    }


def build_summary(
    label: str,
    results: List[Dict[str, Any]],
    version_parity_ok: bool,
    exe_rev: Optional[str],
    py_rev: Optional[str],
    chain: Dict[str, Any],
    ignore_cursor_bytes: bool,
) -> Dict[str, Any]:
    metrics = compute_metrics(results, version_parity_ok)
    n_match = sum(1 for r in results if r["status"] == "MATCH")
    n_diff = sum(1 for r in results if r["status"] == "DIFF")
    n_err = sum(1 for r in results if r["status"] == "ERROR")
    n_skip = sum(1 for r in results if r["status"] == "SKIP")
    return {
        "label": label,
        "created_at": utc_now(),
        "ignore_cursor_bytes": ignore_cursor_bytes,
        "version": {
            "exe_parity_spec_rev": exe_rev,
            "py_parity_spec_rev": py_rev,
            "version_parity_ok": version_parity_ok,
        },
        "chain_inputs": chain,
        "counts": {
            "match": n_match,
            "diff": n_diff,
            "error": n_err,
            "skip": n_skip,
            "total": len(results),
        },
        "metrics": metrics,
        "diagnostics": build_error_diagnostics(results),
    }


def build_per_action_row(r: Dict[str, Any]) -> Dict[str, Any]:
    """Flatten a run_action result into a compact JSONL row."""
    return {
        "label": r["label"],
        "status": r["status"],
        "diff_count": len(r.get("diffs", [])),
        "warning_count": len(r.get("warnings", [])),
        "error": r.get("error"),
        "expected_error": bool(r.get("expected_error")),
        "error_kind": r.get("error_kind", "none"),
        "exe_exit_code": r.get("exe_exit_code"),
        "py_exit_code": r.get("py_exit_code"),
    }


# ------------------------------------------------------------------ run subcommand


def cmd_run(args: argparse.Namespace) -> int:
    exe_path = pathlib.Path(args.exe_path).resolve() if args.exe_path else DEFAULT_EXE_PATH
    py_path = pathlib.Path(args.py_path).resolve() if args.py_path else DEFAULT_PY_PATH
    output_dir = pathlib.Path(args.output_dir).resolve()
    label = args.label
    ignore_cursor_bytes = args.ignore_cursor_bytes

    output_dir.mkdir(parents=True, exist_ok=True)

    exe_missing = not exe_path.exists()
    py_missing = not py_path.exists()

    if exe_missing:
        print(f"[WARN] exe not found at {exe_path} -- all exe actions will be skipped gracefully.")
    if py_missing:
        print(f"[WARN] py not found at {py_path} -- all py actions will be skipped gracefully.")

    print(f"Offline parity benchmark -- label={label!r}")
    print(f"  exe = {exe_path}  (exists={not exe_missing})")
    print(f"  py  = {py_path}  (exists={not py_missing})")
    print(f"  output-dir = {output_dir}")
    print(f"  ignore-cursor-bytes = {ignore_cursor_bytes}\n")

    # Version parity
    ver_ok: bool = False
    exe_rev: Optional[str] = None
    py_rev: Optional[str] = None
    if not exe_missing and not py_missing:
        ver_ok, exe_rev, py_rev = check_version_parity(exe_path, py_path, MONO_ROOT)
    else:
        ver_ok = False
    print(f"Version parity: exe_rev={exe_rev!r}  py_rev={py_rev!r}  ok={ver_ok}\n")

    # Chain discovery (requires exe)
    if not exe_missing:
        chain = discover_chain_inputs(exe_path, MONO_ROOT)
    else:
        chain = {"uclass": None, "decision_id": None, "risk_path": "Docs/SPEC_CORE.md"}
    benchmark_inputs = build_benchmark_inputs(
        "OfflineParity",
        extra_files={"offline_exe": exe_path, "offline_python": py_path},
        plugin_root=MONO_ROOT,
    )
    print(
        f"Chain inputs: uclass={chain.get('uclass')!r} "
        f"decision_id={chain.get('decision_id')!r} "
        f"risk_path={chain.get('risk_path')!r}\n"
    )

    actions = build_actions(chain)
    results: List[Dict[str, Any]] = []
    per_action_jsonl_path = output_dir / "per_action.jsonl"
    if per_action_jsonl_path.exists():
        per_action_jsonl_path.unlink()

    PARTIAL_INTERVAL = 5

    for index, action_entry in enumerate(actions, 1):
        lbl, ns, action, aargs, compare, metadata = unpack_action_entry(action_entry)

        if exe_missing or py_missing:
            r = skipped_action(lbl, "tool not found: " + ("exe" if exe_missing else "py"))
        elif aargs is None:
            r = skipped_action(lbl, "current DB corpus has no decision_id input")
        else:
            r = run_action(
                lbl, ns, action, aargs, ignore_cursor_bytes,
                exe_path, py_path, MONO_ROOT, compare,
                expected_error=bool(metadata.get("expected_error")),
            )

        results.append(r)
        row = build_per_action_row(r)
        with per_action_jsonl_path.open("a", encoding="utf-8", newline="\n") as fh:
            fh.write(json.dumps(row, ensure_ascii=False, sort_keys=True))
            fh.write("\n")

        status_sym = {"MATCH": ".", "DIFF": "D", "ERROR": "E", "SKIP": "S"}.get(r["status"], "?")
        print(
            f"[{index:2d}/{len(actions)}] {status_sym} {r['label']}"
            + (f"  diffs={len(r['diffs'])}" if r["status"] == "DIFF" else "")
            + (f"  err={r['error'][:80]!r}" if r["status"] == "ERROR" else ""),
            flush=True,
        )

        if index % PARTIAL_INTERVAL == 0 or index == len(actions):
            partial = build_summary(
                label, results, ver_ok, exe_rev, py_rev, chain, ignore_cursor_bytes
            )
            partial["completed_action_count"] = index
            partial["total_action_count"] = len(actions)
            attach_benchmark_inputs(partial, benchmark_inputs)
            write_json(output_dir / "partial_summary.json", partial)

    summary = build_summary(label, results, ver_ok, exe_rev, py_rev, chain, ignore_cursor_bytes)
    attach_benchmark_inputs(summary, benchmark_inputs)
    write_json(output_dir / "summary.json", summary)

    m = summary["metrics"]
    c = summary["counts"]
    print(f"\n{'='*72}")
    print(
        f"RESULT: {c['match']}/{c['match']+c['diff']+c['error']} MATCH "
        f"| {c['diff']} DIFF | {c['error']} ERROR | {c['skip']} SKIP"
    )
    print(f"offline_parity_score = {m['offline_parity_score']:.4f}")
    print(f"{'='*72}")
    print(json.dumps(summary, indent=2, ensure_ascii=False))
    return 0


# ------------------------------------------------------------------ compare subcommand


def cmd_compare(args: argparse.Namespace) -> int:
    baseline_path = pathlib.Path(args.baseline).resolve()
    current_path = pathlib.Path(args.current).resolve()
    output_dir = pathlib.Path(args.output_dir).resolve()

    baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
    current = json.loads(current_path.read_text(encoding="utf-8"))

    base_metrics = baseline.get("metrics", {})
    cur_metrics = current.get("metrics", {})

    deltas: Dict[str, Any] = {}
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
    _write_comparison_markdown(output_dir / "comparison.md", comparison)

    print(json.dumps({"output_dir": str(output_dir), "deltas": deltas}, indent=2, ensure_ascii=False))
    return 0


def _write_comparison_markdown(path: pathlib.Path, comparison: Dict[str, Any]) -> None:
    baseline = comparison["baseline"]
    current = comparison["current"]
    deltas = comparison["deltas"]

    metrics_order = [
        "offline_parity_score",
        "match_rate",
        "diff_rate",
        "error_rate",
        "skip_rate",
        "expected_error_rate",
        "real_error_rate",
        "version_parity_score",
        "total_actions",
        "comparable_actions",
        "expected_error_count",
        "expected_error_problem_count",
        "real_error_count",
        "mean_diffs_per_diff_action",
    ]

    lines = [
        "# Monolith Offline Parity Benchmark Comparison",
        "",
        f"- Created: `{comparison['created_at']}`",
        f"- Baseline: `{baseline.get('label', '?')}`",
        f"- Current: `{current.get('label', '?')}`",
        "",
        "| Metric | Baseline | Current | Delta |",
        "| --- | ---: | ---: | ---: |",
    ]
    for metric in metrics_order:
        base_value = baseline.get("metrics", {}).get(metric)
        cur_value = current.get("metrics", {}).get(metric)
        delta = deltas.get(metric, "")
        lines.append(f"| `{metric}` | {base_value} | {cur_value} | {delta} |")

    lines += [
        "",
        "Higher is better for `offline_parity_score`, `match_rate`, `version_parity_score`.",
        "Lower is better for `diff_rate`, `error_rate`, `real_error_rate`, `skip_rate`, `mean_diffs_per_diff_action`.",
        "`expected_error_*` fields track negative-case parity and are additive diagnostics.",
    ]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


# ------------------------------------------------------------------ report subcommand


def cmd_report(args: argparse.Namespace) -> int:
    summary_path = pathlib.Path(args.summary).resolve()
    summary = json.loads(summary_path.read_text(encoding="utf-8"))

    label = summary.get("label", "?")
    created_at = summary.get("created_at", "?")
    m = summary.get("metrics", {})
    c = summary.get("counts", {})
    v = summary.get("version", {})
    d = summary.get("diagnostics", {})
    expected_diag = d.get("expected_error", {})
    real_diag = d.get("real_error", {})

    print(f"Offline Parity Benchmark Report")
    print(f"  label      : {label}")
    print(f"  created_at : {created_at}")
    print(f"  exe_rev    : {v.get('exe_parity_spec_rev', '?')}")
    print(f"  py_rev     : {v.get('py_parity_spec_rev', '?')}")
    print(f"  ver_ok     : {v.get('version_parity_ok', '?')}")
    print()
    print(f"  {'offline_parity_score':<30} {m.get('offline_parity_score', '?')}")
    print(f"  {'match_rate':<30} {m.get('match_rate', '?')}")
    print(f"  {'diff_rate':<30} {m.get('diff_rate', '?')}")
    print(f"  {'error_rate':<30} {m.get('error_rate', '?')}")
    print(f"  {'skip_rate':<30} {m.get('skip_rate', '?')}")
    print(f"  {'expected_error_rate':<30} {m.get('expected_error_rate', '?')}")
    print(f"  {'real_error_rate':<30} {m.get('real_error_rate', '?')}")
    print(f"  {'version_parity_score':<30} {m.get('version_parity_score', '?')}")
    print(f"  {'total_actions':<30} {m.get('total_actions', '?')}")
    print(f"  {'comparable_actions':<30} {m.get('comparable_actions', '?')}")
    print(f"  {'expected_error_count':<30} {m.get('expected_error_count', '?')}")
    print(f"  {'expected_error_problem_count':<30} {m.get('expected_error_problem_count', '?')}")
    print(f"  {'real_error_count':<30} {m.get('real_error_count', '?')}")
    print(f"  {'mean_diffs_per_diff_action':<30} {m.get('mean_diffs_per_diff_action', '?')}")
    print()
    print(
        f"  Counts: {c.get('match', 0)} MATCH | {c.get('diff', 0)} DIFF "
        f"| {c.get('error', 0)} ERROR | {c.get('skip', 0)} SKIP"
    )
    print(
        f"  Error diagnostics: expected={expected_diag.get('match_count', 0)} "
        f"expected_problem={expected_diag.get('problem_count', 0)} "
        f"real={real_diag.get('count', 0)}"
    )
    return 0


# ------------------------------------------------------------------ entry point


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = parser.add_subparsers(dest="cmd", required=True)

    run_p = sub.add_parser("run", help="Run the offline parity benchmark and produce summary.json")
    run_p.add_argument(
        "--exe-path",
        default=None,
        help=f"Path to monolith_query.exe (default: Binaries/monolith_query.exe relative to plugin root)",
    )
    run_p.add_argument(
        "--py-path",
        default=None,
        help=f"Path to monolith_offline.py (default: Scripts/monolith_offline.py relative to plugin root)",
    )
    run_p.add_argument("--output-dir", required=True, help="Directory to write summary.json, per_action.jsonl, partial_summary.json")
    run_p.add_argument("--label", required=True, help="Run label (e.g. 'current', 'v1.2.3')")
    run_p.add_argument(
        "--ignore-cursor-bytes",
        action="store_true",
        help="Decode cursors and compare {p,tc}; treat qh mismatch as WARNING.",
    )

    cmp_p = sub.add_parser("compare", help="Compare two summary.json files and produce comparison.json + comparison.md")
    cmp_p.add_argument("--baseline", required=True, help="Path to baseline summary.json")
    cmp_p.add_argument("--current", required=True, help="Path to current summary.json")
    cmp_p.add_argument("--output-dir", required=True, help="Directory to write comparison output")

    rep_p = sub.add_parser("report", help="Print a human-readable summary of a summary.json")
    rep_p.add_argument("--summary", required=True, help="Path to summary.json")

    args = parser.parse_args(argv)
    if args.cmd == "run":
        return cmd_run(args)
    if args.cmd == "compare":
        return cmd_compare(args)
    if args.cmd == "report":
        return cmd_report(args)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
