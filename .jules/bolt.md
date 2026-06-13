# Bolt PR Guidance

## PR Intent
Bolt PRs are performance-only maintenance changes. They should reduce avoidable allocations, repeated registry lookups, or temporary string construction while preserving the public action contract and response shape exactly.

## Code Work Improvements
- Keep Bolt changes behavior-neutral: no parameter rename, no new required field, no response field removal, and no error-message rewrite unless the PR explicitly documents a public contract reason.
- Prefer small local optimizations such as `Reserve`, avoiding temporary `FString::Join` allocations, or caching a registry lookup once per function.
- Do not mix performance work with param hardening, docs count changes, or release hygiene. Those belong to ParamGuard, ActionCountKeeper, or Curator-style PRs.
- When optimizing loops, reserve from the source collection size and still preserve existing filtering semantics.
- C++ optimization PRs still need a compile-oriented review. A syntactically small branch can break build if control-flow edits leave dangling `else` blocks or undeclared temporaries.

## Review Gate
- Run `git diff --check`.
- Inspect the exact changed function, not only the diff summary.
- If a `.cpp` changed, run or request a full UBT build before claiming UE verification.

## Recent Learnings
- **Avoid PR overlap**: If an existing PR reserves arrays for a specific module (e.g., #285 for MonolithMaterial), do NOT submit another PR reserving arrays in the same module. Verify open PRs and their changed files using `gh pr list` or similar tools before making changes.

## 2026-06-11 - Forbid numeric branch evasion
**Coordination issue:** Bolt generated multiple branches with large numeric suffixes (e.g., `-15020827319703132869`, `-12586458760977925137`) to bypass collision checks when branch names were taken.
**Learning:** General instructions in `AGENTS.md` to avoid random suffixes are missed unless directly included in the agent's specific instructions.
**Prevention:** Never append numeric task IDs, UUIDs, or timestamp suffixes to branch names. If your chosen branch name is taken or overlapping work exists, stop without PR instead of renaming it.
**Avoid:** Generating branches with `-<number>` suffixes.
