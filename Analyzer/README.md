# Monolith Invocation Log Analyzer

Local read-only analyzer for Monolith invocation logs.

Run from the Monolith plugin root:

```powershell
python Analyzer/analyze_invocation_logs.py --log-root Logs --out Saved/Monolith/LogAnalysis/latest --format markdown,json,csv
```

The analyzer reads `Logs/yyyyMMdd/{proxy,action,query}.jsonl`, normalizes mixed
record versions, classifies heartbeat/test/maintenance noise, and emits:

- `summary.md`
- `findings.json`
- `action_stats.csv`
- `slow_calls.csv`
- `duplicates.csv`
- `parse_warnings.csv`
- `normalized.jsonl` when `--emit-normalized-jsonl` is passed

Fixture smoke:

```powershell
python Analyzer/analyze_invocation_logs.py --log-root Analyzer/fixtures/invocation_logs --out Saved/Monolith/LogAnalysis/fixture --format markdown,json,csv
```

The tool does not mutate `Logs/` and does not open Monolith SQLite databases.

Client-side MCP availability failures can happen before a call reaches Monolith,
so they never appear in `Logs/yyyyMMdd/action.jsonl`. Use the session transcript
analyzer for that blind spot:

```powershell
python Analyzer/analyze_session_transcripts.py --codex-root $HOME\.codex\sessions --claude-root $HOME\.claude\projects --out Saved/Monolith/SessionAnalysis/latest
```

Use both reports for the improvement loop: `LogAnalysis` ranks server-captured
action/query/proxy issues, while `SessionAnalysis` ranks server-blind transport
failures such as direct clients failing to reach `localhost:9316`.
