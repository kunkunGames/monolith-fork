#!/usr/bin/env python3
"""Monolith client-side session-transcript analyzer (SPEC_MonolithToolCallReliabilityBacklog.md s6C).

Server-side invocation logs (Logs/{action,query,proxy}.jsonl) only record calls that
*reach* Monolith. Calls that die at the transport (endpoint localhost:9316 down,
"Session not found", connection closed) never produce a server row, so the server-side
analyzer is structurally blind to MCP availability failures. Those failures live only in
the agent client session transcripts.

This read-only sibling of analyze_invocation_logs.py parses Codex
(~/.codex/sessions/**/*.jsonl) and Claude Code (~/.claude/projects/**/*.jsonl) rollouts
and reports an `mcp_availability` finding: how many Monolith MCP tool calls failed, split
into transport/availability (server-blind) vs server-captured vs other.

CRITICAL: it classifies only structured tool-RESULT payloads (Codex `function_call_output`,
Claude `tool_result`). Raw grep over transcripts is contaminated because the injected
CLAUDE.md text itself contains `9316`, `recover_mcp`, and `ECONNREFUSED`; those live in
prompt/instruction records, never in a tool result, so this reader ignores them.

Safety: never writes under Logs/, never opens SQLite DBs, emits counts + marker keywords
only (no raw payloads unless --include-samples, which bounds + redacts).
"""

import argparse
import json
import os
import re
import glob
from collections import Counter, defaultdict
from datetime import datetime

# --- classification markers (matched against tool-RESULT text only) ----------------------

# Generic "the MCP tool call did not succeed" signature. Codex wraps a failed call as
# "tool call error: tool call failed for `monolith/<tool>`\n\nCaused by: ...". This marks the
# result as an error; the TRANSPORT/SERVER_CAPTURED regexes below then say which kind.
TOOL_FAIL = re.compile(
    r"tool call error|tool call failed|Transport send error|rmcp::transport|McpError|MCP error|"
    r'"isError"\s*:\s*true',
    re.I,
)
# Transport / availability: the call did not get a real answer from Monolith (server-blind).
# NOTE: never match bare "9316" — it appears in successful monolith_status output ("server_port":9316).
TRANSPORT = re.compile(
    r"Transport send error|rmcp::transport|ECONNREFUSED|ECONNRESET|EPIPE|socket hang up|fetch failed|"
    r"connection (?:refused|closed|reset|error)|not connected|"
    r"Session not found|session (?:expired|closed)|Protocol version mismatch|"
    r"failed to (?:connect|reach|start)|MCP (?:server )?(?:unreachable|not (?:running|connected|available)|down)|"
    r"endpoint (?:unreachable|not reachable)|unreachable|timed out|request timed out|"
    r"transport (?:error|closed)",
    re.I,
)
# Server-captured: Monolith answered with a structured rejection (also in action.jsonl).
SERVER_CAPTURED = re.compile(
    r"Missing required param|Invalid param|Unknown namespace|Unknown action|Unknown tool|"
    r"No (?:symbol|file) found|-32601|-32602|-32603|validation error|requires ",
    re.I,
)

DATE_RX = re.compile(r"(20\d{2})[-/]?(\d{2})[-/]?(\d{2})")


def is_monolith(name):
    return bool(name) and "monolith" in name.lower()


def path_date(path):
    """Best-effort YYYYMMDD from a Codex session path/filename (Claude uuids have none)."""
    m = DATE_RX.search(path.replace("\\", "/"))
    return f"{m.group(1)}{m.group(2)}{m.group(3)}" if m else None


def classify(text):
    if TRANSPORT.search(text):
        return "transport_availability"
    if SERVER_CAPTURED.search(text):
        return "server_captured"
    return "other_error"


def result_is_error(output, explicit_is_error):
    """Decide if a tool result is an error from structured signals first, text last.

    Codex wraps the real result inside a "Wall time: ...\\nOutput:\\n[...]" string, so the
    error signature ("tool call error", "Transport send error", isError:true) is embedded,
    not at the start. TOOL_FAIL catches that; structured flags are checked first.
    """
    if explicit_is_error is True:
        return True
    if isinstance(output, dict):
        if output.get("success") is False or output.get("is_error") is True or output.get("isError") is True:
            return True
        err = output.get("error")
        if isinstance(err, dict) and ("code" in err or "message" in err):
            return True
    txt = output if isinstance(output, str) else json.dumps(output, ensure_ascii=False)
    if TOOL_FAIL.search(txt):
        return True
    low = txt.lower()
    return (
        '"success": false' in low
        or '"success":false' in low
        or low.lstrip().startswith("error:")
    )


def as_text(output):
    if output is None:
        return ""
    if isinstance(output, str):
        return output
    return json.dumps(output, ensure_ascii=False)


def iter_codex(path):
    """Yield (tool_name, is_error, result_text) for Monolith calls in a Codex rollout."""
    names = {}  # call_id -> tool name
    for line in _lines(path):
        rec = _loads(line)
        if not rec or rec.get("type") != "response_item":
            continue
        payload = rec.get("payload") if isinstance(rec.get("payload"), dict) else rec
        ptype = payload.get("type", "")
        if ptype in ("function_call", "custom_tool_call", "local_shell_call"):
            cid = payload.get("call_id") or payload.get("id")
            nm = payload.get("name") or ""
            if cid:
                names[cid] = nm
        elif ptype in ("function_call_output", "custom_tool_call_output", "local_shell_call_output"):
            cid = payload.get("call_id") or payload.get("id")
            nm = names.get(cid, "")
            if not is_monolith(nm):
                continue
            output = payload.get("output")
            yield nm, result_is_error(output, None), as_text(output)


def iter_claude(path):
    """Yield (tool_name, is_error, result_text) for Monolith calls in a Claude transcript."""
    names = {}  # tool_use_id -> name
    for line in _lines(path):
        rec = _loads(line)
        if not rec:
            continue
        msg = rec.get("message")
        content = msg.get("content") if isinstance(msg, dict) else None
        if not isinstance(content, list):
            continue
        for it in content:
            if not isinstance(it, dict):
                continue
            t = it.get("type")
            if t == "tool_use":
                if it.get("id"):
                    names[it["id"]] = it.get("name") or ""
            elif t == "tool_result":
                nm = names.get(it.get("tool_use_id"), "")
                if not is_monolith(nm):
                    continue
                out = it.get("content")
                yield nm, result_is_error(out, it.get("is_error")), as_text(out)


def _lines(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                line = line.strip()
                if line:
                    yield line
    except OSError:
        return


def _loads(line):
    try:
        return json.loads(line)
    except json.JSONDecodeError:
        return None


def discover(codex_root, claude_root):
    sources = []
    if codex_root and os.path.isdir(codex_root):
        for p in glob.glob(os.path.join(codex_root, "**", "*.jsonl"), recursive=True):
            sources.append(("codex", p))
    if claude_root and os.path.isdir(claude_root):
        for p in glob.glob(os.path.join(claude_root, "**", "*.jsonl"), recursive=True):
            sources.append(("claude", p))
    return sources


def in_window(date, since, until):
    if date is None:
        return since is None and until is None or True  # undated (Claude): keep, filter later if needed
    if since and date < since:
        return False
    if until and date > until:
        return False
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    home = os.path.expanduser("~")
    ap.add_argument("--codex-root", default=os.path.join(home, ".codex", "sessions"))
    ap.add_argument("--claude-root", default=os.path.join(home, ".claude", "projects"))
    ap.add_argument("--since", help="YYYYMMDD; filter dated (Codex) sessions on/after")
    ap.add_argument("--until", help="YYYYMMDD; filter dated (Codex) sessions on/before")
    ap.add_argument("--out", default=None, help="output dir (default Saved/Monolith/SessionAnalysis/<ts>)")
    ap.add_argument("--include-samples", action="store_true", help="include bounded redacted error samples")
    ap.add_argument("--stamp", default=None, help="timestamp label for the default out dir")
    args = ap.parse_args()

    sources = discover(args.codex_root, args.claude_root)

    totals = Counter()                       # calls, errors
    by_source = defaultdict(Counter)         # source -> {calls, errors, transport_availability, server_captured, other_error}
    by_bucket = Counter()
    by_day = defaultdict(Counter)            # day -> {calls, errors, transport_availability,...}
    sessions_with_mono = set()
    sessions_with_transport = set()
    transport_markers = Counter()
    samples = []
    files_scanned = 0
    sample_seen = set()

    for source, path in sources:
        day = path_date(path) if source == "codex" else None
        if day is not None and not in_window(day, args.since, args.until):
            continue
        files_scanned += 1
        emit = iter_codex(path) if source == "codex" else iter_claude(path)
        per_session_mono = 0
        for name, is_err, text in emit:
            per_session_mono += 1
            totals["calls"] += 1
            by_source[source]["calls"] += 1
            if day:
                by_day[day]["calls"] += 1
            if not is_err:
                continue
            totals["errors"] += 1
            by_source[source]["errors"] += 1
            bucket = classify(text)
            by_bucket[bucket] += 1
            by_source[source][bucket] += 1
            if day:
                by_day[day]["errors"] += 1
                by_day[day][bucket] += 1
            if bucket == "transport_availability":
                sessions_with_transport.add(path)
                m = TRANSPORT.search(text)
                if m:
                    transport_markers[m.group(0).lower()] += 1
                if args.include_samples:
                    snippet = re.sub(r"\s+", " ", text)[:120]
                    snippet = re.sub(r"(token|key|secret|password|bearer)[=:]\S+", r"\1=<redacted>", snippet, flags=re.I)
                    key = (name, snippet)
                    if key not in sample_seen and len(samples) < 25:
                        sample_seen.add(key)
                        samples.append({"source": source, "tool": name, "snippet": snippet})
        if per_session_mono:
            sessions_with_mono.add(path)

    transport = by_bucket["transport_availability"]
    server = by_bucket["server_captured"]
    other = by_bucket["other_error"]
    errors = totals["errors"]
    calls = totals["calls"]

    finding = {
        "finding_id": "mcp_availability:client_session_transcripts",
        "category": "mcp_availability",
        "severity": "high" if transport >= 10 else ("medium" if transport > 0 else "info"),
        "title": "Client-side Monolith MCP availability failures (server-blind)",
        "summary": {
            "files_scanned": files_scanned,
            "monolith_tool_results": calls,
            "monolith_errors": errors,
            "error_rate": round(errors / calls, 3) if calls else 0.0,
            "transport_availability": transport,
            "server_captured": server,
            "other_error": other,
            "sessions_with_monolith": len(sessions_with_mono),
            "sessions_with_transport_failure": len(sessions_with_transport),
            "transport_share_of_errors": round(transport / errors, 3) if errors else 0.0,
        },
        "by_source": {s: dict(c) for s, c in by_source.items()},
        "transport_markers": dict(transport_markers.most_common()),
        "recommendation": (
            "transport_availability failures are invisible to server-side action.jsonl. Investigate MCP "
            "endpoint uptime at localhost:9316 and the recover_mcp.ps1 / RunHeadlessEditor reconnect path "
            "(CLAUDE.md s14); server_captured errors are already covered by analyze_invocation_logs.py."
        ),
        "measurement_caveat": (
            "This scan only sees tool RESULTS. A client that cannot connect at all (ConnectionRefused at "
            "MCP handshake/reconnect) produces no tool result and is NOT counted as a transport failure. "
            "A low transport count in a window with near-zero Monolith calls is therefore not evidence of "
            "endpoint health; cross-check watchdog.jsonl uptime and per-source call volume against the "
            "baseline window before claiming an availability improvement."
        ),
    }
    if args.include_samples:
        finding["transport_samples"] = samples

    out = args.out or os.path.join(
        "Saved", "Monolith", "SessionAnalysis", args.stamp or datetime.now().strftime("%Y%m%d-%H%M%S")
    )
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "findings.json"), "w", encoding="utf-8") as fh:
        json.dump({"schema_version": 1, "finding": finding, "by_day": {d: dict(c) for d, c in sorted(by_day.items())}}, fh, indent=2, ensure_ascii=False)

    md = []
    md.append("# Monolith Client-Side MCP Availability (session transcripts)\n")
    md.append(f"- Files scanned: `{files_scanned}`")
    md.append(f"- Monolith tool results: `{calls}` across `{len(sessions_with_mono)}` sessions")
    md.append(f"- Errors: `{errors}` (rate `{finding['summary']['error_rate']}`)")
    md.append(f"- **Transport/availability (server-blind): `{transport}`** across `{len(sessions_with_transport)}` sessions "
              f"(`{finding['summary']['transport_share_of_errors']}` of errors)")
    md.append(f"- Server-captured (already in action.jsonl): `{server}`  ·  other: `{other}`\n")
    md.append("## By source\n\n| Source | Calls | Errors | Transport | Server-captured | Other |\n|---|---:|---:|---:|---:|---:|")
    for s, c in sorted(by_source.items()):
        md.append(f"| {s} | {c['calls']} | {c['errors']} | {c['transport_availability']} | {c['server_captured']} | {c['other_error']} |")
    md.append("\n## Transport markers (from tool results only)\n\n| Marker | Count |\n|---|---:|")
    for k, v in transport_markers.most_common():
        md.append(f"| `{k}` | {v} |")
    if by_day:
        md.append("\n## By day (dated Codex sessions)\n\n| Day | Calls | Errors | Transport |\n|---|---:|---:|---:|")
        for d, c in sorted(by_day.items()):
            md.append(f"| {d} | {c['calls']} | {c['errors']} | {c['transport_availability']} |")
    md.append(f"\n## Recommendation\n\n{finding['recommendation']}\n")
    md.append(f"\n## Measurement caveat\n\n{finding['measurement_caveat']}\n")
    with open(os.path.join(out, "summary.md"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(md))

    print(f"Scanned {files_scanned} session files. Monolith results: {calls}, errors: {errors} "
          f"(transport={transport}, server_captured={server}, other={other}).")
    print(f"Report: {os.path.join(out, 'summary.md')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
