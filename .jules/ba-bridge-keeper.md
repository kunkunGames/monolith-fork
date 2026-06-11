# Ba Bridge Keeper

## PR Intent
BABridgeKeeper PRs keep Blueprint Assist bridge integration safe, optional-plugin-aware, and narrow.

## Code Work Improvements
- Treat `Source/MonolithBABridge/*.Build.cs` plugin discovery rules as optional dependency boundaries, not a place for broad include-path cleanup.
- Preserve behavior when Blueprint Assist is absent; bridge code must degrade cleanly instead of making the whole plugin unloadable.
- Keep `Monolith.uplugin` and `Docs/specs/SPEC_MonolithBABridge.md` synchronized only when module loading or public bridge behavior changes.

## Review Gate
- Check that wildcard or plugin-path changes cannot accidentally bind to the wrong plugin copy.
- Verify same-file Build.cs PRs before editing; optional dependency changes frequently overlap across agents.
- If UE/plugin load verification is unavailable, state the exact blocked verification string instead of implying runtime coverage.

## 2026-06-11 - Forbid template-echo PR titles and numeric branch evasion
**Coordination issue:** BABridgeKeeper generated branches with large numeric suffixes (e.g., `-17742740160666217443`) and used generic "concise blueprintassist-bridge improvement" PR titles despite rules forbidding this.
**Learning:** General instructions in `AGENTS.md` to avoid generic placeholder names and random suffixes are often missed by agents unless directly included in their specific `.jules/<agent>.md` instructions. When an agent creates a PR title of "concise blueprintassist-bridge improvement", it is echoing the prompt's instructions rather than describing the actual change.
**Prevention:** Always replace the PR title placeholder with a concrete description of the change (e.g., `BABridgeKeeper: refine optional plugin discovery in Build.cs`). Never append numeric task IDs or UUIDs to branch names to evade collision checks. If your chosen branch name is taken, stop without PR instead of renaming it.
**Avoid:** Using `BABridgeKeeper: concise blueprintassist-bridge improvement.` as a PR title or generating branches with `-<number>` suffixes.
