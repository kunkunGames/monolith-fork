# Font Family Ingest Bounds Verification

**Date:** 2026-08-02 (KST)
**Branch:** `jules/codex/asset/font-ingest-bounds`
**Fork base:** `kunkunGames/monolith-fork@07faaf0583a8190f5aa021c9c6cc22fe556427c5`
**Engine floor:** Unreal Engine 5.7
**Engine ceiling tested:** Unreal Engine 5.8
**Status:** PASS

---

## 1. Goal

Bound every allocation accepted by `asset.import_font_family`, reject oversized requests before package creation, and remove the second source-file read between preflight and mutation. The action must keep its all-or-nothing package behavior while remaining source-compatible with Unreal Engine 5.7 and 5.8.

| Contract | Required result |
|----------|-----------------|
| Face count | Accept 1-64 entries; reject 65 or more with `-32602`. |
| Per-face source bytes | Accept at most 64 MiB; reject a larger file before allocating its payload. |
| Family source bytes | Accept at most 256 MiB across the request. |
| Read consistency | Validate and retain one read of each accepted file; do not reopen it during mutation. |
| Failure atomicity | An invalid or oversized request creates no `UFont` or `UFontFace` package. |

---

## 2. Unreal Engine Source Verification

Both supported engines implement `FFileHelper::LoadFileToArray(TArray<uint8>&, ...)` by opening an `FArchive`, reading `TotalSize()`, resetting the destination array to that size, adding the full uninitialized payload, and only then serializing the file. The helper's `MAX_int32` guard is not an application-level memory budget.

| Engine | Source inspected | Relevant implementation | Finding |
|--------|------------------|-------------------------|---------|
| UE 5.7 | `Engine/Source/Runtime/Core/Private/Misc/FileHelper.cpp` | Lines 39-69 | `Result.Reset(TotalSize + 2)` and `Result.AddUninitialized(TotalSize)` allocate the whole accepted file. |
| UE 5.8 | `Engine/Source/Runtime/Core/Private/Misc/FileHelper.cpp` | Lines 43-73 | The same full-file allocation contract remains in 5.8. |

The Monolith action therefore uses its own `FArchive` reader, checks `TotalSize()` against the action budgets before `TArray` allocation, reads once, and retains those exact bytes for the package mutation phase.

---

## 3. Implementation Contract

| Area | Before | After |
|------|--------|-------|
| Request cardinality | Any non-empty `faces[]` length was accepted. | More than 64 face specifications fail before filesystem or package work. |
| File allocation | Every `.ttf` was loaded without an action-level size cap. | Each source is capped at 64 MiB and the request aggregate at 256 MiB before allocation. |
| Source reads | Preflight loaded each file, discarded the bytes, and mutation reopened it. | Preflight retains validated bytes and mutation consumes them with `MoveTemp`. |
| Mutation boundary | A source could change between validation and package creation. | Package creation receives the exact payload that passed validation. |
| API documentation | The accepted resource envelope was unspecified. | Action description, schema, API reference, and module spec state the same bounds. |

The pure limit helpers live in `Source/MonolithAsset/Private/MonolithAssetFontIngestInternal.h`; the action and its automation test share the same constants rather than duplicating numeric policy.

---

## 4. Build and Automation Results

| Gate | Command / evidence | Result |
|------|--------------------|--------|
| UE 5.7 full current-byte build | `RunUAT.bat BuildPlugin -Plugin=D:\P4\MonolithForkFontBounds\Monolith.uplugin -Package=D:\P4\MonolithFontBoundsUE57Package -NoTargetPlatforms` with `MONOLITH_RELEASE_BUILD=1` | PASS — UnrealEditor `529/529`, UAT exit code 0. |
| UE 5.7 focused automation | `Automation RunTests MonolithAsset.ImportFontFamily.InvalidParams` | PASS — `D:\P4\MonolithFontBoundsUE57Host\Saved\Logs\Automation_20260802_FontBounds_UE57_Final.log:1144`. |
| UE 5.7 linked module | `UnrealEditor-MonolithAsset.dll`, 1,595,392 bytes | SHA-256 `1DCF3658BEE271D79C5B852D53EC38CFF7FA1B97AF971E880BF00B7DD72AFFCC`. |
| UE 5.8 full current-byte build | `RunUAT.bat BuildPlugin -Plugin=D:\P4\MonolithForkFontBounds\Monolith.uplugin -Package=D:\P4\MonolithFontBoundsUE58Package -NoTargetPlatforms` with `MONOLITH_RELEASE_BUILD=1` | PASS — UnrealEditor `529/529`, UAT exit code 0. |
| UE 5.8 focused automation | `Automation RunTests MonolithAsset.ImportFontFamily.InvalidParams` | PASS — `D:\P4\MonolithFontBoundsUE58Host\Saved\Logs\Automation_20260802_FontBounds_UE58_Final.log:2054`. |
| UE 5.8 linked module | `UnrealEditor-MonolithAsset.dll`, 1,504,768 bytes | SHA-256 `EC86E8133D0850DEDAD8ABA4E3E1D5400655AC4AAD6A2659C7359901A5ED592C`. |
| Patch hygiene | `git diff --check` | PASS. |

The focused automation covers the 65-face rejection, exact per-face and aggregate boundaries, a real 64 MiB + 1 byte file, `-32602`, explanatory errors, and absence of family/face packages after rejection.

The isolated commandlets logged a failed attempt to bind `127.0.0.1:9316` because an externally owned Monolith editor already held the endpoint. Action registration and automation remained available in-process, so this did not alter either test result; the owning editor was not stopped or modified.

---

## 5. Presentation and Side Effects

| Item | Result |
|------|--------|
| Runtime/editor presentation | N/A — this is headless input validation and allocation control; no UI, asset appearance, or gameplay state changed. |
| Screenshot / Discord upload | N/A — no visual acceptance surface exists for this change. |
| Repository binaries | No generated `.dll`, `.pdb`, package, or test fixture is included in the change. |
| Source control isolation | Implementation stayed in the dedicated Git worktree; the Speed Perforce checkout and its running editor were not changed. |
