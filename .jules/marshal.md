
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

## 2026-05-11 - Enforce public action contract stability
**Coordination issue:** Agents performing routine refactoring or performance tasks were casually modifying JSON parameter schemas or public action outputs to simplify their code.
**Learning:** Casual changes to public contracts break downstream consumers and invalidate API documentation without proper spec review.
**Prevention:** Added 'Public Action Contracts' rule to AGENTS.md explicitly forbidding changes to expected inputs/outputs during non-feature work without clear justification.
**Avoid:** Changing parameter names, adding required fields, or altering JSON return structures just to make internal C++ refactoring easier.
