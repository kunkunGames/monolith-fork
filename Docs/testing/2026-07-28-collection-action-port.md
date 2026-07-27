# Content Browser Collection Action Port Verification

| Field | Value |
|-------|-------|
| Date | 2026-07-28 |
| Status | Passed |
| Tested source | `d287c59e016053c6d06ac33b8098cdbd9cdb6e85` |
| Scope | `MonolithIndex` `collection` namespace; 13 actions |

---

## 1. Goal

Verify that the Content Browser collection action pack is registered at runtime, rejects malformed JSON without coercion, and completes real local static/dynamic collection lifecycles on both supported engine versions.

---

## 2. Isolation and Engine Resolution

Each engine used a separate minimal host project whose `Plugins\Monolith` junction resolved to the exact tested source. Engine roots were resolved from each host `.uproject` `EngineAssociation`; no engine path was selected as a source-code fallback.

| Host | Engine association | Resolved engine root | Monolith source |
|------|--------------------|----------------------|-----------------|
| `D:\P4\MonolithCollectionUE57Host` | `5.7` | `D:\Engine\UE_5.7` | `D:\P4\speed\Saved\GitWorktrees\Monolith-fork-collection` |
| `D:\P4\MonolithCollectionUE58Host` | `5.8` | `D:\Engine\UE_5.8` | detached worktree at `d287c59e016053c6d06ac33b8098cdbd9cdb6e85` |

An initial direct `UnrealEditor -Plugin=<worktree>` attempt was rejected as evidence: UBT returned success while reusing a prior plugin action graph and produced no collection build output. The final gates therefore use unique host targets, isolated `Intermediate`/`Binaries`, and the exact source revision.

---

## 3. Build Verification

`MONOLITH_RELEASE_BUILD=1` was set for both builds.

| Engine | Target command | Result | Linked DLL size | SHA-256 |
|--------|----------------|--------|-----------------|---------|
| UE 5.7 | `Build.bat UnrealEditor Win64 Development -Project=D:\P4\MonolithCollectionUE57Host\MonolithCollectionUE57Host.uproject -WaitMutex -NoHotReloadFromIDE` | Pass | 926,208 bytes | `375CA6668838981F7376AC672527E5D10F0AB33A8F18D27504C5949CA2FFFFA1` |
| UE 5.8 | `Build.bat UnrealEditor Win64 Development -Project=D:\P4\MonolithCollectionUE58Host\MonolithCollectionUE58Host.uproject -WaitMutex -NoHotReloadFromIDE` | Pass | 889,344 bytes | `613AB8C16B1D5B49C122510CCBEBFF54F7E573B09F4992CAEEEC4E33CFEA77BD` |

Both final builds compiled `AssetCollectionActions.cpp` and linked `UnrealEditor-MonolithIndex.dll`.

---

## 4. Focused Automation

| Engine | Report | Passed | Warnings | Errors |
|--------|--------|--------|----------|--------|
| UE 5.7 | `D:\P4\MonolithCollectionUE57Host\Saved\Automation\CollectionActionPort-UE57-Final\index.json` | 2/2 | 0 | 0 |
| UE 5.8 | `D:\P4\MonolithCollectionUE58Host\Saved\Automation\CollectionActionPort-UE58-Final\index.json` | 2/2 | 0 | 0 |

| Test | Contract |
|------|----------|
| `Monolith.Collection.RegistrationAndValidation` | All 13 action names resolve in the registry. Required fields, enum text, bools, arrays and elements, color objects/channels, and string scalars reject wrong JSON types. |
| `Monolith.Collection.LocalLifecycle` | Create static collection; add/contains/list/color/remove; delete. Create dynamic collection; set/get query; delete. Scope-exit cleanup removes any fixture left by an earlier assertion. |

The current catalog generator found 1,574 in-tree registrations, including exactly 13 `collection` actions (base: 1,561; delta: +13).

Final editor logs:

- UE 5.7: `D:\P4\MonolithCollectionUE57Host\Saved\Logs\CollectionActionPort-Automation-UE57-Final.log`
- UE 5.8: `D:\P4\MonolithCollectionUE58Host\Saved\Logs\CollectionActionPort-Automation-UE58-Final.log`

Both logs contain two successful test-completion records, zero focused failure records, and no collection-action overwrite warning. No `MonolithStatic_*` or `MonolithDynamic_*` fixture remained under either host's `Saved\Collections`.

---

## 5. Side-effect Review

| Risk | Control | Verified result |
|------|---------|-----------------|
| Wrong collection scope mutated | Every operation carries the requested share type; read-only containers reject writes | No fallback or share-type substitution path exists |
| Non-empty collection deleted accidentally | `force` defaults to `false` | Deletion rejects a non-empty collection unless explicitly forced |
| MCP JSON strings silently treated as bool/number/object | Exact `EJson` type checks precede every scalar read | Wrong types return `-32602` |
| Partial add/remove loses diagnostics | Handler returns counts and attaches partial-result data to `-32603` | Caller can identify requested vs. changed items |
| Automation leaves editor collections behind | RAII cleanup plus explicit successful deletion | No test-named collection files remained |

---

## 6. Visual and Discord Evidence

Not applicable. The change adds editor data-management actions and no visual, gameplay, UI, VFX, animation, material, or presentation behavior; therefore no `1920x1080` screenshot or Discord screenshot upload was required.
