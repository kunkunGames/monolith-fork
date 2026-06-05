# Monolith - Skills Audit and Symlink Distribution

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Status:** Implemented, Windows onboarding automation and filesystem validation added
**Scope:** `Skills/` audit, Agent Skills entrypoint normalization, Codex and Claude local skill distribution
**Created:** 2026-06-05
**Doc reconciled with checkout:** 2026-06-05

---

## 1. Purpose

Monolith's in-repo `Skills/` folder should be the single source of truth for local AI-agent skills. Codex and Claude should consume those same folders through directory links instead of copied `SKILL.md` files. This avoids installed-copy drift and makes skill fixes reviewable in the repository.

This spec defines:

- The current audit findings for `Skills/`.
- The required folder shape for Codex, Claude Code, and Agent Skills compatibility.
- The symlink/junction installation contract for Windows agent skill roots such as `~/.codex/skills` and `~/.claude/skills`.
- The backlog for missing or under-covered Monolith skill surfaces.
- Verification gates for implementation and future changes.

## 2. External Compatibility Facts

The baseline standard is the Agent Skills directory format: one skill directory containing a required `SKILL.md` file with YAML frontmatter and Markdown instructions. The `name` field must match the parent directory, use lowercase letters, digits, and hyphens, and stay within 64 characters. The `description` field is required by the Agent Skills specification and has a 1024-character maximum.

Claude.ai custom skills also require a skill directory with `SKILL.md`, and Claude.ai upload currently limits descriptions to 200 characters. Claude Code local skills use `~/.claude/skills/<skill-name>/SKILL.md` for personal skills and `.claude/skills/<skill-name>/SKILL.md` for project skills.

OpenAI's Skills help page states that OpenAI skills follow the Agent Skills open standard, are supported in Codex, and do not sync across products. Therefore Monolith must explicitly install or link the same source folders into each product-specific skill root.

Reference sources checked on 2026-06-05:

- `https://agentskills.io/specification`
- `https://claude.com/docs/skills/how-to`
- `https://code.claude.com/docs/en/slash-commands`
- `https://help.openai.com/en/articles/20001066-skills-in-chatgpt`

## 3. Current Audit Findings

### 3.1 Repository Structure

Pre-implementation audit found 46 skill directories under `Skills/`. Every directory contained exactly one named Markdown entrypoint at:

```text
Skills/<skill-name>/<skill-name>.md
```

No in-repo skill directory contained:

```text
Skills/<skill-name>/SKILL.md
```

All audited skill frontmatter blocks had a matching `name` value and a non-empty `description`. No audited description exceeded the Agent Skills 1024-character maximum. Thirty-nine descriptions exceeded Claude.ai's 200-character upload limit, so Claude.ai ZIP upload compatibility needs a separate export pass if that surface becomes a goal.

Implementation result:

- The 46 existing skill entrypoints were renamed to `Skills/<skill-name>/SKILL.md`.
- Four missing P1 surfaces were accepted and added: `unreal-asset`, `monolith-schema`, `unreal-reflection-intel`, and `unreal-sprite`.
- The repository now has 50 Monolith skill directories using the Agent Skills entrypoint shape.
- In-repo references now point at `Skills/<skill-name>/SKILL.md`.

Two files are large enough to require close review before adding more inline material:

| Skill | Lines |
|---|---:|
| `unreal-niagara` | 412 |
| `unreal-ai` | 316 |

Both remain under the 500-line guidance, but any further action tables or long gotcha sections should move to referenced files rather than expanding `SKILL.md`.

### 3.2 Installed Copies

Both local product roots currently contain Monolith skill directories:

```text
C:\Users\12336\.codex\skills\<skill-name>\SKILL.md
C:\Users\12336\.claude\skills\<skill-name>\SKILL.md
```

Those directories are ordinary directories/files, not links to this checkout. Hash comparison found drift in both product roots for:

| Skill | Current evidence |
|---|---|
| `monolith-mcp` | Installed copy is missing repository guidance that static docs and skills are workflow guidance, not exhaustive action rosters. |
| `unreal-imagegen` | Repository file has the current SVG source actions, while installed copies still show the older 6-action surface. |
| `unreal-niagara` | Installed copy differs from the repository's current 129-action Niagara guidance. |

This is the primary defect this spec addresses: installed file copies diverge from the reviewed repository source.

### 3.3 Static Count Drift

Several static action counts disagree across `Skills/README.md`, individual skill files, specs, and the API reference. Examples observed in this checkout:

| Surface | Count in `Skills/README.md` | Count elsewhere |
|---|---:|---|
| `blueprint` | 100 | `unreal-blueprints` says 122; `SPEC_MonolithBlueprint.md` says 121. |
| `niagara` | 109 | `unreal-niagara` and `SPEC_MonolithNiagara.md` say 129. |
| `ui` | 119 | `SPEC_MonolithUI.md` says 130 under `WITH_COMMONUI=1`; `Docs/API_REFERENCE.md` currently says 138. |
| `imagegen` | 6 | `unreal-imagegen` and `SPEC_MonolithImageGen.md` include 9 after SVG source actions. |

Static counts must be treated as routing hints only. Exact action names and schemas remain runtime registry facts from `monolith_find` and `monolith_discover`.

### 3.4 Missing or Under-Covered Skill Surfaces

The current folder set covers the main public domains, but several implemented or documented surfaces are either buried inside another skill or not represented as first-class skills.

| Candidate skill | Covered namespace(s) | Current issue | Priority |
|---|---|---|---|
| `unreal-asset` | `asset` | Generic asset workflows are embedded in `unreal-materials`, but texture/font ingest, file import, save/delete, asset inspection, naming validation, and batch rename are cross-domain. | P1 |
| `monolith-schema` | `bulk_fill`, `describe` | Schema-first and reflection bulk-fill workflows are central and cross-domain, but no focused skill teaches the `project.search -> describe -> bulk_fill.apply` path. | P1 |
| `unreal-reflection-intel` | `cppreflect`, `network`, `decision`, `risk`, `reflect` | `cppreflect` appears in `unreal-cpp` and `unreal-project-search`, but RI network, decision, and risk queries are not discoverable from a dedicated skill. | P1 |
| `unreal-sprite` | `sprite` | `SPEC_MonolithSpriteAsset.md` documents an implemented `sprite` namespace and production contract, but the skill is absent. | P1 |

These four candidates were accepted because each maps to an implemented or documented namespace family and improves trigger accuracy over burying the workflow inside another skill:

- `unreal-asset` is backed by `Docs/specs/SPEC_MonolithAsset.md` and the `asset` namespace.
- `monolith-schema` is backed by the core `describe`/`bulk_fill` schema workflow and contract tests.
- `unreal-reflection-intel` is backed by Reflection Intelligence namespaces documented in `Scripts/monolith_offline.py` and `Source/MonolithReflectionIntel`.
- `unreal-sprite` is backed by `Docs/specs/SPEC_MonolithSpriteAsset.md`.

## 4. Required Repository Shape

The repository must move to the Agent Skills entrypoint shape:

```text
Skills/
  <skill-name>/
    SKILL.md
    references/      # optional
    scripts/         # optional
    assets/          # optional
```

Rules:

1. `SKILL.md` is the canonical in-repo entrypoint.
2. Do not keep duplicate text copies such as both `SKILL.md` and `<skill-name>.md`.
3. Because this checkout has `core.symlinks=false`, do not rely on in-repo file symlinks for canonical content.
4. Update in-repo references from `Skills/<skill>/<skill>.md` to `Skills/<skill>/SKILL.md`.
5. Keep skill frontmatter minimal for cross-product compatibility: `name` and `description` only unless a later product-specific requirement is explicitly justified.
6. Keep `description` under 1024 characters for Agent Skills compatibility. If Claude.ai upload support is required, generate a separate packaged export with descriptions shortened to 200 characters or less; do not compromise local trigger quality just for upload packaging.
7. Move bulky reference tables into `references/` when a skill approaches 500 lines or when a section is not needed for every invocation.

## 5. Symlink Installation Contract

The implementation provides a Windows onboarding entrypoint:

```text
Scripts/onboard_monolith.ps1
```

This script reads data-only target adapters from:

```text
Templates/Onboarding/*.json
```

It validates repository skills, delegates skill linking to `Scripts/install_monolith_skills.ps1`, configures target MCP clients, and manages project instruction blocks. Dry-run/plan mode is the default when `-Execute` is omitted. Codex and Claude use user/global MCP config through their CLIs by default; project-scoped `.mcp.json` generation is opt-in via `-ProjectMcpConfig` or an explicit `-McpConfigPath`.

The lower-level skill-link installer remains available for direct repair:

```text
Scripts/install_monolith_skills.ps1
```

Required behavior:

1. Default source root: repository `Skills/`.
2. Default targets:
   - `C:\Users\<user>\.codex\skills`
   - `C:\Users\<user>\.claude\skills`
3. For every `Skills/<skill-name>/SKILL.md`, create a directory link at each target:
   - `<target-root>\<skill-name>` -> `<repo>\Skills\<skill-name>`
4. On Windows, prefer directory junctions when symbolic links are unavailable; support true directory symlinks when the environment allows them.
5. Never overwrite a non-link installed skill directory by default.
6. In dry-run mode, report every planned create, skip, conflict, and drift.
7. In execute mode, fail closed on conflicts unless an explicit replace flag is supplied.
8. If replacing an existing copied install is requested, first prove the existing copy is either identical to the repository source or move it to a timestamped backup path. Do not delete user files directly.
9. After installation, verify:
   - Target directory exists.
   - Target directory is a link or junction.
   - `<target>\<skill-name>\SKILL.md` resolves to the repository skill content.
   - Hashes match for all linked skills.

The installer supports:

```text
-Targets Codex,Claude
-TargetRootSpecs Name=Path
-Execute
-ReplaceCopies
-UseSymlink
-SourceRoot <path>
-CodexRoot <path>
-ClaudeRoot <path>
```

Dry-run is the default when `-Execute` is omitted.

## 6. Skill Audit Requirements

The implementation includes a deterministic validation script:

```text
Scripts/validate_monolith_skills.ps1
```

The validator must check:

1. Every `Skills/<skill>` directory has `SKILL.md`.
2. `SKILL.md` starts with YAML frontmatter bounded by `---`.
3. `name` exists, matches the directory name, is lowercase hyphen-case, is 1-64 characters, does not start/end with `-`, and does not contain `--`.
4. `description` exists, is non-empty, and is at most 1024 characters.
5. Claude.ai export mode additionally enforces `description.Length <= 200`.
6. Body length is reported when over 300 lines and fails when over 500 lines unless an allowlist entry is explicitly documented.
7. Relative Markdown links resolve from the skill root.
8. No skill file contains absolute maintainer paths such as `C:\Users\...`, API keys, bearer tokens, cookies, private keys, or authentication headers.
9. Static action counts in `Skills/README.md` and skill headers are either removed, marked as snapshots, or generated from a single source so they cannot silently drift.
10. Installed target roots, when supplied, contain only links/junctions for Monolith-owned skills and have zero hash drift.

## 7. Execution Plan

1. Rename each `Skills/<skill>/<skill>.md` to `Skills/<skill>/SKILL.md`.
2. Update `Skills/README.md`, `Docs/MONOLITH_GUIDE.md`, `Docs/SPEC_CORE.md`, and any other in-repo references to use `Skills/<skill>/SKILL.md`.
3. Add `Scripts/validate_monolith_skills.ps1` with the checks in section 6.
4. Add `Scripts/install_monolith_skills.ps1` with dry-run as the default and execute mode requiring an explicit flag.
5. Run the validator against the repository skills.
6. Run installer dry-runs for Codex and Claude target roots, verifying it detects the current copied installs and reports drift instead of overwriting them.
7. Triage missing-skill candidates from section 3.4 by live or offline namespace evidence; add only focused skills whose trigger value is higher than embedding a reference in an existing skill.
8. Re-run the validator after any new skill additions.
9. Run the project's static checks: `python Scripts/ci_static_checks.py --config .github/monolith-static-ci.json --github check`.
10. Run `git diff --check` and `git status --short`.
11. Complete pre-commit steps to ensure proper testing, verification, review, and reflection are done.

## 8. Acceptance Criteria

The implementation is complete when:

- All Monolith skills have exactly one canonical in-repo entrypoint: `Skills/<skill>/SKILL.md`.
- Codex and Claude local skill roots can consume Monolith skills by directory link or junction with no copied `SKILL.md` drift.
- The validator passes in normal Agent Skills mode.
- Claude.ai export compatibility is either passing in explicit export mode or documented as intentionally unsupported for this release.
- `unreal-imagegen`, `unreal-niagara`, and `monolith-mcp` installed-copy drift can be eliminated by running the installer with `-Execute -ReplaceCopies`, which backs up copied installs and creates links rather than copy-refreshing.
- Static action counts are no longer silently inconsistent; either they are generated, removed, or clearly marked as snapshots with runtime discovery as authority.
- Missing-skill candidates have been explicitly accepted into new skills or rejected with a documented reason.
- `Templates/Onboarding/Onboarding.md` documents MCP configuration, project instruction setup, and Codex/Claude global skill linking.
- `Scripts/onboard_monolith.ps1` is the Windows-first orchestration entrypoint and target behavior is data-driven through `Templates/Onboarding/*.json`.

## 9. Verification Notes

Do not claim live Unreal Editor, MCP, build, packaging, or Claude/Codex runtime activation success unless those tools were run in the current environment. If only filesystem validation was run, state that the verification was filesystem-only.

The existing dirty worktree contains unrelated changes in docs, `unreal-imagegen`, and `Source/MonolithImageGen`. Future implementation must preserve those changes and avoid treating them as part of this spec unless the owner explicitly includes them.

This implementation intentionally verifies filesystem shape, scripts, dry-run behavior, and static checks only. It does not claim live Unreal Editor, MCP runtime activation, Codex runtime activation, Claude runtime activation, or global link execution.
