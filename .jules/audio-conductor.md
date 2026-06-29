2026-05-14 - Malformed Input Checking
Target: Audio asset actions
Learning: TryGet*Field handles type coercion errors silently when returning false, meaning fields with incorrect types can mutate assets with unintended default values.
Prevention: Future audio handlers must wrap TryGet*Field checks in a HasField guard to properly return malformed input errors when a field is present but has the incorrect type.

## 2026-06-28 - Forbid numeric branch evasion
**Coordination issue:** AudioConductor generated branches with large numeric suffixes (e.g., `-12321137640600355834`, `-9475852892734893661`) to bypass collision checks when branch names were taken.
**Learning:** General instructions in `AGENTS.md` to avoid random suffixes are missed unless directly included in the agent's specific instructions.
**Prevention:** Never append numeric task IDs, UUIDs, or timestamp suffixes to branch names. If your chosen branch name is taken or overlapping work exists, stop without PR instead of renaming the branch to bypass collision checks.
**Avoid:** Generating branches with `-<number>` suffixes.
