# Monolith Agent Coordination and Conventions

This repository relies on several scheduled Jules tasks (agents) to maintain and optimize the codebase. To prevent collisions and ensure a clean history, all agents MUST follow these coordination rules:

## 1. Branch and PR Naming Conventions
Agents must use a strict, predictable branch naming convention to make active work easily discoverable:
- **Format:** `jules/<agent>/<module-or-area>/<short-behavior>`
- **Avoid:** Non-standard prefixes like `bolt-...`, `perf-...`, `sentinel-...`, or raw `jules-<id>-...` branches.

## 2. Duplicate / Collision Guard
Before making any changes, agents must perform a thorough duplicate and collision check:
- Use `git branch -r` and identify any existing `jules/<agent>/...` branches.
- Stop without PR if a similar branch exists, if an open PR has the same WorkFingerprint, or touches the same intended files. If an open branch or PR addresses the intended changes, or if the collision is ambiguous, **stop without PR**. No-op is a perfectly acceptable and expected outcome when the queue is healthy or work overlaps.
- PR descriptions must include a 'Duplicate check' section detailing inspected PRs/branches and the reason the work is non-overlapping.

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
Do not claim Unreal Engine (UE) verification or release packaging was successful unless the tools were actually executed in the current VM. If the UE Editor or build tools are unavailable, explicitly note `[blocked: UE editor unavailable]` or similar in the verification logs.
