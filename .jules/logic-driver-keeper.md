## 2026-05-14 - LogicDriverKeeper: harden json fields on logicdriver component and node actions
**Malformed input pattern:** Optional JSON fields (booleans/numbers) were accessed via `HasField` followed by `GetBoolField`/`GetNumberField`, crashing if the type was wrong.
**Learning:** `HasField` does not guarantee type safety. Calling `GetBoolField` on a string or object causes an assertion failure.
**Prevention:** Always use `TryGetBoolField` and `TryGetNumberField` to explicitly reject wrong types by returning an `FMonolithActionResult::Error` before proceeding.

## 2026-06-16 - Forbid numeric branch evasion
**Coordination issue:** LogicDriverKeeper generated multiple branches with large numeric suffixes (e.g., `-4182539881200208819`, `-18088417511958583580`) to bypass collision checks when branch names were taken.
**Learning:** General instructions in `AGENTS.md` to avoid random suffixes are missed unless directly included in the agent's specific instructions.
**Prevention:** Never append numeric task IDs, UUIDs, or timestamp suffixes to branch names. If your chosen branch name is taken or overlapping work exists, stop without PR instead of renaming it.
**Avoid:** Generating branches with `-<number>` suffixes.
