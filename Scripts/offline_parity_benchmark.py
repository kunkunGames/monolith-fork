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

# Externalized benchmark definition (gets hosted-static-CI line-count validation
# via .github/monolith-static-ci.json -> benchmark_definitions). The action table
# lives in actions.jsonl as data, not as a hard-coded Python literal.
DEFAULT_ACTIONS = MONO_ROOT / "Benchmarks" / "OfflineParity" / "actions.jsonl"
DEFAULT_MANIFEST = MONO_ROOT / "Benchmarks" / "OfflineParity" / "manifest.json"

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

# The action table is externalized to actions.jsonl; manifest.json carries the
# authoritative count. EXPECTED_ACTION_COUNT is kept as a documented fallback for
# environments where the manifest is unavailable, and is asserted against the
# loaded table so drift between the data file and this constant is loud.
EXPECTED_ACTION_COUNT = 317

# Placeholder tokens substituted from chain-discovery at load time. Keeping them
# as explicit constants makes the actions.jsonl contract self-documenting.
TOKEN_UCLASS = "{{uclass}}"
TOKEN_DECISION_ID = "{{decision_id}}"
TOKEN_RISK_PATH = "{{risk_path}}"

# error_kind classification groups (shared by category breakdown, metrics, and
# diagnostics so the buckets stay consistent).
#   - EXPECTED_MATCH_KINDS: the two offline tools AGREE on a failure that is not a
#     regression (bad-input probe, or an offline-unsupported surface). These keep
#     status=MATCH and must NOT drag the score down.
#   - EXPECTED_PROBLEM_KINDS: a negative/offline-unsupported row where the tools
#     DISAGREE (genuine exe-vs-py parity break). These fail.
EXPECTED_MATCH_KINDS = ("expected", "expected_offline")
EXPECTED_PROBLEM_KINDS = ("expected_missing", "expected_mismatch", "offline_parity_break")


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
# Externalized to Benchmarks/OfflineParity/actions.jsonl (one action per line) so
# the table is reviewable as data and gets hosted-static-CI line-count validation.


def _read_manifest_action_count():
    """Return the manifest's authoritative action_count, or None if unavailable."""
    try:
        manifest = json.loads(DEFAULT_MANIFEST.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    count = manifest.get("action_count")
    return count if isinstance(count, int) else None


def _substitute_tokens(args, chain):
    """
    Replace placeholder tokens with chain-discovered values.

    Returns None (-> SKIP) when a required chain input (currently decision_id) is
    missing, mirroring the legacy `[did] if did else None` behavior.
    """
    out = []
    for raw in args:
        if raw == TOKEN_UCLASS:
            value = chain.get("uclass")
        elif raw == TOKEN_DECISION_ID:
            value = chain.get("decision_id")
        elif raw == TOKEN_RISK_PATH:
            value = chain.get("risk_path")
        else:
            out.append(str(raw))
            continue
        if value is None:
            return None
        out.append(str(value))
    return out


def load_action_specs(actions_path=None):
    """
    Load the raw action specs from actions.jsonl (no token substitution yet).

    Each line is a JSON object with keys:
      label, namespace, action, args (list, may contain placeholder tokens),
      and optional: compare ("text"|"json"), expected_error (bool),
      offline_unsupported (bool), requires ("decision_id").

    Validates against the manifest action_count and EXPECTED_ACTION_COUNT so a
    drifted data file fails loudly instead of silently changing the denominator.
    """
    path = actions_path or DEFAULT_ACTIONS
    if not path.exists():
        raise FileNotFoundError(f"offline parity action table not found: {path}")

    specs = []
    seen_labels = set()
    with path.open("r", encoding="utf-8") as handle:
        for line_no, line in enumerate(handle, 1):
            stripped = line.strip()
            if not stripped:
                continue
            try:
                spec = json.loads(stripped)
            except ValueError as exc:
                raise ValueError(f"{path.name}:{line_no} is not valid JSON: {exc}")
            for key in ("label", "namespace", "action", "args"):
                if key not in spec:
                    raise ValueError(f"{path.name}:{line_no} missing required key {key!r}")
            if not isinstance(spec["args"], list):
                raise ValueError(f"{path.name}:{line_no} 'args' must be a list")
            label = spec["label"]
            if label in seen_labels:
                raise ValueError(f"{path.name}:{line_no} duplicate label {label!r}")
            seen_labels.add(label)
            specs.append(spec)

    manifest_count = _read_manifest_action_count()
    if manifest_count is not None and len(specs) != manifest_count:
        raise AssertionError(
            f"{path.name} has {len(specs)} actions but manifest action_count is "
            f"{manifest_count}; update manifest.json and the data file together."
        )
    if len(specs) != EXPECTED_ACTION_COUNT:
        raise AssertionError(
            f"offline parity action table must contain {EXPECTED_ACTION_COUNT} actions; "
            f"got {len(specs)} (update EXPECTED_ACTION_COUNT and manifest.json together)."
        )
    return specs


def build_actions(chain, actions_path=None):
    """
    Load the externalized parity action table and bind chain-discovered args.

    Each returned entry is (label, namespace, action, args_or_None, compare, metadata)
    where compare defaults to "json" ("text" uses strict byte-compare) and metadata
    carries expected_error / offline_unsupported flags consumed by run_action.
    args_or_None is None when a required chain input is missing -> the row is SKIPped.
    """
    actions = []
    for spec in load_action_specs(actions_path):
        args = _substitute_tokens(spec["args"], chain)
        compare = str(spec.get("compare", "json"))
        metadata = {}
        if spec.get("expected_error"):
            metadata["expected_error"] = True
        if spec.get("offline_unsupported"):
            metadata["offline_unsupported"] = True
        actions.append((spec["label"], spec["namespace"], spec["action"], args, compare, metadata))
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
    offline_unsupported: bool = False,
) -> Dict[str, Any]:
    """
    Returns a result dict:
      status: MATCH | DIFF | ERROR
      diffs:  list of (path, exe_val, py_val)
      warnings: list of (path, exe_val, py_val)
      error:  str | None
      error_kind: none | expected | expected_offline | offline_parity_break
                  | expected_missing | expected_mismatch | real

    Buckets:
      - expected_error: deliberate bad-input probe; both tools SHOULD fail.
        both-fail -> MATCH(expected); otherwise DIFF.
      - offline_unsupported: the offline surface may legitimately be absent.
        Parity is about agreement, so if BOTH offline tools fail the same way the
        row is a MATCH(expected_offline) instead of a real ERROR that masks signal.
        Only a genuine exe-vs-py disagreement (exactly one tool fails) is a
        DIFF(offline_parity_break). If both succeed, outputs compare normally.
      - normal: either tool failing is a real ERROR.
    """
    res: Dict[str, Any] = {
        "label": label,
        "args": args,
        "status": None,
        "diffs": [],
        "warnings": [],
        "error": None,
        "expected_error": expected_error,
        "offline_unsupported": offline_unsupported,
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

    if offline_unsupported and (erc != 0 or prc != 0):
        # The offline surface may legitimately be absent. Score by AGREEMENT:
        # both tools failing is parity (MATCH expected_offline); exactly one
        # failing is a genuine exe-vs-py disagreement (DIFF offline_parity_break).
        res["error_sources"] = {
            "exe": _process_diagnostics(erc, eout, eerr),
            "py": _process_diagnostics(prc, pout, perr),
        }
        exe_failed = erc != 0
        py_failed = prc != 0
        if exe_failed and py_failed:
            res["status"] = "MATCH"
            res["error_kind"] = "expected_offline"
            if erc != prc:
                res["warnings"] = [("offline_unsupported.exit_code", erc, prc)]
            return res
        res["status"] = "DIFF"
        res["error_kind"] = "offline_parity_break"
        res["error"] = (
            "offline parity break: only one tool exited non-zero on an "
            "offline-unsupported action (exe={0}, py={1})".format(erc, prc)
        )
        res["diffs"] = [("offline_unsupported.nonzero", exe_failed, py_failed)]
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
        "offline_unsupported": False,
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
        n_expected = sum(1 for r in rows if r.get("error_kind") in EXPECTED_MATCH_KINDS)
        n_expected_problem = sum(
            1 for r in rows
            if r.get("error_kind") in EXPECTED_PROBLEM_KINDS
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
    expected_matches = [r["label"] for r in results if r.get("error_kind") in EXPECTED_MATCH_KINDS]
    expected_problems = [
        r["label"] for r in results
        if r.get("error_kind") in EXPECTED_PROBLEM_KINDS
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
    n_expected_error = sum(1 for r in results if r.get("error_kind") in EXPECTED_MATCH_KINDS)
    n_expected_error_problem = sum(
        1 for r in results
        if r.get("error_kind") in EXPECTED_PROBLEM_KINDS
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
        "offline_unsupported": bool(r.get("offline_unsupported")),
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
                offline_unsupported=bool(metadata.get("offline_unsupported")),
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
