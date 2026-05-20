# Monolith Agent Coordination and Conventions

This repository relies on several scheduled Jules tasks (agents) to maintain and optimize the codebase. To prevent collisions and ensure a clean history, all agents MUST follow these coordination rules:

## 1. Branch and PR Naming Conventions
Agents must use a strict, predictable branch and PR naming convention to make active work easily discoverable:
- **Branch Format:** `jules/<agent>/<module-or-area>/<short-behavior>` (Note: `<module-or-area>` and `<short-behavior>` are placeholders that must be replaced with descriptive text).
- **PR Title Format:** `<Emoji> <Agent>: <short description>` (e.g., `⚡ Bolt: [description]`, `🛡️ Sentinel: [description]`).
- **Avoid:** Non-standard branch prefixes like `bolt-...`, `perf-...`, `sentinel-...`, or raw `jules-<id>-...` branches, generic PR titles without agent prefixes, and using literal strings like `short-topic` or `module-or-area` for placeholders.

## 2. Duplicate / Collision Guard
Before making any changes, agents must perform a thorough duplicate and collision check. Because agents may run in concurrent VMs, you must avoid race conditions:
- Always run `git fetch origin --prune` immediately before checking branches.
- Use `git branch -r` and identify any existing `jules/<agent>/...` branches. You MUST also check for legacy or non-standard branch prefixes (e.g., `bolt-*`, `perf-*`, `sentinel-*`, `fix-*`) to catch overlapping older work.
- Stop without PR if a similar branch exists, if an open PR has the same WorkFingerprint, or touches the same intended files. If an open branch or PR addresses the intended changes, or if the collision is ambiguous, **stop without PR**. No-op is a perfectly acceptable and expected outcome when the queue is healthy or work overlaps. If you decide to no-op, **do not create a branch or an empty PR to report it.**
- PR descriptions must include a 'Duplicate check' section detailing inspected PRs/branches and the reason the work is non-overlapping.
- **Staggered Schedules:** If multiple agents of the same type are scheduled, their runs should be staggered to allow PR visibility to propagate.

## 3. WorkFingerprint Requirement
Every agent PR description must include a `WorkFingerprint` containing at least:
- `agent`: (e.g., Marshal, Bolt, Sentinel)
- `category`: (e.g., performance, test, orchestration)
- `module`: (e.g., Monolith)
- `component/action/helper`: (e.g., MonolithCore, MonolithMesh)
- `intended files`: (e.g., comma-separated list)
- `risk type`: (e.g., collision, regression)
- `public API impact`: (e.g., yes, no)
- `docs/spec impact`: (e.g., yes, no)

## 4. Verification Claims
Do not claim Unreal Engine (UE) verification or release packaging was successful unless the tools were actually executed in the current VM. If the UE Editor is unavailable, explicitly note `[blocked: UE 5.7 editor unavailable in Jules VM]` in a dedicated 'Blocked verification' section in the PR description. If build, packaging, or other UE command-line tools are unavailable while the editor itself is not the blocker, use the matching concrete blocker text, for example `[blocked: UE 5.7 build tools unavailable in Jules VM]`.

## 5. Journal Hygiene
When updating `.jules/` journal files, only document durable coordination rules and learnings. Do not use them as routine work logs or task journals.

## 6. Temporary Workflow Artifacts
Agents often create temporary files (such as `pr_body.txt`, helper Python scripts, or JSON dumps) during their workflows. To maintain repository hygiene, you must ensure all temporary workflow artifacts are completely deleted before staging and committing your final changes.

## 7. Single Responsibility
Agents must keep PRs tightly scoped. Do not mix unrelated security, test, spec, performance, release, refactor, and prompt-governance work in one PR. If the only remaining useful change requires bundling unrelated concerns, stop without PR instead.

## 8. Public Action Contracts
Agents performing routine refactoring, performance optimization (Bolt), or hygiene (Curator) tasks must not modify public action contracts or JSON parameter schemas without explicit justification. If a behavior change is not the primary goal, do not alter expected inputs/outputs just to simplify code.

## 9. Minimum PR Value Threshold
Passing static CI is not enough to make a scheduled PR worth merging; a PR must also be non-overlapping, current after rebase, and clearly more valuable than a no-op. Stop without PR if the only available change is a micro-edit against shared coordination docs, action-count docs, or release docs where multiple agents often race.

## 10. External CI Limits
If a GitHub Actions CI check fails with a billing-related error (e.g., "recent account payments have failed" or "spending limit needs to be increased"), recognize that this is an external repository limit, not a code defect. Do not attempt to fix it via code changes; simply inform the user.

## 11. Index Freshness and CRG Cache
Project and source indexing must keep their CRG projection/cache data in sync. Successful ProjectIndex and EngineSource indexing completion rebuilds the matching CRG projection/cache automatically; if `project.health` or `source.health` reports stale CRG parity, run the matching `repair_crg_cache` action with execute enabled.

After a successful C++ build, Live Coding, or hot reload, EngineSource.db should refresh through incremental project source indexing. If the post-build reload hook did not fire or the editor was unavailable, run `source.trigger_project_reindex` once the existing EngineSource.db bootstrap is present.
