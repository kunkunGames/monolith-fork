#include "MonolithAssetMoveActions.h"
#include "MonolithAssetMoveModalPolicy.h"
#include "MonolithAssetLifecycleActions.h"

#include "MonolithParamSchema.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "Misc/Paths.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	constexpr int32 InvalidParamsErrorCode = -32602;
	constexpr int32 MaxMoveCount = 512;
	constexpr int32 MaxCleanupCount = 200;

	struct FAssetMovePlan
	{
		FString SourcePackage;
		FString DestinationPackage;
		FString SourceObjectPath;
		FString DestinationObjectPath;
		FString SourceFilename;
		FString DestinationFilename;
		FAssetData SourceAssetData;
		FAssetData DestinationAssetData;
		TArray<FAssetData> SourceRedirectorAssets;
		bool bSourceAlreadyCleaned = false;
		TArray<FString> PreflightErrors;
		TSharedPtr<FJsonObject> Report;
	};

	static TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	static bool ReadBoolParam(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		bool& InOutValue,
		FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		if (!Params->HasTypedField<EJson::Boolean>(FieldName)
			|| !Params->TryGetBoolField(FieldName, InOutValue))
		{
			OutError = FString::Printf(TEXT("Param '%s' must be a boolean"), FieldName);
			return false;
		}
		return true;
	}

	static bool NormalizeStrictPackageName(
		const FString& Input,
		const TCHAR* FieldName,
		FString& OutPackageName,
		FString& OutError,
		const bool bIncludeReadOnlyRoots = false)
	{
		OutPackageName = Input;
		OutPackageName.TrimStartAndEndInline();
		if (OutPackageName.IsEmpty())
		{
			OutError = FString::Printf(TEXT("%s must be a non-empty long package name"), FieldName);
			return false;
		}
		if (!OutPackageName.Equals(Input, ESearchCase::CaseSensitive)
			|| OutPackageName.Contains(TEXT("\\"))
			|| OutPackageName.Contains(TEXT("."))
			|| OutPackageName.Contains(TEXT("//"))
			|| OutPackageName.EndsWith(TEXT("/"))
			|| !FPackageName::IsValidLongPackageName(OutPackageName, bIncludeReadOnlyRoots))
		{
			OutError = FString::Printf(
				TEXT("%s must be an exact writable long package name such as /Game/Folder/Asset: %s"),
				FieldName,
				*Input);
			return false;
		}
		return true;
	}

	static bool NormalizeStrictRoot(
		const FString& Input,
		const TCHAR* FieldName,
		FString& OutRoot,
		FString& OutError,
		const bool bIncludeReadOnlyRoots = false)
	{
		OutRoot = Input;
		OutRoot.TrimStartAndEndInline();
		while (OutRoot.EndsWith(TEXT("/")) && OutRoot.Len() > 1)
		{
			OutRoot.LeftChopInline(1);
		}
		if (OutRoot.IsEmpty()
			|| !OutRoot.StartsWith(TEXT("/"))
			|| OutRoot.Contains(TEXT("\\"))
			|| OutRoot.Contains(TEXT("."))
			|| OutRoot.Contains(TEXT("//"))
			|| !FPackageName::IsValidLongPackageName(OutRoot + TEXT("/__MonolithRootProbe"), bIncludeReadOnlyRoots))
		{
			OutError = FString::Printf(
				TEXT("%s entries must be writable long package roots such as /Game/Folder: %s"),
				FieldName,
				*Input);
			return false;
		}
		return true;
	}

	static bool NormalizeStrictObjectPath(
		const FString& Input,
		const TCHAR* FieldName,
		const FString& ExpectedPackageName,
		FString& OutObjectPath,
		FString& OutError,
		const bool bIncludeReadOnlyRoots)
	{
		OutObjectPath = Input;
		OutObjectPath.TrimStartAndEndInline();
		FText Reason;
		if (OutObjectPath.IsEmpty()
			|| !OutObjectPath.Equals(Input, ESearchCase::CaseSensitive)
			|| OutObjectPath.Contains(TEXT("\\"))
			|| OutObjectPath.Contains(TEXT(":"))
			|| !FPackageName::IsValidObjectPath(OutObjectPath, &Reason))
		{
			OutError = FString::Printf(
				TEXT("%s must be an exact top-level object path such as /Game/Folder/Asset.Asset: %s (%s)"),
				FieldName,
				*Input,
				*Reason.ToString());
			return false;
		}

		FString ObjectPackage;
		if (!NormalizeStrictPackageName(
				FPackageName::ObjectPathToPackageName(OutObjectPath),
				FieldName,
				ObjectPackage,
				OutError,
				bIncludeReadOnlyRoots)
			|| !ObjectPackage.Equals(ExpectedPackageName, ESearchCase::CaseSensitive))
		{
			if (OutError.IsEmpty())
			{
				OutError = FString::Printf(
					TEXT("%s package must exactly match %s: %s"),
					FieldName,
					*ExpectedPackageName,
					*Input);
			}
			return false;
		}
		return true;
	}

	static bool ReadAllowedRoots(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		TArray<FString>& OutRoots,
		FString& OutError,
		const bool bIncludeReadOnlyRoots = false)
	{
		OutRoots.Reset();
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			OutError = FString::Printf(TEXT("Missing required non-empty array param '%s'"), FieldName);
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(FieldName, Values) || !Values || Values->Num() == 0)
		{
			OutError = FString::Printf(TEXT("Param '%s' must be a non-empty array of package roots"), FieldName);
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Input;
			FString Root;
			if (!Value.IsValid() || !Value->TryGetString(Input)
				|| !NormalizeStrictRoot(Input, FieldName, Root, OutError, bIncludeReadOnlyRoots))
			{
				if (OutError.IsEmpty())
				{
					OutError = FString::Printf(TEXT("Param '%s' must contain only non-empty string package roots"), FieldName);
				}
				return false;
			}
			OutRoots.AddUnique(Root);
		}
		return true;
	}

	static bool IsUnderRoot(const FString& PackageName, const FString& Root)
	{
		return PackageName.Equals(Root, ESearchCase::IgnoreCase)
			|| PackageName.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase);
	}

	static bool IsUnderAnyRoot(const FString& PackageName, const TArray<FString>& Roots)
	{
		for (const FString& Root : Roots)
		{
			if (IsUnderRoot(PackageName, Root))
			{
				return true;
			}
		}
		return false;
	}

	static bool ReadMoveSpecs(
		const TSharedPtr<FJsonObject>& Params,
		TArray<FAssetMovePlan>& OutPlans,
		FString& OutError)
	{
		OutPlans.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Moves = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("moves"), Moves) || !Moves || Moves->Num() == 0)
		{
			OutError = TEXT("Missing or empty required array param 'moves'");
			return false;
		}
		if (Moves->Num() > MaxMoveCount)
		{
			OutError = FString::Printf(TEXT("Param 'moves' exceeds the maximum of %d entries"), MaxMoveCount);
			return false;
		}

		TSet<FName> SourcePackages;
		TSet<FName> DestinationPackages;
		for (int32 Index = 0; Index < Moves->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Move = (*Moves)[Index].IsValid() ? (*Moves)[Index]->AsObject() : nullptr;
			if (!Move.IsValid())
			{
				OutError = FString::Printf(TEXT("moves[%d] must be an object with source and destination strings"), Index);
				return false;
			}

			FString SourceInput;
			FString DestinationInput;
			if (!Move->TryGetStringField(TEXT("source"), SourceInput)
				|| !Move->TryGetStringField(TEXT("destination"), DestinationInput))
			{
				OutError = FString::Printf(TEXT("moves[%d] requires string fields 'source' and 'destination'"), Index);
				return false;
			}

			FAssetMovePlan Plan;
			if (!NormalizeStrictPackageName(SourceInput, TEXT("source"), Plan.SourcePackage, OutError)
				|| !NormalizeStrictPackageName(DestinationInput, TEXT("destination"), Plan.DestinationPackage, OutError))
			{
				OutError = FString::Printf(TEXT("moves[%d]: %s"), Index, *OutError);
				return false;
			}
			if (Plan.SourcePackage.Equals(Plan.DestinationPackage, ESearchCase::IgnoreCase))
			{
				OutError = FString::Printf(TEXT("moves[%d] source and destination must differ"), Index);
				return false;
			}

			const FName SourceName(*Plan.SourcePackage);
			const FName DestinationName(*Plan.DestinationPackage);
			if (SourcePackages.Contains(SourceName))
			{
				OutError = FString::Printf(TEXT("Duplicate move source is not allowed: %s"), *Plan.SourcePackage);
				return false;
			}
			if (DestinationPackages.Contains(DestinationName))
			{
				OutError = FString::Printf(TEXT("Duplicate move destination is not allowed: %s"), *Plan.DestinationPackage);
				return false;
			}
			SourcePackages.Add(SourceName);
			DestinationPackages.Add(DestinationName);
			OutPlans.Add(MoveTemp(Plan));
		}

		for (const FAssetMovePlan& Plan : OutPlans)
		{
			if (SourcePackages.Contains(FName(*Plan.DestinationPackage)))
			{
				OutError = FString::Printf(
					TEXT("Move chains and cycles are not supported; destination is also a source: %s"),
					*Plan.DestinationPackage);
				return false;
			}
		}
		return true;
	}

	static bool ReadCleanupSpecs(
		const TSharedPtr<FJsonObject>& Params,
		TArray<FAssetMovePlan>& OutPlans,
		FString& OutError)
	{
		OutPlans.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Moves = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("moves"), Moves) || !Moves || Moves->Num() == 0)
		{
			OutError = TEXT("Missing or empty required array param 'moves'");
			return false;
		}
		if (Moves->Num() > MaxCleanupCount)
		{
			OutError = FString::Printf(TEXT("Param 'moves' exceeds the cleanup maximum of %d entries"), MaxCleanupCount);
			return false;
		}

		TSet<FName> SourcePackages;
		TSet<FName> SourceObjectPaths;
		for (int32 Index = 0; Index < Moves->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Move = (*Moves)[Index].IsValid() ? (*Moves)[Index]->AsObject() : nullptr;
			if (!Move.IsValid())
			{
				OutError = FString::Printf(TEXT("moves[%d] must be an object"), Index);
				return false;
			}

			FString SourceInput;
			FString DestinationInput;
			if (!Move->TryGetStringField(TEXT("source"), SourceInput)
				|| !Move->TryGetStringField(TEXT("destination"), DestinationInput))
			{
				OutError = FString::Printf(TEXT("moves[%d] requires string fields 'source' and 'destination'"), Index);
				return false;
			}

			FAssetMovePlan Plan;
			if (!NormalizeStrictPackageName(SourceInput, TEXT("source"), Plan.SourcePackage, OutError)
				|| !NormalizeStrictPackageName(
					DestinationInput,
					TEXT("destination"),
					Plan.DestinationPackage,
					OutError,
					/*bIncludeReadOnlyRoots=*/true))
			{
				OutError = FString::Printf(TEXT("moves[%d]: %s"), Index, *OutError);
				return false;
			}
			if (Plan.SourcePackage.Equals(Plan.DestinationPackage, ESearchCase::IgnoreCase))
			{
				OutError = FString::Printf(TEXT("moves[%d] source and destination packages must differ"), Index);
				return false;
			}

			const bool bHasSourceObjectPath = Move->HasField(TEXT("source_object_path"));
			const bool bHasDestinationObjectPath = Move->HasField(TEXT("destination_object_path"));
			if (bHasSourceObjectPath != bHasDestinationObjectPath)
			{
				OutError = FString::Printf(
					TEXT("moves[%d] must provide both source_object_path and destination_object_path, or neither"),
					Index);
				return false;
			}

			if (bHasSourceObjectPath)
			{
				FString SourceObjectInput;
				FString DestinationObjectInput;
				if (!Move->TryGetStringField(TEXT("source_object_path"), SourceObjectInput)
					|| !Move->TryGetStringField(TEXT("destination_object_path"), DestinationObjectInput)
					|| !NormalizeStrictObjectPath(
						SourceObjectInput,
						TEXT("source_object_path"),
						Plan.SourcePackage,
						Plan.SourceObjectPath,
						OutError,
						/*bIncludeReadOnlyRoots=*/false)
					|| !NormalizeStrictObjectPath(
						DestinationObjectInput,
						TEXT("destination_object_path"),
						Plan.DestinationPackage,
						Plan.DestinationObjectPath,
						OutError,
						/*bIncludeReadOnlyRoots=*/true))
				{
					OutError = FString::Printf(TEXT("moves[%d]: %s"), Index, *OutError);
					return false;
				}
			}
			else
			{
				Plan.SourceObjectPath = Plan.SourcePackage + TEXT(".")
					+ FPackageName::GetLongPackageAssetName(Plan.SourcePackage);
				Plan.DestinationObjectPath = Plan.DestinationPackage + TEXT(".")
					+ FPackageName::GetLongPackageAssetName(Plan.DestinationPackage);
			}

			if (Plan.SourceObjectPath.Equals(Plan.DestinationObjectPath, ESearchCase::IgnoreCase))
			{
				OutError = FString::Printf(TEXT("moves[%d] source and destination objects must differ"), Index);
				return false;
			}
			if (SourcePackages.Contains(FName(*Plan.SourcePackage))
				|| SourceObjectPaths.Contains(FName(*Plan.SourceObjectPath)))
			{
				OutError = FString::Printf(TEXT("Duplicate cleanup source is not allowed: %s"), *Plan.SourceObjectPath);
				return false;
			}
			SourcePackages.Add(FName(*Plan.SourcePackage));
			SourceObjectPaths.Add(FName(*Plan.SourceObjectPath));
			OutPlans.Add(MoveTemp(Plan));
		}

		for (const FAssetMovePlan& Plan : OutPlans)
		{
			if (SourcePackages.Contains(FName(*Plan.DestinationPackage))
				|| SourceObjectPaths.Contains(FName(*Plan.DestinationObjectPath)))
			{
				OutError = FString::Printf(
					TEXT("Cleanup chains and cycles are not supported; destination is also a source: %s"),
					*Plan.DestinationObjectPath);
				return false;
			}
		}
		return true;
	}

	static void AddPreflightError(FAssetMovePlan& Plan, const FString& Error)
	{
		Plan.PreflightErrors.AddUnique(Error);
	}

	static void PreflightMove(
		FAssetMovePlan& Plan,
		IAssetRegistry& AssetRegistry,
		const TArray<FString>& AllowedSourceRoots,
		const TArray<FString>& AllowedDestinationRoots)
	{
		Plan.SourceObjectPath = Plan.SourcePackage + TEXT(".") + FPackageName::GetLongPackageAssetName(Plan.SourcePackage);
		Plan.DestinationObjectPath = Plan.DestinationPackage + TEXT(".") + FPackageName::GetLongPackageAssetName(Plan.DestinationPackage);

		if (!IsUnderAnyRoot(Plan.SourcePackage, AllowedSourceRoots))
		{
			AddPreflightError(Plan, TEXT("source_outside_allowed_roots"));
		}
		if (!IsUnderAnyRoot(Plan.DestinationPackage, AllowedDestinationRoots))
		{
			AddPreflightError(Plan, TEXT("destination_outside_allowed_roots"));
		}

		const bool bSourceExistsOnDisk =
			FPackageName::DoesPackageExist(Plan.SourcePackage, &Plan.SourceFilename);
		TArray<FAssetData> SourceAssets;
		AssetRegistry.GetAssetsByPackageName(
			FName(*Plan.SourcePackage),
			SourceAssets,
			/*bIncludeOnlyOnDiskAssets=*/false);
		if (SourceAssets.Num() == 0
			&& bSourceExistsOnDisk
			&& !AssetRegistry.IsLoadingAssets())
		{
			// Custom/late mount points may contain a valid package that has not
			// entered the registry cache yet. Scan the one resolved file; never
			// broaden the search or load the asset during preflight.
			AssetRegistry.ScanFilesSynchronous(
				{ Plan.SourceFilename },
				/*bForceRescan=*/true);
			AssetRegistry.GetAssetsByPackageName(
				FName(*Plan.SourcePackage),
				SourceAssets,
				/*bIncludeOnlyOnDiskAssets=*/false);
		}
		const FString ExpectedSourceName = FPackageName::GetLongPackageAssetName(Plan.SourcePackage);
		if (SourceAssets.Num() != 1)
		{
			AddPreflightError(
				Plan,
				SourceAssets.Num() == 0
					? (bSourceExistsOnDisk
						? (AssetRegistry.IsLoadingAssets()
							? TEXT("asset_registry_loading")
							: TEXT("source_not_indexed_after_exact_scan"))
						: TEXT("source_missing"))
					: TEXT("source_package_must_contain_one_primary_asset"));
		}
		else if (SourceAssets[0].AssetName.ToString() != ExpectedSourceName)
		{
			AddPreflightError(Plan, TEXT("source_primary_asset_name_mismatch"));
		}
		else if (SourceAssets[0].IsRedirector())
		{
			AddPreflightError(Plan, TEXT("source_is_redirector"));
		}
		else
		{
			Plan.SourceAssetData = SourceAssets[0];
		}

		TArray<FAssetData> DestinationAssets;
		AssetRegistry.GetAssetsByPackageName(
			FName(*Plan.DestinationPackage),
			DestinationAssets,
			/*bIncludeOnlyOnDiskAssets=*/false);
		if (DestinationAssets.Num() > 0)
		{
			AddPreflightError(Plan, TEXT("destination_exists_in_asset_registry"));
		}
		if (FPackageName::DoesPackageExist(Plan.DestinationPackage, &Plan.DestinationFilename))
		{
			AddPreflightError(Plan, TEXT("destination_exists_on_disk"));
		}
		if (FindPackage(nullptr, *Plan.DestinationPackage) != nullptr)
		{
			AddPreflightError(Plan, TEXT("destination_package_is_loaded"));
		}

		TArray<FName> HardReferencers;
		TArray<FName> SoftReferencers;
		AssetRegistry.GetReferencers(
			FName(*Plan.SourcePackage),
			HardReferencers,
			UE::AssetRegistry::EDependencyCategory::Package,
			UE::AssetRegistry::EDependencyQuery::Hard);
		AssetRegistry.GetReferencers(
			FName(*Plan.SourcePackage),
			SoftReferencers,
			UE::AssetRegistry::EDependencyCategory::Package,
			UE::AssetRegistry::EDependencyQuery::Soft);
		if (Plan.DestinationFilename.IsEmpty()
			&& !FPackageName::TryConvertLongPackageNameToFilename(
				Plan.DestinationPackage,
				Plan.DestinationFilename,
				FPackageName::GetAssetPackageExtension()))
		{
			AddPreflightError(Plan, TEXT("destination_mount_is_not_writable_or_registered"));
		}

		Plan.Report = MakeShared<FJsonObject>();
		Plan.Report->SetStringField(TEXT("source"), Plan.SourcePackage);
		Plan.Report->SetStringField(TEXT("destination"), Plan.DestinationPackage);
		Plan.Report->SetStringField(TEXT("source_object_path"), Plan.SourceObjectPath);
		Plan.Report->SetStringField(TEXT("destination_object_path"), Plan.DestinationObjectPath);
		Plan.Report->SetStringField(TEXT("source_filename"), Plan.SourceFilename);
		Plan.Report->SetStringField(TEXT("destination_filename"), Plan.DestinationFilename);
		Plan.Report->SetNumberField(TEXT("hard_referencer_count"), HardReferencers.Num());
		Plan.Report->SetNumberField(TEXT("soft_referencer_count"), SoftReferencers.Num());
		Plan.Report->SetBoolField(TEXT("preflight_ok"), Plan.PreflightErrors.Num() == 0);
		Plan.Report->SetArrayField(TEXT("preflight_errors"), StringsToJson(Plan.PreflightErrors));
		if (Plan.SourceAssetData.IsValid())
		{
			Plan.Report->SetStringField(TEXT("asset_class"), Plan.SourceAssetData.AssetClassPath.ToString());
		}
	}

	static void PreflightMovedRedirectorCleanup(
		FAssetMovePlan& Plan,
		IAssetRegistry& AssetRegistry,
		const TArray<FString>& AllowedSourceRoots,
		const TArray<FString>& AllowedDestinationRoots)
	{
		Plan.Report = MakeShared<FJsonObject>();
		Plan.Report->SetStringField(TEXT("source"), Plan.SourcePackage);
		Plan.Report->SetStringField(TEXT("destination"), Plan.DestinationPackage);
		Plan.Report->SetStringField(TEXT("source_object_path"), Plan.SourceObjectPath);
		Plan.Report->SetStringField(TEXT("destination_object_path"), Plan.DestinationObjectPath);

		if (!IsUnderAnyRoot(Plan.SourcePackage, AllowedSourceRoots))
		{
			AddPreflightError(Plan, TEXT("source_outside_allowed_roots"));
		}
		if (!IsUnderAnyRoot(Plan.DestinationPackage, AllowedDestinationRoots))
		{
			AddPreflightError(Plan, TEXT("destination_outside_allowed_roots"));
		}

		TArray<FAssetData> SourceAssets;
		AssetRegistry.GetAssetsByPackageName(
			FName(*Plan.SourcePackage),
			SourceAssets,
			/*bIncludeOnlyOnDiskAssets=*/false);
		const FAssetData ExactSourceAsset = AssetRegistry.GetAssetByObjectPath(
			FSoftObjectPath(Plan.SourceObjectPath),
			/*bIncludeOnlyOnDiskAssets=*/false,
			/*bSkipARFilteredAssets=*/true);
		const bool bSourceOnDisk = FPackageName::DoesPackageExist(Plan.SourcePackage, &Plan.SourceFilename);
		if (!ExactSourceAsset.IsValid() && SourceAssets.Num() == 0)
		{
			Plan.bSourceAlreadyCleaned = !bSourceOnDisk;
			if (bSourceOnDisk)
			{
				AddPreflightError(Plan, TEXT("source_file_exists_without_registry_asset"));
			}
		}
		bool bSourcePackageRedirectorsValid = true;
		if (!ExactSourceAsset.IsValid() && !Plan.bSourceAlreadyCleaned)
		{
			AddPreflightError(Plan, TEXT("source_redirector_object_missing"));
			bSourcePackageRedirectorsValid = false;
		}
		else if (ExactSourceAsset.IsValid() && !ExactSourceAsset.IsRedirector())
		{
			AddPreflightError(Plan, TEXT("source_is_not_redirector"));
			bSourcePackageRedirectorsValid = false;
		}

		TArray<TSharedPtr<FJsonValue>> SourcePackageAssets;
		SourcePackageAssets.Reserve(SourceAssets.Num());
		bool bExactSourceListed = false;
		for (const FAssetData& SourceAsset : SourceAssets)
		{
			const FString SourceAssetObjectPath = SourceAsset.GetSoftObjectPath().ToString();
			bExactSourceListed |= SourceAssetObjectPath.Equals(
				Plan.SourceObjectPath,
				ESearchCase::CaseSensitive);
			FString DestinationExportPath;
			FString TaggedDestinationObjectPath;
			const bool bHasDestinationTag = SourceAsset.GetTagValue(
				FName(TEXT("DestinationObject")),
				DestinationExportPath);
			if (bHasDestinationTag)
			{
				TaggedDestinationObjectPath = FPackageName::ExportTextPathToObjectPath(DestinationExportPath);
			}
			const FString TaggedDestinationPackage = TaggedDestinationObjectPath.IsEmpty()
				? FString()
				: FPackageName::ObjectPathToPackageName(TaggedDestinationObjectPath);
			const bool bTargetsExpectedPackage = TaggedDestinationPackage.Equals(
				Plan.DestinationPackage,
				ESearchCase::CaseSensitive);

			TSharedPtr<FJsonObject> SourceAssetReport = MakeShared<FJsonObject>();
			SourceAssetReport->SetStringField(TEXT("object_path"), SourceAssetObjectPath);
			SourceAssetReport->SetStringField(TEXT("asset_class"), SourceAsset.AssetClassPath.ToString());
			SourceAssetReport->SetBoolField(TEXT("is_redirector"), SourceAsset.IsRedirector());
			SourceAssetReport->SetStringField(TEXT("destination_export_path"), DestinationExportPath);
			SourceAssetReport->SetStringField(TEXT("destination_object_path"), TaggedDestinationObjectPath);
			SourceAssetReport->SetStringField(TEXT("destination_package"), TaggedDestinationPackage);
			SourceAssetReport->SetBoolField(TEXT("targets_expected_package"), bTargetsExpectedPackage);
			SourcePackageAssets.Add(MakeShared<FJsonValueObject>(SourceAssetReport));

			if (!SourceAsset.IsRedirector())
			{
				AddPreflightError(Plan, TEXT("source_package_contains_non_redirector"));
				bSourcePackageRedirectorsValid = false;
			}
			else if (!bHasDestinationTag || TaggedDestinationObjectPath.IsEmpty())
			{
				AddPreflightError(Plan, TEXT("source_redirector_destination_tag_missing"));
				bSourcePackageRedirectorsValid = false;
			}
			else if (!bTargetsExpectedPackage)
			{
				AddPreflightError(Plan, TEXT("source_package_redirector_target_outside_destination_package"));
				bSourcePackageRedirectorsValid = false;
			}

			if (SourceAssetObjectPath.Equals(Plan.SourceObjectPath, ESearchCase::CaseSensitive))
			{
				Plan.Report->SetStringField(TEXT("redirector_destination_export_path"), DestinationExportPath);
				Plan.Report->SetStringField(TEXT("redirector_destination_object_path"), TaggedDestinationObjectPath);
				if (!TaggedDestinationObjectPath.Equals(
						Plan.DestinationObjectPath,
						ESearchCase::CaseSensitive))
				{
					AddPreflightError(Plan, TEXT("source_redirector_target_mismatch"));
					bSourcePackageRedirectorsValid = false;
				}
			}
		}
		Plan.Report->SetNumberField(TEXT("source_package_asset_count"), SourceAssets.Num());
		Plan.Report->SetArrayField(TEXT("source_package_assets"), MoveTemp(SourcePackageAssets));
		if (ExactSourceAsset.IsValid() && !bExactSourceListed)
		{
			AddPreflightError(Plan, TEXT("source_redirector_missing_from_package_query"));
			bSourcePackageRedirectorsValid = false;
		}
		if (bSourcePackageRedirectorsValid && ExactSourceAsset.IsValid())
		{
			Plan.SourceAssetData = ExactSourceAsset;
			Plan.SourceRedirectorAssets = SourceAssets;
		}

		const FAssetData ExactDestinationAsset = AssetRegistry.GetAssetByObjectPath(
			FSoftObjectPath(Plan.DestinationObjectPath),
			/*bIncludeOnlyOnDiskAssets=*/false,
			/*bSkipARFilteredAssets=*/true);
		if (!ExactDestinationAsset.IsValid())
		{
			AddPreflightError(Plan, TEXT("destination_object_missing"));
		}
		else
		{
			Plan.DestinationAssetData = ExactDestinationAsset;
		}

		const bool bDestinationOnDisk = FPackageName::DoesPackageExist(
			Plan.DestinationPackage,
			&Plan.DestinationFilename);
		if (!bDestinationOnDisk)
		{
			AddPreflightError(Plan, TEXT("destination_missing_on_disk"));
		}
		else if (IFileManager::Get().FileSize(*Plan.DestinationFilename) <= 0)
		{
			AddPreflightError(Plan, TEXT("destination_file_is_empty"));
		}

		TArray<FName> HardReferencers;
		TArray<FName> SoftReferencers;
		if (Plan.SourceAssetData.IsValid())
		{
			AssetRegistry.GetReferencers(
				FName(*Plan.SourcePackage),
				HardReferencers,
				UE::AssetRegistry::EDependencyCategory::Package,
				UE::AssetRegistry::EDependencyQuery::Hard);
			AssetRegistry.GetReferencers(
				FName(*Plan.SourcePackage),
				SoftReferencers,
				UE::AssetRegistry::EDependencyCategory::Package,
				UE::AssetRegistry::EDependencyQuery::Soft);
		}
		TArray<FString> RemainingReferencers;
		for (const FName Referencer : HardReferencers)
		{
			RemainingReferencers.AddUnique(Referencer.ToString());
		}
		for (const FName Referencer : SoftReferencers)
		{
			RemainingReferencers.AddUnique(Referencer.ToString());
		}
		RemainingReferencers.Sort();
		if (!RemainingReferencers.IsEmpty())
		{
			AddPreflightError(Plan, TEXT("source_redirector_still_has_referencers"));
		}

		Plan.Report->SetStringField(TEXT("source_filename"), Plan.SourceFilename);
		Plan.Report->SetStringField(TEXT("destination_filename"), Plan.DestinationFilename);
		Plan.Report->SetBoolField(TEXT("source_already_cleaned"), Plan.bSourceAlreadyCleaned);
		Plan.Report->SetBoolField(TEXT("source_redirector_registered"), Plan.SourceAssetData.IsValid());
		Plan.Report->SetBoolField(TEXT("source_package_on_disk"), bSourceOnDisk);
		Plan.Report->SetBoolField(TEXT("destination_asset_registered"), Plan.DestinationAssetData.IsValid());
		Plan.Report->SetBoolField(TEXT("destination_package_on_disk"), bDestinationOnDisk);
		Plan.Report->SetArrayField(TEXT("remaining_referencers"), StringsToJson(RemainingReferencers));
		Plan.Report->SetNumberField(TEXT("remaining_referencer_count"), RemainingReferencers.Num());
		Plan.Report->SetBoolField(TEXT("preflight_ok"), Plan.PreflightErrors.Num() == 0);
		Plan.Report->SetArrayField(TEXT("preflight_errors"), StringsToJson(Plan.PreflightErrors));
	}

	static TSharedPtr<FJsonObject> MakeCleanupReport(
		const TArray<FAssetMovePlan>& Plans,
		const TArray<FString>& AllowedSourceRoots,
		const TArray<FString>& AllowedDestinationRoots,
		bool bDryRun,
		bool bConfirm,
		const FString& Status,
		bool bSuccess)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Reserve(Plans.Num());
		for (const FAssetMovePlan& Plan : Plans)
		{
			Rows.Add(MakeShared<FJsonValueObject>(Plan.Report));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("namespace"), TEXT("asset"));
		Result->SetStringField(TEXT("action"), TEXT("cleanup_moved_redirectors"));
		Result->SetStringField(TEXT("status"), Status);
		Result->SetBoolField(TEXT("success"), bSuccess);
		Result->SetBoolField(TEXT("dry_run"), bDryRun);
		Result->SetBoolField(TEXT("confirmed"), bConfirm);
		Result->SetNumberField(TEXT("requested_count"), Plans.Num());
		Result->SetNumberField(TEXT("max_cleanup_count"), MaxCleanupCount);
		Result->SetArrayField(TEXT("allowed_source_roots"), StringsToJson(AllowedSourceRoots));
		Result->SetArrayField(TEXT("allowed_destination_roots"), StringsToJson(AllowedDestinationRoots));
		Result->SetArrayField(TEXT("moves"), Rows);
		return Result;
	}

	static TSharedPtr<FJsonObject> MakeReport(
		const TArray<FAssetMovePlan>& Plans,
		const TArray<FString>& AllowedSourceRoots,
		const TArray<FString>& AllowedDestinationRoots,
		bool bDryRun,
		bool bConfirm,
		bool bCleanupRedirectors,
		const FString& Status,
		bool bSuccess)
	{
		TArray<TSharedPtr<FJsonValue>> MoveRows;
		MoveRows.Reserve(Plans.Num());
		for (const FAssetMovePlan& Plan : Plans)
		{
			MoveRows.Add(MakeShared<FJsonValueObject>(Plan.Report));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("namespace"), TEXT("asset"));
		Result->SetStringField(TEXT("action"), TEXT("move_assets"));
		Result->SetStringField(TEXT("status"), Status);
		Result->SetBoolField(TEXT("success"), bSuccess);
		Result->SetBoolField(TEXT("dry_run"), bDryRun);
		Result->SetBoolField(TEXT("confirmed"), bConfirm);
		Result->SetBoolField(TEXT("cleanup_redirectors"), bCleanupRedirectors);
		Result->SetNumberField(TEXT("requested_count"), Plans.Num());
		Result->SetNumberField(TEXT("max_move_count"), MaxMoveCount);
		Result->SetArrayField(TEXT("allowed_source_roots"), StringsToJson(AllowedSourceRoots));
		Result->SetArrayField(TEXT("allowed_destination_roots"), StringsToJson(AllowedDestinationRoots));
		Result->SetArrayField(TEXT("moves"), MoveRows);
		return Result;
	}

	static void RefreshMovedPaths(IAssetRegistry& AssetRegistry, const TArray<FAssetMovePlan>& Plans)
	{
		TArray<FString> Paths;
		for (const FAssetMovePlan& Plan : Plans)
		{
			Paths.AddUnique(FPackageName::GetLongPackagePath(Plan.SourcePackage));
			Paths.AddUnique(FPackageName::GetLongPackagePath(Plan.DestinationPackage));
		}
		if (Paths.Num() > 0)
		{
			AssetRegistry.ScanPathsSynchronous(Paths, /*bForceRescan=*/true, /*bIgnoreDenyListScanFilters=*/false);
		}
	}

	static bool CapturePostRenameCleanupState(
		IAssetRegistry& AssetRegistry,
		TArray<FAssetMovePlan>& Plans,
		FString& OutError)
	{
		for (FAssetMovePlan& Plan : Plans)
		{
			TArray<FAssetData> SourceAssets;
			AssetRegistry.GetAssetsByPackageName(
				FName(*Plan.SourcePackage),
				SourceAssets,
				/*bIncludeOnlyOnDiskAssets=*/false);
			if (SourceAssets.IsEmpty())
			{
				OutError = FString::Printf(
					TEXT("rename did not leave a redirector package at '%s'"),
					*Plan.SourcePackage);
				return false;
			}

			bool bFoundExactSourceRedirector = false;
			for (const FAssetData& SourceAsset : SourceAssets)
			{
				const FString SourceObjectPath = SourceAsset.GetSoftObjectPath().ToString();
				const bool bIsExactSourceRedirector = SourceObjectPath.Equals(
					Plan.SourceObjectPath,
					ESearchCase::CaseSensitive);
				bFoundExactSourceRedirector |= bIsExactSourceRedirector;

				UObjectRedirector* Redirector = SourceAsset.IsRedirector()
					? Cast<UObjectRedirector>(SourceAsset.GetAsset())
					: nullptr;
				const FString DestinationObjectPath = Redirector && Redirector->DestinationObject
					? Redirector->DestinationObject->GetPathName()
					: FString();
				const FString DestinationPackage = DestinationObjectPath.IsEmpty()
					? FString()
					: FPackageName::ObjectPathToPackageName(DestinationObjectPath);
				if (!Redirector
					|| !Redirector->DestinationObject
					|| !DestinationPackage.Equals(
						Plan.DestinationPackage,
						ESearchCase::CaseSensitive)
					|| (bIsExactSourceRedirector
						&& !DestinationObjectPath.Equals(
							Plan.DestinationObjectPath,
							ESearchCase::CaseSensitive)))
				{
					OutError = FString::Printf(
						TEXT("post-rename source object '%s' is not an expected redirector into destination package '%s'"),
						*SourceObjectPath,
						*Plan.DestinationPackage);
					return false;
				}
			}
			if (!bFoundExactSourceRedirector)
			{
				OutError = FString::Printf(
					TEXT("post-rename source package '%s' does not contain the exact requested redirector '%s'"),
					*Plan.SourcePackage,
					*Plan.SourceObjectPath);
				return false;
			}

			const FAssetData DestinationAsset = AssetRegistry.GetAssetByObjectPath(
				FSoftObjectPath(Plan.DestinationObjectPath),
				/*bIncludeOnlyOnDiskAssets=*/false,
				/*bSkipARFilteredAssets=*/true);
			FString DestinationFilename;
			if (!DestinationAsset.IsValid()
				|| !Plan.SourceAssetData.IsValid()
				|| DestinationAsset.AssetClassPath != Plan.SourceAssetData.AssetClassPath
				|| !FPackageName::DoesPackageExist(Plan.DestinationPackage, &DestinationFilename)
				|| IFileManager::Get().FileSize(*DestinationFilename) <= 0)
			{
				OutError = FString::Printf(
					TEXT("post-rename destination object '%s' failed exact integrity validation"),
					*Plan.DestinationObjectPath);
				return false;
			}

			Plan.SourceRedirectorAssets = MoveTemp(SourceAssets);
			Plan.DestinationAssetData = DestinationAsset;
			Plan.Report->SetNumberField(
				TEXT("post_rename_redirector_count"),
				Plan.SourceRedirectorAssets.Num());
		}
		return true;
	}

	static bool DeleteUnreferencedMovedRedirectors(
		IAssetRegistry& AssetRegistry,
		const TArray<FAssetMovePlan>& Plans,
		int32& OutSubmittedRedirectorCount,
		int32& OutDeletedObjectCount,
		FString& OutError)
	{
		OutSubmittedRedirectorCount = 0;
		OutDeletedObjectCount = 0;
		if (AssetRegistry.IsLoadingAssets())
		{
			OutError = TEXT("asset registry is still discovering assets");
			return false;
		}

		TArray<FString> AssetPathsToDelete;
		TArray<FString> AllowedPrefixesToDelete;
		for (const FAssetMovePlan& Plan : Plans)
		{
			if (Plan.bSourceAlreadyCleaned)
			{
				continue;
			}
			TArray<FAssetData> SourceAssets;
			AssetRegistry.GetAssetsByPackageName(
				FName(*Plan.SourcePackage),
				SourceAssets,
				/*bIncludeOnlyOnDiskAssets=*/false);
			FString SourceFilename;
			const bool bSourceOnDisk = FPackageName::DoesPackageExist(
				Plan.SourcePackage,
				&SourceFilename);
			if (SourceAssets.IsEmpty() && !bSourceOnDisk)
			{
				continue;
			}

			TSet<FString> ExpectedSourceObjectPaths;
			for (const FAssetData& ExpectedSourceAsset : Plan.SourceRedirectorAssets)
			{
				ExpectedSourceObjectPaths.Add(ExpectedSourceAsset.GetSoftObjectPath().ToString());
			}
			if (SourceAssets.Num() != ExpectedSourceObjectPaths.Num())
			{
				OutError = FString::Printf(
					TEXT("source package '%s' redirector set changed after preflight (expected=%d actual=%d)"),
					*Plan.SourcePackage,
					ExpectedSourceObjectPaths.Num(),
					SourceAssets.Num());
				return false;
			}
			for (const FAssetData& SourceAsset : SourceAssets)
			{
				const FString SourceObjectPath = SourceAsset.GetSoftObjectPath().ToString();
				if (!ExpectedSourceObjectPaths.Contains(SourceObjectPath) || !SourceAsset.IsRedirector())
				{
					OutError = FString::Printf(
						TEXT("source package '%s' no longer contains only the preflighted redirectors"),
						*Plan.SourcePackage);
					return false;
				}
			}
			if (!ExpectedSourceObjectPaths.Contains(Plan.SourceObjectPath))
			{
				OutError = FString::Printf(
					TEXT("source package '%s' no longer contains the exact requested redirector"),
					*Plan.SourcePackage);
				return false;
			}

			TArray<FName> HardReferencers;
			TArray<FName> SoftReferencers;
			AssetRegistry.GetReferencers(
				FName(*Plan.SourcePackage),
				HardReferencers,
				UE::AssetRegistry::EDependencyCategory::Package,
				UE::AssetRegistry::EDependencyQuery::Hard);
			AssetRegistry.GetReferencers(
				FName(*Plan.SourcePackage),
				SoftReferencers,
				UE::AssetRegistry::EDependencyCategory::Package,
				UE::AssetRegistry::EDependencyQuery::Soft);
			if (!HardReferencers.IsEmpty() || !SoftReferencers.IsEmpty())
			{
				OutError = FString::Printf(
					TEXT("redirector '%s' acquired referencers after preflight (hard=%d soft=%d)"),
					*Plan.SourcePackage,
					HardReferencers.Num(),
					SoftReferencers.Num());
				return false;
			}

			for (const FAssetData& SourceAsset : SourceAssets)
			{
				const FString SourceObjectPath = SourceAsset.GetSoftObjectPath().ToString();
				UObjectRedirector* Redirector = Cast<UObjectRedirector>(SourceAsset.GetAsset());
				const FString DestinationObjectPath = Redirector && Redirector->DestinationObject
					? Redirector->DestinationObject->GetPathName()
					: FString();
				const FString DestinationPackage = DestinationObjectPath.IsEmpty()
					? FString()
					: FPackageName::ObjectPathToPackageName(DestinationObjectPath);
				const bool bExactRequestedTargetMatches = !SourceObjectPath.Equals(
						Plan.SourceObjectPath,
						ESearchCase::CaseSensitive)
					|| DestinationObjectPath.Equals(
						Plan.DestinationObjectPath,
						ESearchCase::CaseSensitive);
				if (!Redirector
					|| !Redirector->DestinationObject
					|| !DestinationPackage.Equals(
						Plan.DestinationPackage,
						ESearchCase::CaseSensitive)
					|| !bExactRequestedTargetMatches)
				{
					OutError = FString::Printf(
						TEXT("redirector '%s' failed package or exact target revalidation before delete"),
						*SourceObjectPath);
					return false;
				}

				AssetPathsToDelete.Add(SourceObjectPath);
				++OutSubmittedRedirectorCount;
			}

			const FAssetData DestinationAsset = AssetRegistry.GetAssetByObjectPath(
				FSoftObjectPath(Plan.DestinationObjectPath),
				/*bIncludeOnlyOnDiskAssets=*/false,
				/*bSkipARFilteredAssets=*/true);
			FString DestinationFilename;
			if (!DestinationAsset.IsValid()
				|| !Plan.DestinationAssetData.IsValid()
				|| DestinationAsset.AssetClassPath != Plan.DestinationAssetData.AssetClassPath
				|| !FPackageName::DoesPackageExist(Plan.DestinationPackage, &DestinationFilename)
				|| IFileManager::Get().FileSize(*DestinationFilename) <= 0)
			{
				OutError = FString::Printf(
					TEXT("destination object '%s' failed exact integrity revalidation before delete"),
					*Plan.DestinationObjectPath);
				return false;
			}

			AllowedPrefixesToDelete.AddUnique(Plan.SourcePackage);
		}

		if (OutSubmittedRedirectorCount == 0)
		{
			return true;
		}
		if (AssetPathsToDelete.Num() > MaxCleanupCount)
		{
			OutError = FString::Printf(
				TEXT("redirector cleanup exceeds the single-delete batch maximum of %d"),
				MaxCleanupCount);
			return false;
		}

		TSharedPtr<FJsonObject> DeleteParams = MakeShared<FJsonObject>();
		DeleteParams->SetArrayField(TEXT("asset_paths"), StringsToJson(AssetPathsToDelete));
		DeleteParams->SetArrayField(TEXT("allowed_prefixes"), StringsToJson(AllowedPrefixesToDelete));
		DeleteParams->SetBoolField(TEXT("dry_run"), false);
		DeleteParams->SetBoolField(TEXT("force"), false);
		DeleteParams->SetBoolField(TEXT("require_source_control"), true);
		const FMonolithActionResult DeleteResult =
			FMonolithAssetLifecycleActions::DeleteAssets(DeleteParams);
		if (!DeleteResult.bSuccess)
		{
			OutError = FString::Printf(
				TEXT("hardened asset.delete_assets batch failed before or during cleanup: %s"),
				*DeleteResult.ErrorMessage);
			return false;
		}
		bool bDeleteReportSuccess = false;
		if (!DeleteResult.Result.IsValid()
			|| !DeleteResult.Result->TryGetBoolField(TEXT("success"), bDeleteReportSuccess)
			|| !bDeleteReportSuccess)
		{
			OutError = TEXT("hardened asset.delete_assets batch reported one or more target or source-control failures");
			return false;
		}
		double DeletedObjectCount = 0.0;
		if (!DeleteResult.Result->TryGetNumberField(
				TEXT("object_delete_reported"),
				DeletedObjectCount))
		{
			OutError = TEXT("hardened asset.delete_assets batch omitted object_delete_reported");
			return false;
		}
		OutDeletedObjectCount = static_cast<int32>(DeletedObjectCount);
		RefreshMovedPaths(AssetRegistry, Plans);
		return true;
	}

	static bool CollectPostconditions(
		FAssetMovePlan& Plan,
		IAssetRegistry& AssetRegistry,
		bool bCleanupRedirectors)
	{
		TArray<FAssetData> DestinationAssets;
		AssetRegistry.GetAssetsByPackageName(FName(*Plan.DestinationPackage), DestinationAssets, /*bIncludeOnlyOnDiskAssets=*/false);
		const FString ExpectedDestinationName = FPackageName::GetLongPackageAssetName(Plan.DestinationPackage);
		const FAssetData* DestinationPrimary = nullptr;
		for (const FAssetData& Asset : DestinationAssets)
		{
			if (!Asset.IsRedirector() && Asset.AssetName.ToString() == ExpectedDestinationName)
			{
				DestinationPrimary = &Asset;
				break;
			}
		}

		FString DestinationFilename;
		const bool bDestinationOnDisk = FPackageName::DoesPackageExist(Plan.DestinationPackage, &DestinationFilename);
		const bool bDestinationFileNonEmpty = bDestinationOnDisk && IFileManager::Get().FileSize(*DestinationFilename) > 0;
		const bool bDestinationClassMatches = DestinationPrimary
			&& DestinationPrimary->AssetClassPath == Plan.SourceAssetData.AssetClassPath;

		TArray<FAssetData> SourceAssets;
		AssetRegistry.GetAssetsByPackageName(FName(*Plan.SourcePackage), SourceAssets, /*bIncludeOnlyOnDiskAssets=*/false);
		bool bSourcePrimaryRemaining = false;
		bool bRedirectorPresent = false;
		bool bRedirectorTargetMatches = false;
		for (const FAssetData& Asset : SourceAssets)
		{
			if (!Asset.IsRedirector())
			{
				bSourcePrimaryRemaining = true;
				continue;
			}
			bRedirectorPresent = true;
			if (UObjectRedirector* Redirector = Cast<UObjectRedirector>(Asset.GetAsset()))
			{
				bRedirectorTargetMatches = Redirector->DestinationObject
					&& Redirector->DestinationObject->GetPathName().Equals(Plan.DestinationObjectPath, ESearchCase::CaseSensitive);
			}
		}

		FString SourceFilename;
		const bool bSourceOnDisk = FPackageName::DoesPackageExist(Plan.SourcePackage, &SourceFilename);
		const bool bSourceStateValid = bCleanupRedirectors
			? (!bSourcePrimaryRemaining && !bRedirectorPresent && !bSourceOnDisk)
			: (!bSourcePrimaryRemaining
				&& ((bRedirectorPresent && bRedirectorTargetMatches && bSourceOnDisk)
					|| (!bRedirectorPresent && !bSourceOnDisk)));
		const bool bPostconditionsMet = DestinationPrimary
			&& bDestinationClassMatches
			&& bDestinationFileNonEmpty
			&& bSourceStateValid;

		Plan.Report->SetBoolField(TEXT("destination_asset_registered"), DestinationPrimary != nullptr);
		Plan.Report->SetBoolField(TEXT("destination_class_matches"), bDestinationClassMatches);
		Plan.Report->SetBoolField(TEXT("destination_package_on_disk"), bDestinationOnDisk);
		Plan.Report->SetBoolField(TEXT("destination_file_non_empty"), bDestinationFileNonEmpty);
		Plan.Report->SetStringField(TEXT("destination_filename_after"), DestinationFilename);
		Plan.Report->SetBoolField(TEXT("source_primary_remaining"), bSourcePrimaryRemaining);
		Plan.Report->SetBoolField(TEXT("source_redirector_present"), bRedirectorPresent);
		Plan.Report->SetBoolField(TEXT("source_redirector_target_matches"), bRedirectorTargetMatches);
		Plan.Report->SetBoolField(TEXT("source_package_on_disk"), bSourceOnDisk);
		Plan.Report->SetBoolField(TEXT("postconditions_met"), bPostconditionsMet);
		Plan.Report->SetStringField(
			TEXT("status"),
			bPostconditionsMet
				? (bRedirectorPresent ? TEXT("moved_with_redirector") : TEXT("moved"))
				: TEXT("failed"));
		return bPostconditionsMet;
	}

	static bool CollectMovedRedirectorCleanupPostconditions(
		FAssetMovePlan& Plan,
		IAssetRegistry& AssetRegistry)
	{
		TArray<FAssetData> SourceAssets;
		AssetRegistry.GetAssetsByPackageName(
			FName(*Plan.SourcePackage),
			SourceAssets,
			/*bIncludeOnlyOnDiskAssets=*/false);
		FString SourceFilename;
		const bool bSourceOnDisk = FPackageName::DoesPackageExist(Plan.SourcePackage, &SourceFilename);

		const FAssetData DestinationAsset = AssetRegistry.GetAssetByObjectPath(
			FSoftObjectPath(Plan.DestinationObjectPath),
			/*bIncludeOnlyOnDiskAssets=*/false,
			/*bSkipARFilteredAssets=*/true);
		FString DestinationFilename;
		const bool bDestinationOnDisk = FPackageName::DoesPackageExist(
			Plan.DestinationPackage,
			&DestinationFilename);
		const bool bDestinationFileNonEmpty = bDestinationOnDisk
			&& IFileManager::Get().FileSize(*DestinationFilename) > 0;
		const bool bDestinationClassStable = DestinationAsset.IsValid()
			&& Plan.DestinationAssetData.IsValid()
			&& DestinationAsset.AssetClassPath == Plan.DestinationAssetData.AssetClassPath;
		const bool bPostconditionsMet = SourceAssets.IsEmpty()
			&& !bSourceOnDisk
			&& DestinationAsset.IsValid()
			&& bDestinationClassStable
			&& bDestinationFileNonEmpty;

		Plan.Report->SetBoolField(TEXT("source_asset_registered_after"), !SourceAssets.IsEmpty());
		Plan.Report->SetBoolField(TEXT("source_package_on_disk_after"), bSourceOnDisk);
		Plan.Report->SetStringField(TEXT("source_filename_after"), SourceFilename);
		Plan.Report->SetBoolField(TEXT("destination_asset_registered_after"), DestinationAsset.IsValid());
		Plan.Report->SetBoolField(TEXT("destination_class_stable"), bDestinationClassStable);
		Plan.Report->SetBoolField(TEXT("destination_package_on_disk_after"), bDestinationOnDisk);
		Plan.Report->SetBoolField(TEXT("destination_file_non_empty_after"), bDestinationFileNonEmpty);
		Plan.Report->SetStringField(TEXT("destination_filename_after"), DestinationFilename);
		Plan.Report->SetBoolField(TEXT("postconditions_met"), bPostconditionsMet);
		Plan.Report->SetStringField(
			TEXT("status"),
			bPostconditionsMet
				? (Plan.bSourceAlreadyCleaned ? TEXT("already_cleaned") : TEXT("cleaned"))
				: TEXT("failed"));
		return bPostconditionsMet;
	}
}

void FMonolithAssetMoveActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(
		TEXT("asset"),
		TEXT("move_assets"),
		TEXT("Move explicit source packages to exact destination packages through IAssetTools::RenameAssets. Defaults to dry-run, never overwrites, and verifies registry, disk, source, destination, and redirector postconditions."),
		FMonolithActionHandler::CreateStatic(&FMonolithAssetMoveActions::MoveAssets),
		FParamSchemaBuilder()
			.Required(TEXT("moves"), TEXT("array"), TEXT("1-512 exact move objects: [{\"source\":\"/Root/Old\",\"destination\":\"/OtherRoot/New\"}]"))
			.Required(TEXT("allowed_source_roots"), TEXT("array"), TEXT("Required non-empty source package-root allowlist; matching is package-segment bounded"))
			.Required(TEXT("allowed_destination_roots"), TEXT("array"), TEXT("Required non-empty destination package-root allowlist; matching is package-segment bounded"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Validate and report without loading or moving assets"), TEXT("true"))
			.Optional(TEXT("confirm"), TEXT("bool"), TEXT("Required when dry_run=false"), TEXT("false"))
			.Optional(TEXT("cleanup_redirectors"), TEXT("bool"), TEXT("Delete at most 200 exact source redirectors in one fully preflighted, source-control-verified batch; never opens the modal AssetTools fixup report"), TEXT("false"))
			.Optional(TEXT("accept_cdo_reference_warning"), TEXT("bool"), TEXT("Explicitly accept only AssetRenameManager's exact CDO/config reference warning; mutation requires an editor launched without -Unattended"), TEXT("false"))
			.StrictComplexTypes()
			.Build(),
		TEXT("Lifecycle"));

	Registry.RegisterAction(
		TEXT("asset"),
		TEXT("cleanup_moved_redirectors"),
		TEXT("Idempotently delete only exact redirectors whose expected destination objects are intact, whose AssetRegistry hard/soft referencer sets are empty, and whose source-control delete/revert-add postconditions are verified. Defaults to dry-run and never performs a rename or reference rewrite."),
		FMonolithActionHandler::CreateStatic(&FMonolithAssetMoveActions::CleanupMovedRedirectors),
		FParamSchemaBuilder()
			.Required(TEXT("moves"), TEXT("array"), TEXT("1-200 exact completed move objects. Optional paired source_object_path/destination_object_path fields preserve non-leaf exact targets and allow many-to-one destinations."))
			.Required(TEXT("allowed_source_roots"), TEXT("array"), TEXT("Required non-empty source package-root allowlist; matching is package-segment bounded"))
			.Required(TEXT("allowed_destination_roots"), TEXT("array"), TEXT("Required non-empty destination package-root allowlist; read-only roots are allowed because destinations are validated but never mutated"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Validate exact redirector targets, zero referencers, destination integrity, and idempotent already-cleaned state without loading or deleting"), TEXT("true"))
			.Optional(TEXT("confirm"), TEXT("bool"), TEXT("Required when dry_run=false"), TEXT("false"))
			.StrictComplexTypes()
			.Build(),
		TEXT("Lifecycle"));
}

FMonolithActionResult FMonolithAssetMoveActions::MoveAssets(const TSharedPtr<FJsonObject>& Params)
{
	bool bDryRun = true;
	bool bConfirm = false;
	bool bCleanupRedirectors = false;
	bool bAcceptCdoReferenceWarning = false;
	FString Error;
	if (!ReadBoolParam(Params, TEXT("dry_run"), bDryRun, Error)
		|| !ReadBoolParam(Params, TEXT("confirm"), bConfirm, Error)
		|| !ReadBoolParam(Params, TEXT("cleanup_redirectors"), bCleanupRedirectors, Error)
		|| !ReadBoolParam(Params, TEXT("accept_cdo_reference_warning"), bAcceptCdoReferenceWarning, Error))
	{
		return FMonolithActionResult::Error(Error, InvalidParamsErrorCode);
	}
	if (!bDryRun && !bConfirm)
	{
		return FMonolithActionResult::Error(
			TEXT("asset.move_assets requires dry_run=true or confirm=true"),
			InvalidParamsErrorCode);
	}

	TArray<FString> AllowedSourceRoots;
	TArray<FString> AllowedDestinationRoots;
	if (!ReadAllowedRoots(Params, TEXT("allowed_source_roots"), AllowedSourceRoots, Error)
		|| !ReadAllowedRoots(Params, TEXT("allowed_destination_roots"), AllowedDestinationRoots, Error))
	{
		return FMonolithActionResult::Error(Error, InvalidParamsErrorCode);
	}

	TArray<FAssetMovePlan> Plans;
	if (!ReadMoveSpecs(Params, Plans, Error))
	{
		return FMonolithActionResult::Error(Error, InvalidParamsErrorCode);
	}
	if (bCleanupRedirectors && Plans.Num() > MaxCleanupCount)
	{
		return FMonolithActionResult::Error(
			FString::Printf(
				TEXT("cleanup_redirectors=true supports at most %d moves so cleanup can use one fully preflighted delete batch"),
				MaxCleanupCount),
			InvalidParamsErrorCode);
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	int32 PreflightErrorCount = 0;
	for (FAssetMovePlan& Plan : Plans)
	{
		PreflightMove(Plan, AssetRegistry, AllowedSourceRoots, AllowedDestinationRoots);
		PreflightErrorCount += Plan.PreflightErrors.Num();
		Plan.Report->SetStringField(
			TEXT("status"),
			Plan.PreflightErrors.Num() == 0 ? (bDryRun ? TEXT("would_move") : TEXT("ready")) : TEXT("blocked"));
	}

	if (PreflightErrorCount > 0)
	{
		TSharedPtr<FJsonObject> Report = MakeReport(
			Plans,
			AllowedSourceRoots,
			AllowedDestinationRoots,
			bDryRun,
			bConfirm,
			bCleanupRedirectors,
			TEXT("preflight_failed"),
			false);
		Report->SetNumberField(TEXT("preflight_error_count"), PreflightErrorCount);
		Report->SetNumberField(TEXT("loaded_asset_count"), 0);
		Report->SetBoolField(TEXT("accept_cdo_reference_warning"), bAcceptCdoReferenceWarning);
		return FMonolithActionResult::Error(TEXT("asset.move_assets preflight failed"), InvalidParamsErrorCode)
			.WithErrorData(Report);
	}

	if (bDryRun)
	{
		TSharedPtr<FJsonObject> Report = MakeReport(
			Plans,
			AllowedSourceRoots,
			AllowedDestinationRoots,
			true,
			bConfirm,
			bCleanupRedirectors,
			TEXT("dry_run"),
			true);
		Report->SetNumberField(TEXT("would_move_count"), Plans.Num());
		Report->SetNumberField(TEXT("loaded_asset_count"), 0);
		Report->SetBoolField(TEXT("accept_cdo_reference_warning"), bAcceptCdoReferenceWarning);
		return FMonolithActionResult::Success(Report);
	}
	const bool bHasInteractiveEditorContext = GIsEditor
		&& !IsRunningCommandlet()
		&& !FApp::IsUnattended()
		&& !GIsRunningUnattendedScript
		&& IsInGameThread();
	const bool bHasDirectCleanupContext = GIsEditor
		&& !IsRunningCommandlet()
		&& IsInGameThread()
		&& !AssetRegistry.IsLoadingAssets()
		&& ISourceControlModule::Get().IsEnabled()
		&& ISourceControlModule::Get().GetProvider().IsAvailable();
	if ((bAcceptCdoReferenceWarning && !bHasInteractiveEditorContext)
		|| (bCleanupRedirectors && !bHasDirectCleanupContext))
	{
		TSharedPtr<FJsonObject> Report = MakeReport(
			Plans,
			AllowedSourceRoots,
			AllowedDestinationRoots,
			false,
			bConfirm,
			bCleanupRedirectors,
			TEXT("mutation_precondition_failed"),
			false);
		Report->SetBoolField(TEXT("accept_cdo_reference_warning"), bAcceptCdoReferenceWarning);
		Report->SetBoolField(TEXT("is_editor"), GIsEditor);
		Report->SetBoolField(TEXT("is_commandlet"), IsRunningCommandlet());
		Report->SetBoolField(TEXT("app_is_unattended"), FApp::IsUnattended());
		Report->SetBoolField(TEXT("is_running_unattended_script"), GIsRunningUnattendedScript);
		Report->SetBoolField(TEXT("is_in_game_thread"), IsInGameThread());
		Report->SetBoolField(TEXT("asset_registry_loading"), AssetRegistry.IsLoadingAssets());
		Report->SetBoolField(TEXT("source_control_enabled"), ISourceControlModule::Get().IsEnabled());
		Report->SetBoolField(
			TEXT("source_control_available"),
			ISourceControlModule::Get().IsEnabled()
				&& ISourceControlModule::Get().GetProvider().IsAvailable());
		return FMonolithActionResult::Error(
			TEXT("asset.move_assets CDO-warning acceptance requires an attended editor game-thread call; redirector cleanup requires an editor game-thread call, a completed AssetRegistry scan, and available source control"),
			InvalidParamsErrorCode)
			.WithErrorData(Report);
	}

	TArray<FAssetRenameData> RenameData;
	RenameData.Reserve(Plans.Num());
	for (FAssetMovePlan& Plan : Plans)
	{
		UObject* Asset = Plan.SourceAssetData.GetAsset();
		if (!Asset)
		{
			Plan.Report->SetStringField(TEXT("status"), TEXT("failed_to_load"));
			Plan.Report->SetStringField(TEXT("failure_reason"), TEXT("source_asset_load_failed_before_mutation"));
			TSharedPtr<FJsonObject> Report = MakeReport(
				Plans,
				AllowedSourceRoots,
				AllowedDestinationRoots,
				false,
				bConfirm,
				bCleanupRedirectors,
				TEXT("load_failed"),
				false);
			Report->SetNumberField(TEXT("loaded_asset_count"), RenameData.Num());
			Report->SetBoolField(TEXT("accept_cdo_reference_warning"), bAcceptCdoReferenceWarning);
			return FMonolithActionResult::Error(TEXT("asset.move_assets failed to load every source before mutation"))
				.WithErrorData(Report);
		}

		FAssetRenameData Data(Asset, FPackageName::GetLongPackagePath(Plan.DestinationPackage), FPackageName::GetLongPackageAssetName(Plan.DestinationPackage));
		RenameData.Add(MoveTemp(Data));
		Plan.Report->SetStringField(TEXT("status"), TEXT("loaded"));
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	bool bAssetToolsSuccess = false;
	bool bCleanupAttempted = false;
	bool bCleanupSkippedDueToRenameFailure = false;
	UE::Monolith::AssetMove::FCdoModalPolicyState CdoModalPolicyState;
	int32 RedirectorsSubmittedForCleanup = 0;
	int32 CleanupDeletedCount = 0;
	FString CleanupError;
	if (bAcceptCdoReferenceWarning)
	{
		TMap<FString, int32> AllowedAssetNameCounts;
		for (const FAssetMovePlan& Plan : Plans)
		{
			AllowedAssetNameCounts.FindOrAdd(Plan.SourceAssetData.AssetName.ToString()) += 2;
		}
		{
			UE::Monolith::AssetMove::FScopedAssetRenameCdoModalPolicy ModalPolicy(
				MoveTemp(AllowedAssetNameCounts),
				CdoModalPolicyState);
			bAssetToolsSuccess = AssetTools.RenameAssets(RenameData);
		}
	}
	else
	{
		TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
		bAssetToolsSuccess = AssetTools.RenameAssets(RenameData);
	}
	if (CdoModalPolicyState.UnexpectedModalCount > 0)
	{
		bAssetToolsSuccess = false;
	}

	RefreshMovedPaths(AssetRegistry, Plans);
	if (bCleanupRedirectors && bAssetToolsSuccess)
	{
		bCleanupAttempted = true;
		if (CapturePostRenameCleanupState(AssetRegistry, Plans, CleanupError))
		{
			DeleteUnreferencedMovedRedirectors(
				AssetRegistry,
				Plans,
				RedirectorsSubmittedForCleanup,
				CleanupDeletedCount,
				CleanupError);
		}
	}
	else if (bCleanupRedirectors)
	{
		bCleanupSkippedDueToRenameFailure = true;
		CleanupError = TEXT("redirector_cleanup_skipped_because_rename_failed");
	}

	int32 MovedCount = 0;
	for (FAssetMovePlan& Plan : Plans)
	{
		if (CollectPostconditions(Plan, AssetRegistry, bCleanupAttempted))
		{
			++MovedCount;
		}
	}

	const bool bCleanupSuccess = !bCleanupRedirectors
		|| (bCleanupAttempted && CleanupError.IsEmpty() && MovedCount == Plans.Num());
	const bool bSuccess = bAssetToolsSuccess && bCleanupSuccess && MovedCount == Plans.Num();
	const FString Status = bSuccess
		? TEXT("success")
		: (MovedCount > 0 ? TEXT("partial_failure") : TEXT("failed"));
	TSharedPtr<FJsonObject> Report = MakeReport(
		Plans,
		AllowedSourceRoots,
		AllowedDestinationRoots,
		false,
		bConfirm,
		bCleanupRedirectors,
		Status,
		bSuccess);
	Report->SetBoolField(TEXT("asset_tools_success"), bAssetToolsSuccess);
	Report->SetBoolField(TEXT("accept_cdo_reference_warning"), bAcceptCdoReferenceWarning);
	Report->SetBoolField(TEXT("cdo_reference_warning_seen"), CdoModalPolicyState.TargetWarningCount > 0);
	Report->SetNumberField(TEXT("cdo_reference_warning_count"), CdoModalPolicyState.TargetWarningCount);
	Report->SetBoolField(TEXT("cdo_reference_warning_accepted"), CdoModalPolicyState.bTargetWarningAccepted);
	Report->SetBoolField(TEXT("unexpected_modal_encountered"), CdoModalPolicyState.UnexpectedModalCount > 0);
	Report->SetNumberField(TEXT("unexpected_modal_count"), CdoModalPolicyState.UnexpectedModalCount);
	if (!CdoModalPolicyState.UnexpectedModalSummary.IsEmpty())
	{
		Report->SetStringField(TEXT("unexpected_modal_message"), CdoModalPolicyState.UnexpectedModalSummary);
	}
	Report->SetNumberField(TEXT("loaded_asset_count"), RenameData.Num());
	Report->SetNumberField(TEXT("moved_count"), MovedCount);
	Report->SetNumberField(TEXT("failed_count"), Plans.Num() - MovedCount);
	Report->SetBoolField(TEXT("cleanup_requested"), bCleanupRedirectors);
	Report->SetBoolField(TEXT("cleanup_attempted"), bCleanupAttempted);
	Report->SetBoolField(TEXT("cleanup_skipped_due_to_rename_failure"), bCleanupSkippedDueToRenameFailure);
	Report->SetStringField(
		TEXT("cleanup_status"),
		!bCleanupRedirectors
			? TEXT("not_requested")
			: (bCleanupSkippedDueToRenameFailure
				? TEXT("skipped_due_to_rename_failure")
				: (bCleanupSuccess ? TEXT("success") : TEXT("failed"))));
	Report->SetNumberField(TEXT("redirectors_submitted_for_cleanup"), RedirectorsSubmittedForCleanup);
	Report->SetNumberField(TEXT("cleanup_deleted_count"), CleanupDeletedCount);
	Report->SetBoolField(TEXT("cleanup_success"), bCleanupSuccess);
	if (!CleanupError.IsEmpty())
	{
		Report->SetStringField(TEXT("cleanup_error"), CleanupError);
	}

	if (!bSuccess)
	{
		return FMonolithActionResult::Error(TEXT("asset.move_assets failed one or more requested move postconditions"))
			.WithErrorData(Report);
	}
	return FMonolithActionResult::Success(Report);
}

FMonolithActionResult FMonolithAssetMoveActions::CleanupMovedRedirectors(
	const TSharedPtr<FJsonObject>& Params)
{
	bool bDryRun = true;
	bool bConfirm = false;
	FString Error;
	if (!ReadBoolParam(Params, TEXT("dry_run"), bDryRun, Error)
		|| !ReadBoolParam(Params, TEXT("confirm"), bConfirm, Error))
	{
		return FMonolithActionResult::Error(Error, InvalidParamsErrorCode);
	}
	if (!bDryRun && !bConfirm)
	{
		return FMonolithActionResult::Error(
			TEXT("asset.cleanup_moved_redirectors requires dry_run=true or confirm=true"),
			InvalidParamsErrorCode);
	}

	TArray<FString> AllowedSourceRoots;
	TArray<FString> AllowedDestinationRoots;
	if (!ReadAllowedRoots(Params, TEXT("allowed_source_roots"), AllowedSourceRoots, Error)
		|| !ReadAllowedRoots(
			Params,
			TEXT("allowed_destination_roots"),
			AllowedDestinationRoots,
			Error,
			/*bIncludeReadOnlyRoots=*/true))
	{
		return FMonolithActionResult::Error(Error, InvalidParamsErrorCode);
	}

	TArray<FAssetMovePlan> Plans;
	if (!ReadCleanupSpecs(Params, Plans, Error))
	{
		return FMonolithActionResult::Error(Error, InvalidParamsErrorCode);
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	RefreshMovedPaths(AssetRegistry, Plans);
	int32 PreflightErrorCount = 0;
	int32 AlreadyCleanedCount = 0;
	int32 EligibleSourcePackageCount = 0;
	int32 EligibleRedirectorCount = 0;
	for (FAssetMovePlan& Plan : Plans)
	{
		PreflightMovedRedirectorCleanup(
			Plan,
			AssetRegistry,
			AllowedSourceRoots,
			AllowedDestinationRoots);
		PreflightErrorCount += Plan.PreflightErrors.Num();
		AlreadyCleanedCount += Plan.bSourceAlreadyCleaned ? 1 : 0;
		EligibleSourcePackageCount += Plan.SourceAssetData.IsValid() ? 1 : 0;
		EligibleRedirectorCount += Plan.SourceRedirectorAssets.Num();
		Plan.Report->SetStringField(
			TEXT("status"),
			Plan.PreflightErrors.Num() > 0
				? TEXT("blocked")
				: (Plan.bSourceAlreadyCleaned
					? TEXT("already_cleaned")
					: (bDryRun ? TEXT("would_clean") : TEXT("ready"))));
	}

	if (PreflightErrorCount > 0)
	{
		TSharedPtr<FJsonObject> Report = MakeCleanupReport(
			Plans,
			AllowedSourceRoots,
			AllowedDestinationRoots,
			bDryRun,
			bConfirm,
			TEXT("preflight_failed"),
			false);
		Report->SetNumberField(TEXT("preflight_error_count"), PreflightErrorCount);
		Report->SetNumberField(TEXT("already_cleaned_count"), AlreadyCleanedCount);
		Report->SetNumberField(TEXT("eligible_source_package_count"), EligibleSourcePackageCount);
		Report->SetNumberField(TEXT("eligible_redirector_count"), EligibleRedirectorCount);
		Report->SetNumberField(TEXT("loaded_redirector_count"), 0);
		return FMonolithActionResult::Error(
			TEXT("asset.cleanup_moved_redirectors preflight failed"),
			InvalidParamsErrorCode)
			.WithErrorData(Report);
	}
	if (EligibleRedirectorCount > MaxCleanupCount)
	{
		TSharedPtr<FJsonObject> Report = MakeCleanupReport(
			Plans,
			AllowedSourceRoots,
			AllowedDestinationRoots,
			bDryRun,
			bConfirm,
			TEXT("redirector_object_limit_exceeded"),
			false);
		Report->SetNumberField(TEXT("eligible_source_package_count"), EligibleSourcePackageCount);
		Report->SetNumberField(TEXT("eligible_redirector_count"), EligibleRedirectorCount);
		Report->SetNumberField(TEXT("maximum_redirector_count"), MaxCleanupCount);
		Report->SetNumberField(TEXT("loaded_redirector_count"), 0);
		return FMonolithActionResult::Error(
			FString::Printf(
				TEXT("asset.cleanup_moved_redirectors resolved %d redirector objects, exceeding the single-delete batch maximum of %d"),
				EligibleRedirectorCount,
				MaxCleanupCount),
			InvalidParamsErrorCode)
			.WithErrorData(Report);
	}

	if (bDryRun)
	{
		TSharedPtr<FJsonObject> Report = MakeCleanupReport(
			Plans,
			AllowedSourceRoots,
			AllowedDestinationRoots,
			true,
			bConfirm,
			TEXT("dry_run"),
			true);
		Report->SetNumberField(TEXT("would_clean_count"), EligibleSourcePackageCount);
		Report->SetNumberField(TEXT("would_clean_redirector_count"), EligibleRedirectorCount);
		Report->SetNumberField(TEXT("already_cleaned_count"), AlreadyCleanedCount);
		Report->SetNumberField(TEXT("loaded_redirector_count"), 0);
		return FMonolithActionResult::Success(Report);
	}
	const bool bHasMutationContext = GIsEditor
		&& !IsRunningCommandlet()
		&& IsInGameThread()
		&& !AssetRegistry.IsLoadingAssets()
		&& ISourceControlModule::Get().IsEnabled()
		&& ISourceControlModule::Get().GetProvider().IsEnabled()
		&& ISourceControlModule::Get().GetProvider().IsAvailable();
	if (!bHasMutationContext)
	{
		TSharedPtr<FJsonObject> Report = MakeCleanupReport(
			Plans,
			AllowedSourceRoots,
			AllowedDestinationRoots,
			false,
			bConfirm,
			TEXT("mutation_precondition_failed"),
			false);
		Report->SetBoolField(TEXT("is_editor"), GIsEditor);
		Report->SetBoolField(TEXT("is_commandlet"), IsRunningCommandlet());
		Report->SetBoolField(TEXT("is_in_game_thread"), IsInGameThread());
		Report->SetBoolField(TEXT("asset_registry_loading"), AssetRegistry.IsLoadingAssets());
		Report->SetBoolField(TEXT("source_control_enabled"), ISourceControlModule::Get().IsEnabled());
		Report->SetBoolField(
			TEXT("source_control_available"),
			ISourceControlModule::Get().IsEnabled()
				&& ISourceControlModule::Get().GetProvider().IsAvailable());
		return FMonolithActionResult::Error(
			TEXT("asset.cleanup_moved_redirectors mutation requires an editor game-thread call, a completed AssetRegistry scan, and available source control"),
			InvalidParamsErrorCode)
			.WithErrorData(Report);
	}

	ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();
	int32 AlreadyCleanedSourceControlErrorCount = 0;
	for (FAssetMovePlan& Plan : Plans)
	{
		if (!Plan.bSourceAlreadyCleaned)
		{
			continue;
		}

		FString ExpectedSourceFilename;
		const FString DestinationExtension = FPaths::GetExtension(Plan.DestinationFilename, /*bIncludeDot=*/true);
		const FString SourceExtension = DestinationExtension.IsEmpty()
			? FPackageName::GetAssetPackageExtension()
			: DestinationExtension;
		if (!FPackageName::TryConvertLongPackageNameToFilename(
				Plan.SourcePackage,
				ExpectedSourceFilename,
				SourceExtension))
		{
			AddPreflightError(Plan, TEXT("source_control_filename_resolution_failed"));
			++AlreadyCleanedSourceControlErrorCount;
			continue;
		}
		ExpectedSourceFilename = FPaths::ConvertRelativePathToFull(ExpectedSourceFilename);
		FPaths::NormalizeFilename(ExpectedSourceFilename);
		const FSourceControlStatePtr State = SourceControlProvider.GetState(
			ExpectedSourceFilename,
			EStateCacheUsage::ForceUpdate);
		const bool bStateProvesCleaned = State.IsValid()
			&& !State->IsUnknown()
			&& (State->IsDeleted()
				|| (!State->IsSourceControlled() && !State->IsAdded()));
		Plan.Report->SetStringField(TEXT("source_control_filename"), ExpectedSourceFilename);
		Plan.Report->SetBoolField(TEXT("source_control_state_valid"), State.IsValid());
		if (State.IsValid())
		{
			Plan.Report->SetBoolField(TEXT("source_control_unknown"), State->IsUnknown());
			Plan.Report->SetBoolField(TEXT("source_control_source_controlled"), State->IsSourceControlled());
			Plan.Report->SetBoolField(TEXT("source_control_added"), State->IsAdded());
			Plan.Report->SetBoolField(TEXT("source_control_deleted"), State->IsDeleted());
		}
		Plan.Report->SetBoolField(TEXT("source_control_cleanup_postcondition_met"), bStateProvesCleaned);
		if (!bStateProvesCleaned)
		{
			AddPreflightError(Plan, TEXT("source_control_delete_postcondition_missing"));
			Plan.Report->SetStringField(TEXT("status"), TEXT("blocked"));
			++AlreadyCleanedSourceControlErrorCount;
		}
	}
	if (AlreadyCleanedSourceControlErrorCount > 0)
	{
		TSharedPtr<FJsonObject> Report = MakeCleanupReport(
			Plans,
			AllowedSourceRoots,
			AllowedDestinationRoots,
			false,
			bConfirm,
			TEXT("source_control_preflight_failed"),
			false);
		Report->SetNumberField(TEXT("source_control_error_count"), AlreadyCleanedSourceControlErrorCount);
		Report->SetNumberField(TEXT("already_cleaned_count"), AlreadyCleanedCount);
		Report->SetNumberField(TEXT("eligible_source_package_count"), EligibleSourcePackageCount);
		Report->SetNumberField(TEXT("eligible_redirector_count"), EligibleRedirectorCount);
		return FMonolithActionResult::Error(
			TEXT("asset.cleanup_moved_redirectors could not prove source-control cleanup state before mutation"),
			InvalidParamsErrorCode)
			.WithErrorData(Report);
	}

	if (EligibleRedirectorCount == 0)
	{
		TSharedPtr<FJsonObject> Report = MakeCleanupReport(
			Plans,
			AllowedSourceRoots,
			AllowedDestinationRoots,
			false,
			bConfirm,
			TEXT("already_cleaned"),
			true);
		Report->SetNumberField(TEXT("already_cleaned_count"), AlreadyCleanedCount);
		Report->SetNumberField(TEXT("eligible_source_package_count"), 0);
		Report->SetNumberField(TEXT("eligible_redirector_count"), 0);
		Report->SetNumberField(TEXT("redirectors_submitted_for_delete"), 0);
		Report->SetNumberField(TEXT("delete_objects_return_count"), 0);
		Report->SetNumberField(TEXT("postcondition_success_count"), Plans.Num());
		Report->SetNumberField(TEXT("failed_count"), 0);
		return FMonolithActionResult::Success(Report);
	}

	int32 SubmittedRedirectorCount = 0;
	int32 DeletedObjectCount = 0;
	const bool bDeleteCallSucceeded = DeleteUnreferencedMovedRedirectors(
		AssetRegistry,
		Plans,
		SubmittedRedirectorCount,
		DeletedObjectCount,
		Error);

	int32 PostconditionSuccessCount = 0;
	for (FAssetMovePlan& Plan : Plans)
	{
		if (CollectMovedRedirectorCleanupPostconditions(Plan, AssetRegistry))
		{
			++PostconditionSuccessCount;
		}
	}
	const bool bSuccess = bDeleteCallSucceeded && PostconditionSuccessCount == Plans.Num();
	const FString Status = bSuccess
		? TEXT("success")
		: (PostconditionSuccessCount > 0 ? TEXT("partial_failure") : TEXT("failed"));
	TSharedPtr<FJsonObject> Report = MakeCleanupReport(
		Plans,
		AllowedSourceRoots,
		AllowedDestinationRoots,
		false,
		bConfirm,
		Status,
		bSuccess);
	Report->SetNumberField(TEXT("already_cleaned_count"), AlreadyCleanedCount);
	Report->SetNumberField(TEXT("eligible_source_package_count"), EligibleSourcePackageCount);
	Report->SetNumberField(TEXT("eligible_redirector_count"), EligibleRedirectorCount);
	Report->SetNumberField(TEXT("redirectors_submitted_for_delete"), SubmittedRedirectorCount);
	Report->SetNumberField(TEXT("delete_objects_return_count"), DeletedObjectCount);
	Report->SetNumberField(TEXT("postcondition_success_count"), PostconditionSuccessCount);
	Report->SetNumberField(TEXT("failed_count"), Plans.Num() - PostconditionSuccessCount);
	if (!Error.IsEmpty())
	{
		Report->SetStringField(TEXT("cleanup_error"), Error);
	}

	if (!bSuccess)
	{
		return FMonolithActionResult::Error(
			TEXT("asset.cleanup_moved_redirectors failed one or more exact cleanup postconditions"))
			.WithErrorData(Report);
	}
	return FMonolithActionResult::Success(Report);
}
