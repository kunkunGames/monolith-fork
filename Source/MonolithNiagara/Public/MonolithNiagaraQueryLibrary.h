#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MonolithNiagaraQueryLibrary.generated.h"

/**
 * Blueprint-callable facades over the read-only Niagara inspection/search dispatcher actions
 * (issue tumourlove/monolith#64). Each node is a thin, named wrapper around a single
 * `niagara` namespace action in FMonolithToolRegistry: it builds a JSON params object from
 * typed inputs, executes the action, and returns the result re-serialized to a JSON FString.
 *
 * JSON-in / JSON-out by design — the user accepted a single FString JSON return rather than
 * typed output pins. Every node additionally exposes `bSuccess` + `OutError` out-pins so a BP
 * author can branch without parsing JSON.
 *
 * EDITOR GAME-THREAD ONLY. MonolithNiagara is an Editor-type module and is never cooked into a
 * packaged build, so these nodes add zero shipping-game cost. The underlying dispatcher actions
 * inspect assets at editor time and are not thread-safe; these nodes are BlueprintCallable
 * (NOT BlueprintThreadSafe) so the engine will not schedule them off-thread. Call only from
 * Editor Utility Widgets / BP utility graphs.
 *
 * Tranche 1 (this file's initial set): the ~17 pure-wrap nodes whose backing action already
 * exists, is non-stub, and is correctly mapped. The search/discovery gap-action nodes (#11
 * GetNiagaraDataInterfaces, SearchNiagaraByParameter/ByMaterial/ByDataInterface,
 * QueryNiagaraSystems, FindNiagaraReferences, FindSimilarNiagaraSystems) land in Tranche 2 once
 * their new dispatcher actions are registered.
 */
UCLASS(MinimalAPI)
class UMonolithNiagaraQueryLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// ---------------------------------------------------------------------------------------
	// Inspection (Monolith|Niagara|Inspection)
	// ---------------------------------------------------------------------------------------

	/** System-level summary (emitter count, sim targets, bounds, etc.). Wraps niagara.get_system_summary. */
	UFUNCTION(BlueprintCallable,
		Category = "Monolith|Niagara|Inspection",
		meta = (AdvancedDisplay = "DetailLevel",
		        Keywords = "niagara system info summary inspect"))
	static MONOLITHNIAGARA_API FString GetNiagaraSystemInfo(
		const FString& AssetPath,
		const FString& DetailLevel,
		UPARAM(DisplayName = "Success") bool& bSuccess,
		FString& OutError);

	/** List the emitters in a system. Wraps niagara.list_emitters. */
	UFUNCTION(BlueprintCallable,
		Category = "Monolith|Niagara|Inspection",
		meta = (Keywords = "niagara emitters list"))
	static MONOLITHNIAGARA_API FString GetNiagaraEmitters(
		const FString& AssetPath,
		UPARAM(DisplayName = "Success") bool& bSuccess,
		FString& OutError);

	/** Per-emitter summary. Wraps niagara.get_emitter_summary. */
	UFUNCTION(BlueprintCallable,
		Category = "Monolith|Niagara|Inspection",
		meta = (AdvancedDisplay = "DetailLevel",
		        Keywords = "niagara emitter summary inspect"))
	static MONOLITHNIAGARA_API FString GetNiagaraEmitterSummary(
		const FString& AssetPath,
		const FString& Emitter,
		const FString& DetailLevel,
		UPARAM(DisplayName = "Success") bool& bSuccess,
		FString& OutError);

	/** Ordered module stack for an emitter usage. Wraps niagara.get_ordered_modules. */
	UFUNCTION(BlueprintCallable,
		Category = "Monolith|Niagara|Inspection",
		meta = (AdvancedDisplay = "Usage",
		        Keywords = "niagara modules ordered stack"))
	static MONOLITHNIAGARA_API FString GetNiagaraModules(
		const FString& AssetPath,
		const FString& Emitter,
		const FString& Usage,
		UPARAM(DisplayName = "Success") bool& bSuccess,
		FString& OutError);

	/** Inputs of a specific module node. Wraps niagara.get_module_inputs. */
	UFUNCTION(BlueprintCallable,
		Category = "Monolith|Niagara|Inspection",
		meta = (Keywords = "niagara module inputs"))
	static MONOLITHNIAGARA_API FString GetNiagaraModuleInputs(
		const FString& AssetPath,
		const FString& Emitter,
		const FString& ModuleNode,
		UPARAM(DisplayName = "Success") bool& bSuccess,
		FString& OutError);

	/** Node graph of a module script. Wraps niagara.get_module_graph. */
	UFUNCTION(BlueprintCallable,
		Category = "Monolith|Niagara|Inspection",
		meta = (Keywords = "niagara module graph script"))
	static MONOLITHNIAGARA_API FString GetNiagaraModuleGraph(
		const FString& ScriptPath,
		UPARAM(DisplayName = "Success") bool& bSuccess,
		FString& OutError);

	/** All parameters in a system (optionally scoped to an emitter). Wraps niagara.get_all_parameters. */
	UFUNCTION(BlueprintCallable,
		Category = "Monolith|Niagara|Inspection",
		meta = (AdvancedDisplay = "Emitter,Scope",
		        Keywords = "niagara parameters all"))
	static MONOLITHNIAGARA_API FString GetNiagaraParameters(
		const FString& AssetPath,
		const FString& Emitter,
		const FString& Scope,
		UPARAM(DisplayName = "Success") bool& bSuccess,
		FString& OutError);

	/** Exposed user parameters of a system. Wraps niagara.get_user_parameters. */
	UFUNCTION(BlueprintCallable,
		Category = "Monolith|Niagara|Inspection",
		meta = (Keywords = "niagara user parameters exposed"))
	static MONOLITHNIAGARA_API FString GetNiagaraUserParameters(
		const FString& AssetPath,
		UPARAM(DisplayName = "Success") bool& bSuccess,
		FString& OutError);

	/**
	 * Renderers of an emitter. Wraps niagara.list_renderers. If bIncludeBindings is true, fires a
	 * follow-up niagara.get_renderer_bindings for RendererIndex instead (single renderer's bindings).
	 */
	UFUNCTION(BlueprintCallable,
		Category = "Monolith|Niagara|Inspection",
		meta = (AdvancedDisplay = "bIncludeBindings,RendererIndex",
		        Keywords = "niagara renderers bindings"))
	static MONOLITHNIAGARA_API FString GetNiagaraRenderers(
		const FString& AssetPath,
		const FString& Emitter,
		bool bIncludeBindings,
		int32 RendererIndex,
		UPARAM(DisplayName = "Success") bool& bSuccess,
		FString& OutError);

	/** Event handlers of an emitter. Wraps niagara.get_event_handlers. */
	UFUNCTION(BlueprintCallable,
		Category = "Monolith|Niagara|Inspection",
		meta = (Keywords = "niagara events handlers"))
	static MONOLITHNIAGARA_API FString GetNiagaraEvents(
		const FString& AssetPath,
		const FString& Emitter,
		UPARAM(DisplayName = "Success") bool& bSuccess,
		FString& OutError);

	/** Functions exposed by a Data Interface class (CDO reflection). Wraps niagara.get_di_functions. */
	UFUNCTION(BlueprintCallable,
		Category = "Monolith|Niagara|Inspection",
		meta = (Keywords = "niagara data interface functions di"))
	static MONOLITHNIAGARA_API FString GetNiagaraDIFunctions(
		const FString& DIClass,
		UPARAM(DisplayName = "Success") bool& bSuccess,
		FString& OutError);

	/** Simulation stages of an emitter. Wraps niagara.get_simulation_stages. */
	UFUNCTION(BlueprintCallable,
		Category = "Monolith|Niagara|Inspection",
		meta = (Keywords = "niagara simulation stages sim"))
	static MONOLITHNIAGARA_API FString GetNiagaraSimulationStages(
		const FString& AssetPath,
		const FString& Emitter,
		UPARAM(DisplayName = "Success") bool& bSuccess,
		FString& OutError);

	/** System diagnostics / stats. Wraps niagara.get_system_diagnostics. */
	UFUNCTION(BlueprintCallable,
		Category = "Monolith|Niagara|Inspection",
		meta = (AdvancedDisplay = "bCompileFirst",
		        Keywords = "niagara stats diagnostics cost"))
	static MONOLITHNIAGARA_API FString GetNiagaraStats(
		const FString& AssetPath,
		bool bCompileFirst,
		UPARAM(DisplayName = "Success") bool& bSuccess,
		FString& OutError);

	/** List Niagara systems in the project. Wraps niagara.list_systems. */
	UFUNCTION(BlueprintCallable,
		Category = "Monolith|Niagara|Inspection",
		meta = (AdvancedDisplay = "Search,Path,Limit",
		        Keywords = "niagara inventory list systems"))
	static MONOLITHNIAGARA_API FString GetNiagaraInventory(
		const FString& Search,
		const FString& Path,
		int32 Limit,
		UPARAM(DisplayName = "Success") bool& bSuccess,
		FString& OutError);

	/** Compiled GPU HLSL output for an emitter. Wraps niagara.get_compiled_gpu_hlsl. */
	UFUNCTION(BlueprintCallable,
		Category = "Monolith|Niagara|Inspection",
		meta = (Keywords = "niagara hlsl gpu compiled output"))
	static MONOLITHNIAGARA_API FString GetNiagaraHLSLOutput(
		const FString& AssetPath,
		const FString& Emitter,
		UPARAM(DisplayName = "Success") bool& bSuccess,
		FString& OutError);

	// ---------------------------------------------------------------------------------------
	// Search & Discovery (Monolith|Niagara|Search)
	// ---------------------------------------------------------------------------------------

	/** Search Niagara systems by name/path filter. Wraps niagara.list_systems. */
	UFUNCTION(BlueprintCallable,
		Category = "Monolith|Niagara|Search",
		meta = (AdvancedDisplay = "Path,Limit",
		        Keywords = "niagara search systems find"))
	static MONOLITHNIAGARA_API FString SearchNiagaraSystems(
		const FString& Search,
		const FString& Path,
		int32 Limit,
		UPARAM(DisplayName = "Success") bool& bSuccess,
		FString& OutError);

	/** Search Niagara module scripts. Wraps niagara.list_module_scripts. */
	UFUNCTION(BlueprintCallable,
		Category = "Monolith|Niagara|Search",
		meta = (AdvancedDisplay = "Usage,Limit,bIncludeMetadata",
		        Keywords = "niagara search modules scripts"))
	static MONOLITHNIAGARA_API FString SearchNiagaraModules(
		const FString& Search,
		const FString& Usage,
		int32 Limit,
		bool bIncludeMetadata,
		UPARAM(DisplayName = "Success") bool& bSuccess,
		FString& OutError);
};
