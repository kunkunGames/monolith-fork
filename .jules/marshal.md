
## 2026-05-10 - Require checking legacy branch prefixes
**Coordination issue:** Agents were missing collisions with older open work because they only checked for the strict `jules/<agent>/...` branch pattern.
**Learning:** Legacy PRs often use non-standard prefixes (e.g., `bolt-*`, `perf-*`, `sentinel-*`), which evade the strict `git branch -r` filter in duplicate checks.
**Prevention:** Updated `AGENTS.md` to explicitly require agents to check for legacy or non-standard branch prefixes during their duplicate guard process.
**Avoid:** Assuming all open branches strictly follow the current naming convention without verifying older, pending work.

## 2024-05-24 - Standardize branch naming and verification claims
**Coordination issue:** Agents used inconsistent branch prefixes (e.g., `bolt-*`, `perf-*`, `sentinel-*`) making duplicate detection extremely difficult.
**Learning:** Without a single, strict convention, agents fail to correctly parse `git branch -r` and identify overlapping work.
**Prevention:** Created root `AGENTS.md` explicitly requiring the `jules/<agent>/<module-or-area>/<short-behavior>` pattern and banning ad-hoc prefixes. Also clarified no-op acceptability and verification claim truthfulness.
**Avoid:** Agents creating un-prefixed branches or blindly claiming Unreal Engine verification in headless VMs.

## 2024-05-25 - Harden WorkFingerprint and Duplicate check requirements
**Coordination issue:** Agents were not consistently reporting their duplicate checks, and WorkFingerprints lacked sufficient detail (like risk type and API/docs impact) to effectively deduplicate work.
**Learning:** Broad requirements for WorkFingerprints and duplicate checks lead to agents omitting crucial information needed by other agents to determine overlap.
**Prevention:** Updated `AGENTS.md` to explicitly require a 'Duplicate check' section in PRs and expanded the `WorkFingerprint` to include module, component/action/helper, risk type, public API impact, and docs/spec impact.
**Avoid:** Submitting PRs without explicit duplicate check documentation or with sparse WorkFingerprints that fail to identify potential overlaps.

## 2024-05-26 - Require stopping for identical intended files and WorkFingerprint
**Coordination issue:** Agents were proceeding with work when other agents were already touching the same intended files or had matching WorkFingerprints.
**Learning:** Vague collision instructions allowed agents to create overlapping PRs because they judged their approach as non-overlapping despite editing the same files.
**Prevention:** Added a strict rule to `AGENTS.md` requiring agents to explicitly "Stop without PR if a similar branch exists, if an open PR has the same WorkFingerprint, or touches the same intended files."
**Avoid:** Proceeding with file edits when another agent has explicitly claimed the same target files in their WorkFingerprint or PR diff.

## 2024-05-27 - Restrict journal files to durable learnings
**Coordination issue:** Agents were using `.jules/` files as routine work logs, causing unnecessary repo noise and burying important rules.
**Learning:** Without explicit instruction, agents default to writing task journals instead of durable coordination rules.
**Prevention:** Added a 'Journal Hygiene' section to AGENTS.md to explicitly ban routine work logs in `.jules/` files.
**Avoid:** Updating `.jules/` files with daily summaries or non-reusable logs.

## 2026-05-08 - Standardize PR title prefixes
**Coordination issue:** PR titles were not standardized, making it harder to visually scan the open PR queue for specific agent or track overlaps.
**Learning:** The existing naming conventions focused only on branch names and omitted explicit requirements for PR titles, leading to inconsistent PR list views.
**Prevention:** Updated `AGENTS.md` to explicitly require PR titles to be prefixed with an emoji and the agent name (e.g., `⚡ Bolt: ...`).
**Avoid:** Submitting PRs with generic titles that do not clearly indicate the originating agent track.

## 2026-05-09 - Add staggered schedule recommendation and fetch requirement
**Coordination issue:** Agents were creating overlapping PRs (e.g., multiple `bolt/monolithmesh/tarray-reserve` branches) despite duplicate checks, likely due to concurrent VM execution and stale local branch lists.
**Learning:** Checking `git branch -r` without fetching first, or running agents simultaneously, allows race conditions where agents miss each other's active work.
**Prevention:** Updated `AGENTS.md` to recommend staggered scheduling and explicitly require running `git fetch origin --prune` before checking for collisions.
**Avoid:** Running duplicate checks without fetching the latest origin state or scheduling identical agents concurrently.

## 2026-05-10 - Enforce temporary workflow artifact cleanup
**Coordination issue:** Agents were leaving temporary scratchpad scripts (e.g., `pr_body.txt`, `fix.py`) in the codebase when committing, polluting the repository.
**Learning:** Without explicit cleanup instructions, agents focus on completing the task and overlook removing the intermediate files they created.
**Prevention:** Added 'Temporary Workflow Artifacts' rule to `AGENTS.md` requiring the explicit deletion of all temporary files before staging and committing.
**Avoid:** Committing and pushing temporary helper scripts or PR description drafts.

## 2026-05-10 - Standardize blocked verification wording
**Coordination issue:** Agents used vague or inconsistent wording when UE verification was blocked, making it difficult for reviewers and tooling to grep for blocked status.
**Learning:** The previous "or similar" instruction allowed for too much variance in blocked verification logs.
**Prevention:** Updated `AGENTS.md` to mandate the exact string `[blocked: UE editor unavailable]` in a dedicated 'Blocked verification' PR section.
**Avoid:** Claiming UE verification without running it, or using non-standard wording to describe a blocked state.

## 2026-05-12 - Distinguish blocked editor and build tools
**Coordination issue:** Agents were logging missing UE verification tools with one generic editor-unavailable string even when the blocked tool was a build, packaging, or command-line UE tool.
**Learning:** One exact blocked string improves grepability but hides the concrete unavailable tool when the editor is not the blocker.
**Prevention:** Use `[blocked: UE 5.7 editor unavailable in Jules VM]` for editor blockers and concrete tool-specific strings such as `[blocked: UE 5.7 build tools unavailable in Jules VM]` for build-tool blockers.
**Avoid:** Claiming UE verification without running it, or using editor-unavailable wording for build-tool-only failures.

## 2026-05-11 - Enforce public action contract stability
**Coordination issue:** Agents performing routine refactoring or performance tasks were casually modifying JSON parameter schemas or public action outputs to simplify their code.
**Learning:** Casual changes to public contracts break downstream consumers and invalidate API documentation without proper spec review.
**Prevention:** Added 'Public Action Contracts' rule to AGENTS.md explicitly forbidding changes to expected inputs/outputs during non-feature work without clear justification.
**Avoid:** Changing parameter names, adding required fields, or altering JSON return structures just to make internal C++ refactoring easier.

## 2026-05-13 - Enforce single responsibility and tight scoping
**Coordination issue:** Agents were grouping unrelated changes or stacking several tiny maintenance ideas into separate overlapping PRs, which made queue triage close more PRs than expected.
**Learning:** A PR that is "small" can still be low-value if it races another PR, touches shared prompt/docs files, or bundles unrelated categories such as security, tests, performance, release, and refactor work.
**Prevention:** AGENTS.md now requires scheduled agents to keep PRs tightly scoped and to stop without PR when the only candidate would mix unrelated concerns.
**Avoid:** Creating a PR just because one small edit exists; no-op when the useful change is already covered or the scope would be awkwardly bundled.

## 2026-05-14 - Raise the value threshold before scheduled PR creation
**Coordination issue:** Bulk triage of the Jules queue closed many small PRs because they were empty, superseded, or overlapped another PR touching the same files.
**Learning:** Passing static CI is not enough to make a scheduled PR worth merging; a PR must also be non-overlapping, current after rebase, and clearly more valuable than a no-op.
**Prevention:** During duplicate checks, treat same intended files, same WorkFingerprint, stale action-count baselines, and micro-PRs against shared coordination docs as reasons to stop without PR unless the change is uniquely valuable.
**Avoid:** Creating a PR just because a tiny edit is available, especially in `AGENTS.md`, `.jules/*`, `.gitignore`, release docs, or action-count docs where multiple agents often race.

## 2026-05-16 - Establish file boundary map for agent coordination
**Coordination issue:** Agents from different domains (e.g., Code vs. Docs vs. Infrastructure) overlapping and modifying files outside their primary responsibility, leading to mixed-concern PRs and collisions.
**Learning:** High-level 'single responsibility' rules were not sufficient without concrete mappings of which agent types own which files/directories.
**Prevention:** Created `.jules/agent-coordination.md` to explicitly map agent categories to their primary target directories and established strict domain boundaries.
**Avoid:** Agents creating PRs that mix codebase changes with broad coordination doc changes (`AGENTS.md`) or documentation updates, ensuring single responsibility at the directory level.

## 2026-05-18 - Recognize external CI billing limits
**Coordination issue:** Agents were attempting to fix external GitHub Actions billing-related errors ("spending limit needs to be increased") via code changes.
**Learning:** Billing or spending-limit errors in remote GitHub Actions are an external repository configuration state, not a code defect that an agent can resolve.
**Prevention:** Updated `AGENTS.md` to instruct agents to recognize these external limit errors and inform the user rather than attempting to fix them via code changes.
**Avoid:** Creating pointless PRs to "fix" an external CI billing issue.

## 2026-05-19 - Forbid literal branch name placeholders
**Coordination issue:** Agents were creating branches like `jules/skill-doc-smith/short-topic` or `jules/ui-smith/short-topic`, using literal prompt placeholder strings instead of describing their work.
**Learning:** Without explicit instruction, agents often copy-paste the exact placeholder string provided in their system prompts rather than replacing them with descriptive text.
**Prevention:** Updated `AGENTS.md` to explicitly forbid using literal strings like `short-topic` or `module-or-area`, requiring them to be replaced with descriptive text.
**Avoid:** Creating branches containing literal placeholder strings like `short-topic` or `module-or-area`.
