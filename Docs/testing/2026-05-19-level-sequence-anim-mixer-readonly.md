# 2026-05-19 — Level Sequence Anim Mixer Read-Only Probe

**Scope:** `MonolithLevelSequence` reflection-only parity probe for UE 5.8 Experimental `MovieSceneAnimMixer` while preserving UE 5.7 compatibility.

---

## Result

PASS. The new read-only Anim Mixer actions compile under UE 5.7 without any `MovieSceneAnimMixer` include or link dependency.

## Gates

| Gate | Command | Result |
|------|---------|--------|
| Whitespace | `git diff --check` | PASS |
| Static CI | `uv run python Scripts\ci_static_checks.py --config .github\monolith-static-ci.json --github check` | PASS: 0 blocking findings; existing `.claude/agents` advisory only |
| UE 5.7 plugin build | Resolve `GO.uproject` engine root, then run UBT `UnrealEditor Win64 Development -Plugin=<worktree>\Monolith.uplugin` | PASS |

## Notes

- The new actions do not include or link against `MovieSceneAnimMixer`; they soft-probe plugin/module/class availability and inspect tracks/layers through reflected property names.
- UE 5.7 projects without the experimental plugin should return successful status responses and `track_count=0` for sequences without reflected mixer tracks.
- UBT still reports the existing MassEntity deprecation warning from the plugin/project dependency graph; no new blocking compile warning remained in the Level Sequence changes.
