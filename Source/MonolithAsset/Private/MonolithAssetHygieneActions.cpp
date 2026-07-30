#include "MonolithAssetHygieneActions.h"

#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/SoftObjectPath.h"

void FMonolithAssetHygieneActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	RegisterValidateNamingConventions(Registry);
	RegisterBatchRenameAssets(Registry);
}

void FMonolithAssetHygieneActions::RegisterValidateNamingConventions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("asset"), TEXT("validate_naming_conventions"),
		TEXT("Scan assets by path and flag names not matching prefix conventions (SM_ for StaticMesh, SK_ for SkeletalMesh, M_ for Material, MI_ for MaterialInstance, T_ for Texture, BP_ for Blueprint). Supports custom rules."),
		FMonolithActionHandler::CreateStatic(&FMonolithAssetHygieneActions::ValidateNamingConventions),
		FParamSchemaBuilder()
			.Optional(TEXT("scan_path"), TEXT("string"), TEXT("Content path to scan (e.g. /Game/Environment)"), TEXT("/Game"))
			.Optional(TEXT("max_results"), TEXT("integer"), TEXT("Maximum violations to return"), TEXT("100"))
			.Optional(TEXT("custom_rules"), TEXT("object"), TEXT("Custom prefix rules: {\"ClassName\": \"Prefix_\", ...}"))
			.StrictComplexTypes()
			.Build());
}

void FMonolithAssetHygieneActions::RegisterBatchRenameAssets(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("asset"), TEXT("batch_rename_assets"),
		TEXT("Rename assets with find/replace, prefix add/remove, or suffix add/remove. Uses IAssetTools::RenameAssets for automatic reference fixup and redirector creation."),
		FMonolithActionHandler::CreateStatic(&FMonolithAssetHygieneActions::BatchRenameAssets),
		FParamSchemaBuilder()
			.Required(TEXT("asset_paths"), TEXT("array"), TEXT("Array of asset paths to rename"))
			.Optional(TEXT("find"), TEXT("string"), TEXT("String to find in asset name"))
			.Optional(TEXT("replace"), TEXT("string"), TEXT("Replacement string"))
			.Optional(TEXT("add_prefix"), TEXT("string"), TEXT("Prefix to add"))
			.Optional(TEXT("remove_prefix"), TEXT("string"), TEXT("Prefix to remove"))
			.Optional(TEXT("add_suffix"), TEXT("string"), TEXT("Suffix to add"))
			.Optional(TEXT("remove_suffix"), TEXT("string"), TEXT("Suffix to remove"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview renames without applying"), TEXT("false"))
			.StrictComplexTypes()
			.Build());
}

FMonolithActionResult FMonolithAssetHygieneActions::ValidateNamingConventions(const TSharedPtr<FJsonObject>& Params)
{
	FString ScanPath = TEXT("/Game");
	Params->TryGetStringField(TEXT("scan_path"), ScanPath);

	double MaxResultsD = 100.0;
	Params->TryGetNumberField(TEXT("max_results"), MaxResultsD);
	const int32 MaxResults = FMath::Clamp(static_cast<int32>(MaxResultsD), 1, 1000);

	TMap<FString, FString> PrefixRules;
	PrefixRules.Add(TEXT("StaticMesh"), TEXT("SM_"));
	PrefixRules.Add(TEXT("SkeletalMesh"), TEXT("SK_"));
	PrefixRules.Add(TEXT("Material"), TEXT("M_"));
	PrefixRules.Add(TEXT("MaterialInstanceConstant"), TEXT("MI_"));
	PrefixRules.Add(TEXT("Texture2D"), TEXT("T_"));
	PrefixRules.Add(TEXT("Blueprint"), TEXT("BP_"));
	PrefixRules.Add(TEXT("AnimSequence"), TEXT("AS_"));
	PrefixRules.Add(TEXT("AnimMontage"), TEXT("AM_"));
	PrefixRules.Add(TEXT("AnimBlueprint"), TEXT("ABP_"));
	PrefixRules.Add(TEXT("NiagaraSystem"), TEXT("NS_"));
	PrefixRules.Add(TEXT("NiagaraEmitter"), TEXT("NE_"));
	PrefixRules.Add(TEXT("SoundCue"), TEXT("SC_"));
	PrefixRules.Add(TEXT("SoundWave"), TEXT("SW_"));
	PrefixRules.Add(TEXT("ParticleSystem"), TEXT("PS_"));
	PrefixRules.Add(TEXT("WidgetBlueprint"), TEXT("WBP_"));

	const TSharedPtr<FJsonObject>* CustomRulesObj;
	if (Params->TryGetObjectField(TEXT("custom_rules"), CustomRulesObj) && CustomRulesObj->IsValid())
	{
		for (const auto& Pair : (*CustomRulesObj)->Values)
		{
			FString Prefix;
			if (Pair.Value->TryGetString(Prefix))
			{
				PrefixRules.Add(MonolithKeyToString(Pair.Key), Prefix);
			}
		}
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*ScanPath));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> AllAssets;
	AssetRegistry.GetAssets(Filter, AllAssets);

	TArray<TSharedPtr<FJsonValue>> Violations;
	int32 TotalScanned = 0;
	int32 TotalPassed = 0;

	int32 TotalViolations = 0;
	for (const FAssetData& Asset : AllAssets)
	{
		const FString ClassName = Asset.AssetClassPath.GetAssetName().ToString();
		const FString* ExpectedPrefix = PrefixRules.Find(ClassName);
		if (!ExpectedPrefix)
		{
			continue;
		}

		TotalScanned++;
		const FString AssetName = Asset.AssetName.ToString();

		if (AssetName.StartsWith(*ExpectedPrefix))
		{
			TotalPassed++;
			continue;
		}

		TSharedPtr<FJsonObject> Violation = MakeShared<FJsonObject>();
		Violation->SetStringField(TEXT("asset_path"), Asset.GetObjectPathString());
		Violation->SetStringField(TEXT("asset_name"), AssetName);
		Violation->SetStringField(TEXT("asset_class"), ClassName);
		Violation->SetStringField(TEXT("expected_prefix"), *ExpectedPrefix);
		Violation->SetStringField(TEXT("suggested_name"), *ExpectedPrefix + AssetName);
		// Keep scanning past the cap so total_assets_scanned, passed, and the
		// violation total describe the whole requested content path. Breaking
		// early made those aggregates describe only a prefix.
		++TotalViolations;
		if (Violations.Num() < MaxResults)
		{
			Violations.Add(MakeShared<FJsonValueObject>(Violation));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("scan_path"), ScanPath);
	Result->SetNumberField(TEXT("total_assets_scanned"), TotalScanned);
	Result->SetNumberField(TEXT("passed"), TotalPassed);
	Result->SetNumberField(TEXT("violations"), TotalViolations);
	Result->SetNumberField(TEXT("violations_returned"), Violations.Num());
	// Only a genuinely dropped violation is truncation; a count that merely
	// equals the cap is complete.
	Result->SetBoolField(TEXT("truncated"), TotalViolations > Violations.Num());
	Result->SetArrayField(TEXT("violations_list"), Violations);

	TArray<TSharedPtr<FJsonValue>> RulesArr;
	for (const auto& Rule : PrefixRules)
	{
		TSharedPtr<FJsonObject> RuleObj = MakeShared<FJsonObject>();
		RuleObj->SetStringField(TEXT("class"), Rule.Key);
		RuleObj->SetStringField(TEXT("prefix"), Rule.Value);
		RulesArr.Add(MakeShared<FJsonValueObject>(RuleObj));
	}
	Result->SetArrayField(TEXT("rules_applied"), RulesArr);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAssetHygieneActions::BatchRenameAssets(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* PathsArr;
	if (!Params->TryGetArrayField(TEXT("asset_paths"), PathsArr) || PathsArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: asset_paths"));
	}

	if (PathsArr->Num() > 200)
	{
		return FMonolithActionResult::Error(TEXT("Too many assets to rename (max 200)"));
	}

	FString FindStr;
	FString ReplaceStr;
	FString AddPrefix;
	FString RemovePrefix;
	FString AddSuffix;
	FString RemoveSuffix;
	Params->TryGetStringField(TEXT("find"), FindStr);
	Params->TryGetStringField(TEXT("replace"), ReplaceStr);
	Params->TryGetStringField(TEXT("add_prefix"), AddPrefix);
	Params->TryGetStringField(TEXT("remove_prefix"), RemovePrefix);
	Params->TryGetStringField(TEXT("add_suffix"), AddSuffix);
	Params->TryGetStringField(TEXT("remove_suffix"), RemoveSuffix);

	bool bDryRun = false;
	if (Params->HasField(TEXT("dry_run"))
		&& (!Params->HasTypedField<EJson::Boolean>(TEXT("dry_run"))
			|| !Params->TryGetBoolField(TEXT("dry_run"), bDryRun)))
	{
		return FMonolithActionResult::Error(
			TEXT("Invalid parameter 'dry_run': must be a boolean."),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	if (FindStr.IsEmpty() && AddPrefix.IsEmpty() && RemovePrefix.IsEmpty() && AddSuffix.IsEmpty() && RemoveSuffix.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Must specify at least one rename operation (find/replace, add_prefix, remove_prefix, add_suffix, remove_suffix)"));
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();

	TArray<FAssetRenameData> RenameData;
	TArray<TSharedPtr<FJsonValue>> PreviewArr;

	for (const TSharedPtr<FJsonValue>& PathVal : *PathsArr)
	{
		FString AssetPath;
		if (!PathVal->TryGetString(AssetPath) || AssetPath.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("Each entry in asset_paths must be a non-empty string"));
		}

		if (!AssetPath.Contains(TEXT(".")))
		{
			const FString BaseName = FPaths::GetBaseFilename(AssetPath);
			AssetPath = AssetPath + TEXT(".") + BaseName;
		}

		const FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
		if (!AssetData.IsValid())
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
		}

		const FString OldName = AssetData.AssetName.ToString();
		FString NewName = OldName;

		if (!RemovePrefix.IsEmpty() && NewName.StartsWith(RemovePrefix))
		{
			NewName.RightChopInline(RemovePrefix.Len());
		}
		if (!RemoveSuffix.IsEmpty() && NewName.EndsWith(RemoveSuffix))
		{
			NewName.LeftChopInline(RemoveSuffix.Len());
		}
		if (!FindStr.IsEmpty())
		{
			NewName.ReplaceInline(*FindStr, *ReplaceStr);
		}
		if (!AddPrefix.IsEmpty())
		{
			NewName = AddPrefix + NewName;
		}
		if (!AddSuffix.IsEmpty())
		{
			NewName = NewName + AddSuffix;
		}

		if (NewName == OldName)
		{
			continue;
		}

		if (NewName.IsEmpty())
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Rename would result in empty name for %s"), *AssetPath));
		}

		const FString PackagePath = FPackageName::GetLongPackagePath(AssetData.PackageName.ToString());

		FAssetRenameData Data;
		Data.Asset = AssetData.GetAsset();
		Data.NewName = NewName;
		Data.NewPackagePath = PackagePath;

		if (Data.Asset.IsValid())
		{
			RenameData.Add(Data);
		}

		TSharedPtr<FJsonObject> PreviewObj = MakeShared<FJsonObject>();
		PreviewObj->SetStringField(TEXT("old_path"), AssetPath);
		PreviewObj->SetStringField(TEXT("old_name"), OldName);
		PreviewObj->SetStringField(TEXT("new_name"), NewName);
		PreviewObj->SetStringField(TEXT("new_path"), PackagePath / NewName);
		PreviewArr.Add(MakeShared<FJsonValueObject>(PreviewObj));
	}

	if (RenameData.Num() == 0)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), TEXT("no_changes"));
		Result->SetNumberField(TEXT("renamed_count"), 0);
		return FMonolithActionResult::Success(Result);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	if (bDryRun)
	{
		Result->SetStringField(TEXT("status"), TEXT("dry_run"));
		Result->SetNumberField(TEXT("would_rename_count"), RenameData.Num());
		Result->SetArrayField(TEXT("renames"), PreviewArr);
		return FMonolithActionResult::Success(Result);
	}

	const bool bSuccess = AssetTools.RenameAssets(RenameData);

	Result->SetStringField(TEXT("status"), bSuccess ? TEXT("success") : TEXT("partial_failure"));
	Result->SetNumberField(TEXT("renamed_count"), RenameData.Num());
	Result->SetArrayField(TEXT("renames"), PreviewArr);
	Result->SetStringField(TEXT("note"), TEXT("Redirectors created for reference fixup. Run 'Fix Up Redirectors' to clean up."));

	if (!bSuccess)
	{
		return FMonolithActionResult::Error(TEXT("AssetTools.RenameAssets failed for one or more assets"))
			.WithErrorData(Result);
	}

	return FMonolithActionResult::Success(Result);
}
