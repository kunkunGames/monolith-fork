# Contributing to Monolith

Thanks for your interest in contributing. This guide covers everything you need to get started.

## Dev Environment Setup

### Prerequisites

- **Unreal Engine 5.7** (source or launcher build) is the compile floor. Contributions must build on **both UE 5.7 and UE 5.8** -- both are shipped and supported. Any engine API newer than 5.7 must sit behind an `ENGINE_MINOR_VERSION` gate, e.g. `#if ENGINE_MINOR_VERSION >= 8`, with a 5.7 code path alongside it. A change that only compiles on 5.8 will be sent back.
- **Windows, macOS, or Linux** — see [README Installation](README.md#installation) for per-platform proxy setup
- **Python 3.10+** (only needed for engine source indexing and for the cross-platform MCP proxy on macOS/Linux)
- **Git**

### Clone & Build

```bash
# Clone into your project's Plugins directory
cd YourProject/Plugins
git clone https://github.com/kunkunGames/monolith.git Monolith

# Or clone the standalone development repo
git clone https://github.com/kunkunGames/monolith.git C:\Projects\Monolith
```

Generate project files and build from your UE project as usual. Monolith is an editor-facing plugin: every module is `Type: "Editor"` except the small `MonolithAudioRuntime` helper, which is `Type: "Runtime"`.

### Development Workflow

Clone the repo into your UE project's `Plugins/` folder and develop in-place:

```
YourProject/Plugins/Monolith/   — edit, build, commit, push from here
```

---

## Code Structure

Monolith ships **~1,400+ actions across 25+ namespaces** (an approximate, rounded-down figure -- run `monolith_discover()` for the live count; per-module counts are deliberately not listed here because they go stale the moment an action is added).

Each module owns a specific domain:

| Module | Namespace | What It Does |
|--------|-----------|--------------|
| **MonolithCore** | `monolith` | HTTP server, tool registry, discovery, bulk-fill/describe framework, settings, auto-updater |
| **MonolithAsset** | `asset` | Exact-path texture/font ingest, save/delete, inspection/search, guarded move/rename, and package-graph copy/fixup/closure workflows |
| **MonolithBlueprint** | `blueprint` | Blueprint read/write, variable/component/graph CRUD, node operations, compile, auto-layout |
| **MonolithMaterial** | `material` | Material graph editing, inspection, CRUD, instances, functions, HLSL |
| **MonolithAnimation** | `animation` | Sequences, montages, ABPs, curves, notifies, skeletons, PoseSearch, IKRig, Control Rig |
| **MonolithNiagara** | `niagara` | Particle systems, emitters, modules, renderers, HLSL, dynamic inputs, event handlers, sim stages |
| **MonolithMesh** | `mesh` | Mesh inspection, scene manipulation, spatial queries, blockout, procedural geometry, lighting, experimental town gen |
| **MonolithEditor** | `editor`, `animation` | Build triggers, live compile, log capture, crash context, scene capture, texture import |
| **MonolithConfig** | `config` | INI resolution, explain, diff, search |
| **MonolithIndex** | `project`, `collection` | SQLite FTS5 deep project indexer and Content Browser collection management |
| **MonolithConfig** | `config`, `localization` | INI/CVar inspection plus guarded StringTable culture, validation, mutation, and CSV round-trip workflows |
| **MonolithIndex** | `project` | SQLite FTS5 deep project indexer |
| **MonolithSource** | `source` | Engine source lookup, call graphs, class hierarchy |
| **MonolithSourceControl** | `source_control` | Provider-backed file preparation and bounded Perforce opened/path mapping |
| **MonolithUI** | `ui` | Widget Blueprint CRUD, templates, styling, animation, settings scaffolding, accessibility |
| **MonolithGAS** | `gas`, `input` | Gameplay Ability System authoring plus 10 Enhanced Input asset actions. `gas` registration follows `bEnableGAS`; `input` remains available independently, and `WITH_GBA` gates only optional Blueprint Attributes integration |
| **MonolithGAS** | `gas` | Gameplay Ability System: abilities, attributes, effects, ASC, tags, cues, targeting, input, inspect, scaffold (gated on `WITH_GBA`) |
| **MonolithGameFeatures** | `gamefeatures` | Optional Game Feature plugin/data inspection plus guarded ActionSet and `UGameFeatureData` instanced-action authoring |
| **MonolithAI** | `ai` | Behavior trees, blackboards, EQS, StateTree, SmartObjects, perception, navigation, AI controllers |
| **MonolithAudio** | `audio` | Sound cues, waves, classes, submixes, attenuation, concurrency, MetaSounds |
| **MonolithAudioRuntime** | -- | Runtime support for the audio module (registers no MCP actions) |
| **MonolithLevelSequence** | `level_sequence` | Sequencer inspection: bindings, directors, event bindings |
| **MonolithInterchange** | `interchange` | Guarded asset import, batch import, reimport-source management, reimport, export, and source inspection |
| **MonolithReflectionIntel** | `cppreflect`, `reflect`, `decision`, `risk`, `pipeline`, `network` (plus additions to existing namespaces) | Reflection intelligence over project C++ and assets: UCLASS/UPROPERTY/UFUNCTION queries, replication audits, decision records, churn/hotspot analysis, release readiness |
| **MonolithComboGraph** | `combograph` | Optional ComboGraph integration (gated on `WITH_COMBOGRAPH`) |
| **MonolithLogicDriver** | `logicdriver` | Optional Logic Driver Pro integration (gated on `WITH_LOGICDRIVER`) |
| **MonolithBABridge** | -- | Optional Blueprint Assist integration bridge (registers no MCP actions) |

Each module follows the same file structure:

```
Source/MonolithFoo/
  Public/
    MonolithFooModule.h
    MonolithFooActions.h
  Private/
    MonolithFooModule.cpp
    MonolithFooActions.cpp
```

---

## How to Add a New Action

Actions are the atomic units of functionality. Each domain module registers actions with the central `FMonolithToolRegistry`.

### 1. Declare the handler

In your module's `Actions.h`, add a static method:

```cpp
static FMonolithActionResult HandleMyAction(const TSharedPtr<FJsonObject>& Params);
```

### 2. Implement the handler

In your module's `Actions.cpp`:

```cpp
FMonolithActionResult FMonolithFooActions::HandleMyAction(const TSharedPtr<FJsonObject>& Params)
{
    // Extract params
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return FMonolithActionResult::Error(TEXT("Missing asset_path"));
    }

    // Do work (on game thread — handlers run on game thread via AsyncTask)

    // Return result
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("status"), TEXT("success"));
    return FMonolithActionResult::Success(Result);
}
```

### 3. Register in StartupModule

In your module's `Module.cpp`:

```cpp
void FMonolithFooModule::StartupModule()
{
    FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

    Registry.RegisterAction(
        TEXT("foo"),                    // namespace
        TEXT("my_action"),             // action name
        TEXT("Description of what it does"),
        FMonolithActionHandler::CreateStatic(&FMonolithFooActions::HandleMyAction),
        FParamSchemaBuilder()           // param schema
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset to load"))
            .Build()
    );
}
```

### 4. Update the skill

If your domain has a skill in `Skills/`, add the new action to its action table.

---

## How to Add a New Indexer

MonolithIndex uses a plugin-style indexer system. Each indexer implements `IMonolithIndexer`.

### 1. Create the indexer class

```cpp
class FMyIndexer : public IMonolithIndexer
{
public:
    virtual TArray<FString> GetSupportedClasses() const override
    {
        return { TEXT("MyAssetClass") };
    }

    virtual bool IndexAsset(
        const FAssetData& AssetData,
        UObject* LoadedAsset,
        FMonolithIndexDatabase& DB,
        int64 AssetId) override
    {
        // Extract data and write to DB using prepared statements
        FIndexedNode Node;
        Node.AssetId = AssetId;
        Node.NodeType = TEXT("MyNodeType");
        Node.NodeName = LoadedAsset->GetName();
        DB.InsertNode(Node);

        return true;
    }

    virtual FString GetName() const override { return TEXT("MyIndexer"); }
};
```

### 2. Register in the subsystem

Add your indexer to `UMonolithIndexSubsystem::RegisterDefaultIndexers()`:

```cpp
RegisterIndexer(MakeShared<FMyIndexer>());
```

### 3. Add DB tables if needed

If your indexer needs new tables, execute the schema during indexing using `DB.GetRawDatabase()` (or add it to `GCreateTablesSQL` in `MonolithIndexDatabase.cpp`). Follow the existing pattern with `CREATE TABLE IF NOT EXISTS`.

If the new rows should be discoverable via full-text search, you must also define a corresponding `CREATE VIRTUAL TABLE IF NOT EXISTS fts_your_table USING fts5(...)` and add `AFTER INSERT`, `AFTER DELETE`, and `AFTER UPDATE` synchronization triggers in `GCreateTablesSQL`.

---

## Coding Conventions

### General

- **UE coding standard** — `F` prefix for structs, `U` for UObjects, `T` for templates, `b` prefix for bools
- **Static action handlers** — All action classes use static methods, no instance state
- **Game thread execution** — Handlers execute on the game thread via `AsyncTask(ENamedThreads::GameThread, ...)`

### Logging

Use the `LogMonolith` category for all log output:

```cpp
UE_LOG(LogMonolith, Log, TEXT("Something happened: %s"), *Value);
UE_LOG(LogMonolith, Warning, TEXT("Something unexpected: %s"), *Value);
UE_LOG(LogMonolith, Error, TEXT("Something failed: %s"), *Error);
```

Do **not** use `LogTemp`.

### Database Access

All SQL must use prepared statements to prevent injection:

```cpp
FSQLitePreparedStatement Stmt;
Stmt.Create(*Database, TEXT("INSERT INTO nodes (asset_id, name) VALUES (?, ?)"));
Stmt.SetBindingValueByIndex(1, AssetId);
Stmt.SetBindingValueByIndex(2, NodeName);
Stmt.Execute();
```

Never use string formatting to build SQL queries.

### Error Handling

Return errors with a clear message, and optionally attach structured `ErrorData`:

```cpp
FMonolithActionResult Result = FMonolithActionResult::Error(TEXT("Asset not found"));
Result.ErrorData = MakeShared<FJsonObject>();
Result.ErrorData->SetStringField(TEXT("asset_path"), AssetPath);
return Result;
```

### Asset Loading

Use the 4-tier fallback in `FMonolithAssetUtils`:

```cpp
UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(AssetPath);
```

This handles: StaticLoadObject -> PackageName.ObjectName -> FindObject+_C suffix -> ForEachObjectWithPackage.

---

## Testing

Monolith exposes a Streamable HTTP MCP server. You can test with curl or any MCP client.

### curl Examples

**Discover available tools:**
```bash
curl -X POST http://localhost:9316/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
```

**Call an action:**
```bash
curl -X POST http://localhost:9316/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"blueprint_query","arguments":{"action":"list_graphs","asset_path":"/Game/MyBlueprint.MyBlueprint"}}}'
```

**Check server status:**
```bash
curl -X POST http://localhost:9316/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"monolith_status","arguments":{}}}'
```

### MCP Client

Configure your `.mcp.json` (see `Templates/.mcp.json.example`):

```json
{
  "mcpServers": {
    "monolith": {
      "type": "streamableHttp",
      "url": "http://localhost:9316/mcp"
    }
  }
}
```

Then use Claude Code or any MCP-compatible client to interact with the tools.

### What to Verify

- Your action appears in `monolith_discover` output
- Valid params return correct results
- Missing/invalid params return clear error JSON (not crashes)
- Asset paths with various formats work (the 4-tier fallback)

---

## Pull Request Process

1. **Branch from `master`** — Use descriptive branch names: `feature/niagara-scalability`, `fix/material-connection-crash`

2. **Test in-editor** — Build and run in your UE project. Verify with curl or an MCP client that your changes work

4. **Update docs** — If you add actions, update:
   - The relevant skill in `Skills/`
   - `Docs/specs/SPEC_<Module>.md` action tables (per-module spec for the namespace you touched)
   - `CHANGELOG.md` under `## [Unreleased]`
   - `README.md` only if the approximate total crosses a rounded threshold. Public counts stay rounded down with a `+` (for example `~1,400+`); do not introduce exact integers.

5. **Commit messages** — Use conventional format: `feat:`, `fix:`, `docs:`, `refactor:`

6. **One concern per PR** — Don't mix unrelated changes

---

## Release Workflow

When preparing a new release of Monolith:

1. **Update versions:** Update the version number in `Monolith.uplugin`, `Source/MonolithCore/Public/MonolithCoreModule.h`, `CHANGELOG.md`, `README.md`, `Docs/API_REFERENCE.md`, and `Docs/SPEC_CORE.md`.
2. **Action counts:** Verify every count-bearing release file against `monolith_discover()` / the action-count synchronization rule: `README.md`, `Docs/API_REFERENCE.md`, `Docs/SPEC_CORE.md`, and `Monolith.uplugin`.
3. **Commit release metadata:** Commit and push the version/count/doc updates before packaging so the release script sees the intended clean tree.
   ```bash
   git status --short
   git add Monolith.uplugin Source/MonolithCore/Public/MonolithCoreModule.h CHANGELOG.md README.md Docs/API_REFERENCE.md Docs/SPEC_CORE.md
   git commit -m "Release vX.Y.Z metadata"
   git push origin master
   ```
4. **Clean working tree:** Ensure `git status --porcelain` is empty. The release script refuses to run with uncommitted changes (the `-AllowDirtyTree` switch cannot be used for the standard multi-engine release).
5. **Build release ZIP:** Run the release script from PowerShell:
   ```powershell
   powershell -ExecutionPolicy Bypass -File Scripts/make_release.ps1 -Version "X.Y.Z"
   ```
6. **Publish:** Create a GitHub Release with the new tag as a draft, verify the release notes, and then publish it.
   ```bash
   gh release create vX.Y.Z "../../Monolith-vX.Y.Z.zip" "../../Monolith-vX.Y.Z-UE5.7.zip" "../../Monolith-vX.Y.Z-UE5.8.zip" --title "Monolith vX.Y.Z" --notes-file release_notes.md --draft
   powershell -ExecutionPolicy Bypass -File Scripts/verify_release_body.ps1 -Version "X.Y.Z"
   gh release edit vX.Y.Z --draft=false
   ```
   **Crucial:** You must copy the exact SHA256 marker lines printed by the release script into the release notes body before publishing. Per-engine assets use `Monolith-SHA256-v2-UE5.7: <hash>` / `Monolith-SHA256-v2-UE5.8: <hash>`, and the script also prints legacy `Monolith-SHA256-v2: <hash>` for compatibility. If the matching platform/engine marker is missing, the auto-updater aborts the installation (the "warn and proceed" fallback only applies to legacy assets).

## Architecture Notes

- **Discovery/dispatch pattern** — Each domain exposes one `{namespace}_query(action, params)` MCP tool. The registry dispatches to the correct handler. This keeps AI context lean (a couple of dozen dispatch tools instead of one endpoint per action).
- **Thread safety** — `FMonolithToolRegistry` releases its lock before executing handlers. DB access uses `FCriticalSection`.
- **Stateless server** — No session tracking. Every request is independent.
- **MCP protocol version** — 2025-03-26, Streamable HTTP transport.

---

## License

By contributing, you agree that your contributions will be licensed under the [MIT License](LICENSE).
