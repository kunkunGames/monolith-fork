# Monolith — Skill Catalog Drift Guard

**Parent:** [../SPEC_CORE.md](../SPEC_CORE.md)
**Owner module:** MonolithCore
**Status:** Implemented and verified 2026-06-13; wired into `ci_static_checks.py` as the `skill_drift` check 2026-06-14 (§7)
**Scope:** `Scripts/check_skill_catalog_drift.ps1`, the `skill_drift` check in `Scripts/ci_static_checks.py` (catalog-source-adaptive), and the `skill_drift` block in `.github/monolith-static-ci.json`; complementary to `Scripts/validate_monolith_skills.ps1`
**Created:** 2026-06-13

---

## 1. Purpose

The Agent Skills under `Skills/<skill>/SKILL.md` (and the hoisted
`Skills/<skill>/references/*.md` tables) document, per namespace, an action
reference and full parameter signatures. Those tables are a hand-maintained
snapshot of the live `monolith_discover` catalog. As the runtime catalog
changes — actions renamed/removed, params added/removed, a param flipping
required/optional — the snapshot rots silently. `validate_monolith_skills.ps1`
checks skill *structure* (frontmatter, link integrity, body size) but never
compares documented action/param content against the live registry, so all the
skill-enrichment effort can degrade undetected.

`check_skill_catalog_drift.ps1` is the structural fix: it resolves each skill's
backing namespace(s), fetches the LIVE catalog per namespace, and compares it
to what the skill documents. Hard drift fails the run so the check can gate CI;
the live catalog stays the only source of truth (the script never edits skills).

This is a docs-governance helper, not an MCP action. It does not wrap or extend
any namespace action.

## 2. Skill → Namespace Map

The script carries the authoritative skill → namespace(s) map (mirrors CLAUDE.md
section 17 and the per-namespace skill ownership). Highlights:

| Skill | Namespace(s) | Notes |
|---|---|---|
| `monolith-mcp` | `monolith` | Documents both the standalone MCP tools (`monolith_find`/`monolith_discover`/…) and the `monolith` admin-namespace actions (`find`/`discover`/…); the `monolith_`-prefixed form resolves to the bare namespace action. |
| `monolith-schema` | `describe`, `bulk_fill` | |
| `unreal-cpp` | `source`, `config` | The skill's "Reflection Intelligence" section points at `cppreflect` actions it does not own; those resolve as cross-references (XREF), not drift. |
| `unreal-materials` | `material`, `asset` | Multi-namespace skill. |
| `unreal-performance` | `config`, `material`, `niagara` | Multi-namespace skill. |
| `unreal-reflection-intel` | `cppreflect`, `network`, `decision`, `risk`, `reflect` | Multi-namespace skill. |
| `unreal-build`, `unreal-debugging` | `editor` | Two skills over one namespace. |
| `unreal-logicdriver`, `unreal-combograph` | `logicdriver`, `combograph` | Optional-plugin namespaces; when the plugin is not loaded these report `skipped: plugin not loaded`, NOT drift. |
| `material-reference`, `niagara-reference` | (none) | KNOWLEDGE-only skills drive no namespace and report `no_namespace`. |

Every other `unreal-*` skill maps to its single same-named namespace (see the
`$SkillNamespaceMap` table in the script for the full list).

## 3. What Is Compared

For each skill, documented actions and params are parsed from the skill's
`SKILL.md` plus any same-skill `references/*.md` files it links. A markdown
table is treated as an ACTION table only when its header row's first column is
exactly `Action` or `Tool`; the PARAMS column is the column whose header
contains `Param` or `Signature`. This header-driven rule excludes node-type
tables (`| node_type | … |`), property tables, and example/workflow tables.

Param notation parsed from the params cell (leading run only — see §6):

| Notation | Meaning |
|---|---|
| `name*` | required |
| `name?` | optional |
| `name=default` | optional, has a default |
| `name?=default` | optional, marker + default (the dominant form) |

Reported drift classes:

| Class | Condition | Severity |
|---|---|---|
| (a) | Action documented but ABSENT from the entire live catalog | HARD (fails) |
| (b) | A documented param name not present in the action's live schema | HARD (fails) |
| (c) | A param documented required (`name*`) that the schema reports optional, or documented optional (`name?`/`name=`/`name?=`) that the schema reports required | HARD (fails) |
| (d) | Live actions the skill does not document | informational (`INFO-UNDOC`, never fails; printed only with `-ShowUndocumented`) |
| XREF | Action documented in a skill but owned by another live namespace (cross-namespace reference) | informational (never fails) |

"Absent from the live catalog" (a) means absent from the *whole* loaded
catalog, not just the skill's own namespaces: an action that exists in another
namespace is an XREF, not drift. This keeps deliberate cross-namespace pointers
(e.g. `unreal-cpp` → `cppreflect`) out of the failure set.

A namespace that is not loaded (`status:"not_installed"`, or `Unknown
namespace`) is reported `skipped: plugin not loaded`. A skill whose namespaces
are all skipped is itself `skipped` — never drift.

## 4. Modes and Parameters

| Parameter | Default | Notes |
|---|---|---|
| `-SkillsRoot` | `<plugin>/Skills` | Skills root containing per-skill directories. |
| `-McpUrl` | `MONOLITH_URL` env var, else `http://localhost:9316/mcp` | LIVE-mode JSON-RPC endpoint. |
| `-Offline` | off | Use pre-fetched namespace dumps from `-DumpDir` instead of querying the MCP. |
| `-DumpDir` | (none) | Directory of `<namespace>.json` catalog dumps for `-Offline` mode. |
| `-Skill` | all mapped | Restrict the run to one or more skill names. |
| `-ShowUndocumented` | off | Print the (d) informational live-but-undocumented list. |
| `-ReportOnly` | off | Print the full report but always exit 0 (do not fail on hard drift). |
| `-GatedAllowlist` | `<script dir>/skill_drift_gated_actions.json` | Sidecar JSON (skill → [action names]) of feature-gated actions reported as `GATED` (informational), not hard drift. Pass `''` to disable the allowlist. |

LIVE mode (default) lists each namespace's actions + full param schemas through
paginated `monolith_discover` calls (`mode=actions`, `detail=true`,
`limit=1000`, `offset=<next_offset>`). The script consumes
`$resp.result.structuredContent` when present, falls back to
`$resp.result.content[0].text` for older clients, and merges every page's
`.actions[]` rows before comparing documented tables. Each action row carries
`.action` and a `.params` map of `name → {type, required, default,
description}`. Pagination is required because live discovery normalizes
`limit <= 0` to its bounded default page size instead of returning the whole
namespace. Offline mode reads one `<namespace>.json` per namespace; each dump
holds either the raw catalog object (`{ namespace, actions:[…] }`) or the
not_installed sentinel (`{ namespace, actions:0, status:"not_installed" }`).
Offline namespaces with no dump file are skipped (`no_dump`), never treated as
drift. The offline
`Binaries/monolith_query.exe` only covers source/project/bridge/cppreflect/
network/decision/risk and its per-action `--help` shows a SUBSET, so it is
NEVER used to assert an action is absent; `-Offline` relies on full
`monolith_discover` dumps instead.

## 5. Output and Exit Codes

Line-oriented status: `INFO`, `SKILL=<name> STATUS=… hard_drift=N gated=N xref=N …`,
per-finding `DRIFT` / `GATED` / `XREF` / `INFO-UNDOC`, and one terminal `RESULT=`
token carrying `hard_drift=` and `gated=` totals.

| Exit code | Meaning |
|---|---|
| 0 | No hard drift (or `-ReportOnly`); `RESULT=OK` or `RESULT=DRIFT` under `-ReportOnly`. Feature-gated actions in `-GatedAllowlist` are reported as `GATED` and do NOT count as hard drift. |
| 2 | Hard drift (a/b/c) found in at least one skill; `RESULT=DRIFT` |
| 3 | Blocked: skills root missing, LIVE mode with the MCP endpoint down, `-Offline` without a valid `-DumpDir`, or a catalog became unavailable mid-run; `RESULT=BLOCKED` |

A blocked catalog is never masked by `-ReportOnly`: the script refuses to assert
"no drift" when it could not read the catalog it was comparing against.

## 6. False-Positive Discipline

The check is only durable if it almost never cries wolf, otherwise it gets
muted. Two precision rules:

- **Header-driven action tables.** Only `| Action | … |` / `| Tool | … |`
  tables contribute actions; the node-type table in `unreal-blueprints`
  (`| node_type | … |`, holding values like `CallFunction`/`Branch`) and prose
  in `Purpose`/`Use` columns are excluded.
- **Leading-run param extraction.** Params are the leading run of standalone,
  param-shaped backtick tokens at the start of the params cell, separated only
  by commas/whitespace. The first prose fragment (`(`, `(alias`, `DSL:`,
  `e.g.`, `if …`) or non-param backtick ends the run. This drops explanatory
  backticks (`WITH_GBA`, `inventory_supported=false`, nested
  `{ …sort_priority? }` sub-fields, DSL keywords) that would otherwise read as
  phantom params. Missing a trailing real param only drops a check; it never
  invents drift.

## 7. Integration with `validate_monolith_skills.ps1` and CI

`validate_monolith_skills.ps1` and `check_skill_catalog_drift.ps1` are
complementary and run as separate steps:

| Concern | `validate_monolith_skills.ps1` | `check_skill_catalog_drift.ps1` |
|---|---|---|
| Frontmatter / link integrity / body size / installed-link hashes | yes | no |
| Documented action/param content vs the live catalog | no | yes |
| Requires a live MCP / catalog dump | no | yes (LIVE) or dump (Offline) |

`python Scripts/ci_static_checks.py --config .github/monolith-static-ci.json
--github check` is the project static-check entry point, and the drift guard is
now wired into it as the `skill_drift` check (`check_skill_catalog_drift` in
`ci_static_checks.py`, config block `skill_drift`). Because the guard needs a
catalog source, the check is **catalog-source-adaptive** so it never fails a
headless run for lack of an editor:

| Catalog source (in preference order) | Behavior |
|---|---|
| Committed dumps at `dump_dir` (`Skills/_catalog_dumps`, `*.json` present) | Runs `-Offline -DumpDir`; **hard drift (exit 2) → blocker** |
| Else live editor reachable at `mcp_url` (TCP probe of `9316`) | Runs LIVE; **hard drift (exit 2) → blocker** |
| Else (no dumps, editor down) | **Advisory skip** (non-blocking), with a message to run with the editor up or commit dumps |
| Script exit 3 (catalog unavailable / usage) | Advisory skip — not a code defect |

So the guard gates automatically wherever a catalog is available (an agent/dev
running the static checks locally with the editor up, or any CI job with
committed dumps), and degrades to a visible advisory in headless GitHub CI.

**To make it a HARD gate in headless GitHub CI**, commit per-namespace
`monolith_discover` dumps to `Skills/_catalog_dumps/<namespace>.json`, refreshed
from a live editor whenever the catalog changes (follow-up: add a `-ExportDumps`
mode to the guard, or a small `Scripts/dump_skill_namespace_catalog.ps1` that
loops `monolith_discover` per namespace). Until dumps are committed, the headless
run is advisory and the real gate is the local/live-editor run.

`validate_monolith_skills.ps1` remains the separate structural step (frontmatter,
links, body size); it does not compare catalog content. Adoption notes:

1. The four flag-gated skills (§8) are already reconciled via
   `skill_drift_gated_actions.json`, so a default-build LIVE run is `RESULT=OK`
   (exit 0) with `gated=55`. Exit-2 gating is therefore usable immediately.
2. The `ci_static_checks` integration maps exit 0 → pass, exit 2 → blocker,
   exit 3/other → advisory skip; it does not pass `-ReportOnly` so real drift
   gates wherever a catalog is present.

## 8. Known Drift / Build-State Sensitivity

Drift class (a) is relative to the *running* editor's loaded action set. Skills
that document actions gated behind disabled-by-default or flag-gated surfaces
would otherwise report drift on a default build:

- `unreal-worldgen` (27) and `unreal-scene` (20) document the EXPERIMENTAL
  procedural town generator plus the spatial-registry / debug-view / auto-volume
  surface (disabled by default via `bEnableProceduralTownGen`; see SPEC_CORE.md
  MonolithMesh row), so those actions are absent from a default catalog.
- `unreal-slate` (5) and `unreal-gamefeatures` (3) expose only a single status
  action until their inspector flag is enabled; their documented inspection
  actions are absent otherwise.

These 55 actions exist in source (e.g. `capture_floor_plan` /
`capture_building_views` in `MonolithMeshDebugViewActions.cpp`) and are listed in
`skill_drift_gated_actions.json`, so the guard reports them as `GATED`
(informational) — the affected skills stay `STATUS=ok` and a default-build run is
`RESULT=OK` (exit 0, `gated=55`). To fully VERIFY their documented signatures,
enable the matching flag and re-run with `-GatedAllowlist ''` so the
now-registered actions are checked as normal. Remove a skill's entries from the
allowlist once its feature ships enabled by default.

## 9. Verification Record (2026-06-13)

| Gate | Result |
|---|---|
| LIVE full sweep | 51 skills checked against the live v0.18.1 catalog (1722 tools). `combograph`/`logicdriver` reported `skipped: plugin not loaded`; `material-reference`/`niagara-reference` reported `no_namespace`; `unreal-cpp` reported 6 XREF (cppreflect) with zero hard drift. |

## 10. Verification Record (2026-06-30)

| Gate | Result |
|---|---|
| LIVE paginated namespace fetch | `Scripts\check_skill_catalog_drift.ps1 -Skill unreal-ui` checked the full paginated `ui` catalog instead of only the first 50 rows and returned `RESULT=OK`, `hard_drift=0`, `documented_actions=120`, `undocumented=25` against Monolith v0.20.3. |
| (a) detection | `unreal-worldgen` (27), `unreal-scene` (20), `unreal-slate` (5), `unreal-gamefeatures` (3) flagged actions absent from the whole catalog (experimental/flag-gated surfaces); `RESULT=DRIFT`, exit 2. |
| XREF (not drift) | `unreal-cpp`'s six `cppreflect` actions (`get_uclass`, `list_uproperties`, `list_ufunctions`, `find_interface_impls`, `find_class_specifier`, `list_class_specifiers`) resolved as cross-references, keeping the skill `ok`. |
| (b) detection | Offline dump with trimmed `find_assets` params flagged `path`/`recursive`/`class_names` as documented-but-absent params. |
| (c) detection | Offline dump flipping `list_bodies.limit` to required produced `(c) … param 'limit' documented optional(?/=) but schema is required`. |
| Param-grammar coverage | Adding `name?=default` support pulled 400+ optional params into the (b)/(c) checks with zero new false positives (every previously-`ok` skill stayed `ok`). |
| Offline skip semantics | `not_installed` sentinel → `skipped`; namespaces with no dump → `skipped` (`no_dump`), never drift. |
| Mode/exit codes | `RESULT=OK` exit 0 (single clean skill); `RESULT=DRIFT` exit 2; `-ReportOnly` exit 0 on drift; `RESULT=BLOCKED` exit 3 when the MCP endpoint was down. |
| Gated allowlist (post-§8) | After adding `skill_drift_gated_actions.json` (55 actions across the four flag-gated skills), the full LIVE sweep re-ran `RESULT=OK skills_checked=46 hard_drift=0 gated=55` (exit 0); the four skills now report `STATUS=ok … gated=N`. Confirms zero hard drift across the enriched skills and makes the guard immediately exit-2 gateable. |
| Environment note (not a script defect) | The Go checkout's headless editor crash-loops on the known `GenericWindow.cpp:113` layout-save fatal (see SPEC_MonolithAgentOpsScripts.md / SPEC_MonolithEditor.md); when the MCP dropped mid-sweep the guard correctly returned `RESULT=BLOCKED` (exit 3) rather than asserting drift against an incomplete catalog. |
