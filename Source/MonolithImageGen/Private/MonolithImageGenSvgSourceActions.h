// Copyright tumourlove. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

namespace MonolithImageGen::SvgSource
{
	FString GetDefaultVectorAssetPath();
	FString GetDefaultVectorSourceDir();
	TArray<FString> GetSupportedProfiles();
	TArray<FString> GetGeometryPolicies();
	TArray<FString> GetFillRulePolicies();
	void AddSvgDefaults(TSharedPtr<FJsonObject> Result);
	void AddSvgModelEntries(TArray<TSharedPtr<FJsonValue>>& Models);

	FMonolithActionResult HandleGenerateSvg(const TSharedPtr<FJsonObject>& Params);
	FMonolithActionResult HandleImportGeneratedSvg(const TSharedPtr<FJsonObject>& Params);
	FMonolithActionResult HandleValidateSvg(const TSharedPtr<FJsonObject>& Params);
	FMonolithActionResult HandleGenerateMsdfFromSvg(const TSharedPtr<FJsonObject>& Params);
}
