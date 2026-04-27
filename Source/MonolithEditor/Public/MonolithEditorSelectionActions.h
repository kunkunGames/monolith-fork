#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"
#include "AssetRegistry/AssetData.h"

class AActor;
class UActorComponent;
class UClass;
class UWorld;

class FMonolithEditorSelectionActions
{
public:
	static void RegisterActions();

	static FMonolithActionResult HandleGetSelectedActors(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetSelectedAssets(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetActiveAssetEditor(const TSharedPtr<FJsonObject>& Params);

	static bool MatchesClassFilter(const UClass* Class, const FString& Filter);
	static bool MatchesClassFilter(const FTopLevelAssetPath& ClassPath, const FString& Filter);

private:
	static TSharedPtr<FJsonObject> ActorToJson(AActor* Actor, bool bIncludeComponents);
	static TSharedPtr<FJsonObject> ComponentToJson(UActorComponent* Component);
	static TSharedPtr<FJsonObject> AssetDataToJson(const FAssetData& AssetData, const FString& VirtualPath = FString());
	static TSharedPtr<FJsonObject> ObjectToAssetEditorJson(UObject* Asset, const FString& Source, const FString& EditorName);
	static TSharedPtr<FJsonObject> OpenEditorSummaryToJson(class IAssetEditorInstance* Editor, const TArray<UObject*>& EditedAssets);

	static FString GetClassPath(const UClass* Class);
	static FString GetObjectPath(const UObject* Object);
	static FString GetWorldTypeString(const UWorld* World);
	static FString GetAssetVirtualPath(const FAssetData& AssetData);
};
