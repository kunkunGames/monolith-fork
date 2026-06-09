#include "MonolithGASInternal.h"
#include "MonolithAssetUtils.h"
#include "MonolithPackagePathValidator.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Abilities/GameplayAbility.h"
#include "AbilitySystemInterface.h"
#include "AttributeSet.h"
#include "GameplayEffect.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "UObject/Package.h"
#include "AssetRegistry/AssetRegistryModule.h"

namespace MonolithGAS
{

UBlueprint* LoadBlueprintFromParams(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, FString& OutError)
{
	if (!Params->TryGetStringField(TEXT("asset_path"), OutAssetPath) || OutAssetPath.IsEmpty())
	{
		OutError = TEXT("Missing required parameter: asset_path");
		return nullptr;
	}

	// Try loading as Blueprint first.
	// `_C`-suffix paths are skipped because they refer to a generated class, not the
	// UBlueprint asset — those go through LoadClass elsewhere. The non-`_C` branch
	// uses the canonical 4-tier resolver (registry-first, class-mismatch-terminal).
	FString FullPath = OutAssetPath;
	if (!FullPath.EndsWith(TEXT("_C")))
	{
		if (UBlueprint* BP = Cast<UBlueprint>(FMonolithAssetUtils::LoadAssetByPath(UBlueprint::StaticClass(), FullPath)))
		{
			return BP;
		}
	}

	OutError = FString::Printf(TEXT("Blueprint not found: %s"), *OutAssetPath);
	return nullptr;
}

UObject* LoadAssetFromPath(const FString& AssetPath, FString& OutError)
{
	UObject* Obj = FMonolithAssetUtils::LoadAssetByPath(UObject::StaticClass(), AssetPath);
	if (!Obj)
	{
		OutError = FString::Printf(TEXT("Asset not found: %s"), *AssetPath);
	}
	return Obj;
}

UPackage* GetOrCreatePackage(const FString& SavePath, FString& OutError)
{
	FString PackageName = SavePath;
	if (PackageName.StartsWith(TEXT("/Game/")))
	{
		// Already in game content format
	}
	else if (!PackageName.StartsWith(TEXT("/")))
	{
		PackageName = TEXT("/Game/") + PackageName;
	}

	// Defensive: reject malformed paths (e.g. "//Game/...") before CreatePackage asserts and kills the editor.
	if (const FString ValidationError = MonolithCore::ValidatePackagePath(PackageName); !ValidationError.IsEmpty())
	{
		UE_LOG(LogMonolithGAS, Warning, TEXT("GetOrCreatePackage rejected path: %s"), *ValidationError);
		OutError = ValidationError;
		return nullptr;
	}

	if (FindPackage(nullptr, *PackageName))
	{
		OutError = FString::Printf(TEXT("Package already exists in memory: %s"), *PackageName);
		return nullptr;
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("Failed to create package: %s"), *PackageName);
		return nullptr;
	}
	return Package;
}

FString TagToString(const FGameplayTag& Tag)
{
	return Tag.IsValid() ? Tag.ToString() : TEXT("");
}

FGameplayTag StringToTag(const FString& TagString)
{
	if (TagString.IsEmpty())
	{
		return FGameplayTag();
	}
	return FGameplayTag::RequestGameplayTag(FName(*TagString), false);
}

FGameplayTagContainer ParseTagContainer(const TSharedPtr<FJsonObject>& Params, const FString& FieldName,
	TArray<FString>& OutSkipped)
{
	FGameplayTagContainer Container;
	const TArray<TSharedPtr<FJsonValue>>* TagArray;
	if (Params->TryGetArrayField(FieldName, TagArray))
	{
		for (const auto& Val : *TagArray)
		{
			FString TagStr;
			if (Val->TryGetString(TagStr) && !TagStr.IsEmpty())
			{
				FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagStr), false);
				if (Tag.IsValid())
				{
					Container.AddTag(Tag);
				}
				else
				{
					// F.7b — surface unregistered tag strings to caller via OutSkipped (was: silent drop).
					UE_LOG(LogMonolithGAS, Warning, TEXT("Gameplay tag not found: '%s' (field=%s)"), *TagStr, *FieldName);
					OutSkipped.Add(TagStr);
				}
			}
		}
	}
	return Container;
}

FGameplayTagContainer ParseTagContainer(const TSharedPtr<FJsonObject>& Params, const FString& FieldName)
{
	// Backwards-compat one-arg overload — delegates to the skipped-aware version and discards OutSkipped.
	TArray<FString> Discarded;
	return ParseTagContainer(Params, FieldName, Discarded);
}

TSharedPtr<FJsonValue> TagContainerToJson(const FGameplayTagContainer& Container)
{
	TArray<TSharedPtr<FJsonValue>> TagArray;
	for (const FGameplayTag& Tag : Container)
	{
		TagArray.Add(MakeShared<FJsonValueString>(Tag.ToString()));
	}
	return MakeShared<FJsonValueArray>(TagArray);
}

bool IsAbilityBlueprint(UBlueprint* BP)
{
	if (!BP || !BP->GeneratedClass)
	{
		return false;
	}
	return BP->GeneratedClass->IsChildOf(UGameplayAbility::StaticClass());
}

bool IsAttributeSetBlueprint(UBlueprint* BP)
{
	if (!BP || !BP->GeneratedClass)
	{
		return false;
	}
	return BP->GeneratedClass->IsChildOf(UAttributeSet::StaticClass());
}

bool IsGameplayEffectBlueprint(UBlueprint* BP)
{
	if (!BP || !BP->GeneratedClass)
	{
		return false;
	}
	return BP->GeneratedClass->IsChildOf(UGameplayEffect::StaticClass());
}

TSharedPtr<FJsonObject> MakeAssetResult(const FString& AssetPath, const FString& Message)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	if (!Message.IsEmpty())
	{
		Result->SetStringField(TEXT("message"), Message);
	}
	return Result;
}

TArray<FString> ParseStringArray(const TSharedPtr<FJsonObject>& Params, const FString& FieldName)
{
	TArray<FString> Result;
	const TArray<TSharedPtr<FJsonValue>>* Arr;
	if (Params->TryGetArrayField(FieldName, Arr))
	{
		for (const auto& Val : *Arr)
		{
			FString Str;
			if (Val->TryGetString(Str))
			{
				Result.Add(Str);
			}
		}
	}
	return Result;
}

bool RequireStringParam(const TSharedPtr<FJsonObject>& Params, const FString& ParamName, FString& OutValue, FMonolithActionResult& OutError)
{
	OutValue.Reset();
	if (!Params.IsValid() || !Params->HasField(ParamName))
	{
		OutError = FMonolithActionResult::Error(
			FString::Printf(TEXT("Missing required parameter: %s"), *ParamName));
		return false;
	}

	TSharedPtr<FJsonValue> Value = Params->TryGetField(ParamName);
	if (!Value.IsValid() || Value->Type == EJson::Null)
	{
		OutError = FMonolithActionResult::Error(
			FString::Printf(TEXT("Missing required parameter: %s"), *ParamName));
		return false;
	}

	if (Value->Type != EJson::String || !Value->TryGetString(OutValue))
	{
		OutError = FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid parameter: %s must be a string"), *ParamName));
		return false;
	}

	OutValue.TrimStartAndEndInline();
	if (OutValue.IsEmpty())
	{
		OutError = FMonolithActionResult::Error(
			FString::Printf(TEXT("Missing required parameter: %s"), *ParamName));
		return false;
	}
	return true;
}

static FString GetParamDisplayName(const FString& ParamName, const FString& DisplayName)
{
	return DisplayName.IsEmpty() ? ParamName : DisplayName;
}

bool TryReadOptionalStringParam(const TSharedPtr<FJsonObject>& Params, const FString& ParamName, FString& OutValue,
	FString& OutError, const FString& DisplayName, bool bAllowEmpty)
{
	if (!Params.IsValid() || !Params->HasField(ParamName))
	{
		return true;
	}

	TSharedPtr<FJsonValue> Value = Params->TryGetField(ParamName);
	if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(OutValue))
	{
		OutError = FString::Printf(TEXT("Invalid parameter: %s must be a string"), *GetParamDisplayName(ParamName, DisplayName));
		return false;
	}
	if (!bAllowEmpty)
	{
		OutValue.TrimStartAndEndInline();
		if (OutValue.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Invalid parameter: %s must be a non-empty string"), *GetParamDisplayName(ParamName, DisplayName));
			return false;
		}
	}
	return true;
}

bool TryReadOptionalNumberParam(const TSharedPtr<FJsonObject>& Params, const FString& ParamName, double& OutValue,
	FString& OutError, const FString& DisplayName)
{
	if (!Params.IsValid() || !Params->HasField(ParamName))
	{
		return true;
	}

	TSharedPtr<FJsonValue> Value = Params->TryGetField(ParamName);
	if (!Value.IsValid() || Value->Type != EJson::Number || !Value->TryGetNumber(OutValue))
	{
		OutError = FString::Printf(TEXT("Invalid parameter: %s must be a number"), *GetParamDisplayName(ParamName, DisplayName));
		return false;
	}
	return true;
}

bool TryReadOptionalBoolParam(const TSharedPtr<FJsonObject>& Params, const FString& ParamName, bool& OutValue,
	FString& OutError, const FString& DisplayName)
{
	if (!Params.IsValid() || !Params->HasField(ParamName))
	{
		return true;
	}

	TSharedPtr<FJsonValue> Value = Params->TryGetField(ParamName);
	if (!Value.IsValid() || Value->Type != EJson::Boolean || !Value->TryGetBool(OutValue))
	{
		OutError = FString::Printf(TEXT("Invalid parameter: %s must be a boolean"), *GetParamDisplayName(ParamName, DisplayName));
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Asset Existence Guard
// ---------------------------------------------------------------------------

bool EnsureAssetPathFree(const FString& PackagePath, const FString& AssetName, FString& OutError)
{
	FString FullObjectPath = PackagePath + TEXT(".") + AssetName;

	// Tier 1: Asset Registry (catches on-disk assets without loading them)
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData ExistingAsset = AR.GetAssetByObjectPath(FSoftObjectPath(FullObjectPath));
	if (ExistingAsset.IsValid())
	{
		OutError = FString::Printf(TEXT("Asset already exists at '%s'. Delete it first or use a different path."), *PackagePath);
		return false;
	}

	// Tier 2: FindObject global (catches in-memory assets not yet in AR)
	if (FindObject<UObject>(nullptr, *FullObjectPath))
	{
		OutError = FString::Printf(TEXT("Asset already exists in memory at '%s'. Delete it first or use a different path."), *PackagePath);
		return false;
	}

	// Tier 3: FindPackage + FindObject scoped (edge case: package loaded but object path didn't match)
	UPackage* ExistingPkg = FindPackage(nullptr, *PackagePath);
	if (ExistingPkg && FindObject<UObject>(ExistingPkg, *AssetName))
	{
		OutError = FString::Printf(TEXT("Asset already exists in package '%s'. Delete it first or use a different path."), *PackagePath);
		return false;
	}

	if (FindPackage(nullptr, *PackagePath))
	{
		OutError = FString::Printf(TEXT("Package '%s' already exists in memory. Delete it first."), *PackagePath);
		return false;
	}

	return true;
}

// ---------------------------------------------------------------------------
// PIE Runtime Helpers (A2)
// ---------------------------------------------------------------------------

UWorld* GetPIEWorld()
{
	if (!GEngine) return nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE && Context.World())
		{
			return Context.World();
		}
	}
	return nullptr;
}

AActor* FindActorInPIE(const FString& ActorIdentifier)
{
	UWorld* World = GetPIEWorld();
	if (!World) return nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->GetActorLabel() == ActorIdentifier ||
			It->GetName() == ActorIdentifier ||
			It->GetPathName() == ActorIdentifier)
		{
			return *It;
		}
	}
	return nullptr;
}

UAbilitySystemComponent* GetASCFromActor(AActor* Actor)
{
	if (!Actor) return nullptr;
	if (IAbilitySystemInterface* ASI = Cast<IAbilitySystemInterface>(Actor))
	{
		return ASI->GetAbilitySystemComponent();
	}
	return Actor->FindComponentByClass<UAbilitySystemComponent>();
}

// ---------------------------------------------------------------------------
// GE Load Helper (A3)
// ---------------------------------------------------------------------------

bool LoadGameplayEffectBP(const FString& Path, UBlueprint*& OutBP, UGameplayEffect*& OutGE, FString& OutError)
{
	OutBP = nullptr;
	OutGE = nullptr;

	UObject* Obj = LoadAssetFromPath(Path, OutError);
	OutBP = Cast<UBlueprint>(Obj);
	if (!OutBP)
	{
		OutError = FString::Printf(TEXT("Failed to load GameplayEffect Blueprint: %s — %s"), *Path, *OutError);
		return false;
	}
	if (!IsGameplayEffectBlueprint(OutBP))
	{
		OutError = FString::Printf(TEXT("'%s' is not a GameplayEffect Blueprint"), *Path);
		return false;
	}
	OutGE = GetBlueprintCDO<UGameplayEffect>(OutBP);
	if (!OutGE)
	{
		OutError = FString::Printf(TEXT("Failed to get CDO for GameplayEffect: %s"), *Path);
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Project Source Helper (A4)
// ---------------------------------------------------------------------------

static FString GetBuildCSModuleName(const FString& BuildCSPath)
{
	FString FileName = FPaths::GetCleanFilename(BuildCSPath);
	if (!FileName.RemoveFromEnd(TEXT(".Build.cs"), ESearchCase::IgnoreCase))
	{
		return FString();
	}
	return FileName;
}

static int32 ScoreProjectCodeModule(const FString& ModuleName)
{
	const FString ProjectName = FApp::GetProjectName();
	const bool bIsEditorModule = ModuleName.EndsWith(TEXT("Editor"), ESearchCase::IgnoreCase);

	int32 Score = 0;
	if (ModuleName.Equals(ProjectName, ESearchCase::IgnoreCase))
	{
		Score += 1000;
	}
	if (!bIsEditorModule)
	{
		Score += 500;
	}
	else
	{
		Score -= 200;
	}
	if (ModuleName.Contains(TEXT("Game"), ESearchCase::IgnoreCase))
	{
		Score += 50;
	}
	if (!ProjectName.IsEmpty() && ModuleName.Contains(ProjectName, ESearchCase::IgnoreCase))
	{
		Score += 20;
	}
	return Score;
}

bool ResolveProjectCodeModule(FProjectCodeModuleInfo& OutInfo, FString* OutError)
{
	OutInfo = FProjectCodeModuleInfo();

	const FString SourceRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Source"));
	if (!FPaths::DirectoryExists(SourceRoot))
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("Project Source directory not found: %s"), *SourceRoot);
		}
		return false;
	}

	TArray<FString> BuildCSFiles;
	IFileManager::Get().FindFilesRecursive(BuildCSFiles, *SourceRoot, TEXT("*.Build.cs"), true, false);
	if (BuildCSFiles.Num() == 0)
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("No project Build.cs files found under: %s"), *SourceRoot);
		}
		return false;
	}

	BuildCSFiles.Sort();

	int32 BestIndex = INDEX_NONE;
	int32 BestScore = -MAX_int32;
	for (int32 Index = 0; Index < BuildCSFiles.Num(); ++Index)
	{
		const FString ModuleName = GetBuildCSModuleName(BuildCSFiles[Index]);
		if (ModuleName.IsEmpty())
		{
			continue;
		}

		const int32 Score = ScoreProjectCodeModule(ModuleName);
		if (BestIndex == INDEX_NONE || Score > BestScore)
		{
			BestIndex = Index;
			BestScore = Score;
		}
	}

	if (BestIndex == INDEX_NONE)
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("No valid project module Build.cs files found under: %s"), *SourceRoot);
		}
		return false;
	}

	OutInfo.BuildCSPath = FPaths::ConvertRelativePathToFull(BuildCSFiles[BestIndex]);
	FPaths::NormalizeFilename(OutInfo.BuildCSPath);
	OutInfo.ModuleName = GetBuildCSModuleName(OutInfo.BuildCSPath);
	OutInfo.ModuleDir = FPaths::GetPath(OutInfo.BuildCSPath);
	FPaths::NormalizeFilename(OutInfo.ModuleDir);
	OutInfo.ApiMacro = OutInfo.ModuleName.ToUpper() + TEXT("_API");
	OutInfo.bIsRuntimeModule = !OutInfo.ModuleName.EndsWith(TEXT("Editor"), ESearchCase::IgnoreCase);
	return true;
}

FString GetProjectSourceDir()
{
	FProjectCodeModuleInfo ModuleInfo;
	if (ResolveProjectCodeModule(ModuleInfo))
	{
		return ModuleInfo.ModuleDir;
	}
	return FPaths::ProjectDir() / TEXT("Source") / FApp::GetProjectName();
}

} // namespace MonolithGAS
