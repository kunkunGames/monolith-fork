# CRG Review Extensions — RX-1 / RX-2 / RX-3 / RX-7 / RX-8 Verification

| | |
|---|---|
| Date | 2026-05-17 |
| Branch | `feat/crg-index-navigation-p0` (PR #447) |
| Spec | `Docs/specs/SPEC_MonolithSource.md`, `Docs/specs/SPEC_MonolithIndex.md` |
| Scope | RX-2 (offline CRG cache read parity), RX-1 (offline `detect_changes`), RX-3 (offline `find_unused`), RX-7 (scoring v3 sensitivity), RX-8 (`review_hotspots`) |

---

## 1. Build

- `Tools/MonolithQuery/build.bat` (MSVC cl.exe, `/std:c++17 /O2 /MT`, `-DSQLITE_ENABLE_FTS5`)
  → `Result: Succeeded`, copied to `Plugins/Monolith/Binaries/monolith_query.exe`.
- Only the pre-existing `C4819` encoding warning (unchanged from prior builds).
- No editor module changed → UBT `GoGameEditor` build not required for this change
  (offline tool is a standalone cl.exe target; CLAUDE.md UBT command applies to
  `Source/**` edits, none here).

## 2. RX-2 — offline CRG cache read parity (live DBs)

| Command | Result |
|---|---|
| `source risk_score IsValid` | `scoring_version=2`, `summary` "CRG cache hit", item `cache.status=hit` (`cache_version=1`, `scoring_version=2`) |
| `source health` | 25 checks incl. 10 `crg:*`. Correctly `warning` on `crg:edges_row_parity` (valid native edges 3,080,547 vs crg_edges 3,161,799) and `crg:orphan_edges` 81,252 — accurately surfaces the known stale dangling-ref cache. No regression: offline source health was already `warning` from the native `integrity:orphan_references` check. |
| `project risk_score /Game/Developers/TH/King/Textures/King` | `scoring_version=2`, item `cache.status=hit` |
| `project health` | 10 `crg:*` checks, all `ok` (project cache clean, 0 orphans, parity exact) |

Cache-absent path (`crg:cache_absent` `info`, no warning) preserves offline
`health` "ok" for pre-cache DBs per projection-cache `REQ-006`.

## 3. RX-1 — offline `detect_changes` (live DBs)

| Command | Result |
|---|---|
| `source detect_changes ADPCMAudioInfo.cpp --max-results=30` (minimal) | `status=ok`, `scoring_version=2`, 14 symbols mapped, `risk=low`, 14 advisory test-gaps, top-3 `review_priorities`, minimal keys only |
| `source detect_changes ADPCMAudioInfo.cpp --detail-level=standard --max-results=10` | `changed_entities=10` (capped + `truncated`), per-entity `cache.status=hit`/`scoring_version=2`, `impact{depth:1}`, `test_gaps` with `confidence=medium` + heuristic reason |
| `project detect_changes Content/Developers/TH/King/Textures/King.uasset --detail-level=standard` | `status=ok`, `scoring_version=2`, King `Texture2D` `cache.status=hit`, `impact.impacted_count=47` (matches known ~48 fan-in) |

RX-1 correctly reuses the RX-2 cached risk (`scoring_version=2`, `cache.status=hit`)
and falls back to query-time when the cache is absent.

## 3b. RX-3 — offline `find_unused` (live DBs)

| Command | Result |
|---|---|
| `source find_unused --kind=function --limit=3` | `ok`, 3 items, `truncated`, `confidence=medium`, 3 reasons each |
| `source find_unused --kind=class --limit=3` | `ok`, 3 items, confidence graded by name-ambiguity |
| `project find_unused --limit=5` (default `min-confidence=low`) | `ok`, returns orphan assets (e.g. `DT_*` DataTables) `confidence=low` with reasons; `truncated` |
| `project find_unused --min-confidence=medium` | filters to 0 here (all 77 orphans are indirect-reference classes → honest `low`) |
| raw SQL cross-check | 81 assets never a dep target; 77 after root-class exclusion — matches the action |

Bug found & fixed during test: default `min_confidence=medium` hid every
genuine orphan (contradicts the spec's own recall-first mandate). Corrected
to default `low` in code + spec RX-3 contract.

## 3c. Sequential build + test of reflected features (clean rebuild from committed source)

Working copy verified identical to committed `80ebc05`; full clean rebuild
(obj/exe deleted) → `build.bat` PASS (only pre-existing C4819).

| # | Test | Result |
|---|---|---|
| RX-2.1 | source `risk_score` cache hit (sv=2, `cache.status=hit`) | PASS |
| RX-2.2 | source `health` emits 10 `crg:*` checks | PASS |
| RX-2.3 | project `risk_score` cache hit (sv=2) | PASS |
| RX-2.4 | project `health` 10 `crg:*` all ok | PASS |
| RX-2.5a | cache-absent (temp DB, `crg_*` dropped) → query-time fallback sv=1, `cache.status=unavailable` | PASS |
| RX-2.5b | cache-absent → `crg:cache_absent` info, no crg warning (no regression; native orphan check still drives `warning` as before) | PASS |
| RX-1.1 | source `detect_changes` minimal, sv=2 (reuses RX-2 cache) | PASS |
| RX-1.2 | source `detect_changes` standard: entities+impact+test_gaps+priorities, truncates at max-results | PASS |
| RX-1.3 | source `detect_changes` no-path → graceful error | PASS |
| RX-1.4 | project `detect_changes` cache hit + bounded impact (King → 47) | PASS |
| RX-1.5 | project `detect_changes` unknown path → ok, minimal-shaped, 0 entities, clear summary | PASS (after fix) |
| RX-3.1 | source `find_unused` function: items + confidence + reasons + truncation | PASS |
| RX-3.2 | source `find_unused --min-confidence=medium` filters out `low` | PASS |
| RX-3.3 | project `find_unused` default(low) returns orphans | PASS |
| RX-3.4 | project `find_unused --min-confidence=high` → 0 (never reports `high`; advisory) | PASS |
| RX-3.5 | `find_unused` never mutates (symbols/assets counts unchanged) | PASS |

**Bug found & fixed during sequential test (RX-1.5):** project
`detect_changes` empty-match path early-returned and skipped minimal/standard
shaping (no `changed_entity_count`). Fixed: it now flows through normal
shaping with a tailored summary + `next_actions`. Rebuilt; RX-1.1/1.4
re-verified no regression.

## 4. Notes / Deferred

- Editor-side `*.detect_changes` actions are deferred: the source variant needs a
  by-file-path symbol query on `FMonolithSourceDatabase`, which currently carries
  an unrelated **uncommitted** local change; adding to it now would entangle the
  commit. Tracked in `Docs/TODO.md` and the spec.
- Headless `-nullrhi` automation remains unusable (pre-existing engine crash); the
  offline tool is verified directly against the live SQLite indexes instead.
- The 81,252 dangling-ref baseline is pre-existing native data (documented in
  `2026-05-16-crg-index-navigation.md` and the projection-cache spec); RX-2 health
  surfaces it accurately rather than hiding it.

## 5. RX-7 / RX-8 editor + offline validation (follow-up)

### Build

- `Tools/MonolithQuery/build.bat`
  - Result: PASS. `monolith_query.exe` rebuilt and copied to `Plugins/Monolith/Binaries/monolith_query.exe`.
  - Warnings: pre-existing MSVC `C4819` codepage warnings only.
- `UnrealBuildTool.exe GoGameEditor Win64 Development -Project="D:\P4\_codex_build\game-pr447-roi\GO.uproject" -WaitMutex -NoHotReloadFromIDE`
  - Result: PASS, 729 actions.
  - Notes: P4 password/prebuild warning and deprecated MassEntity Build.cs warnings did not fail the build.

### Automation

Command:

```powershell
& "D:\Engine\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "D:\P4\_codex_build\game-pr447-roi\GO.uproject" -NullRHI -NoSound -Unattended -NoSplash -NoP4 -ExecCmds="Automation RunTests Monolith.IndexGuard" -TestExit="Automation Test Queue Empty" -ReportExportPath="D:\P4\_codex_build\game-pr447-roi\Saved\AutomationReports\pr447-roi"
```

Result: PASS. Automation report `index.json` shows 23 succeeded, 0 failed, 0 succeeded-with-warnings, total duration 9.029s.

New coverage:

| Test | Result |
|---|---|
| `Monolith.IndexGuard.Project.RiskScoreSensitivity` | PASS |
| `Monolith.IndexGuard.Project.ReviewHotspotsLarge` | PASS |
| `Monolith.IndexGuard.Source.RiskScoreSensitivity` | PASS |
| `Monolith.IndexGuard.Source.ReviewHotspotsLarge` | PASS |

RX-7 behavior verified: query-time scoring and CRG cache rebuild paths surface
`raw_counts.sensitivity`, a sensitivity reason, and `scoring_version=3`.

RX-8 behavior verified: source/project `review_hotspots` return capped hotspot
lists for large/sensitive fixture entries and preserve the expected output
contract (`input`, `limits`, `hotspots`, optional `questions`, `truncated`,
`next_actions`).
