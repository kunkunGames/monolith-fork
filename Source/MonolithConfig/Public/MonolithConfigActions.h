#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Config/INI domain action handlers for Monolith.
 * 6 actions using GConfig/FConfigCacheIni for config hierarchy resolution.
 */
class FMonolithConfigActions
{
public:
	/** Register all config actions with the tool registry */
	static void RegisterActions(FMonolithToolRegistry& Registry);

	// --- Action handlers ---
	static FMonolithActionResult ResolveSetting(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ExplainSetting(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DiffFromDefault(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SearchConfig(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetSection(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetConfigFiles(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListPlugins(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetPlugin(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetCVar(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult FindCVars(const TSharedPtr<FJsonObject>& Params);

private:
	/** Map shortname (e.g. "DefaultEngine") to full file path */
	static FString ResolveConfigFilePath(const FString& ShortName);

	/** Get the hierarchy of config files for a given category (e.g. "Engine", "Game") */
	static TArray<TPair<FString, FString>> GetConfigHierarchy(const FString& Category);
};
