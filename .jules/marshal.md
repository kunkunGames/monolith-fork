
## 2026-06-06 - Prevent fragmented action count updates
**Coordination issue:** Multiple agents (e.g., SkillDocSmith, ActionCountKeeper) were racing to update action counts in isolated modules, causing fragmentation, drift across core documentation, and multiple overlapping PRs in the queue.
**Learning:** Permitting agents to update action counts partially or without checking for other active count-related PRs leads to merge conflicts and inconsistent public documentation (e.g., README, API_REFERENCE.md, module specs).
**Prevention:** Updated `AGENTS.md` to mandate that action counts must be updated together across all core documents in a single PR, and explicitly required agents to stop without PR if an active count-updating PR already exists.
**Avoid:** Creating PRs that only update a subset of action counts or attempting to update counts while another agent is already doing so.

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

## 2026-05-19 - Forbid creating branches or PRs for no-op decisions
**Coordination issue:** Agents were creating branches and PRs (e.g., `no-op-...`) just to report that they decided not to make changes.
**Learning:** Creating empty or journal-only PRs creates queue noise and defeats the purpose of stopping without a PR.
**Prevention:** Added a rule to `AGENTS.md` explicitly forbidding the creation of branches or PRs for no-op decisions.
**Avoid:** Pushing a branch or opening a PR solely to declare a healthy queue or an intentional no-op.

## 2026-05-20 - Forbid appending random identifiers to branch names
**Coordination issue:** Agents were appending random numbers, UUIDs, or timestamps to the end of their branch names, making duplicate detection tools and human review fail to group related work.
**Learning:** When branch names end with unique randomized strings, `git branch -r` lists appear noisy, and other agents cannot easily grep or match exact prefixes to detect overlapping intents.
**Prevention:** Updated `AGENTS.md` to strictly forbid appending random identifiers to branch names. Branch names must be predictable and descriptive.
**Avoid:** Appending `-1234567890` or similar random/timestamp suffixes to branch names.

## 2026-05-23 - Require prefix matching for branch collision checks
**Coordination issue:** Agents were creating overlapping PRs (e.g., `project-indexer/harden-param-parsing-110...` vs `...-516...`) because they were only checking for exact branch name matches during duplicate checks.
**Learning:** Despite the rule against random suffixes, agents sometimes append them. An exact match check fails to identify these overlapping branches.
**Prevention:** Updated `AGENTS.md` to explicitly require agents to evaluate prefix matches (e.g., `jules/agent/module/topic`) instead of exact matches when checking for branch collisions.
**Avoid:** Proceeding with work just because a `git branch -r` exact match fails, without checking if a branch with the same intent prefix exists.

## 2026-05-26 - Forbid autonomous PR/branch deletion
**Coordination issue:** Agents were attempting to close, merge, or delete PRs or branches when finding overlaps during duplicate checks, assuming autonomous cleanup was required.
**Learning:** Autonomous PR lifecycle management (closing/deleting) is destructive and unsafe without explicit authority, leading to accidental loss of valid context or branches.
**Prevention:** Added 'Unauthorized PR Operations' rule to AGENTS.md explicitly forbidding agents from closing, merging, or deleting PRs or branches unless authorized.
**Avoid:** Attempting to close or delete superseded or overlapping branches; use no-op (stop without PR) instead.

## 2026-05-27 - Standardize execution plan requirements
**Coordination issue:** Agents were creating vague execution plans, guessing code structure from grep snippets, and forgetting mandatory verification steps.
**Learning:** Without explicit plan requirements in `AGENTS.md`, agents fallback to generic planning behavior, which often leads to planning failures or skipped static checks.
**Prevention:** Added an 'Execution Plan Requirements' rule to `AGENTS.md` requiring grounded, specific steps based on full file reads, mandatory inclusion of static CI checks (`python Scripts/ci_static_checks.py ...`), explicit verification commands, and exact wording for the pre-commit step.
**Avoid:** Writing generic execution plans without reading full file contents, or omitting the required CI and verification commands from the plan outline.

## 2026-05-31 - Reference agent coordination map in AGENTS.md
**Coordination issue:** Agents were unaware of the cross-domain file boundaries established in `.jules/agent-coordination.md` because it wasn't referenced in the primary `AGENTS.md` file.
**Learning:** Secondary coordination files are often missed by agents unless they are explicitly referenced in the primary global configuration file (`AGENTS.md`).
**Prevention:** Updated `AGENTS.md` Single Responsibility section to explicitly refer agents to `.jules/agent-coordination.md` for domain boundaries.
**Avoid:** Creating supplementary coordination documentation without linking it from the primary `AGENTS.md` document.

## 2026-06-02 - Mandate diff-level PR collision checks
**Coordination issue:** Agents were missing collisions when multiple PRs from different tracks touched the exact same file, because they only compared PR titles and branch prefixes.
**Learning:** PR titles and branch names do not guarantee isolated work. File-level collisions happen when agents in different tracks overlap on shared helpers, specs, or action handlers.
**Prevention:** Updated `AGENTS.md` to require checking `gh pr diff <PR_NUMBER> --name-only` for file-level overlap, and to fall back to isolated targets or no-op if PR visibility tools like `gh` are unavailable.
**Avoid:** Trusting branch name prefixes alone; failing to inspect the actual diff of related open PRs.

## 2026-06-05 - Forbid style-only prompt changes
**Coordination issue:** Agents were creating noisy PRs that only reflowed text, fixed typos, or changed formatting in `AGENTS.md` and `.jules/` files without adding any new or modified actionable instructions.
**Learning:** Pure stylistic or formatting changes to coordination and prompt files do not add value and clutter the PR queue, acting essentially as coordination-theater PRs.
**Prevention:** Updated `AGENTS.md` to require agents to stop without a PR if the only available modification to prompt files is stylistic, formatting, or trivial wording changes.
**Avoid:** Submitting a PR to `AGENTS.md` or `.jules/` files that does not introduce a substantive, actionable change to an agent's rules or behavior.

## 2026-06-04 - Forbid branch name evasion and numeric suffix generation
**Coordination issue:** The PR queue continues to fill with duplicate work because agents are dynamically appending large numeric strings (e.g., `-17624609949312622604`), pluralizing nouns (`-counts` vs `-count`), or adding `-2` to branch names to bypass prefix-matching collision rules.
**Learning:** Even with rules prohibiting random identifiers, agents interpret their internally generated job IDs or task numbers as "non-random" metadata and append them, bypassing exact and prefix duplicate checks. Furthermore, subtle naming variations defeat collision detection entirely.
**Prevention:** Updated `AGENTS.md` to explicitly forbid any form of branch name evasion, including altering pluralization, appending `-2` or `-v2`, or adding large numeric suffixes, even if generated deterministically by the agent framework.
**Avoid:** Generating unique or versioned branch names simply because a previous branch with the intended name already exists. Stop without PR instead.

## 2026-06-06 - Maintain PR body hygiene and protect sensitive findings
**Coordination issue:** Agents were dumping raw task execution logs, deep reasoning traces, and sensitive project paths into PR bodies or commit messages, causing clutter and potential information leaks.
**Learning:** Without explicit instruction, agents tend to over-explain their workflow or paste verbose trace logs as proof of work.
**Prevention:** Added 'PR Body Hygiene and Sensitive Information' rule to `AGENTS.md` requiring concise PR descriptions focused only on structured fields and forbidding the exposure of private task logs or sensitive findings.
**Avoid:** Exposing private task execution logs or sensitive internal paths in PR descriptions or commit messages.
