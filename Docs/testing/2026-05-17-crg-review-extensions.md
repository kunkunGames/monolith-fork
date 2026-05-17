# CRG Review Extensions — RX-1 / RX-2 Offline Verification

| | |
|---|---|
| Date | 2026-05-17 |
| Branch | `feat/crg-index-navigation-p0` (PR #447) |
| Spec | `Plugins/Monolith/CRG/spec/monolith-crg-review-extensions-spec.md` |
| Scope | RX-2 (offline CRG cache read parity) + RX-1 (offline `detect_changes`), `Tools/MonolithQuery/monolith_query.cpp` only |

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
