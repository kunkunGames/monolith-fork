# Content Browser Collection Action Port Verification

| Field | Value |
|-------|-------|
| Verification date | 2026-07-29 |
| Status | Passed |
| Engine-tested implementation | `efb46eee24ed67980b8e6b61dad66d9259c5bc4a` |
| Scope | `MonolithIndex` `collection` namespace; 13 actions; native/Python proxy cold-start parity |

---

## 1. Goal

Verify that the Content Browser collection action pack:

1. registers and describes all 13 actions through the live MCP catalog;
2. preserves the difference between a missing collection and an existing empty collection;
3. rejects malformed JSON and empty required strings without coercion;
4. completes real local static/dynamic collection lifecycles on UE 5.7 and UE 5.8;
5. remains discoverable through both stdio proxies when the editor is down and no catalog cache exists.

---

## 2. Review Findings and Resolutions

| Review finding | Resolution | Proof |
|----------------|------------|-------|
| Empty dynamic query text was accepted | `set_dynamic_query` now rejects an empty `query_text` with invalid params | Both engine automation runs and live UE 5.8 MCP call |
| Missing `list_assets` lookup looked like an empty collection | The handler checks `GetAssetsInCollection` and returns a scoped missing-collection error | Live missing lookup is an error; existing empty collection succeeds with `count=0` |
| Missing `contains_asset` lookup looked like `contains=false` | The handler verifies collection existence before membership lookup and propagates engine errors | Live missing lookup is an error; existing empty collection succeeds with `contains=false` |
| Other target-specific actions had inconsistent missing-resource codes | A shared preflight now covers all nine actions that require an existing collection, after each action's own input validation | Automation checks invalid-params codes; live MCP checks all nine missing targets and four validation-precedence cases |
| Skill used a non-existent discovery signature | Exact schema guidance now uses `describe_query("action_schema", ...)`; whole-namespace guidance uses `detail:true` | Live schema request for `collection.list_assets` returned its exact three-parameter schema |
| Skill routed to skills not shipped by this repository | The skill uses shipped `unreal-project-search` / `unreal-gas` guidance and delegates source control to the host workflow | Skill link audit plus repository path check |
| Editor-down proxies omitted the new namespace | `collection_query` was added to the native and Python cold-start seed lists | Fresh isolated caches returned 21 tools with exactly one `collection_query` in each proxy |
| Unique-name helper looked mutating and could return an unusable candidate | The action validates its generated candidate and the skill identifies it as non-mutating | Invalid base is rejected; valid candidate lookup returns “does not exist” |

---

## 3. Isolation and Engine Resolution

Each engine used a separate minimal host project whose `Plugins\Monolith` junction resolved to the exact tested implementation. Engine roots were resolved from each host `.uproject` `EngineAssociation`; no alternate engine checkout was used as a source fallback.

| Host | Engine association | Resolved engine root | Monolith source |
|------|--------------------|----------------------|-----------------|
| `D:\P4\MonolithCollectionUE57Host` | `5.7` | `D:\Engine\UE_5.7` | `D:\P4\speed\Saved\GitWorktrees\Monolith-fork-collection` |
| `D:\P4\MonolithCollectionUE58Host` | `5.8` | `D:\Engine\UE_5.8` | detached validation worktree at `efb46eee24ed67980b8e6b61dad66d9259c5bc4a` |

An earlier direct `UnrealEditor -Plugin=<worktree>` path remains rejected as evidence because UBT can reuse another worktree's plugin action graph. The accepted gates use unique host targets and isolated host `Intermediate` / plugin `Binaries` directories.

---

## 4. Build Verification

`MONOLITH_RELEASE_BUILD=1` was set for both accepted builds.

| Engine | Target command | Accepted log | Result | Linked DLL size | SHA-256 |
|--------|----------------|--------------|--------|-----------------|---------|
| UE 5.7 | `Build.bat MonolithCollectionUE57HostEditor Win64 Development -Project=D:\P4\MonolithCollectionUE57Host\MonolithCollectionUE57Host.uproject -WaitMutex -NoHotReloadFromIDE` | `D:\P4\MonolithCollectionUE57Host\Saved\Logs\CollectionActionPort-Build-UE57-Accepted-20260729-235044.out.log` | Pass | 941,568 bytes | `265A11E1E16097B60E942A7D35B8A7373D9DE147F81680B4B76ADC365D40BC84` |
| UE 5.8 | `Build.bat MonolithCollectionUE58HostEditor Win64 Development -Project=D:\P4\MonolithCollectionUE58Host\MonolithCollectionUE58Host.uproject -WaitMutex -NoHotReloadFromIDE` | `D:\P4\MonolithCollectionUE58Host\Saved\Logs\CollectionActionPort-Build-UE58-Accepted-20260729-235100.out.log` | Pass | 903,680 bytes | `DFEC0658DA2BB206B252647D122199C58B39C586BC220B8E7ADFD8CB1DFAEA92` |

The preceding `b8c94e9d` build compiled both `AssetCollectionActions.cpp` and `AssetCollectionActionsTests.cpp`. The final `efb46eee` correction changed only handler validation order, so both accepted incremental logs recompiled `AssetCollectionActions.cpp` and relinked `UnrealEditor-MonolithIndex.dll`; the already-compiled test source was unchanged.

The first UE 5.8 launch was not accepted: it overlapped the UE 5.7 UBT process and failed before compilation while both processes contended for the user-global `UnrealBuildTool\Log.txt` backup. The serial retry above compiled and linked cleanly; no source workaround or alternate engine path was introduced.

---

## 5. Focused Automation

| Engine | Report | Passed | Warnings | Errors |
|--------|--------|--------|----------|--------|
| UE 5.7 | `D:\P4\MonolithCollectionUE57Host\Saved\Automation\CollectionActionPort-UE57-Accepted-20260729-235131\index.json` | 2/2 | 0 | 0 |
| UE 5.8 | `D:\P4\MonolithCollectionUE58Host\Saved\Automation\CollectionActionPort-UE58-Accepted-20260729-235131\index.json` | 2/2 | 0 | 0 |

| Test | Contract |
|------|----------|
| `Monolith.Collection.RegistrationAndValidation` | Exactly 13 actions are registered. Wrong scalar/array/object types retain precedence, empty query text and invalid unique-name candidates fail, and every action targeting a missing collection returns invalid params. Valid unique-name generation returns a non-empty candidate without creating it. |
| `Monolith.Collection.LocalLifecycle` | An existing empty static collection returns an empty success; static membership/color and dynamic-query round trips complete; every created collection is deleted. |

Final editor logs:

- UE 5.7: `D:\P4\MonolithCollectionUE57Host\Saved\Logs\CollectionActionPort-Automation-UE57-Accepted-20260729-235131.log`
- UE 5.8: `D:\P4\MonolithCollectionUE58Host\Saved\Logs\CollectionActionPort-Automation-UE58-Accepted-20260729-235131.log`

No `MonolithStatic_*`, `MonolithDynamic_*`, or `MonolithUnique_*` fixture remained in either host's `Saved\Collections`.

The first automation run against `b8c94e9d` was rejected: early existence checks masked malformed `force`, `asset_paths`, and `color` values. Commit `efb46eee` moved target lookup after action-specific input parsing. The accepted reports above prove both the original type errors and the new missing-target cases.

---

## 6. Live MCP Verification

The UE 5.8 host was launched with an isolated `Config\DefaultMonolith.ini` listener on port `9432`. `GET /health` returned HTTP 200, plugin `0.21.3`, and `tools_registered:1278`.

| Call | Observed result |
|------|-----------------|
| `monolith_discover({namespace:"collection"})` | `total:13`; all intended action names present |
| `describe_query("action_schema", target_namespace="collection", target_action="list_assets")` | Exact `name`, `share_type`, and `recursive` schema returned |
| `list_assets` on a missing local collection | MCP error: scoped collection “does not exist” |
| `contains_asset` on a missing local collection | MCP error: scoped collection “does not exist” |
| `set_dynamic_query` with `query_text:""` | MCP error: missing or empty `query_text` |
| Missing target across get/delete/add/remove/list/contains/set-query/get-query/set-color | All nine calls return the scoped “does not exist” error |
| Malformed `force`, `asset_paths[1]`, `color`, or empty query on a missing target | The action-specific validation error wins; lookup does not mask invalid input |
| `list_assets` on a created empty static collection | Success with `assets:[]`, `count:0` |
| `contains_asset` on that empty collection | Success with `contains:false` |
| `create_unique_collection_name` with `Invalid/Collection` | MCP error identifies `base_name` as unable to produce a valid candidate |
| `create_unique_collection_name`, then `get_collection` for its candidate | Candidate generation succeeds; lookup fails because no collection was created |
| Dynamic create / set `Type=Texture2D` / get / delete | Exact query text round-tripped and cleanup succeeded |

The accepted live editor log is `D:\P4\MonolithCollectionUE58Host\Saved\Logs\CollectionActionPort-LiveMCP-UE58-Accepted-20260729-2352.log`. The editor then accepted `QUIT_EDITOR`, exited cleanly, and released port `9432`. No `MonolithLiveStatic_*` or `MonolithLiveDynamic_*` collection remained.

---

## 7. Proxy Cold-Start Verification

| Gate | Result |
|------|--------|
| `python -m py_compile Scripts\monolith_proxy.py` | Pass |
| `Tools\MonolithProxy\build_proxy.bat` | Pass; Visual C++ x64 toolchain discovered through `vswhere` |
| Native binary | 559,104 bytes; SHA-256 `D896E77C3BD9B5710591629BAA530B0A2951EC17E5591332D2A05FC50D9596B6` |
| Native proxy with isolated `LOCALAPPDATA` and unreachable `MONOLITH_URL` | 21 seed tools; exactly one `collection_query` |
| Python proxy with isolated `LOCALAPPDATA` and unreachable `MONOLITH_URL` | 21 seed tools; exactly one `collection_query` |

The isolated cache roots were:

- `D:\P4\speed\Saved\Temp\MonolithPR2ProxyNativeFinal-2bc8931ac08c49298153279a1af67a7a`
- `D:\P4\speed\Saved\Temp\MonolithPR2ProxyPython-278484875b41407b94246127889b3178`

These checks prove the namespace comes from the shipped seed lists rather than a previously cached live catalog.

---

## 8. Side-Effect Review

| Risk | Control | Verified result |
|------|---------|-----------------|
| Wrong collection scope mutated | Every operation carries the requested share type; read-only containers reject writes | No fallback or share-type substitution path exists |
| Missing collection mistaken for a valid empty result | Missing lookup and existing-empty cases are tested separately | Missing calls fail; existing-empty calls succeed |
| Non-empty collection deleted accidentally | `force` defaults to `false` | Deletion rejects a non-empty collection unless explicitly forced |
| MCP JSON values silently coerced | Exact `EJson` checks precede typed reads | Wrong types and empty required strings return invalid params |
| Automation or live probes leave editor collections behind | Scope-exit cleanup plus explicit live deletion | No named fixture remained |
| New namespace disappears when the editor is down | Native/Python seed lists are changed and checked together | Both fresh cold-start manifests contain one `collection_query` |

---

## 9. Visual and Discord Evidence

Not applicable. This change adds editor data-management actions, failure contracts, skill guidance, and stdio proxy discovery. It does not change gameplay, UI, VFX, animation, materials, editor panel presentation, or another visual surface, so no `1920x1080` screenshot or Discord screenshot upload was required.
