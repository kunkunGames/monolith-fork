#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class AActor;
class UWorld;

class FMonolithEditorLevelMetadataActions
{
public:
	static void RegisterActions();

	static FMonolithActionResult HandlePreviewMetadataExport(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleExportMetadata(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetLevelMetadata(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleValidateMetadataExport(const TSharedPtr<FJsonObject>& Params);

private:
	struct FExportOptions
	{
		bool bIncludeFoliage = true;
		bool bIncludeObjects = true;
		int32 MaxActors = 10000;
		FString OutputDir;
	};

	static FExportOptions ParseOptions(const TSharedPtr<FJsonObject>& Params);
	static UWorld* GetEditorWorld();
	static TSharedPtr<FJsonObject> BuildLevelMetadata(const FExportOptions& Options, bool bIncludeObjectRows);
	static TSharedPtr<FJsonObject> ActorToMetadataJson(AActor* Actor);
	static bool IsFoliageActor(const AActor* Actor);
	static bool ResolveOutputDir(const FString& RequestedDir, FString& OutDir, FString& OutError);
	static bool JsonObjectToString(const TSharedPtr<FJsonObject>& Object, FString& OutJson);
	static FString MakeSafeBaseName(const FString& LevelName);
};
