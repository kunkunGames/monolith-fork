#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithToolProfileActions
{
public:
	static void RegisterAll();

private:
	static FMonolithActionResult HandleListToolProfiles(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetToolProfile(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCreateToolProfile(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleUpdateToolProfile(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDeleteToolProfile(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetActiveToolProfile(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetActionEnabled(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetNamespaceEnabled(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetActionDescriptionOverride(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetEffectiveDiscovery(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleValidateToolProfile(const TSharedPtr<FJsonObject>& Params);
};
