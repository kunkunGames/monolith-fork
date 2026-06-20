#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "MonolithSettings.generated.h"

UENUM()
enum class EMonolithLogVerbosity : uint8
{
	Quiet,
	Normal,
	Verbose,
	VeryVerbose
};

UCLASS(config=Monolith, defaultconfig, meta=(DisplayName="Monolith"))
class MONOLITHCORE_API UMonolithSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UMonolithSettings();

	// --- MCP Server ---

	/** Master enable for the Monolith MCP HTTP server. Set false to disable
	 *  the localhost listener entirely (e.g. when working on an untrusted
	 *  network and you don't need AI tooling for the session). UE's
	 *  FHttpServerModule has no bind-address parameter, so the listener is
	 *  reachable on all network interfaces — this flag is the user-facing
	 *  kill-switch. Takes effect on next editor restart. (Issue #38) */
	UPROPERTY(config, EditAnywhere, Category="MCP Server")
	bool bMcpServerEnabled = true;

	/** Port for the embedded MCP HTTP server */
	UPROPERTY(config, EditAnywhere, Category="MCP Server", meta=(ClampMin="1024", ClampMax="65535"))
	int32 ServerPort = 9316;

	/** Allow browser pages from loopback origins to read MCP responses.
	 *  Non-browser MCP clients do not rely on CORS and continue to work when
	 *  this is disabled. */
	UPROPERTY(config, EditAnywhere, Category="MCP Server|Compatibility",
		meta=(DisplayName="Enable Browser Loopback CORS",
			  ToolTip="Echo Access-Control-Allow-Origin only for localhost/127.0.0.1/[::1] browser origins. Disable to block browser reads while keeping non-browser MCP clients working."))
	bool bEnableBrowserLoopbackCors = true;

	/** Enable domain catalog actions that let clients list, describe, and mark
	 *  active namespaces without exposing additional namespace tools by default. */
	UPROPERTY(config, EditAnywhere, Category="MCP Server|Discovery",
		meta=(DisplayName="Enable Deferred Domain Catalog",
			  ToolTip="Registers monolith.list_domains, describe_domain, load_domain, and get_loaded_domains. Default off to preserve existing MCP tool-list behavior."))
	bool bEnableDeferredDomainCatalog = false;

	/** Legacy compatibility opt-in for clients that need loaded domains to appear
	 *  as namespace query tools. Keep disabled for single-dispatcher clients. */
	UPROPERTY(config, EditAnywhere, Category="MCP Server|Discovery",
		meta=(DisplayName="Expose Loaded Domains As MCP Tools",
			  EditCondition="bEnableDeferredDomainCatalog",
			  ToolTip="Reserved compatibility flag. The first domain catalog slice records this status but does not change tools/list."))
	bool bExposeLoadedDomainsAsMcpTools = false;

	/** Enables read-only MCP resources/list and resources/read dispatch once the
	 *  resource registry implementation is present. Default off for compatibility. */
	UPROPERTY(config, EditAnywhere, Category="MCP Server|Resources",
		meta=(DisplayName="Enable MCP Resources",
			  ToolTip="Reserved feature flag for MCP resources/list and resources/read support. Default off until resource providers are implemented."))
	bool bEnableMcpResources = false;

	/** Adds MCP structuredContent while replacing successful text JSON with a
	 *  compact status line. Default off for existing clients. */
	UPROPERTY(config, EditAnywhere, Category="MCP Server|Results",
		meta=(DisplayName="Enable Structured Tool Results",
			  ToolTip="Emit successful tool JSON in structuredContent instead of duplicating it in text content."))
	bool bEnableStructuredToolResults = false;

	/** Enables persistent MCP session/request state, progress, and cancellation
	 *  once the execution-context implementation is present. */
	UPROPERTY(config, EditAnywhere, Category="MCP Server|Sessions",
		meta=(DisplayName="Enable MCP Session Mode",
			  ToolTip="Reserved feature flag for persistent MCP sessions, progress, and cancellation. Default off."))
	bool bEnableMcpSessionMode = false;

	/** Extends the current action audit into redacted ToolCall records and
	 *  analysis actions once advanced recording is implemented. */
	UPROPERTY(config, EditAnywhere, Category="MCP Server|Audit",
		meta=(DisplayName="Enable Advanced ToolCall Records",
			  ToolTip="Reserved feature flag for redacted ToolCall records and analysis. Raw payload logging remains disabled."))
	bool bEnableAdvancedToolCallRecords = false;

	/** Enables local append-only JSONL action invocation logs under
	 *  Plugins/Monolith/Logs/yyyyMMdd/action.jsonl. Default off; proxy/query daily logs are
	 *  controlled by MONOLITH_TOOL_LOG_ENABLED outside the editor. */
	UPROPERTY(config, EditAnywhere, Category="MCP Server|Audit",
		meta=(DisplayName="Enable Daily Action Log",
			  ToolTip="Append bounded/redacted editor action call and return records to Plugins/Monolith/Logs/yyyyMMdd/action.jsonl. Default off."))
	bool bEnableDailyLog = false;

	/** Enables the optional ToolsetRegistry bridge (UE 5.8 Experimental ToolsetRegistry).
	 *  Inert unless the build is compiled with MONOLITH_WITH_TOOLSET_REGISTRY_BRIDGE=1 and
	 *  the ToolsetRegistry plugin is present; never a hard dependency of public builds. */
	UPROPERTY(config, EditAnywhere, Category="MCP Server|Bridge",
		meta=(DisplayName="Enable ToolsetRegistry Bridge",
			  ToolTip="Reserved opt-in for the optional UE 5.8 ToolsetRegistry bridge module. Inert unless the build sets MONOLITH_WITH_TOOLSET_REGISTRY_BRIDGE=1 and the ToolsetRegistry plugin is present. Default off."))
	bool bEnableToolsetRegistryBridge = false;

	/** Enables read-only GameFeatures inspection actions beyond get_status. */
	UPROPERTY(config, EditAnywhere, Category="Project|GameFeatures",
		meta=(DisplayName="Enable GameFeature Inspection Actions",
			  ToolTip="Registers read-only gamefeatures inspection actions beyond get_status. Default off."))
	bool bEnableGameFeatureActions = false;

	/** Reserved guard for a future plugin creation slice. No creation action is
	 *  registered in the read-only first slice. */
	UPROPERTY(config, EditAnywhere, Category="Project|GameFeatures",
		meta=(DisplayName="Allow GameFeature Plugin Creation",
			  EditCondition="bEnableGameFeatureActions",
			  ToolTip="Reserved for future manifest-based GameFeature plugin creation. Current Monolith builds only report this gate."))
	bool bAllowGameFeaturePluginCreation = false;

	// --- Auto-Update ---

	/** Check GitHub Releases for updates on editor startup. Off by default
	 *  (Issue #38) — auto-fetching prebuilt zips from GitHub without
	 *  integrity verification or explicit user opt-in is supply-chain risk
	 *  for a freshly-installed plugin. Users who want auto-update enable
	 *  it explicitly here. */
	UPROPERTY(config, EditAnywhere, Category="Auto-Update")
	bool bAutoUpdateEnabled = false;

	// --- Onboarding ---

	/** Versioned schema for Monolith onboarding state. Increment only when steps change meaning. */
	UPROPERTY(config, EditAnywhere, Category="Onboarding")
	int32 OnboardingSchemaVersion = 1;

	/** Completed local onboarding steps such as server_ready, index_ready, and optional_modules_reviewed. */
	UPROPERTY(config, EditAnywhere, Category="Onboarding")
	TArray<FString> OnboardingCompletedSteps;

	/** User-skipped onboarding steps. Skipped steps remain visible in readiness reports. */
	UPROPERTY(config, EditAnywhere, Category="Onboarding")
	TArray<FString> OnboardingSkippedSteps;

	// --- Notifications ---

	/** Show lightweight editor toast notifications for Monolith server/action events. */
	UPROPERTY(config, EditAnywhere, Category="Notifications")
	bool bNotifyEditorToasts = true;

	/** Reserve setting for notification sounds. Sound playback is opt-in and not triggered by readiness checks. */
	UPROPERTY(config, EditAnywhere, Category="Notifications")
	bool bNotifySounds = false;

	/** Reserve setting for OS taskbar attention on important Monolith events. */
	UPROPERTY(config, EditAnywhere, Category="Notifications")
	bool bNotifyTaskbarAttention = false;

	UPROPERTY(config, EditAnywhere, Category="Notifications")
	bool bNotifyServerErrors = true;

	UPROPERTY(config, EditAnywhere, Category="Notifications")
	bool bNotifyActionErrors = true;

	UPROPERTY(config, EditAnywhere, Category="Notifications")
	bool bNotifyLongRunningActionComplete = true;

	UPROPERTY(config, EditAnywhere, Category="Notifications")
	bool bNotifyIndexingComplete = true;

	UPROPERTY(config, EditAnywhere, Category="Notifications")
	bool bNotifyUpdateAvailable = true;

	// --- Indexing ---

	/** Content paths to index in addition to /Game. Add plugin mount points like /MyPlugin. */
	UPROPERTY(config, EditAnywhere, Category="Indexing")
	TArray<FString> AdditionalContentPaths;

	/** Override path for ProjectIndex.db (empty = default Saved/ location) */
	UPROPERTY(config, EditAnywhere, Category="Indexing", meta=(RelativePath))
	FDirectoryPath DatabasePathOverride;

	/** Override path for engine source DB (empty = default Saved/ location) */
	UPROPERTY(config, EditAnywhere, Category="Indexing", meta=(RelativePath))
	FDirectoryPath EngineSourceDBPathOverride;

	/** Path to UE Engine/Source directory (empty = auto-detect) */
	UPROPERTY(config, EditAnywhere, Category="Indexing", meta=(RelativePath))
	FDirectoryPath EngineSourcePath;

	// --- Indexer Toggles ---

	/** Enable Blueprint deep indexing (graphs, nodes, variables) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Deep Indexers")
	bool bIndexBlueprints = true;

	/** Enable Material deep indexing (expressions, parameters) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Deep Indexers")
	bool bIndexMaterials = true;

	/** Enable generic asset metadata indexing (mesh stats, texture info, audio) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Deep Indexers")
	bool bIndexGenericAssets = true;

	/** Enable Niagara system indexing (emitters, renderers, sim targets) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Deep Indexers")
	bool bIndexNiagara = true;

	/** Enable UserDefinedEnum indexing (enum entries, values) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Deep Indexers")
	bool bIndexUserDefinedEnums = true;

	/** Enable UserDefinedStruct indexing (fields, types, defaults) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Deep Indexers")
	bool bIndexUserDefinedStructs = true;

	/** Enable InputAction indexing (value types, triggers, modifiers) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Deep Indexers")
	bool bIndexInputActions = true;

	/** Enable DataAsset property indexing (serializes all UPROPERTY defaults to JSON) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Deep Indexers")
	bool bIndexDataAssets = true;

	/** Enable Gameplay Ability System indexing (abilities, effects, attribute sets, cues) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Deep Indexers")
	bool bIndexGAS = true;

	/** Walk MetaSound graphs and index nodes/connections/parameters into the project index. Disable to skip MetaSound deep indexing if memory is tight. */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Deep Indexers", meta=(DisplayName="Index MetaSounds",
		ToolTip="Walk MetaSound graphs and index nodes/connections/parameters into the project index. Disable to skip MetaSound deep indexing if memory is tight."))
	bool bIndexMetaSounds = true;

	/** Enable AI asset indexing (behavior trees, blackboards, state trees, EQS, smart objects) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Deep Indexers")
	bool bIndexAI = true;

	/** Enable Level Sequence Director Blueprint indexing (director functions, variables, event-track bindings) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Deep Indexers")
	bool bIndexLevelSequences = true;

	/** Enable dependency graph indexing */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Post-Pass Indexers")
	bool bIndexDependencies = true;

	/** Enable level/world actor indexing */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Post-Pass Indexers")
	bool bIndexLevels = true;

	/** Enable DataTable row indexing */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Post-Pass Indexers")
	bool bIndexDataTables = true;

	/** Enable curated supplemental values for ProjectIndex FTS search (comments, pin defaults, DataTable text fields). */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Search Values")
	bool bIndexSearchableValues = true;

	/** Maximum supplemental search values stored per asset. */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Search Values", meta=(ClampMin="0", ClampMax="2000"))
	int32 MaxSearchableValuesPerAsset = 256;

	/** Maximum supplemental search values stored per indexed object such as one Blueprint node or DataTable row. */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Search Values", meta=(ClampMin="0", ClampMax="128"))
	int32 MaxSearchableValuesPerObject = 16;

	/** Maximum characters stored per supplemental search value. Longer values are truncated. */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Search Values", meta=(ClampMin="32", ClampMax="4096"))
	int32 MaxSearchableValueChars = 512;

	/** Maximum nested struct/container depth for supplemental value extraction. */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Search Values", meta=(ClampMin="0", ClampMax="6"))
	int32 MaxSearchableObjectDepth = 2;

	/** Maximum array/set/map entries inspected per supplemental searchable container. */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Search Values", meta=(ClampMin="0", ClampMax="256"))
	int32 MaxSearchableContainerItems = 32;

	/** Enable config/INI indexing */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Post-Pass Indexers")
	bool bIndexConfigs = true;

	/** Enable C++ symbol indexing (UCLASS, USTRUCT, etc.) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Post-Pass Indexers")
	bool bIndexCppSymbols = true;

	/** Enable animation asset indexing (sequences, montages, blend spaces) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Post-Pass Indexers")
	bool bIndexAnimations = true;

	/** Enable gameplay tag indexing */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Post-Pass Indexers")
	bool bIndexGameplayTags = true;

	/** Enable mesh catalog indexing (bounds, size class, category for all StaticMesh assets) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Post-Pass Indexers")
	bool bIndexMeshCatalog = true;

	/** Index content from enabled marketplace plugins (installed via Fab/Epic launcher) */
	UPROPERTY(config, EditAnywhere, Category="Indexing")
	bool bIndexMarketplacePlugins = true;

	// --- Indexing Performance ---

	/** Memory budget for indexing in megabytes. 0 = auto-detect from installed RAM. Indexing will pause and run GC when this limit is exceeded. */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Performance", DisplayName="Memory Budget (MB)",
		meta=(ClampMin="0", ClampMax="65536", ToolTip="0 = auto-detect tier based on installed RAM (64+ GB -> 32 GB, 32+ GB -> 16 GB, 16 GB -> 6 GB, <16 GB -> 3 GB). Set explicitly to override."))
	int32 MemoryBudgetMB = 0;

	/** Number of assets to process per batch during deep indexing. 0 = auto-detect from installed RAM. Lower values reduce memory spikes but increase indexing time. */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Performance", DisplayName="Deep Index Batch Size",
		meta=(ClampMin="0", ClampMax="64", ToolTip="0 = auto-detect tier (32+ GB -> 8, 16 GB -> 4, <16 GB -> 2). Set explicitly to override. Lower = less memory, slower indexing."))
	int32 DeepIndexBatchSize = 0;

	/** Number of assets to process per batch for post-pass indexers (levels, meshes). 0 = auto-detect from installed RAM. These are memory-heavy so use smaller batches. */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Performance", DisplayName="Post-Pass Batch Size",
		meta=(ClampMin="0", ClampMax="32", ToolTip="0 = auto-detect tier (32+ GB -> 4, 16 GB -> 2, <16 GB -> 1). Set explicitly to override. Lower for large assets."))
	int32 PostPassBatchSize = 0;

	/** Run garbage collection every N batches during indexing. Lower values keep memory lower but slow down indexing. */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Performance", DisplayName="GC Frequency (Batches)",
		meta=(ClampMin="1", ClampMax="20", ToolTip="Run GC every N batches. 1 = every batch, higher = less frequent."))
	int32 GCFrequencyBatches = 2;

	/** Time to yield between batches when memory pressure is detected (seconds). Allows editor to remain responsive. */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Performance", DisplayName="Yield Time (seconds)",
		meta=(ClampMin="0.0", ClampMax="1.0", ToolTip="Sleep time when throttling due to memory pressure."))
	float YieldTimeSeconds = 0.1f;

	/** Defer first-time indexing until explicitly triggered via console command. Useful for very large projects. */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Performance", DisplayName="Defer First-Time Index",
		meta=(ToolTip="If true, first-time indexing won't run automatically. Use 'Monolith.StartIndex' console command to trigger."))
	bool bDeferFirstTimeIndex = false;

	/** Log memory statistics periodically during indexing. Off by default to keep shipped-project logs quiet — opt in when debugging indexer behavior. */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Performance", DisplayName="Log Memory Stats",
		meta=(ToolTip="Log memory usage during indexing for debugging. Default off — enable when investigating memory pressure."))
	bool bLogMemoryStats = false;

	// --- Module Toggles ---

	UPROPERTY(config, EditAnywhere, Category="Modules")
	bool bEnableBlueprint = true;

	UPROPERTY(config, EditAnywhere, Category="Modules")
	bool bEnableMaterial = true;

	UPROPERTY(config, EditAnywhere, Category="Modules")
	bool bEnableAnimation = true;

	UPROPERTY(config, EditAnywhere, Category="Modules")
	bool bEnableNiagara = true;

	UPROPERTY(config, EditAnywhere, Category="Modules")
	bool bEnableEditor = true;

	UPROPERTY(config, EditAnywhere, Category="Modules")
	bool bEnableConfig = true;

	UPROPERTY(config, EditAnywhere, Category="Modules")
	bool bEnableIndex = true;

	UPROPERTY(config, EditAnywhere, Category="Modules")
	bool bEnableSource = true;

	UPROPERTY(config, EditAnywhere, Category="Modules")
	bool bEnableUI = true;

	UPROPERTY(config, EditAnywhere, Category="Modules", DisplayName="Enable Asset Module")
	bool bEnableAsset = true;

	UPROPERTY(config, EditAnywhere, Category="Modules", DisplayName="Enable ImageGen Module")
	bool bEnableImageGen = true;

	UPROPERTY(config, EditAnywhere, Category="Modules", DisplayName="Enable Sprite Module")
	bool bEnableSprite = true;

	UPROPERTY(config, EditAnywhere, Category="Modules|ImageGen", DisplayName="ImageGen Bridge Server URL",
		meta=(ToolTip="Base URL for an external imag2-gen/ima2 server. Monolith sends generation requests here without provider credentials."))
	FString ImageGenBridgeServerUrl = TEXT("http://192.168.1.147:3333");

	UPROPERTY(config, EditAnywhere, Category="Modules|ImageGen", DisplayName="ImageGen Bridge Provider",
		meta=(ToolTip="imag2-gen provider to request: oauth, api, or auto. The default OAuth path uses the imag2-gen host's Codex OAuth session."))
	FString ImageGenBridgeProvider = TEXT("oauth");

	UPROPERTY(config, EditAnywhere, Category="Modules|ImageGen", DisplayName="ImageGen Bridge Default Model",
		meta=(ToolTip="Default image model forwarded to imag2-gen when a request omits model."))
	FString ImageGenBridgeDefaultModel = TEXT("gpt-5.5");

	UPROPERTY(config, EditAnywhere, Category="Modules|ImageGen", DisplayName="ImageGen Bridge Timeout Seconds",
		meta=(ClampMin="1.0", ToolTip="HTTP timeout for imag2-gen generation requests. Long OAuth image generations can take several minutes."))
	float ImageGenBridgeTimeoutSeconds = 420.0f;

	/** Enables detailed read-only Slate inspector actions beyond get_inspector_status. */
	UPROPERTY(config, EditAnywhere, Category="Modules|Optional",
		meta=(DisplayName="Enable Slate Inspector Actions",
			  ToolTip="Registers read-only slate inspection actions beyond get_inspector_status. Default off; restart required."))
	bool bEnableSlateInspectorActions = false;

	UPROPERTY(config, EditAnywhere, Category="Modules", DisplayName="Enable Mesh Module")
	bool bEnableMesh = true;

	// --- Optional Module Toggles ---

	UPROPERTY(config, EditAnywhere, Category="Modules|Optional",
		meta=(DisplayName="Enable Blueprint Assist Integration",
			  ToolTip="When enabled and Blueprint Assist is installed, provides enhanced graph formatting via the IMonolithGraphFormatter bridge."))
	bool bEnableBlueprintAssist = true;

	UPROPERTY(config, EditAnywhere, Category="Modules|Optional",
		meta=(DisplayName="Enable GAS Integration",
			  ToolTip="When enabled, registers gas_query actions for Gameplay Ability System manipulation. Requires GameplayAbilities plugin (engine-bundled)."))
	bool bEnableGAS = true;

	UPROPERTY(config, EditAnywhere, Category="Modules|Optional",
		meta=(DisplayName="Enable ComboGraph Integration",
			  ToolTip="When enabled and ComboGraph is installed, registers combograph_query actions for combo graph manipulation."))
	bool bEnableComboGraph = true;

	UPROPERTY(config, EditAnywhere, Category="Modules|Optional",
		meta=(DisplayName="Enable Logic Driver Integration",
			  ToolTip="When enabled and Logic Driver Pro is installed, registers logicdriver_query actions for state machine manipulation."))
	bool bEnableLogicDriver = true;

	UPROPERTY(config, EditAnywhere, Category="Modules|Optional",
		meta=(DisplayName="Enable AI Module",
			  ToolTip="Registers ai_query actions for AI asset manipulation (BT, BB, ST, EQS, SO, Navigation, Perception)."))
	bool bEnableAI = true;

	UPROPERTY(config, EditAnywhere, Category="Modules|Optional",
		meta=(DisplayName="Enable External Inventory Module",
			  ToolTip="Allows an external sibling plugin to register inventory_query actions."))
	bool bEnableExternalInventoryModule = true;

	UPROPERTY(config, EditAnywhere, Category="Modules|Optional",
		meta=(DisplayName="Enable Audio Module",
			  ToolTip="Registers audio_query actions for audio asset creation, inspection, batch management, Sound Cue graph building, and MetaSound graph building."))
	bool bEnableAudio = true;

	UPROPERTY(config, EditAnywhere, Category="Modules|Optional",
		meta=(DisplayName="Enable Level Sequence Module",
			  ToolTip="Registers level_sequence_query actions for Level Sequence Director Blueprint introspection (director functions, variables, event-track bindings to director functions, cross-sequence reverse lookup of function callers)."))
	bool bEnableLevelSequence = true;

	UPROPERTY(config, EditAnywhere, Category="Modules|Optional",
		meta=(DisplayName="Enable WorldConditions Inspection",
			  ToolTip="Enables read-only world_conditions_query inspection of WorldCondition query definitions. Default off because WorldConditions data can be nested inside optional gameplay systems."))
	bool bEnableWorldConditionsInspection = false;

	// --- Modules|Mesh ---

	UPROPERTY(config, EditAnywhere, Category="Modules|Mesh",
		DisplayName="Enable Procedural Town Generation (Experimental)",
		Meta=(EditCondition="bEnableMesh",
			  ToolTip="Registers town gen actions (city blocks, buildings, facades, roofs, floor plans, furnishing, terrain, spatial registry, debug views). EXPERIMENTAL — known geometry issues. Disable to hide these actions from MCP."))
	bool bEnableProceduralTownGen = false;

	UPROPERTY(config, EditAnywhere, Category="Modules|Mesh", DisplayName="Handle Pool Timeout (seconds)",
		Meta=(ClampMin="10.0", ClampMax="3600.0", EditCondition="bEnableMesh"))
	float MeshHandleTimeoutSeconds = 300.0f;

	UPROPERTY(config, EditAnywhere, Category="Modules|Mesh", DisplayName="Max Active Handles",
		Meta=(ClampMin="1", ClampMax="256", EditCondition="bEnableMesh"))
	int32 MaxActiveHandles = 32;

	UPROPERTY(config, EditAnywhere, Category="Modules|Mesh", DisplayName="Default Size Match Tolerance %",
		Meta=(ClampMin="1.0", ClampMax="100.0", EditCondition="bEnableMesh"))
	float DefaultSizeMatchTolerance = 20.0f;

	UPROPERTY(config, EditAnywhere, Category="Modules|Mesh", DisplayName="Surface Acoustics DataTable Path",
		Meta=(EditCondition="bEnableMesh"))
	FString SurfaceAcousticsTablePath = TEXT("/Game/Data/DT_SurfaceAcoustics");

	// --- Tool Visibility ---

	/** When true, tools/list emits per-namespace {ns}_query dispatcher tools in addition
	 *  to the core monolith_* tools. Default false: expose only core routing tools;
	 *  agents use monolith_query({namespace, action, params}) as the single dispatcher
	 *  and consult skills or monolith_discover for the available action list. */
	UPROPERTY(config, EditAnywhere, Category="Tool Visibility",
		meta=(DisplayName="Expose Namespace Tools",
			  ToolTip="Expose per-namespace _query dispatcher tools in tools/list. Default false: agents use monolith_query as the single dispatcher."))
	bool bExposeNamespaceTools = false;

	/** When false, tools/list advertises ONLY the core routing/discovery monolith_* tools
	 *  (find, discover, status) and suppresses every other monolith action — both the
	 *  management/control-plane tools (tool profiles, execution guard, onboarding,
	 *  readiness, notifications, MCP session, action audit) and the lower-traffic
	 *  routing extras (guide, update, reindex). Combined with the monolith_query
	 *  dispatcher (advertised when bExposeNamespaceTools is false), this yields a minimal
	 *  4-tool surface: monolith_find, monolith_discover, monolith_status, monolith_query.
	 *  The suppressed actions stay registered and executable — reachable by name via
	 *  tools/call or monolith_query({namespace:"monolith", action:"..."}); only their
	 *  tools/list advertisement is hidden. Default true preserves the full control-plane
	 *  surface. Takes effect on next editor restart. */
	UPROPERTY(config, EditAnywhere, Category="Tool Visibility",
		meta=(DisplayName="Expose Management Tools",
			  ToolTip="When false, only core routing monolith_* tools are advertised in tools/list; management/control-plane tools are hidden (still callable by name / via monolith_query). Default true."))
	bool bExposeManagementTools = true;

	// --- Logging ---

	/** Log verbosity for Monolith systems */
	UPROPERTY(config, EditAnywhere, Category="Logging")
	EMonolithLogVerbosity LogVerbosity = EMonolithLogVerbosity::Normal;

	// --- Helpers ---

	static const UMonolithSettings* Get();

	/** Returns /Game plus all AdditionalContentPaths as FName array for FARFilter usage */
	static TArray<FName> GetIndexedContentPaths();

	/** Returns true if the given package path starts with any indexed content path */
	static bool IsIndexedContentPath(const FString& PackagePath);

	/** Settings category path */
	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }
};
