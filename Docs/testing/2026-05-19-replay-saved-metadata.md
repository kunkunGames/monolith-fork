# Saved Replay Metadata Verification

**Date:** 2026-05-19
**Engine:** Unreal Engine 5.7+
**Scope:** `level_sequence.get_saved_replay`

---

## 1. Spec Source

| Artifact | Purpose |
|----------|---------|
| `Docs/specs/SPEC_MonolithReplaySavedMetadata.md` | Defines the single saved replay metadata contract and safety gates. |
| `Docs/specs/SPEC_MonolithLevelSequence.md` | Tracks the `level_sequence` action count and replay inspection surface. |
| `Docs/API_REFERENCE.md` | Records the public action addition. |

---

## 2. Expected Behavior

| Gate | Expected Result |
|------|-----------------|
| Saved-relative input | Absolute filesystem paths and path traversal are rejected before lookup. |
| Replay root guard | Resolved paths must stay under `Saved/Demos`, `Saved/Replays`, or `Saved/Replay`. |
| No byte streaming | Responses return names, extensions, sizes, timestamps, and counts only. |
| Bounded output | Container child file rows are clamped to `0..500`. |

---

## 3. Verification Results

| Gate | Evidence | Result |
|------|----------|--------|
| Spec-first docs | `Docs/specs/SPEC_MonolithReplaySavedMetadata.md`, `Docs/specs/SPEC_MonolithLevelSequence.md`, and `Docs/API_REFERENCE.md` were updated before code verification. | PASS |
| Static diff check | `git diff --check origin/feat/action-execution-policy-metadata...HEAD` | PASS |
| LevelSequence action count | `Select-String Source\MonolithLevelSequence\Private\MonolithLevelSequenceActions.cpp -Pattern 'Registry\.RegisterAction\(TEXT\("level_sequence"\)'` returned 11 registrations. | PASS |
| C++ compile | `UnrealBuildTool.exe UnrealEditor Win64 Development -Project="D:\P4\game\Saved\MonolithPcgStackBuild_20260519_084638\HostProject\HostProject.uproject" -plugin="D:\P4\monolith-prs\replay-saved-metadata\Monolith.uplugin" -Module=MonolithLevelSequence -WaitMutex -NoHotReloadFromIDE -NoUBTMakefiles` | PASS: `Result: Succeeded`. |
