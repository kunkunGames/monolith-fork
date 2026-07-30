#pragma once

#include "CoreMinimal.h"

namespace MonolithLocalizationTargetConfig
{
	enum class EGatherPathRoot : uint8
	{
		Engine,
		Project
	};

	struct FParsedSearchDirectory
	{
		EGatherPathRoot Root = EGatherPathRoot::Project;
		FString RelativePath;
		FString Canonical;
	};

	struct FGatherConfigPatch
	{
		FString DesiredContents;
		TArray<FString> ExistingSearchDirectories;
		bool bChanged = false;
	};

	bool ParseSearchDirectory(
		FString Input,
		FParsedSearchDirectory& OutDirectory,
		FString& OutError);

	bool BuildGatherConfigPatch(
		const FString& ExistingContents,
		const FString& TargetName,
		const TArray<FString>& DesiredSearchDirectories,
		FGatherConfigPatch& OutPatch,
		FString& OutError);
}
