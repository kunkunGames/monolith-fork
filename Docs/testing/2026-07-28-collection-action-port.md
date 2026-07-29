# Content Browser Collection Action Port Verification

| Field | Value |
|-------|-------|
| Verification date | 2026-07-29; AI review follow-up 2026-07-30 |
| Status | Passed |
| Engine-tested implementation | `c8e056eb20e23818e2e4485b1fb5c14493958e40` |
| Scope | `MonolithIndex` `collection` namespace; 13 actions; static/dynamic membership; native/Python proxy cold-start parity |

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
| Dynamic collections always reported `asset_count:0`, `assets:[]`, and `contains:false` | A shared resolver now evaluates saved queries over the active Content Browser `AssetData` source and unions static/dynamic results over the requested hierarchy scope | UE 5.7/5.8 automation plus live UE 5.8 direct, nested, and self-cycle queries |
| Non-forced deletion inspected only stored members, so a populated dynamic collection could be deleted | `delete_collection` now uses the shared resolved-membership path for its safety gate; only `force=true` skips evaluation | UE 5.7/5.8 lifecycle automation and live UE 5.8 non-empty rejection |
| `list_collections` rescanned `/All` once per dynamic collection | One resolution session compiles all requested filters, enumerates assets once, and caches a sorted result per collection | Automation creates two matching dynamic collections and resolves both in one list call; live counts are 3,689 each |
| Nested query failures and reference cycles were swallowed as false membership | The expression context preserves the nested error and aborts the owning read with a specific cycle/source-data error | UE 5.7/5.8 cycle automation and live cyclic `get_collection` error |
| Dynamic `Path` compared a Content Browser virtual path plus the asset leaf | `Path` now compares `FAssetData::PackagePath`, with the internal package path only as a non-asset fallback | Package-folder query matches the default texture; `/All/.../DefaultTexture` does not |
| `set_collection_color` could commit the color and then report failure while rereading invalid dynamic membership | The handler returns a mutation-specific payload directly after the successful color write | A cyclic collection returns the applied RGBA payload successfully |

---

## 3. Isolation and Engine Resolution

Each engine used a separate minimal host project whose `Plugins\Monolith` junction resolved to the exact tested implementation. Engine roots were resolved from each host `.uproject` `EngineAssociation`; no alternate engine checkout was used as a source fallback.

| Host | Engine association | Resolved engine root | Monolith source |
|------|--------------------|----------------------|-----------------|
| `D:\P4\MonolithCollectionUE57Host` | `5.7` | `D:\Engine\UE_5.7` | `D:\P4\speed\Saved\GitWorktrees\Monolith-fork-collection` |
| `D:\P4\MonolithCollectionUE58Host` | `5.8` | `D:\Engine\UE_5.8` | detached validation worktree at `c8e056eb20e23818e2e4485b1fb5c14493958e40` |

An earlier direct `UnrealEditor -Plugin=<worktree>` path remains rejected as evidence because UBT can reuse another worktree's plugin action graph. The accepted gates use unique host targets and isolated host `Intermediate` / plugin `Binaries` directories.

---

## 4. Build Verification

| Engine | Target command | Accepted log | Result | Linked DLL size | SHA-256 |
|--------|----------------|--------------|--------|-----------------|---------|
| UE 5.7 | `UnrealBuildTool.exe MonolithCollectionUE57HostEditor Win64 Development -Project=D:\P4\MonolithCollectionUE57Host\MonolithCollectionUE57Host.uproject -WaitMutex -NoHotReloadFromIDE -Log=<unique>` | `D:\P4\MonolithCollectionUE57Host\Saved\Logs\Collection-Review3-SharedSession-Build-UE57-20260730-032319.log` | Pass | 1,007,616 bytes | `D73340DE2ABA14A12DF890BB0B4BCD88678C96D0F3E4FB8BDFD817038BDA121B` |
| UE 5.8 | `UnrealBuildTool.exe MonolithCollectionUE58HostEditor Win64 Development -Project=D:\P4\MonolithCollectionUE58Host\MonolithCollectionUE58Host.uproject -WaitMutex -NoHotReloadFromIDE -Log=<unique>` | `D:\P4\MonolithCollectionUE58Host\Saved\Logs\Collection-Review3-Build-UE58-20260730-032623.log` | Pass | 968,704 bytes | `DD01C699997E2D8F3520B920B6731842DCCE1C85BD6C35B81E1898F1CF131B6B` |

The follow-up builds compile the final public `ICollectionContainer::TestDynamicQuery` / `ITextFilterExpressionContext` implementation. A preceding UE 5.7 attempt that called `FAssetTextFilter::Compile` and `FCompiledAssetTextFilter::PassesFilter` is rejected evidence: those methods are declared in a public header but are not exported from the UE 5.7/5.8 `ContentBrowser` module, producing `LNK2019`. The final implementation does not use the deprecated `FFrontendFilter_Text` compatibility class.

The first UE 5.8 launch was not accepted: it overlapped the UE 5.7 UBT process and failed before compilation while both processes contended for the user-global `UnrealBuildTool\Log.txt` backup. The serial retry above compiled and linked cleanly; no source workaround or alternate engine path was introduced.

---

## 5. Focused Automation

| Engine | Report | Passed | Warnings | Errors |
|--------|--------|--------|----------|--------|
| UE 5.7 | `D:\P4\MonolithCollectionUE57Host\Saved\Automation\Collection-Review3-Accepted-UE57-20260730-033140\index.json` | 2/2 | 0 | 0 |
| UE 5.8 | `D:\P4\MonolithCollectionUE58Host\Saved\Automation\Collection-Review3-Accepted-UE58-20260730-033211\index.json` | 2/2 | 0 | 0 |

| Test | Contract |
|------|----------|
| `Monolith.Collection.RegistrationAndValidation` | Exactly 13 actions are registered. Wrong scalar/array/object types retain precedence, empty query text and invalid unique-name candidates fail, and every action targeting a missing collection returns invalid params. Valid unique-name generation returns a non-empty candidate without creating it. |
| `Monolith.Collection.LocalLifecycle` | An existing empty static collection returns an empty success; static membership/color completes; two `Type=Texture2D` collections resolve through one list call; package-folder `Path` matches while `/All` plus asset leaf does not; populated dynamic delete requires force; cyclic nesting is an explicit error; color mutation remains independent; every created collection is deleted. |

Final editor logs:

- UE 5.7: `D:\P4\MonolithCollectionUE57Host\Saved\Logs\Collection-Review3-Accepted-Automation-UE57-20260730-033140.log`
- UE 5.8: `D:\P4\MonolithCollectionUE58Host\Saved\Logs\Collection-Review3-Accepted-Automation-UE58-20260730-033211.log`

No `MonolithStatic_*`, `MonolithDynamic_*`, `MonolithCycle*`, or `MonolithUnique_*` fixture remained in either host's `Saved\Collections`.

The first automation run against `b8c94e9d` was rejected: early existence checks masked malformed `force`, `asset_paths`, and `color` values. Commit `efb46eee` moved target lookup after action-specific input parsing. The accepted reports above prove both the original type errors and the new missing-target cases.

---

## 6. Live MCP Verification

The UE 5.8 host was launched with an isolated `Config\DefaultMonolith.ini` listener on port `9432`. `GET /health` returned HTTP 200, plugin `0.21.3`, and `tools_registered:1340`.

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
| Dynamic create before query | `query_text:""` and `asset_count:0`; an unconfigured query does not match every asset |
| Dynamic set `Type=Texture2D` / details / list / contains | `asset_count:3689`, `count:3689`, default texture listed, and `contains:true` |
| Two dynamic `Type=Texture2D` collections in one `list_collections` call | Both rows report `asset_count:3689`, exercising the shared resolution session |
| Non-forced delete of either populated dynamic collection | MCP error: `Collection is non-empty; pass force=true to delete` |
| Dynamic `Path=/Engine/EngineResources` / contains default texture | `contains:true`; package folder is the `Path` comparison surface |
| Mutually nested dynamic `Collection=<other>` references | `get_collection` returns `Cyclic dynamic collection reference detected ...` instead of an incomplete result |
| `set_collection_color` on the cyclic collection | Success with `updated:true`, `color_cleared:false`, and the applied RGBA object |

The accepted live editor log is `D:\P4\MonolithCollectionUE58Host\Saved\Logs\Collection-Review3-Accepted-LiveMCP-UE58-20260730-033242.log`. Both follow-up fixtures were explicitly force-deleted after the safety assertions. The editor then accepted the schema-described `editor.run_console_command` with `QUIT_EDITOR`, exited cleanly, and released port `9432`; no `MonolithLiveA_*` or `MonolithLiveB_*` collection file remained.

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
| Dynamic collection silently treated as an empty static list or rescanned once per list row | One shared session evaluates all requested queries and unions hierarchy members | Two live dynamic rows each return 3,689 members from one list call |
| Invalid nested query silently reduced a membership result | Nested evaluation errors and active cycles abort the owning read | Cyclic `get_collection` returns a specific error |
| Non-empty dynamic collection deleted accidentally | `force` defaults to `false`; the guard resolves live query membership | Populated dynamic deletion is rejected unless explicitly forced |
| Successful color mutation reported as failure due to an unrelated member read | The action returns mutation data directly and does not call `get_collection` | Cyclic collection color succeeds and reports the applied RGBA value |
| MCP JSON values silently coerced | Exact `EJson` checks precede typed reads | Wrong types and empty required strings return invalid params |
| Automation or live probes leave editor collections behind | Scope-exit cleanup plus explicit live deletion | No named fixture remained |
| New namespace disappears when the editor is down | Native/Python seed lists are changed and checked together | Both fresh cold-start manifests contain one `collection_query` |

---

## 9. Visual and Discord Evidence

Not applicable. This change adds editor data-management actions, failure contracts, skill guidance, and stdio proxy discovery. It does not change gameplay, UI, VFX, animation, materials, editor panel presentation, or another visual surface, so no `1920x1080` screenshot or Discord screenshot upload was required.
