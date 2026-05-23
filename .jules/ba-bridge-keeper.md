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
