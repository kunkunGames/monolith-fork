#include "MonolithAssetPackageGraphActions.h"

#include "MonolithParamSchema.h"

#include "AssetToolsModule.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetIdentifier.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace
{
	static constexpr int32 ErrInvalidParams = -32602;

	struct FRootRemap
	{
		FString SourceRoot;
		FString DestinationRoot;
	};

	struct FMutationOptions
	{
		bool bDryRun = false;
		bool bConfirm = false;
		bool bSave = true;
		bool bStrict = true;
	};

	struct FPackageCopyRow
	{
		FString SourcePackage;
		FString DestinationPackage;
		bool bSourceExists = false;
		bool bDestinationExists = false;
		FAssetData SourceAsset;
	};

	struct FReferenceFixupOptions
	{
		FMutationOptions Mutation;
		int32 MaxPackages = 1000;
		bool bRequireTargets = true;
	};

	struct FReferenceFixupStats
	{
		int32 CheckedPackageCount = 0;
		int32 CheckedObjectCount = 0;
		int32 CandidateCount = 0;
		int32 AppliedCount = 0;
		bool bTruncated = false;
		bool bHasBlockingErrors = false;
		TSet<FString> ChangedPackages;
		TArray<TSharedPtr<FJsonValue>> References;
		TArray<TSharedPtr<FJsonValue>> Warnings;
	};

	enum class EDependencyKind : uint8
	{
		Hard,
		Soft
	};

	static TSharedPtr<FJsonObject> ErrorData(const FString& Field, const FString& Detail)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("field"), Field);
		Data->SetStringField(TEXT("detail"), Detail);
		return Data;
	}

	static TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Rows.Add(MakeShared<FJsonValueString>(Value));
		}
		return Rows;
	}

	static FString NormalizeRoot(FString Root)
	{
		Root.TrimStartAndEndInline();
		Root.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (Root.EndsWith(TEXT("/")) && Root.Len() > 1)
		{
			Root.LeftChopInline(1);
		}
		return Root;
	}

	static FString NormalizePackagePath(FString Path)
	{
		Path.TrimStartAndEndInline();
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (Path.Contains(TEXT(".")))
		{
			Path = FPackageName::ObjectPathToPackageName(Path);
		}
		if (Path.EndsWith(TEXT("_C")))
		{
			Path.LeftChopInline(2);
		}
		while (Path.EndsWith(TEXT("/")) && Path.Len() > 1)
		{
			Path.LeftChopInline(1);
		}
		return Path;
	}

	static bool IsValidPackageOrRoot(const FString& Path)
	{
		return Path.StartsWith(TEXT("/")) && !Path.Contains(TEXT("//")) && Path.Len() > 1;
	}

	static bool IsUnderRoot(const FString& PackagePath, const FString& Root)
	{
		return PackagePath.Equals(Root, ESearchCase::IgnoreCase)
			|| PackagePath.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase);
	}

	static bool IsUnderAnyRoot(const FString& PackagePath, const TArray<FString>& Roots)
	{
		for (const FString& Root : Roots)
		{
			if (IsUnderRoot(PackagePath, Root))
			{
				return true;
			}
		}
		return false;
	}

	static bool ReadStringArrayParam(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		bool bRequired,
		TArray<FString>& OutValues,
		FString& OutError)
	{
		OutValues.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(FieldName, Values) || !Values)
		{
			if (bRequired)
			{
				OutError = FString::Printf(TEXT("Missing required array param '%s'"), FieldName);
				return false;
			}
			return true;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString StringValue;
			if (!Value.IsValid() || !Value->TryGetString(StringValue))
			{
				OutError = FString::Printf(TEXT("Param '%s' must be an array of strings"), FieldName);
				return false;
			}
			StringValue = NormalizePackagePath(StringValue);
			if (!StringValue.IsEmpty())
			{
				OutValues.AddUnique(StringValue);
			}
		}

		if (bRequired && OutValues.Num() == 0)
		{
			OutError = FString::Printf(TEXT("Param '%s' must contain at least one path"), FieldName);
			return false;
		}
		return true;
	}

	static bool ReadBoolParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool& InOutValue, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		if (!Params->TryGetBoolField(FieldName, InOutValue))
		{
			OutError = FString::Printf(TEXT("Param '%s' must be a boolean"), FieldName);
			return false;
		}
		return true;
	}

	static bool ReadStringParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FString& InOutValue, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		if (!Params->TryGetStringField(FieldName, InOutValue))
		{
			OutError = FString::Printf(TEXT("Param '%s' must be a string"), FieldName);
			return false;
		}
		InOutValue.TrimStartAndEndInline();
		return true;
	}

	static bool ReadMutationOptions(
		const TSharedPtr<FJsonObject>& Params,
		FMutationOptions& InOutOptions,
		FString& OutError)
	{
		if (!ReadBoolParam(Params, TEXT("dry_run"), InOutOptions.bDryRun, OutError)
			|| !ReadBoolParam(Params, TEXT("confirm"), InOutOptions.bConfirm, OutError)
			|| !ReadBoolParam(Params, TEXT("save"), InOutOptions.bSave, OutError)
			|| !ReadBoolParam(Params, TEXT("strict"), InOutOptions.bStrict, OutError))
		{
			return false;
		}
		if (!InOutOptions.bDryRun && !InOutOptions.bConfirm)
		{
			OutError = TEXT("Mutating package graph actions require dry_run=true or confirm=true");
			return false;
		}
		return true;
	}

	static bool ReadIntParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, int32& InOutValue, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		double Number = 0.0;
		if (!Params->TryGetNumberField(FieldName, Number))
		{
			OutError = FString::Printf(TEXT("Param '%s' must be a number"), FieldName);
			return false;
		}
		InOutValue = FMath::Clamp(static_cast<int32>(Number), 1, 10000);
		return true;
	}

	static bool ReadDependencyKinds(
		const TSharedPtr<FJsonObject>& Params,
		bool bDefaultHard,
		bool bDefaultSoft,
		TArray<EDependencyKind>& OutKinds,
		FString& OutError)
	{
		OutKinds.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Params.IsValid() && Params->TryGetArrayField(TEXT("dependency_kinds"), Values) && Values)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString Kind;
				if (!Value.IsValid() || !Value->TryGetString(Kind))
				{
					OutError = TEXT("Param 'dependency_kinds' must be an array of 'hard'/'soft' strings");
					return false;
				}
				if (Kind.Equals(TEXT("hard"), ESearchCase::IgnoreCase))
				{
					OutKinds.AddUnique(EDependencyKind::Hard);
				}
				else if (Kind.Equals(TEXT("soft"), ESearchCase::IgnoreCase))
				{
					OutKinds.AddUnique(EDependencyKind::Soft);
				}
				else
				{
					OutError = FString::Printf(TEXT("Unsupported dependency kind '%s'; expected 'hard' or 'soft'"), *Kind);
					return false;
				}
			}
		}
		else
		{
			if (bDefaultHard)
			{
				OutKinds.Add(EDependencyKind::Hard);
			}
			if (bDefaultSoft)
			{
				OutKinds.Add(EDependencyKind::Soft);
			}
		}

		if (OutKinds.Num() == 0)
		{
			OutError = TEXT("At least one dependency kind must be enabled");
			return false;
		}
		return true;
	}

	static bool ReadRootRemaps(const TSharedPtr<FJsonObject>& Params, TArray<FRootRemap>& OutRemaps, FString& OutError)
	{
		OutRemaps.Reset();
		const TSharedPtr<FJsonObject>* RemapObject = nullptr;
		if (!Params.IsValid() || !Params->TryGetObjectField(TEXT("root_remaps"), RemapObject) || !RemapObject || !RemapObject->IsValid())
		{
			OutError = TEXT("Missing required object param 'root_remaps'");
			return false;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*RemapObject)->Values)
		{
			FString Destination;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(Destination))
			{
				OutError = TEXT("Param 'root_remaps' must map source root strings to destination root strings");
				return false;
			}

			FRootRemap Remap;
			Remap.SourceRoot = NormalizeRoot(Pair.Key);
			Remap.DestinationRoot = NormalizeRoot(Destination);
			if (!IsValidPackageOrRoot(Remap.SourceRoot) || !IsValidPackageOrRoot(Remap.DestinationRoot))
			{
				OutError = FString::Printf(
					TEXT("Invalid root remap '%s' -> '%s'; roots must be long package roots"),
					*Remap.SourceRoot,
					*Remap.DestinationRoot);
				return false;
			}
			OutRemaps.Add(Remap);
		}

		OutRemaps.Sort([](const FRootRemap& A, const FRootRemap& B)
		{
			return A.SourceRoot.Len() > B.SourceRoot.Len();
		});

		if (OutRemaps.Num() == 0)
		{
			OutError = TEXT("Param 'root_remaps' must contain at least one mapping");
			return false;
		}
		return true;
	}

	static TArray<FString> SourceRootsFromRemaps(const TArray<FRootRemap>& Remaps)
	{
		TArray<FString> Roots;
		for (const FRootRemap& Remap : Remaps)
		{
			Roots.AddUnique(Remap.SourceRoot);
		}
		return Roots;
	}

	static TArray<FString> DestinationRootsFromRemaps(const TArray<FRootRemap>& Remaps)
	{
		TArray<FString> Roots;
		for (const FRootRemap& Remap : Remaps)
		{
			Roots.AddUnique(Remap.DestinationRoot);
		}
		return Roots;
	}

	static TSharedPtr<FJsonObject> CloneParams(const TSharedPtr<FJsonObject>& Params)
	{
		TSharedPtr<FJsonObject> Clone = MakeShared<FJsonObject>();
		if (Params.IsValid())
		{
			Clone->Values = Params->Values;
		}
		return Clone;
	}

	static void SetStringArrayField(TSharedPtr<FJsonObject> Object, const TCHAR* FieldName, const TArray<FString>& Values)
	{
		if (Object.IsValid())
		{
			Object->SetArrayField(FieldName, StringsToJson(Values));
		}
	}

	static TArray<FString> DestinationPackagesFromPlan(const TSharedPtr<FJsonObject>& Plan)
	{
		TArray<FString> DestinationPackages;
		const TArray<TSharedPtr<FJsonValue>>* PackageMap = nullptr;
		if (!Plan.IsValid() || !Plan->TryGetArrayField(TEXT("package_map"), PackageMap) || !PackageMap)
		{
			return DestinationPackages;
		}

		for (const TSharedPtr<FJsonValue>& Value : *PackageMap)
		{
			const TSharedPtr<FJsonObject> Row = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Row.IsValid())
			{
				continue;
			}
			FString DestinationPackage;
			if (Row->TryGetStringField(TEXT("destination_package"), DestinationPackage))
			{
				DestinationPackages.AddUnique(NormalizePackagePath(DestinationPackage));
			}
		}
		DestinationPackages.Sort();
		return DestinationPackages;
	}

	static TSharedPtr<FJsonObject> MakePhaseRow(
		const FString& PhaseName,
		const FString& Status,
		bool bOk,
		const FString& ActionName,
		const FString& Detail = FString())
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("phase"), PhaseName);
		Row->SetStringField(TEXT("status"), Status);
		Row->SetBoolField(TEXT("ok"), bOk);
		if (!ActionName.IsEmpty())
		{
			Row->SetStringField(TEXT("action"), ActionName);
		}
		if (!Detail.IsEmpty())
		{
			Row->SetStringField(TEXT("detail"), Detail);
		}
		return Row;
	}

	static bool IsWorkflowStrategy(const FString& Value)
	{
		return Value.Equals(TEXT("plan_only"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("copy_only"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("copy_fixup"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("copy_fixup_validate"), ESearchCase::IgnoreCase);
	}

	static bool IsCopyStrategy(const FString& Value)
	{
		return Value.Equals(TEXT("auto"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("duplicate_asset"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("advanced_copy"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("raw_package_file_copy"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("header_patched_advanced_copy"), ESearchCase::IgnoreCase);
	}

	static bool ReadStrategyAlias(
		const TSharedPtr<FJsonObject>& Params,
		FString& InOutWorkflow,
		FString& InOutCopyStrategy,
		FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(TEXT("strategy")))
		{
			return true;
		}

		FString Strategy;
		if (!Params->TryGetStringField(TEXT("strategy"), Strategy))
		{
			OutError = TEXT("Param 'strategy' must be a string");
			return false;
		}
		Strategy.TrimStartAndEndInline();
		if (IsWorkflowStrategy(Strategy))
		{
			InOutWorkflow = Strategy;
			return true;
		}
		if (IsCopyStrategy(Strategy))
		{
			InOutCopyStrategy = Strategy;
			return true;
		}

		OutError = FString::Printf(
			TEXT("Unsupported strategy '%s'; expected workflow plan_only/copy_only/copy_fixup/copy_fixup_validate or copy strategy auto/duplicate_asset/advanced_copy/raw_package_file_copy/header_patched_advanced_copy"),
			*Strategy);
		return false;
	}

	static bool IsPackageSelected(const FString& PackagePath, const TArray<FString>& Roots, const TArray<FString>& Packages)
	{
		if (Packages.Contains(PackagePath))
		{
			return true;
		}
		return IsUnderAnyRoot(PackagePath, Roots);
	}

	static bool BuildCopyStrategyPlan(
		const TSharedPtr<FJsonObject>& Plan,
		const FString& RequestedCopyStrategy,
		const TArray<FString>& HeaderPatchedRoots,
		const TArray<FString>& HeaderPatchedPackages,
		const TArray<FString>& RawPackageRoots,
		const TArray<FString>& RawPackagePackages,
		const TArray<FString>& ManualCopyRoots,
		const TArray<FString>& ManualCopyPackages,
		bool bAllowRawPackageCopy,
		TArray<TSharedPtr<FJsonValue>>& OutStrategyRows,
		int32& OutUnsupportedCount,
		int32& OutExecutableCount)
	{
		OutStrategyRows.Reset();
		OutUnsupportedCount = 0;
		OutExecutableCount = 0;

		const TArray<TSharedPtr<FJsonValue>>* PackageMap = nullptr;
		if (!Plan.IsValid() || !Plan->TryGetArrayField(TEXT("package_map"), PackageMap) || !PackageMap)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *PackageMap)
		{
			const TSharedPtr<FJsonObject> PackageRow = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!PackageRow.IsValid())
			{
				continue;
			}

			FString SourcePackage;
			FString DestinationPackage;
			PackageRow->TryGetStringField(TEXT("source_package"), SourcePackage);
			PackageRow->TryGetStringField(TEXT("destination_package"), DestinationPackage);
			SourcePackage = NormalizePackagePath(SourcePackage);
			DestinationPackage = NormalizePackagePath(DestinationPackage);

			FString SelectedStrategy = RequestedCopyStrategy;
			FString Reason = TEXT("requested_strategy");
			bool bExecutableByThisAction = SelectedStrategy.Equals(TEXT("duplicate_asset"), ESearchCase::IgnoreCase);

			const bool bManualSelected = IsPackageSelected(SourcePackage, ManualCopyRoots, ManualCopyPackages);
			const bool bHeaderPatchedSelected = IsPackageSelected(SourcePackage, HeaderPatchedRoots, HeaderPatchedPackages);
			const bool bRawSelected = IsPackageSelected(SourcePackage, RawPackageRoots, RawPackagePackages);

			if (RequestedCopyStrategy.Equals(TEXT("auto"), ESearchCase::IgnoreCase))
			{
				if (bManualSelected)
				{
					SelectedStrategy = TEXT("manual_single_object_duplicate");
					Reason = TEXT("manual_copy_selector");
					bExecutableByThisAction = false;
				}
				else if (bHeaderPatchedSelected)
				{
					SelectedStrategy = TEXT("header_patched_advanced_copy");
					Reason = TEXT("header_patched_selector");
					bExecutableByThisAction = false;
				}
				else if (bRawSelected)
				{
					SelectedStrategy = TEXT("raw_package_file_copy");
					Reason = TEXT("raw_package_selector");
					bExecutableByThisAction = false;
				}
				else
				{
					SelectedStrategy = TEXT("duplicate_asset");
					Reason = TEXT("default_duplicate_asset");
					bExecutableByThisAction = true;
				}
			}
			else if (SelectedStrategy.Equals(TEXT("raw_package_file_copy"), ESearchCase::IgnoreCase) && !bAllowRawPackageCopy)
			{
				bExecutableByThisAction = false;
				Reason = TEXT("allow_raw_package_copy_false");
			}
			else if (!SelectedStrategy.Equals(TEXT("duplicate_asset"), ESearchCase::IgnoreCase))
			{
				bExecutableByThisAction = false;
				Reason = TEXT("strategy_execution_deferred");
			}

			const bool bSupportedStrategy = SelectedStrategy.Equals(TEXT("duplicate_asset"), ESearchCase::IgnoreCase);
			const FString Status = bExecutableByThisAction
				? TEXT("ready")
				: (bSupportedStrategy ? TEXT("blocked") : TEXT("unsupported"));

			if (bExecutableByThisAction)
			{
				++OutExecutableCount;
			}
			else
			{
				++OutUnsupportedCount;
			}

			TSharedPtr<FJsonObject> StrategyRow = MakeShared<FJsonObject>();
			StrategyRow->SetStringField(TEXT("source_package"), SourcePackage);
			StrategyRow->SetStringField(TEXT("destination_package"), DestinationPackage);
			StrategyRow->SetStringField(TEXT("requested_strategy"), RequestedCopyStrategy);
			StrategyRow->SetStringField(TEXT("selected_strategy"), SelectedStrategy);
			StrategyRow->SetStringField(TEXT("status"), Status);
			StrategyRow->SetStringField(TEXT("reason"), Reason);
			StrategyRow->SetBoolField(TEXT("executable_by_this_action"), bExecutableByThisAction);
			OutStrategyRows.Add(MakeShared<FJsonValueObject>(StrategyRow));
		}

		return OutUnsupportedCount == 0;
	}

	static bool ApplyRootRemap(const FString& SourcePackage, const TArray<FRootRemap>& Remaps, FString& OutDestination)
	{
		for (const FRootRemap& Remap : Remaps)
		{
			if (IsUnderRoot(SourcePackage, Remap.SourceRoot))
			{
				const FString Suffix = SourcePackage.Mid(Remap.SourceRoot.Len());
				OutDestination = Remap.DestinationRoot + Suffix;
				return true;
			}
		}
		return false;
	}

	static FString DependencyKindToString(EDependencyKind Kind)
	{
		return Kind == EDependencyKind::Hard ? TEXT("hard") : TEXT("soft");
	}

	static UE::AssetRegistry::EDependencyQuery DependencyQueryForKind(EDependencyKind Kind)
	{
		return Kind == EDependencyKind::Hard
			? UE::AssetRegistry::EDependencyQuery::Hard
			: UE::AssetRegistry::EDependencyQuery::Soft;
	}

	static FMonolithActionExecutionPolicy ExplicitReadOnlyPolicy()
	{
		FMonolithActionExecutionPolicy Policy = FMonolithActionExecutionPolicy::DefaultReadOnly();
		Policy.bDefaulted = false;
		return Policy;
	}

	static FMonolithActionExecutionPolicy MutatingAssetPolicy()
	{
		FMonolithActionExecutionPolicy Policy;
		Policy.PolicyId = TEXT("transaction_optional");
		Policy.bDefaulted = false;
		Policy.bDirtyPackageTracking = true;
		Policy.bTransactionWrapping = true;
		Policy.bPostEditValidation = false;
		Policy.bEnforced = true;
		return Policy;
	}

	static void AppendDependencies(
		IAssetRegistry& AssetRegistry,
		const FString& PackagePath,
		const TArray<EDependencyKind>& Kinds,
		TArray<TPair<FString, EDependencyKind>>& OutDependencies)
	{
		for (EDependencyKind Kind : Kinds)
		{
			TArray<FAssetIdentifier> Dependencies;
			AssetRegistry.GetDependencies(
				FName(*PackagePath),
				Dependencies,
				UE::AssetRegistry::EDependencyCategory::Package,
				DependencyQueryForKind(Kind));

			for (const FAssetIdentifier& Dependency : Dependencies)
			{
				const FString DependencyPackage = NormalizePackagePath(Dependency.PackageName.ToString());
				if (!DependencyPackage.IsEmpty())
				{
					OutDependencies.Add(TPair<FString, EDependencyKind>(DependencyPackage, Kind));
				}
			}
		}
	}

	static TSharedPtr<FJsonObject> EdgeToJson(
		const FString& Source,
		const FString& Target,
		EDependencyKind Kind,
		bool bWillCopy,
		const FString& DestinationSource,
		const FString& DestinationTarget)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("source_package"), Source);
		Row->SetStringField(TEXT("target_package"), Target);
		Row->SetStringField(TEXT("kind"), DependencyKindToString(Kind));
		Row->SetBoolField(TEXT("target_will_copy"), bWillCopy);
		Row->SetStringField(TEXT("destination_source_package"), DestinationSource);
		Row->SetStringField(TEXT("destination_target_package"), DestinationTarget);
		return Row;
	}

	static TArray<FString> ScanPackagesUnderRoots(IAssetRegistry& AssetRegistry, const TArray<FString>& Roots)
	{
		TArray<FString> Packages;
		for (const FString& Root : Roots)
		{
			FARFilter Filter;
			Filter.PackagePaths.Add(FName(*Root));
			Filter.bRecursivePaths = true;

			TArray<FAssetData> Assets;
			AssetRegistry.GetAssets(Filter, Assets);
			for (const FAssetData& Asset : Assets)
			{
				Packages.AddUnique(Asset.PackageName.ToString());
			}
		}
		Packages.Sort();
		return Packages;
	}

	static bool PackageExists(IAssetRegistry& AssetRegistry, const FString& PackagePath)
	{
		TArray<FAssetData> Assets;
		AssetRegistry.GetAssetsByPackageName(FName(*PackagePath), Assets, /*bIncludeOnlyOnDiskAssets=*/false);
		return Assets.Num() > 0;
	}

	static FString ObjectPathForPackageAndObjectName(const FString& PackagePath, const FString& ObjectName)
	{
		return PackagePath + TEXT(".") + ObjectName;
	}

	static FString PrimaryObjectPathForPackage(const FString& PackagePath)
	{
		return ObjectPathForPackageAndObjectName(PackagePath, FPaths::GetBaseFilename(PackagePath));
	}

	static bool TryRemapObjectPath(
		const FString& InObjectPath,
		const TArray<FRootRemap>& Remaps,
		FString& OutObjectPath,
		FString& OutSourcePackage,
		FString& OutDestinationPackage)
	{
		FString NormalizedObjectPath = InObjectPath;
		NormalizedObjectPath.TrimStartAndEndInline();
		NormalizedObjectPath.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (NormalizedObjectPath.IsEmpty())
		{
			return false;
		}

		FSoftObjectPath SoftPath(NormalizedObjectPath);
		FString AssetPath = SoftPath.GetAssetPathString();
		if (AssetPath.IsEmpty())
		{
			AssetPath = NormalizedObjectPath;
		}

		FString ObjectName;
		if (AssetPath.Contains(TEXT(".")))
		{
			OutSourcePackage = NormalizePackagePath(FPackageName::ObjectPathToPackageName(AssetPath));
			ObjectName = FPackageName::ObjectPathToObjectName(AssetPath);
		}
		else
		{
			OutSourcePackage = NormalizePackagePath(AssetPath);
			ObjectName = FPaths::GetBaseFilename(OutSourcePackage);
		}

		if (!ApplyRootRemap(OutSourcePackage, Remaps, OutDestinationPackage))
		{
			return false;
		}

		OutObjectPath = ObjectPathForPackageAndObjectName(OutDestinationPackage, ObjectName);
		const FString SubPath = SoftPath.GetSubPathString();
		if (!SubPath.IsEmpty())
		{
			OutObjectPath += TEXT(":") + SubPath;
		}
		return true;
	}

	static TSharedPtr<FJsonObject> MakeReferenceRow(
		const FString& PackagePath,
		const FString& ObjectPath,
		const FString& PropertyPath,
		const FString& ReferenceKind,
		const FString& OldPath,
		const FString& NewPath,
		bool bApplied,
		const FString& Status)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("package_path"), PackagePath);
		Row->SetStringField(TEXT("object_path"), ObjectPath);
		Row->SetStringField(TEXT("property_path"), PropertyPath);
		Row->SetStringField(TEXT("reference_kind"), ReferenceKind);
		Row->SetStringField(TEXT("old_path"), OldPath);
		Row->SetStringField(TEXT("new_path"), NewPath);
		Row->SetBoolField(TEXT("applied"), bApplied);
		Row->SetStringField(TEXT("status"), Status);
		return Row;
	}

	static void AddWarning(FReferenceFixupStats& Stats, const FString& PackagePath, const FString& Detail)
	{
		TSharedPtr<FJsonObject> Warning = MakeShared<FJsonObject>();
		Warning->SetStringField(TEXT("package_path"), PackagePath);
		Warning->SetStringField(TEXT("detail"), Detail);
		Stats.Warnings.Add(MakeShared<FJsonValueObject>(Warning));
	}

	static bool DoesPackageContainWorld(UPackage* Package)
	{
		if (!Package)
		{
			return false;
		}

		bool bContainsWorld = false;
		ForEachObjectWithPackage(Package, [&bContainsWorld](UObject* Object)
		{
			if (Object && Object->IsA<UWorld>())
			{
				bContainsWorld = true;
				return false;
			}
			return true;
		}, EGetObjectsFlags::IncludeNestedObjects);
		return bContainsWorld;
	}

	static bool SavePackageIfRequested(UPackage* Package, bool bSave, FString& OutSavedFilename, FString& OutError)
	{
		if (!bSave || !Package)
		{
			return true;
		}

		const FString PackageName = Package->GetName();
		const FString Extension = DoesPackageContainWorld(Package)
			? FPackageName::GetMapPackageExtension()
			: FPackageName::GetAssetPackageExtension();
		OutSavedFilename = FPackageName::LongPackageNameToFilename(PackageName, Extension);

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Package, nullptr, *OutSavedFilename, SaveArgs))
		{
			OutError = FString::Printf(TEXT("UPackage::SavePackage failed for '%s'"), *OutSavedFilename);
			return false;
		}
		return true;
	}

	static bool SelectPrimaryAssetForPackage(
		IAssetRegistry& AssetRegistry,
		const FString& PackagePath,
		FAssetData& OutAssetData,
		FString& OutError)
	{
		TArray<FAssetData> Assets;
		AssetRegistry.GetAssetsByPackageName(FName(*PackagePath), Assets, /*bIncludeOnlyOnDiskAssets=*/false);
		if (Assets.Num() == 0)
		{
			OutError = FString::Printf(TEXT("Source package has no asset data: %s"), *PackagePath);
			return false;
		}

		const FString ExpectedName = FPaths::GetBaseFilename(PackagePath);
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetName.ToString().Equals(ExpectedName, ESearchCase::IgnoreCase))
			{
				OutAssetData = Asset;
				return true;
			}
		}

		OutAssetData = Assets[0];
		return true;
	}

	static bool ExtractPackageMapRows(
		IAssetRegistry& AssetRegistry,
		const TSharedPtr<FJsonObject>& Plan,
		const FString& CollisionPolicy,
		TArray<FPackageCopyRow>& OutRows,
		TArray<TSharedPtr<FJsonValue>>& OutErrors)
	{
		OutRows.Reset();
		OutErrors.Reset();

		const TArray<TSharedPtr<FJsonValue>>* PackageMap = nullptr;
		if (!Plan.IsValid() || !Plan->TryGetArrayField(TEXT("package_map"), PackageMap) || !PackageMap)
		{
			TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("detail"), TEXT("Plan result did not include package_map"));
			OutErrors.Add(MakeShared<FJsonValueObject>(Error));
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *PackageMap)
		{
			const TSharedPtr<FJsonObject> RowObject = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!RowObject.IsValid())
			{
				continue;
			}

			FPackageCopyRow Row;
			RowObject->TryGetStringField(TEXT("source_package"), Row.SourcePackage);
			RowObject->TryGetStringField(TEXT("destination_package"), Row.DestinationPackage);
			RowObject->TryGetBoolField(TEXT("source_exists"), Row.bSourceExists);
			RowObject->TryGetBoolField(TEXT("destination_exists"), Row.bDestinationExists);

			FString ErrorText;
			const bool bHasSourceAsset = SelectPrimaryAssetForPackage(AssetRegistry, Row.SourcePackage, Row.SourceAsset, ErrorText);
			if (!Row.bSourceExists || !bHasSourceAsset)
			{
				TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
				Error->SetStringField(TEXT("source_package"), Row.SourcePackage);
				Error->SetStringField(TEXT("destination_package"), Row.DestinationPackage);
				Error->SetStringField(TEXT("reason"), TEXT("source_missing"));
				Error->SetStringField(TEXT("detail"), ErrorText.IsEmpty() ? TEXT("Source package does not exist") : ErrorText);
				OutErrors.Add(MakeShared<FJsonValueObject>(Error));
				continue;
			}

			if (Row.bDestinationExists && CollisionPolicy.Equals(TEXT("fail_if_exists"), ESearchCase::IgnoreCase))
			{
				TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
				Error->SetStringField(TEXT("source_package"), Row.SourcePackage);
				Error->SetStringField(TEXT("destination_package"), Row.DestinationPackage);
				Error->SetStringField(TEXT("reason"), TEXT("destination_exists"));
				Error->SetStringField(TEXT("detail"), TEXT("Destination package exists; use collision_policy=skip_existing to leave it untouched"));
				OutErrors.Add(MakeShared<FJsonValueObject>(Error));
				continue;
			}

			OutRows.Add(Row);
		}

		return OutErrors.Num() == 0;
	}

	static bool FixupObjectProperty(
		FObjectPropertyBase* ObjectProperty,
		void* ValuePtr,
		const FString& PackagePath,
		const FString& ObjectPath,
		const FString& PropertyPath,
		const TArray<FRootRemap>& Remaps,
		const FReferenceFixupOptions& Options,
		FReferenceFixupStats& Stats)
	{
		UObject* OldObject = ObjectProperty ? ObjectProperty->GetObjectPropertyValue(ValuePtr) : nullptr;
		if (!OldObject)
		{
			return false;
		}

		FString NewObjectPath;
		FString SourcePackage;
		FString DestinationPackage;
		if (!TryRemapObjectPath(OldObject->GetPathName(), Remaps, NewObjectPath, SourcePackage, DestinationPackage))
		{
			return false;
		}

		++Stats.CandidateCount;
		UObject* NewObject = FSoftObjectPath(NewObjectPath).TryLoad();
		if (!NewObject)
		{
			const FString Status = TEXT("target_missing");
			Stats.References.Add(MakeShared<FJsonValueObject>(MakeReferenceRow(
				PackagePath,
				ObjectPath,
				PropertyPath,
				TEXT("hard_object"),
				OldObject->GetPathName(),
				NewObjectPath,
				false,
				Status)));
			Stats.bHasBlockingErrors = Options.bRequireTargets;
			return false;
		}

		if (ObjectProperty->PropertyClass && !NewObject->IsA(ObjectProperty->PropertyClass))
		{
			Stats.References.Add(MakeShared<FJsonValueObject>(MakeReferenceRow(
				PackagePath,
				ObjectPath,
				PropertyPath,
				TEXT("hard_object"),
				OldObject->GetPathName(),
				NewObjectPath,
				false,
				TEXT("target_type_mismatch"))));
			Stats.bHasBlockingErrors = Options.bRequireTargets;
			return false;
		}

		const bool bApply = !Options.Mutation.bDryRun;
		if (bApply)
		{
			ObjectProperty->SetObjectPropertyValue(ValuePtr, NewObject);
			++Stats.AppliedCount;
			Stats.ChangedPackages.Add(PackagePath);
		}

		Stats.References.Add(MakeShared<FJsonValueObject>(MakeReferenceRow(
			PackagePath,
			ObjectPath,
			PropertyPath,
			TEXT("hard_object"),
			OldObject->GetPathName(),
			NewObjectPath,
			bApply,
			bApply ? TEXT("applied") : TEXT("dry_run"))));
		return bApply;
	}

	static bool FixupSoftObjectProperty(
		FSoftObjectProperty* SoftObjectProperty,
		void* ValuePtr,
		const FString& PackagePath,
		const FString& ObjectPath,
		const FString& PropertyPath,
		const TArray<FRootRemap>& Remaps,
		const FReferenceFixupOptions& Options,
		FReferenceFixupStats& Stats)
	{
		const FSoftObjectPtr OldSoftPtr = SoftObjectProperty->GetPropertyValue(ValuePtr);
		const FSoftObjectPath OldPath = OldSoftPtr.ToSoftObjectPath();
		const FString OldPathString = OldPath.ToString();
		if (OldPathString.IsEmpty())
		{
			return false;
		}

		FString NewPathString;
		FString SourcePackage;
		FString DestinationPackage;
		if (!TryRemapObjectPath(OldPathString, Remaps, NewPathString, SourcePackage, DestinationPackage))
		{
			return false;
		}

		++Stats.CandidateCount;
		const bool bTargetExists = PackageExists(FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get(), DestinationPackage);
		if (Options.bRequireTargets && !bTargetExists)
		{
			Stats.References.Add(MakeShared<FJsonValueObject>(MakeReferenceRow(
				PackagePath,
				ObjectPath,
				PropertyPath,
				TEXT("soft_object"),
				OldPathString,
				NewPathString,
				false,
				TEXT("target_missing"))));
			Stats.bHasBlockingErrors = true;
			return false;
		}

		const bool bApply = !Options.Mutation.bDryRun;
		if (bApply)
		{
			SoftObjectProperty->SetPropertyValue(ValuePtr, FSoftObjectPtr(FSoftObjectPath(NewPathString)));
			++Stats.AppliedCount;
			Stats.ChangedPackages.Add(PackagePath);
		}

		Stats.References.Add(MakeShared<FJsonValueObject>(MakeReferenceRow(
			PackagePath,
			ObjectPath,
			PropertyPath,
			TEXT("soft_object"),
			OldPathString,
			NewPathString,
			bApply,
			bApply ? TEXT("applied") : TEXT("dry_run"))));
		return bApply;
	}

	static bool FixupSoftObjectPathStruct(
		void* ValuePtr,
		const FString& PackagePath,
		const FString& ObjectPath,
		const FString& PropertyPath,
		const TArray<FRootRemap>& Remaps,
		const FReferenceFixupOptions& Options,
		FReferenceFixupStats& Stats)
	{
		FSoftObjectPath* OldPath = static_cast<FSoftObjectPath*>(ValuePtr);
		if (!OldPath || OldPath->IsNull())
		{
			return false;
		}

		FString NewPathString;
		FString SourcePackage;
		FString DestinationPackage;
		if (!TryRemapObjectPath(OldPath->ToString(), Remaps, NewPathString, SourcePackage, DestinationPackage))
		{
			return false;
		}

		++Stats.CandidateCount;
		const bool bTargetExists = PackageExists(FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get(), DestinationPackage);
		if (Options.bRequireTargets && !bTargetExists)
		{
			Stats.References.Add(MakeShared<FJsonValueObject>(MakeReferenceRow(
				PackagePath,
				ObjectPath,
				PropertyPath,
				TEXT("soft_object_path"),
				OldPath->ToString(),
				NewPathString,
				false,
				TEXT("target_missing"))));
			Stats.bHasBlockingErrors = true;
			return false;
		}

		const bool bApply = !Options.Mutation.bDryRun;
		const FString OldPathString = OldPath->ToString();
		if (bApply)
		{
			*OldPath = FSoftObjectPath(NewPathString);
			++Stats.AppliedCount;
			Stats.ChangedPackages.Add(PackagePath);
		}

		Stats.References.Add(MakeShared<FJsonValueObject>(MakeReferenceRow(
			PackagePath,
			ObjectPath,
			PropertyPath,
			TEXT("soft_object_path"),
			OldPathString,
			NewPathString,
			bApply,
			bApply ? TEXT("applied") : TEXT("dry_run"))));
		return bApply;
	}

	static bool FixupPropertyValue(
		FProperty* Property,
		void* ValuePtr,
		const FString& PackagePath,
		const FString& ObjectPath,
		const FString& PropertyPath,
		const TArray<FRootRemap>& Remaps,
		const FReferenceFixupOptions& Options,
		FReferenceFixupStats& Stats);

	static bool FixupStructProperties(
		UStruct* Struct,
		void* StructValuePtr,
		const FString& PackagePath,
		const FString& ObjectPath,
		const FString& Prefix,
		const TArray<FRootRemap>& Remaps,
		const FReferenceFixupOptions& Options,
		FReferenceFixupStats& Stats)
	{
		bool bChanged = false;
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			FProperty* ChildProperty = *It;
			if (!ChildProperty || ChildProperty->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			{
				continue;
			}

			void* ChildValuePtr = ChildProperty->ContainerPtrToValuePtr<void>(StructValuePtr);
			const FString ChildPath = Prefix.IsEmpty()
				? ChildProperty->GetName()
				: Prefix + TEXT(".") + ChildProperty->GetName();
			bChanged |= FixupPropertyValue(ChildProperty, ChildValuePtr, PackagePath, ObjectPath, ChildPath, Remaps, Options, Stats);
		}
		return bChanged;
	}

	static bool FixupPropertyValue(
		FProperty* Property,
		void* ValuePtr,
		const FString& PackagePath,
		const FString& ObjectPath,
		const FString& PropertyPath,
		const TArray<FRootRemap>& Remaps,
		const FReferenceFixupOptions& Options,
		FReferenceFixupStats& Stats)
	{
		if (!Property || !ValuePtr)
		{
			return false;
		}

		if (FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
		{
			return FixupSoftObjectProperty(SoftObjectProperty, ValuePtr, PackagePath, ObjectPath, PropertyPath, Remaps, Options, Stats);
		}

		if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			return FixupObjectProperty(ObjectProperty, ValuePtr, PackagePath, ObjectPath, PropertyPath, Remaps, Options, Stats);
		}

		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == TBaseStructure<FSoftObjectPath>::Get())
			{
				return FixupSoftObjectPathStruct(ValuePtr, PackagePath, ObjectPath, PropertyPath, Remaps, Options, Stats);
			}
			return FixupStructProperties(StructProperty->Struct, ValuePtr, PackagePath, ObjectPath, PropertyPath, Remaps, Options, Stats);
		}

		if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			bool bChanged = false;
			FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				bChanged |= FixupPropertyValue(
					ArrayProperty->Inner,
					Helper.GetRawPtr(Index),
					PackagePath,
					ObjectPath,
					FString::Printf(TEXT("%s[%d]"), *PropertyPath, Index),
					Remaps,
					Options,
					Stats);
			}
			return bChanged;
		}

		if (FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			bool bChanged = false;
			FScriptSetHelper Helper(SetProperty, ValuePtr);
			for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
			{
				if (!Helper.IsValidIndex(Index))
				{
					continue;
				}
				bChanged |= FixupPropertyValue(
					SetProperty->ElementProp,
					Helper.GetElementPtr(Index),
					PackagePath,
					ObjectPath,
					FString::Printf(TEXT("%s{%d}"), *PropertyPath, Index),
					Remaps,
					Options,
					Stats);
			}
			if (bChanged && !Options.Mutation.bDryRun)
			{
				Helper.Rehash();
			}
			return bChanged;
		}

		if (FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			bool bChanged = false;
			FScriptMapHelper Helper(MapProperty, ValuePtr);
			for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
			{
				if (!Helper.IsValidIndex(Index))
				{
					continue;
				}
				bChanged |= FixupPropertyValue(
					MapProperty->KeyProp,
					Helper.GetKeyPtr(Index),
					PackagePath,
					ObjectPath,
					FString::Printf(TEXT("%s{%d}.Key"), *PropertyPath, Index),
					Remaps,
					Options,
					Stats);
				bChanged |= FixupPropertyValue(
					MapProperty->ValueProp,
					Helper.GetValuePtr(Index),
					PackagePath,
					ObjectPath,
					FString::Printf(TEXT("%s{%d}.Value"), *PropertyPath, Index),
					Remaps,
					Options,
					Stats);
			}
			if (bChanged && !Options.Mutation.bDryRun)
			{
				Helper.Rehash();
			}
			return bChanged;
		}

		return false;
	}

	static bool FixupPackageReferences(
		const FString& PackagePath,
		const TArray<FRootRemap>& Remaps,
		const FReferenceFixupOptions& Options,
		FReferenceFixupStats& Stats)
	{
		UPackage* Package = FindPackage(nullptr, *PackagePath);
		if (!Package)
		{
			Package = LoadPackage(nullptr, *PackagePath, LOAD_None);
		}
		if (!Package)
		{
			AddWarning(Stats, PackagePath, TEXT("Could not load package"));
			Stats.bHasBlockingErrors = Options.Mutation.bStrict;
			return false;
		}

		++Stats.CheckedPackageCount;
		TArray<UObject*> Objects;
		ForEachObjectWithPackage(Package, [&Objects](UObject* Object)
		{
			if (Object && !Object->HasAnyFlags(RF_Transient | RF_ClassDefaultObject))
			{
				Objects.Add(Object);
			}
			return true;
		}, EGetObjectsFlags::IncludeNestedObjects);

		bool bPackageChanged = false;
		for (UObject* Object : Objects)
		{
			if (!Object || !Object->GetClass())
			{
				continue;
			}
			++Stats.CheckedObjectCount;

			const int32 AppliedBefore = Stats.AppliedCount;
			if (!Options.Mutation.bDryRun)
			{
				Object->Modify();
			}
			const bool bChanged = FixupStructProperties(
				Object->GetClass(),
				Object,
				PackagePath,
				Object->GetPathName(),
				FString(),
				Remaps,
				Options,
				Stats);
			if (bChanged && !Options.Mutation.bDryRun && Stats.AppliedCount > AppliedBefore)
			{
				Object->MarkPackageDirty();
				bPackageChanged = true;
			}
		}

		if (bPackageChanged)
		{
			Package->MarkPackageDirty();
		}
		return bPackageChanged;
	}
}

void FMonolithAssetPackageGraphActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("asset"), TEXT("plan_package_graph_copy"),
		TEXT("Plan a package graph copy/remap from AssetRegistry dependencies without loading, copying, or fixing up assets"),
		FMonolithActionHandler::CreateStatic(&FMonolithAssetPackageGraphActions::PlanPackageGraphCopy),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("root_packages"), TEXT("array"), TEXT("Source package roots to include in the plan"))
			.Required(TEXT("root_remaps"), TEXT("object"), TEXT("Object mapping source roots to destination roots, e.g. {\"/Game/UI\":\"/SpeedMaps/UI\"}"))
			.Optional(TEXT("dependency_kinds"), TEXT("array"), TEXT("Dependency kinds to follow: hard, soft. Default: both"))
			.Optional(TEXT("max_packages"), TEXT("integer"), TEXT("Traversal safety cap"), TEXT("512"))
			.Optional(TEXT("strategy"), TEXT("string"), TEXT("Explicit planning strategy; only registry_only_plan is currently implemented"), TEXT("registry_only_plan"))
			.Optional(TEXT("check_collisions"), TEXT("bool"), TEXT("Report existing destination packages"), TEXT("true"))
			.Build(),
		TEXT("PackageGraph"),
		ExplicitReadOnlyPolicy());

	Registry.RegisterAction(TEXT("asset"), TEXT("copy_package_graph_with_remap"),
		TEXT("Copy a planned package dependency graph by duplicating source assets to root-remapped destination packages. Requires dry_run=true or confirm=true; never overwrites existing destinations."),
		FMonolithActionHandler::CreateStatic(&FMonolithAssetPackageGraphActions::CopyPackageGraphWithRemap),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("root_packages"), TEXT("array"), TEXT("Source package roots to copy"))
			.Required(TEXT("root_remaps"), TEXT("object"), TEXT("Object mapping source roots to destination roots"))
			.Optional(TEXT("dependency_kinds"), TEXT("array"), TEXT("Dependency kinds to follow: hard, soft. Default: both"))
			.Optional(TEXT("max_packages"), TEXT("integer"), TEXT("Traversal safety cap"), TEXT("512"))
			.Optional(TEXT("check_collisions"), TEXT("bool"), TEXT("Report existing destination packages"), TEXT("true"))
			.Optional(TEXT("collision_policy"), TEXT("string"), TEXT("fail_if_exists or skip_existing. Overwrite is intentionally unsupported."), TEXT("fail_if_exists"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Return the copy plan without mutating assets"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("bool"), TEXT("Required for mutation when dry_run is false"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Save duplicated destination packages"), TEXT("true"))
			.Build(),
		TEXT("PackageGraph"),
		MutatingAssetPolicy());

	Registry.RegisterAction(TEXT("asset"), TEXT("copy_package_graph_with_strategy"),
		TEXT("Orchestrate a guarded package graph copy strategy: plan-only, copy-only, copy+fixup, or copy+fixup+dependency-closure validation."),
		FMonolithActionHandler::CreateStatic(&FMonolithAssetPackageGraphActions::CopyPackageGraphWithStrategy),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("root_packages"), TEXT("array"), TEXT("Source package roots to copy or plan"))
			.Required(TEXT("root_remaps"), TEXT("object"), TEXT("Object mapping source roots to destination roots"))
			.Optional(TEXT("workflow"), TEXT("string"), TEXT("plan_only, copy_only, copy_fixup, or copy_fixup_validate"), TEXT("copy_fixup_validate"))
			.Optional(TEXT("strategy"), TEXT("string"), TEXT("Compatibility alias: accepts workflow values or copy_strategy values"), TEXT(""))
			.Optional(TEXT("copy_strategy"), TEXT("string"), TEXT("auto, duplicate_asset, advanced_copy, raw_package_file_copy, or header_patched_advanced_copy"), TEXT("auto"))
			.Optional(TEXT("dependency_kinds"), TEXT("array"), TEXT("Dependency kinds to follow: hard, soft. Default: both"))
			.Optional(TEXT("max_packages"), TEXT("integer"), TEXT("Traversal safety cap for plan/copy"), TEXT("512"))
			.Optional(TEXT("fixup_max_packages"), TEXT("integer"), TEXT("Fixup safety cap"), TEXT("1000"))
			.Optional(TEXT("closure_max_packages"), TEXT("integer"), TEXT("Dependency-closure validation safety cap"), TEXT("1000"))
			.Optional(TEXT("check_collisions"), TEXT("bool"), TEXT("Report existing destination packages"), TEXT("true"))
			.Optional(TEXT("collision_policy"), TEXT("string"), TEXT("fail_if_exists or skip_existing. Overwrite is intentionally unsupported."), TEXT("fail_if_exists"))
			.Optional(TEXT("header_patched_roots"), TEXT("array"), TEXT("Source roots that select header_patched_advanced_copy when copy_strategy=auto"))
			.Optional(TEXT("header_patched_packages"), TEXT("array"), TEXT("Source packages that select header_patched_advanced_copy when copy_strategy=auto"))
			.Optional(TEXT("raw_package_roots"), TEXT("array"), TEXT("Source roots that select raw_package_file_copy when copy_strategy=auto"))
			.Optional(TEXT("raw_package_packages"), TEXT("array"), TEXT("Source packages that select raw_package_file_copy when copy_strategy=auto"))
			.Optional(TEXT("manual_copy_roots"), TEXT("array"), TEXT("Source roots that are known to require manual single-object duplication"))
			.Optional(TEXT("manual_copy_packages"), TEXT("array"), TEXT("Source packages that are known to require manual single-object duplication"))
			.Optional(TEXT("allow_raw_package_copy"), TEXT("bool"), TEXT("Opt-in flag for future raw package file copy execution; current slice still reports execution as deferred"), TEXT("false"))
			.Optional(TEXT("allowed_external_roots"), TEXT("array"), TEXT("External roots allowed during dependency-closure validation"))
			.Optional(TEXT("legacy_source_roots"), TEXT("array"), TEXT("Source roots that should not remain referenced; defaults to root_remaps source roots when omitted"))
			.Optional(TEXT("require_targets"), TEXT("bool"), TEXT("Fail fixup when a remapped reference target package is missing"), TEXT("true"))
			.Optional(TEXT("run_fixup_on_dry_run"), TEXT("bool"), TEXT("Run fixup dry-run against existing destination packages instead of only reporting planned params"), TEXT("false"))
			.Optional(TEXT("run_closure_on_dry_run"), TEXT("bool"), TEXT("Run dependency closure dry-run against existing destination packages instead of only reporting planned params"), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Return the orchestrated copy plan without mutating assets"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("bool"), TEXT("Required for mutation when dry_run is false"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Save duplicated or changed packages"), TEXT("true"))
			.Optional(TEXT("strict"), TEXT("bool"), TEXT("Treat fixup blockers as errors"), TEXT("true"))
			.Build(),
		TEXT("PackageGraph"),
		MutatingAssetPolicy());

	Registry.RegisterAction(TEXT("asset"), TEXT("fixup_copied_references"),
		TEXT("Rewrite reflected hard and soft references inside copied destination packages from source roots to root-remapped destination roots. Requires dry_run=true or confirm=true."),
		FMonolithActionHandler::CreateStatic(&FMonolithAssetPackageGraphActions::FixupCopiedReferences),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("root_remaps"), TEXT("object"), TEXT("Object mapping source roots to destination roots"))
			.Optional(TEXT("destination_roots"), TEXT("array"), TEXT("Destination roots to scan; defaults to root_remaps destinations"))
			.Optional(TEXT("package_paths"), TEXT("array"), TEXT("Specific destination packages to fix up"))
			.Optional(TEXT("max_packages"), TEXT("integer"), TEXT("Fixup safety cap"), TEXT("1000"))
			.Optional(TEXT("require_targets"), TEXT("bool"), TEXT("Fail if a remapped reference target package is missing"), TEXT("true"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Report reference rewrites without mutating assets"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("bool"), TEXT("Required for mutation when dry_run is false"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Save changed packages"), TEXT("true"))
			.Optional(TEXT("strict"), TEXT("bool"), TEXT("Treat load/fixup blockers as errors"), TEXT("true"))
			.Build(),
		TEXT("PackageGraph"),
		MutatingAssetPolicy());

	Registry.RegisterAction(TEXT("asset"), TEXT("validate_dependency_closure"),
		TEXT("Validate that destination packages do not depend on disallowed package roots after a copy/remap plan"),
		FMonolithActionHandler::CreateStatic(&FMonolithAssetPackageGraphActions::ValidateDependencyClosure),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("destination_roots"), TEXT("array"), TEXT("Destination roots whose package closure should be validated"))
			.Optional(TEXT("package_paths"), TEXT("array"), TEXT("Specific destination packages to validate; omitted scans destination_roots"))
			.Optional(TEXT("allowed_external_roots"), TEXT("array"), TEXT("External roots allowed in dependencies, e.g. /Script, /Engine"))
			.Optional(TEXT("legacy_source_roots"), TEXT("array"), TEXT("Source roots that should not remain referenced"))
			.Optional(TEXT("dependency_kinds"), TEXT("array"), TEXT("Dependency kinds to validate: hard, soft. Default: both"))
			.Optional(TEXT("max_packages"), TEXT("integer"), TEXT("Validation safety cap"), TEXT("1000"))
			.Build(),
		TEXT("PackageGraph"));

	Registry.SetActionSearchMetadata(TEXT("asset"), TEXT("plan_package_graph_copy"),
		{ TEXT("package graph copy"), TEXT("root remap"), TEXT("dependency closure"), TEXT("dry-run copy plan") },
		{ TEXT("plan asset copy"), TEXT("plan package remap"), TEXT("copy dependency graph") },
		{ TEXT("plan copying /Game/UI to /SpeedMaps/UI without copying assets") });
	Registry.SetActionSearchMetadata(TEXT("asset"), TEXT("copy_package_graph_with_remap"),
		{ TEXT("package graph copy"), TEXT("root remap"), TEXT("duplicate assets"), TEXT("guarded copy") },
		{ TEXT("copy asset graph"), TEXT("duplicate package graph"), TEXT("execute copy plan") },
		{ TEXT("copy /ShooterMaps frontend packages to /SpeedMaps with confirm=true") });
	Registry.SetActionSearchMetadata(TEXT("asset"), TEXT("copy_package_graph_with_strategy"),
		{ TEXT("package graph strategy"), TEXT("copy fixup validate"), TEXT("root remap pipeline"), TEXT("dependency closure") },
		{ TEXT("orchestrate package graph copy"), TEXT("copy graph with strategy"), TEXT("copy fixup validate graph") },
		{ TEXT("dry-run plan/copy/fixup/closure for /ShooterMaps to /SpeedMaps") });
	Registry.SetActionSearchMetadata(TEXT("asset"), TEXT("fixup_copied_references"),
		{ TEXT("fixup copied references"), TEXT("soft object path remap"), TEXT("hard object reference remap") },
		{ TEXT("rewrite copied references"), TEXT("remap asset references") },
		{ TEXT("fix references in /SpeedMaps copied assets from /ShooterMaps to /SpeedMaps") });
	Registry.SetActionSearchMetadata(TEXT("asset"), TEXT("validate_dependency_closure"),
		{ TEXT("dependency closure"), TEXT("legacy source root"), TEXT("copied package validation") },
		{ TEXT("validate copied dependencies"), TEXT("find source-root dependencies") },
		{ TEXT("validate that /SpeedMaps copied assets no longer depend on /ShooterMaps") });

	Registry.SetActionPlanningMetadata(TEXT("asset"), TEXT("plan_package_graph_copy"),
		TEXT("unreal-asset"),
		{ TEXT("Root source packages and explicit source->destination root_remaps are required") },
		{ TEXT("Read-only copy plan with package_map, dependency_edges, external_dependencies, and collisions") },
		{ TEXT("asset.copy_package_graph_with_remap") });
	Registry.SetActionPlanningMetadata(TEXT("asset"), TEXT("copy_package_graph_with_remap"),
		TEXT("unreal-asset"),
		{ TEXT("Run with dry_run=true first; confirm=true is required to duplicate packages; existing destinations are never overwritten") },
		{ TEXT("Copy report with duplicated, skipped, saved, and failure rows") },
		{ TEXT("asset.fixup_copied_references"), TEXT("asset.validate_dependency_closure") });
	Registry.SetActionPlanningMetadata(TEXT("asset"), TEXT("copy_package_graph_with_strategy"),
		TEXT("unreal-asset"),
		{ TEXT("Pick an explicit strategy; plan_only is read-only, copy/copy_fixup/copy_fixup_validate require dry_run=true or confirm=true") },
		{ TEXT("Orchestrated phase report with plan, copy, optional fixup, optional closure, and next recommended domain repairs") },
		{ TEXT("material.repair_copied_material_instance_parameters"), TEXT("ui.repair_slate_font_references"), TEXT("asset.validate_dependency_closure") });
	Registry.SetActionPlanningMetadata(TEXT("asset"), TEXT("fixup_copied_references"),
		TEXT("unreal-asset"),
		{ TEXT("Run after copying packages; root_remaps must describe source->destination roots; dry_run=true should precede confirm=true") },
		{ TEXT("Reference rewrite report with hard/soft paths, missing targets, changed packages, and save status") },
		{ TEXT("asset.validate_dependency_closure") });
	Registry.SetActionPlanningMetadata(TEXT("asset"), TEXT("validate_dependency_closure"),
		TEXT("unreal-asset"),
		{ TEXT("Destination roots or explicit destination packages are required") },
		{ TEXT("ok flag, violation rows, checked package count, and dependency edge count") },
		{ TEXT("asset.plan_package_graph_copy") });
}

FMonolithActionResult FMonolithAssetPackageGraphActions::PlanPackageGraphCopy(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> RootPackages;
	TArray<FRootRemap> Remaps;
	TArray<EDependencyKind> DependencyKinds;
	FString Error;
	if (!ReadStringArrayParam(Params, TEXT("root_packages"), true, RootPackages, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("root_packages"), Error));
	}
	if (!ReadRootRemaps(Params, Remaps, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("root_remaps"), Error));
	}
	if (!ReadDependencyKinds(Params, true, true, DependencyKinds, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("dependency_kinds"), Error));
	}

	int32 MaxPackages = 512;
	bool bCheckCollisions = true;
	if (!ReadIntParam(Params, TEXT("max_packages"), MaxPackages, Error)
		|| !ReadBoolParam(Params, TEXT("check_collisions"), bCheckCollisions, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("params"), Error));
	}

	FString Strategy = TEXT("registry_only_plan");
	Params->TryGetStringField(TEXT("strategy"), Strategy);
	if (!Strategy.Equals(TEXT("registry_only_plan"), ESearchCase::IgnoreCase))
	{
		Error = FString::Printf(TEXT("Unsupported strategy '%s'; only 'registry_only_plan' is implemented"), *Strategy);
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("strategy"), Error));
	}

	const TArray<FString> SourceRoots = SourceRootsFromRemaps(Remaps);
	for (const FString& RootPackage : RootPackages)
	{
		if (!IsValidPackageOrRoot(RootPackage) || !IsUnderAnyRoot(RootPackage, SourceRoots))
		{
			Error = FString::Printf(TEXT("Root package '%s' is not under any root_remaps source root"), *RootPackage);
			return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("root_packages"), Error));
		}
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<FString> Queue = RootPackages;
	TSet<FString> PlannedSet;
	TArray<FString> PlannedPackages;
	TArray<TSharedPtr<FJsonValue>> Edges;
	TArray<TSharedPtr<FJsonValue>> ExternalDependencies;
	TArray<TSharedPtr<FJsonValue>> Collisions;
	bool bTruncated = false;

	for (int32 QueueIndex = 0; QueueIndex < Queue.Num(); ++QueueIndex)
	{
		if (PlannedSet.Num() >= MaxPackages)
		{
			bTruncated = true;
			break;
		}

		const FString CurrentPackage = Queue[QueueIndex];
		if (PlannedSet.Contains(CurrentPackage))
		{
			continue;
		}
		PlannedSet.Add(CurrentPackage);
		PlannedPackages.Add(CurrentPackage);

		FString CurrentDestination;
		ApplyRootRemap(CurrentPackage, Remaps, CurrentDestination);

		TArray<TPair<FString, EDependencyKind>> Dependencies;
		AppendDependencies(AssetRegistry, CurrentPackage, DependencyKinds, Dependencies);
		for (const TPair<FString, EDependencyKind>& Dependency : Dependencies)
		{
			FString DependencyDestination;
			const bool bDependencyWillCopy = ApplyRootRemap(Dependency.Key, Remaps, DependencyDestination);
			Edges.Add(MakeShared<FJsonValueObject>(EdgeToJson(
				CurrentPackage,
				Dependency.Key,
				Dependency.Value,
				bDependencyWillCopy,
				CurrentDestination,
				DependencyDestination)));

			if (bDependencyWillCopy)
			{
				if (!PlannedSet.Contains(Dependency.Key) && !Queue.Contains(Dependency.Key))
				{
					Queue.Add(Dependency.Key);
				}
			}
			else
			{
				ExternalDependencies.Add(MakeShared<FJsonValueObject>(EdgeToJson(
					CurrentPackage,
					Dependency.Key,
					Dependency.Value,
					false,
					CurrentDestination,
					FString())));
			}
		}
	}

	PlannedPackages.Sort();

	TArray<TSharedPtr<FJsonValue>> PackageMap;
	for (const FString& SourcePackage : PlannedPackages)
	{
		FString DestinationPackage;
		ApplyRootRemap(SourcePackage, Remaps, DestinationPackage);

		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("source_package"), SourcePackage);
		Row->SetStringField(TEXT("destination_package"), DestinationPackage);
		Row->SetBoolField(TEXT("source_exists"), PackageExists(AssetRegistry, SourcePackage));
		const bool bDestinationExists = bCheckCollisions && PackageExists(AssetRegistry, DestinationPackage);
		Row->SetBoolField(TEXT("destination_exists"), bDestinationExists);
		PackageMap.Add(MakeShared<FJsonValueObject>(Row));

		if (bDestinationExists)
		{
			TSharedPtr<FJsonObject> Collision = MakeShared<FJsonObject>();
			Collision->SetStringField(TEXT("source_package"), SourcePackage);
			Collision->SetStringField(TEXT("destination_package"), DestinationPackage);
			Collisions.Add(MakeShared<FJsonValueObject>(Collision));
		}
	}

	TArray<TSharedPtr<FJsonValue>> RemapRows;
	for (const FRootRemap& Remap : Remaps)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("source_root"), Remap.SourceRoot);
		Row->SetStringField(TEXT("destination_root"), Remap.DestinationRoot);
		RemapRows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("asset"));
	Result->SetStringField(TEXT("action"), TEXT("plan_package_graph_copy"));
	Result->SetStringField(TEXT("strategy"), Strategy);
	Result->SetBoolField(TEXT("read_only"), true);
	Result->SetBoolField(TEXT("truncated"), bTruncated);
	Result->SetNumberField(TEXT("max_packages"), MaxPackages);
	Result->SetArrayField(TEXT("root_remaps"), RemapRows);
	Result->SetArrayField(TEXT("root_packages"), StringsToJson(RootPackages));
	Result->SetArrayField(TEXT("package_map"), PackageMap);
	Result->SetArrayField(TEXT("dependency_edges"), Edges);
	Result->SetArrayField(TEXT("external_dependencies"), ExternalDependencies);
	Result->SetArrayField(TEXT("collisions"), Collisions);
	Result->SetNumberField(TEXT("package_count"), PlannedPackages.Num());
	Result->SetNumberField(TEXT("dependency_edge_count"), Edges.Num());
	Result->SetNumberField(TEXT("external_dependency_count"), ExternalDependencies.Num());
	Result->SetNumberField(TEXT("collision_count"), Collisions.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAssetPackageGraphActions::CopyPackageGraphWithRemap(const TSharedPtr<FJsonObject>& Params)
{
	FMutationOptions Mutation;
	FString Error;
	if (!ReadMutationOptions(Params, Mutation, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("mutation_guard"), Error));
	}

	FString CollisionPolicy = TEXT("fail_if_exists");
	if (!ReadStringParam(Params, TEXT("collision_policy"), CollisionPolicy, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("collision_policy"), Error));
	}
	if (!CollisionPolicy.Equals(TEXT("fail_if_exists"), ESearchCase::IgnoreCase)
		&& !CollisionPolicy.Equals(TEXT("skip_existing"), ESearchCase::IgnoreCase))
	{
		Error = FString::Printf(TEXT("Unsupported collision_policy '%s'; expected fail_if_exists or skip_existing"), *CollisionPolicy);
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("collision_policy"), Error));
	}

	TSharedPtr<FJsonObject> PlanParams = MakeShared<FJsonObject>();
	if (Params.IsValid())
	{
		PlanParams->Values = Params->Values;
	}
	PlanParams->SetStringField(TEXT("strategy"), TEXT("registry_only_plan"));
	PlanParams->SetBoolField(TEXT("check_collisions"), true);

	FMonolithActionResult PlanResult = PlanPackageGraphCopy(PlanParams);
	if (!PlanResult.bSuccess)
	{
		return PlanResult;
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FPackageCopyRow> Rows;
	TArray<TSharedPtr<FJsonValue>> PreflightErrors;
	ExtractPackageMapRows(AssetRegistry, PlanResult.Result, CollisionPolicy, Rows, PreflightErrors);
	if (PreflightErrors.Num() > 0 && !Mutation.bDryRun)
	{
		TSharedPtr<FJsonObject> ErrorResult = MakeShared<FJsonObject>();
		ErrorResult->SetStringField(TEXT("namespace"), TEXT("asset"));
		ErrorResult->SetStringField(TEXT("action"), TEXT("copy_package_graph_with_remap"));
		ErrorResult->SetArrayField(TEXT("preflight_errors"), PreflightErrors);
		ErrorResult->SetObjectField(TEXT("plan"), PlanResult.Result);
		ErrorResult->SetStringField(TEXT("status"), TEXT("preflight_failed"));
		ErrorResult->SetNumberField(TEXT("preflight_error_count"), PreflightErrors.Num());
		return FMonolithActionResult::Error(TEXT("copy_package_graph_with_remap preflight failed"), ErrInvalidParams)
			.WithErrorData(ErrorResult);
	}

	TArray<TSharedPtr<FJsonValue>> CopyRows;
	TArray<TSharedPtr<FJsonValue>> SavedRows;
	int32 WouldCopyCount = 0;
	int32 CopiedCount = 0;
	int32 SkippedCount = 0;
	int32 SavedCount = 0;

	for (const FPackageCopyRow& Row : Rows)
	{
		const bool bSkipExisting = Row.bDestinationExists && CollisionPolicy.Equals(TEXT("skip_existing"), ESearchCase::IgnoreCase);
		if (bSkipExisting)
		{
			++SkippedCount;
		}
		else
		{
			++WouldCopyCount;
		}

		TSharedPtr<FJsonObject> CopyRow = MakeShared<FJsonObject>();
		CopyRow->SetStringField(TEXT("source_package"), Row.SourcePackage);
		CopyRow->SetStringField(TEXT("destination_package"), Row.DestinationPackage);
		CopyRow->SetStringField(TEXT("source_asset"), Row.SourceAsset.GetSoftObjectPath().ToString());
		CopyRow->SetStringField(TEXT("status"), bSkipExisting ? TEXT("skip_existing") : (Mutation.bDryRun ? TEXT("dry_run") : TEXT("pending")));
		CopyRows.Add(MakeShared<FJsonValueObject>(CopyRow));
	}

	if (!Mutation.bDryRun && WouldCopyCount > 0)
	{
		FScopedTransaction Transaction(NSLOCTEXT("MonolithAsset", "CopyPackageGraphWithRemap", "Monolith Copy Package Graph With Remap"));
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();

		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			const FPackageCopyRow& Row = Rows[Index];
			const bool bSkipExisting = Row.bDestinationExists && CollisionPolicy.Equals(TEXT("skip_existing"), ESearchCase::IgnoreCase);
			if (bSkipExisting)
			{
				continue;
			}

			UObject* SourceAsset = Row.SourceAsset.GetAsset();
			if (!SourceAsset)
			{
				Error = FString::Printf(TEXT("Could not load source asset '%s'"), *Row.SourceAsset.GetSoftObjectPath().ToString());
				return FMonolithActionResult::Error(Error).WithErrorData(PlanResult.Result);
			}

			const FString DestinationPackagePath = FPackageName::GetLongPackagePath(Row.DestinationPackage);
			const FString DestinationAssetName = FPaths::GetBaseFilename(Row.DestinationPackage);
			UObject* Duplicated = AssetTools.DuplicateAsset(DestinationAssetName, DestinationPackagePath, SourceAsset);
			if (!Duplicated)
			{
				Error = FString::Printf(TEXT("DuplicateAsset failed: %s -> %s"), *Row.SourcePackage, *Row.DestinationPackage);
				return FMonolithActionResult::Error(Error).WithErrorData(PlanResult.Result);
			}

			++CopiedCount;
			Duplicated->MarkPackageDirty();
			AssetRegistry.AssetCreated(Duplicated);

			TSharedPtr<FJsonObject> AppliedRow = CopyRows[Index]->AsObject();
			if (AppliedRow.IsValid())
			{
				AppliedRow->SetStringField(TEXT("duplicated_asset"), Duplicated->GetPathName());
				AppliedRow->SetStringField(TEXT("status"), TEXT("copied"));
			}

			FString SavedFilename;
			FString SaveError;
			if (!SavePackageIfRequested(Duplicated->GetOutermost(), Mutation.bSave, SavedFilename, SaveError))
			{
				return FMonolithActionResult::Error(SaveError).WithErrorData(PlanResult.Result);
			}
			if (Mutation.bSave)
			{
				++SavedCount;
				TSharedPtr<FJsonObject> SavedRow = MakeShared<FJsonObject>();
				SavedRow->SetStringField(TEXT("package_path"), Row.DestinationPackage);
				SavedRow->SetStringField(TEXT("filename"), SavedFilename);
				SavedRows.Add(MakeShared<FJsonValueObject>(SavedRow));
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("asset"));
	Result->SetStringField(TEXT("action"), TEXT("copy_package_graph_with_remap"));
	Result->SetBoolField(TEXT("dry_run"), Mutation.bDryRun);
	Result->SetBoolField(TEXT("confirmed"), Mutation.bConfirm);
	Result->SetBoolField(TEXT("save"), Mutation.bSave);
	Result->SetStringField(TEXT("collision_policy"), CollisionPolicy);
	Result->SetStringField(TEXT("status"), Mutation.bDryRun ? TEXT("dry_run") : TEXT("success"));
	Result->SetObjectField(TEXT("plan"), PlanResult.Result);
	Result->SetArrayField(TEXT("preflight_errors"), PreflightErrors);
	Result->SetArrayField(TEXT("copies"), CopyRows);
	Result->SetArrayField(TEXT("saved_packages"), SavedRows);
	Result->SetNumberField(TEXT("would_copy_count"), WouldCopyCount);
	Result->SetNumberField(TEXT("copied_count"), CopiedCount);
	Result->SetNumberField(TEXT("skipped_count"), SkippedCount);
	Result->SetNumberField(TEXT("saved_count"), SavedCount);
	Result->SetNumberField(TEXT("preflight_error_count"), PreflightErrors.Num());
	Result->SetStringField(TEXT("next_recommended_action"), TEXT("asset.fixup_copied_references"));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAssetPackageGraphActions::CopyPackageGraphWithStrategy(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	FString Workflow = TEXT("copy_fixup_validate");
	FString CopyStrategy = TEXT("auto");
	if (!ReadStrategyAlias(Params, Workflow, CopyStrategy, Error)
		|| !ReadStringParam(Params, TEXT("workflow"), Workflow, Error)
		|| !ReadStringParam(Params, TEXT("copy_strategy"), CopyStrategy, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("strategy"), Error));
	}

	const bool bPlanOnly = Workflow.Equals(TEXT("plan_only"), ESearchCase::IgnoreCase);
	const bool bCopyOnly = Workflow.Equals(TEXT("copy_only"), ESearchCase::IgnoreCase);
	const bool bCopyFixup = Workflow.Equals(TEXT("copy_fixup"), ESearchCase::IgnoreCase);
	const bool bCopyFixupValidate = Workflow.Equals(TEXT("copy_fixup_validate"), ESearchCase::IgnoreCase);
	if (!IsWorkflowStrategy(Workflow))
	{
		Error = FString::Printf(
			TEXT("Unsupported workflow '%s'; expected plan_only, copy_only, copy_fixup, or copy_fixup_validate"),
			*Workflow);
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("workflow"), Error));
	}
	if (!IsCopyStrategy(CopyStrategy))
	{
		Error = FString::Printf(
			TEXT("Unsupported copy_strategy '%s'; expected auto, duplicate_asset, advanced_copy, raw_package_file_copy, or header_patched_advanced_copy"),
			*CopyStrategy);
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("copy_strategy"), Error));
	}

	TArray<FRootRemap> Remaps;
	if (!ReadRootRemaps(Params, Remaps, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("root_remaps"), Error));
	}

	TArray<FString> HeaderPatchedRoots;
	TArray<FString> HeaderPatchedPackages;
	TArray<FString> RawPackageRoots;
	TArray<FString> RawPackagePackages;
	TArray<FString> ManualCopyRoots;
	TArray<FString> ManualCopyPackages;
	bool bRunFixupOnDryRun = false;
	bool bRunClosureOnDryRun = false;
	bool bAllowRawPackageCopy = false;
	int32 FixupMaxPackages = 1000;
	int32 ClosureMaxPackages = 1000;
	if (!ReadStringArrayParam(Params, TEXT("header_patched_roots"), false, HeaderPatchedRoots, Error)
		|| !ReadStringArrayParam(Params, TEXT("header_patched_packages"), false, HeaderPatchedPackages, Error)
		|| !ReadStringArrayParam(Params, TEXT("raw_package_roots"), false, RawPackageRoots, Error)
		|| !ReadStringArrayParam(Params, TEXT("raw_package_packages"), false, RawPackagePackages, Error)
		|| !ReadStringArrayParam(Params, TEXT("manual_copy_roots"), false, ManualCopyRoots, Error)
		|| !ReadStringArrayParam(Params, TEXT("manual_copy_packages"), false, ManualCopyPackages, Error)
		|| !ReadBoolParam(Params, TEXT("allow_raw_package_copy"), bAllowRawPackageCopy, Error)
		|| !ReadBoolParam(Params, TEXT("run_fixup_on_dry_run"), bRunFixupOnDryRun, Error)
		|| !ReadBoolParam(Params, TEXT("run_closure_on_dry_run"), bRunClosureOnDryRun, Error)
		|| !ReadIntParam(Params, TEXT("fixup_max_packages"), FixupMaxPackages, Error)
		|| !ReadIntParam(Params, TEXT("closure_max_packages"), ClosureMaxPackages, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("params"), Error));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("asset"));
	Result->SetStringField(TEXT("action"), TEXT("copy_package_graph_with_strategy"));
	Result->SetStringField(TEXT("workflow"), Workflow);
	Result->SetStringField(TEXT("copy_strategy"), CopyStrategy);

	TArray<TSharedPtr<FJsonValue>> Phases;
	const TArray<FString> DestinationRoots = DestinationRootsFromRemaps(Remaps);
	const TArray<FString> SourceRoots = SourceRootsFromRemaps(Remaps);

	auto FailWithPhaseReport = [&Result, &Phases](const FString& Message, int32 Code, const FMonolithActionResult& ChildResult)
	{
		Result->SetArrayField(TEXT("phases"), Phases);
		Result->SetStringField(TEXT("status"), TEXT("failed"));
		Result->SetBoolField(TEXT("ok"), false);
		Result->SetStringField(TEXT("child_error_message"), ChildResult.ErrorMessage);
		Result->SetNumberField(TEXT("child_error_code"), ChildResult.ErrorCode);
		if (ChildResult.ErrorData.IsValid())
		{
			Result->SetObjectField(TEXT("child_error_data"), ChildResult.ErrorData);
		}
		return FMonolithActionResult::Error(Message, Code).WithErrorData(Result);
	};

	TSharedPtr<FJsonObject> PlanParams = CloneParams(Params);
	PlanParams->SetStringField(TEXT("strategy"), TEXT("registry_only_plan"));
	FMonolithActionResult PlanResult = PlanPackageGraphCopy(PlanParams);
	if (!PlanResult.bSuccess)
	{
		Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(TEXT("plan"), TEXT("failed"), false, TEXT("asset.plan_package_graph_copy"), PlanResult.ErrorMessage)));
		return FailWithPhaseReport(TEXT("copy_package_graph_with_strategy plan phase failed"), PlanResult.ErrorCode, PlanResult);
	}

	TArray<TSharedPtr<FJsonValue>> StrategyRows;
	int32 UnsupportedStrategyCount = 0;
	int32 ExecutableStrategyCount = 0;
	const bool bAllStrategiesExecutable = BuildCopyStrategyPlan(
		PlanResult.Result,
		CopyStrategy,
		HeaderPatchedRoots,
		HeaderPatchedPackages,
		RawPackageRoots,
		RawPackagePackages,
		ManualCopyRoots,
		ManualCopyPackages,
		bAllowRawPackageCopy,
		StrategyRows,
		UnsupportedStrategyCount,
		ExecutableStrategyCount);

	const TArray<FString> DestinationPackages = DestinationPackagesFromPlan(PlanResult.Result);
	Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(
		TEXT("plan"),
		TEXT("success"),
		true,
		TEXT("asset.plan_package_graph_copy"))));
	Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(
		TEXT("strategy_plan"),
		bAllStrategiesExecutable ? TEXT("ready") : TEXT("unsupported"),
		bAllStrategiesExecutable,
		TEXT("asset.copy_package_graph_with_strategy"))));

	Result->SetObjectField(TEXT("plan"), PlanResult.Result);
	Result->SetArrayField(TEXT("strategy_plan"), StrategyRows);
	Result->SetNumberField(TEXT("strategy_plan_count"), StrategyRows.Num());
	Result->SetNumberField(TEXT("unsupported_strategy_count"), UnsupportedStrategyCount);
	Result->SetNumberField(TEXT("executable_strategy_count"), ExecutableStrategyCount);
	Result->SetArrayField(TEXT("destination_roots"), StringsToJson(DestinationRoots));
	Result->SetArrayField(TEXT("package_paths"), StringsToJson(DestinationPackages));

	if (bPlanOnly)
	{
		TArray<FString> NextActions;
		NextActions.Add(TEXT("asset.copy_package_graph_with_strategy"));
		NextActions.Add(TEXT("asset.copy_package_graph_with_remap"));
		Result->SetBoolField(TEXT("read_only"), true);
		Result->SetBoolField(TEXT("dry_run"), true);
		Result->SetBoolField(TEXT("confirmed"), false);
		Result->SetBoolField(TEXT("save"), false);
		Result->SetBoolField(TEXT("ok"), bAllStrategiesExecutable);
		Result->SetStringField(TEXT("status"), TEXT("plan_only"));
		Result->SetArrayField(TEXT("phases"), Phases);
		Result->SetArrayField(TEXT("next_recommended_actions"), StringsToJson(NextActions));
		return FMonolithActionResult::Success(Result);
	}

	FMutationOptions Mutation;
	if (!ReadMutationOptions(Params, Mutation, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("mutation_guard"), Error));
	}

	if (!bAllStrategiesExecutable)
	{
		TArray<FString> NextActions;
		NextActions.Add(TEXT("asset.copy_package_graph_with_strategy"));
		NextActions.Add(TEXT("asset.copy_package_graph_with_remap"));
		Result->SetBoolField(TEXT("dry_run"), Mutation.bDryRun);
		Result->SetBoolField(TEXT("confirmed"), Mutation.bConfirm);
		Result->SetBoolField(TEXT("save"), Mutation.bSave);
		Result->SetBoolField(TEXT("ok"), false);
		Result->SetStringField(TEXT("status"), TEXT("unsupported_copy_strategy"));
		Result->SetArrayField(TEXT("phases"), Phases);
		Result->SetArrayField(TEXT("next_recommended_actions"), StringsToJson(NextActions));
		if (Mutation.bDryRun)
		{
			return FMonolithActionResult::Success(Result);
		}
		return FMonolithActionResult::Error(
			TEXT("copy_package_graph_with_strategy selected copy strategies that are not executable by this action"),
			ErrInvalidParams).WithErrorData(Result);
	}

	TSharedPtr<FJsonObject> CopyParams = CloneParams(Params);
	CopyParams->SetStringField(TEXT("strategy"), TEXT("registry_only_plan"));
	FMonolithActionResult CopyResult = CopyPackageGraphWithRemap(CopyParams);
	if (!CopyResult.bSuccess)
	{
		Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(TEXT("copy"), TEXT("failed"), false, TEXT("asset.copy_package_graph_with_remap"), CopyResult.ErrorMessage)));
		return FailWithPhaseReport(TEXT("copy_package_graph_with_strategy copy phase failed"), CopyResult.ErrorCode, CopyResult);
	}

	double PreflightErrorCount = 0.0;
	CopyResult.Result->TryGetNumberField(TEXT("preflight_error_count"), PreflightErrorCount);
	bool bOk = PreflightErrorCount <= 0.0;
	Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(
		TEXT("copy"),
		PreflightErrorCount > 0.0 ? TEXT("preflight_errors") : (Mutation.bDryRun ? TEXT("dry_run") : TEXT("success")),
		PreflightErrorCount <= 0.0,
		TEXT("asset.copy_package_graph_with_remap"))));

	TSharedPtr<FJsonObject> PlanObject;
	const TSharedPtr<FJsonObject>* PlanObjectPtr = nullptr;
	if (CopyResult.Result->TryGetObjectField(TEXT("plan"), PlanObjectPtr) && PlanObjectPtr && PlanObjectPtr->IsValid())
	{
		PlanObject = *PlanObjectPtr;
	}

	Result->SetObjectField(TEXT("copy_report"), CopyResult.Result);

	const bool bStrategyNeedsFixup = bCopyFixup || bCopyFixupValidate;
	if (bStrategyNeedsFixup)
	{
		TSharedPtr<FJsonObject> FixupParams = CloneParams(Params);
		FixupParams->Values.Remove(TEXT("max_packages"));
		FixupParams->SetNumberField(TEXT("max_packages"), FixupMaxPackages);
		SetStringArrayField(FixupParams, TEXT("destination_roots"), DestinationRoots);
		SetStringArrayField(FixupParams, TEXT("package_paths"), DestinationPackages);
		FixupParams->SetBoolField(TEXT("dry_run"), Mutation.bDryRun);
		FixupParams->SetBoolField(TEXT("confirm"), Mutation.bConfirm);
		FixupParams->SetBoolField(TEXT("save"), Mutation.bSave);
		FixupParams->SetBoolField(TEXT("strict"), Mutation.bStrict);

		if (Mutation.bDryRun && !bRunFixupOnDryRun)
		{
			Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(
				TEXT("fixup"),
				TEXT("skipped"),
				true,
				TEXT("asset.fixup_copied_references"),
				TEXT("dry_run does not create destination packages; set run_fixup_on_dry_run=true to scan existing destinations"))));
			Result->SetObjectField(TEXT("planned_fixup_params"), FixupParams);
		}
		else
		{
			FMonolithActionResult FixupResult = FixupCopiedReferences(FixupParams);
			if (!FixupResult.bSuccess)
			{
				Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(TEXT("fixup"), TEXT("failed"), false, TEXT("asset.fixup_copied_references"), FixupResult.ErrorMessage)));
				return FailWithPhaseReport(TEXT("copy_package_graph_with_strategy fixup phase failed"), FixupResult.ErrorCode, FixupResult);
			}

			bool bFixupOk = true;
			FixupResult.Result->TryGetBoolField(TEXT("ok"), bFixupOk);
			bOk &= bFixupOk;
			Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(
				TEXT("fixup"),
				bFixupOk ? (Mutation.bDryRun ? TEXT("dry_run") : TEXT("success")) : TEXT("issues"),
				bFixupOk,
				TEXT("asset.fixup_copied_references"))));
			Result->SetObjectField(TEXT("fixup_report"), FixupResult.Result);
		}
	}

	if (bCopyFixupValidate)
	{
		TSharedPtr<FJsonObject> ClosureParams = CloneParams(Params);
		ClosureParams->Values.Remove(TEXT("max_packages"));
		ClosureParams->SetNumberField(TEXT("max_packages"), ClosureMaxPackages);
		SetStringArrayField(ClosureParams, TEXT("destination_roots"), DestinationRoots);
		SetStringArrayField(ClosureParams, TEXT("package_paths"), DestinationPackages);
		if (!Params.IsValid() || !Params->HasField(TEXT("legacy_source_roots")))
		{
			SetStringArrayField(ClosureParams, TEXT("legacy_source_roots"), SourceRoots);
		}

		if (Mutation.bDryRun && !bRunClosureOnDryRun)
		{
			Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(
				TEXT("closure"),
				TEXT("skipped"),
				true,
				TEXT("asset.validate_dependency_closure"),
				TEXT("dry_run does not create destination packages; set run_closure_on_dry_run=true to validate existing destinations"))));
			Result->SetObjectField(TEXT("planned_closure_params"), ClosureParams);
		}
		else
		{
			FMonolithActionResult ClosureResult = ValidateDependencyClosure(ClosureParams);
			if (!ClosureResult.bSuccess)
			{
				Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(TEXT("closure"), TEXT("failed"), false, TEXT("asset.validate_dependency_closure"), ClosureResult.ErrorMessage)));
				return FailWithPhaseReport(TEXT("copy_package_graph_with_strategy closure phase failed"), ClosureResult.ErrorCode, ClosureResult);
			}

			bool bClosureOk = true;
			ClosureResult.Result->TryGetBoolField(TEXT("ok"), bClosureOk);
			bOk &= bClosureOk;
			Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(
				TEXT("closure"),
				bClosureOk ? TEXT("success") : TEXT("violations"),
				bClosureOk,
				TEXT("asset.validate_dependency_closure"))));
			Result->SetObjectField(TEXT("closure_report"), ClosureResult.Result);
		}
	}

	Result->SetBoolField(TEXT("dry_run"), Mutation.bDryRun);
	Result->SetBoolField(TEXT("confirmed"), Mutation.bConfirm);
	Result->SetBoolField(TEXT("save"), Mutation.bSave);
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetStringField(TEXT("status"), Mutation.bDryRun ? TEXT("dry_run") : (bOk ? TEXT("success") : TEXT("issues")));
	Result->SetArrayField(TEXT("phases"), Phases);
	TArray<FString> NextActions;
	NextActions.Add(TEXT("asset.fixup_copied_references"));
	NextActions.Add(TEXT("asset.validate_dependency_closure"));
	NextActions.Add(TEXT("material.repair_copied_material_instance_parameters"));
	NextActions.Add(TEXT("ui.repair_slate_font_references"));
	Result->SetArrayField(TEXT("next_recommended_actions"), StringsToJson(NextActions));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAssetPackageGraphActions::FixupCopiedReferences(const TSharedPtr<FJsonObject>& Params)
{
	FReferenceFixupOptions Options;
	FString Error;
	if (!ReadMutationOptions(Params, Options.Mutation, Error)
		|| !ReadIntParam(Params, TEXT("max_packages"), Options.MaxPackages, Error)
		|| !ReadBoolParam(Params, TEXT("require_targets"), Options.bRequireTargets, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("params"), Error));
	}

	TArray<FRootRemap> Remaps;
	if (!ReadRootRemaps(Params, Remaps, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("root_remaps"), Error));
	}

	TArray<FString> DestinationRoots;
	TArray<FString> PackagePaths;
	if (!ReadStringArrayParam(Params, TEXT("destination_roots"), false, DestinationRoots, Error)
		|| !ReadStringArrayParam(Params, TEXT("package_paths"), false, PackagePaths, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("params"), Error));
	}
	if (DestinationRoots.Num() == 0)
	{
		DestinationRoots = DestinationRootsFromRemaps(Remaps);
	}
	for (const FString& DestinationRoot : DestinationRoots)
	{
		if (!IsValidPackageOrRoot(DestinationRoot))
		{
			Error = FString::Printf(TEXT("Invalid destination root '%s'"), *DestinationRoot);
			return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("destination_roots"), Error));
		}
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	if (PackagePaths.Num() == 0)
	{
		PackagePaths = ScanPackagesUnderRoots(AssetRegistry, DestinationRoots);
	}
	PackagePaths.Sort();

	FReferenceFixupStats Stats;
	if (PackagePaths.Num() > Options.MaxPackages)
	{
		PackagePaths.SetNum(Options.MaxPackages);
		Stats.bTruncated = true;
	}

	for (const FString& PackagePath : PackagePaths)
	{
		if (!IsUnderAnyRoot(PackagePath, DestinationRoots))
		{
			AddWarning(Stats, PackagePath, TEXT("Package is outside destination_roots and was skipped"));
			if (Options.Mutation.bStrict)
			{
				Stats.bHasBlockingErrors = true;
			}
			continue;
		}
		FixupPackageReferences(PackagePath, Remaps, Options, Stats);
	}

	if (Stats.bHasBlockingErrors && !Options.Mutation.bDryRun && Options.Mutation.bStrict)
	{
		TSharedPtr<FJsonObject> ErrorResult = MakeShared<FJsonObject>();
		ErrorResult->SetStringField(TEXT("namespace"), TEXT("asset"));
		ErrorResult->SetStringField(TEXT("action"), TEXT("fixup_copied_references"));
		ErrorResult->SetArrayField(TEXT("references"), Stats.References);
		ErrorResult->SetArrayField(TEXT("warnings"), Stats.Warnings);
		ErrorResult->SetNumberField(TEXT("candidate_count"), Stats.CandidateCount);
		ErrorResult->SetNumberField(TEXT("warning_count"), Stats.Warnings.Num());
		ErrorResult->SetStringField(TEXT("status"), TEXT("preflight_failed"));
		return FMonolithActionResult::Error(TEXT("fixup_copied_references found blocking reference issues"), ErrInvalidParams)
			.WithErrorData(ErrorResult);
	}

	TArray<TSharedPtr<FJsonValue>> SavedRows;
	if (!Options.Mutation.bDryRun && Options.Mutation.bSave)
	{
		for (const FString& ChangedPackageName : Stats.ChangedPackages)
		{
			UPackage* Package = FindPackage(nullptr, *ChangedPackageName);
			if (!Package)
			{
				AddWarning(Stats, ChangedPackageName, TEXT("Changed package could not be found for save"));
				continue;
			}

			FString SavedFilename;
			FString SaveError;
			if (!SavePackageIfRequested(Package, true, SavedFilename, SaveError))
			{
				return FMonolithActionResult::Error(SaveError);
			}

			TSharedPtr<FJsonObject> SavedRow = MakeShared<FJsonObject>();
			SavedRow->SetStringField(TEXT("package_path"), ChangedPackageName);
			SavedRow->SetStringField(TEXT("filename"), SavedFilename);
			SavedRows.Add(MakeShared<FJsonValueObject>(SavedRow));
		}
	}

	TArray<TSharedPtr<FJsonValue>> RemapRows;
	for (const FRootRemap& Remap : Remaps)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("source_root"), Remap.SourceRoot);
		Row->SetStringField(TEXT("destination_root"), Remap.DestinationRoot);
		RemapRows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TArray<TSharedPtr<FJsonValue>> ChangedPackageRows;
	for (const FString& ChangedPackageName : Stats.ChangedPackages)
	{
		ChangedPackageRows.Add(MakeShared<FJsonValueString>(ChangedPackageName));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("asset"));
	Result->SetStringField(TEXT("action"), TEXT("fixup_copied_references"));
	Result->SetBoolField(TEXT("dry_run"), Options.Mutation.bDryRun);
	Result->SetBoolField(TEXT("confirmed"), Options.Mutation.bConfirm);
	Result->SetBoolField(TEXT("save"), Options.Mutation.bSave);
	Result->SetBoolField(TEXT("strict"), Options.Mutation.bStrict);
	Result->SetBoolField(TEXT("require_targets"), Options.bRequireTargets);
	Result->SetBoolField(TEXT("ok"), !Stats.bHasBlockingErrors);
	Result->SetBoolField(TEXT("truncated"), Stats.bTruncated);
	Result->SetStringField(TEXT("status"), Options.Mutation.bDryRun ? TEXT("dry_run") : TEXT("success"));
	Result->SetArrayField(TEXT("root_remaps"), RemapRows);
	Result->SetArrayField(TEXT("destination_roots"), StringsToJson(DestinationRoots));
	Result->SetArrayField(TEXT("checked_packages"), StringsToJson(PackagePaths));
	Result->SetArrayField(TEXT("references"), Stats.References);
	Result->SetArrayField(TEXT("warnings"), Stats.Warnings);
	Result->SetArrayField(TEXT("changed_packages"), ChangedPackageRows);
	Result->SetArrayField(TEXT("saved_packages"), SavedRows);
	Result->SetNumberField(TEXT("checked_package_count"), Stats.CheckedPackageCount);
	Result->SetNumberField(TEXT("checked_object_count"), Stats.CheckedObjectCount);
	Result->SetNumberField(TEXT("candidate_count"), Stats.CandidateCount);
	Result->SetNumberField(TEXT("applied_count"), Stats.AppliedCount);
	Result->SetNumberField(TEXT("warning_count"), Stats.Warnings.Num());
	Result->SetNumberField(TEXT("changed_package_count"), Stats.ChangedPackages.Num());
	Result->SetNumberField(TEXT("saved_count"), SavedRows.Num());
	Result->SetStringField(TEXT("next_recommended_action"), TEXT("asset.validate_dependency_closure"));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAssetPackageGraphActions::ValidateDependencyClosure(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> DestinationRoots;
	TArray<FString> PackagePaths;
	TArray<FString> AllowedExternalRoots;
	TArray<FString> LegacySourceRoots;
	TArray<EDependencyKind> DependencyKinds;
	FString Error;

	if (!ReadStringArrayParam(Params, TEXT("destination_roots"), true, DestinationRoots, Error)
		|| !ReadStringArrayParam(Params, TEXT("package_paths"), false, PackagePaths, Error)
		|| !ReadStringArrayParam(Params, TEXT("allowed_external_roots"), false, AllowedExternalRoots, Error)
		|| !ReadStringArrayParam(Params, TEXT("legacy_source_roots"), false, LegacySourceRoots, Error)
		|| !ReadDependencyKinds(Params, true, true, DependencyKinds, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("params"), Error));
	}

	int32 MaxPackages = 1000;
	if (!ReadIntParam(Params, TEXT("max_packages"), MaxPackages, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("max_packages"), Error));
	}

	for (const FString& DestinationRoot : DestinationRoots)
	{
		if (!IsValidPackageOrRoot(DestinationRoot))
		{
			Error = FString::Printf(TEXT("Invalid destination root '%s'"), *DestinationRoot);
			return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("destination_roots"), Error));
		}
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	if (PackagePaths.Num() == 0)
	{
		PackagePaths = ScanPackagesUnderRoots(AssetRegistry, DestinationRoots);
	}

	PackagePaths.Sort();
	const bool bTruncated = PackagePaths.Num() > MaxPackages;
	if (PackagePaths.Num() > MaxPackages)
	{
		PackagePaths.SetNum(MaxPackages);
	}

	bool bOk = true;
	int32 EdgeCount = 0;
	TArray<TSharedPtr<FJsonValue>> Violations;

	for (const FString& PackagePath : PackagePaths)
	{
		if (!IsUnderAnyRoot(PackagePath, DestinationRoots))
		{
			TSharedPtr<FJsonObject> Violation = MakeShared<FJsonObject>();
			Violation->SetStringField(TEXT("source_package"), PackagePath);
			Violation->SetStringField(TEXT("target_package"), FString());
			Violation->SetStringField(TEXT("kind"), TEXT("input"));
			Violation->SetStringField(TEXT("reason"), TEXT("package_outside_destination_roots"));
			Violations.Add(MakeShared<FJsonValueObject>(Violation));
			bOk = false;
			continue;
		}

		TArray<TPair<FString, EDependencyKind>> Dependencies;
		AppendDependencies(AssetRegistry, PackagePath, DependencyKinds, Dependencies);
		for (const TPair<FString, EDependencyKind>& Dependency : Dependencies)
		{
			++EdgeCount;
			const bool bInsideDestination = IsUnderAnyRoot(Dependency.Key, DestinationRoots);
			const bool bAllowedExternal = IsUnderAnyRoot(Dependency.Key, AllowedExternalRoots);
			const bool bLegacySource = IsUnderAnyRoot(Dependency.Key, LegacySourceRoots);
			if (!bInsideDestination && (bLegacySource || !bAllowedExternal))
			{
				TSharedPtr<FJsonObject> Violation = MakeShared<FJsonObject>();
				Violation->SetStringField(TEXT("source_package"), PackagePath);
				Violation->SetStringField(TEXT("target_package"), Dependency.Key);
				Violation->SetStringField(TEXT("kind"), DependencyKindToString(Dependency.Value));
				Violation->SetStringField(TEXT("reason"), bLegacySource ? TEXT("legacy_source_root_dependency") : TEXT("disallowed_external_dependency"));
				Violations.Add(MakeShared<FJsonValueObject>(Violation));
				bOk = false;
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("asset"));
	Result->SetStringField(TEXT("action"), TEXT("validate_dependency_closure"));
	Result->SetBoolField(TEXT("read_only"), true);
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetArrayField(TEXT("destination_roots"), StringsToJson(DestinationRoots));
	Result->SetArrayField(TEXT("allowed_external_roots"), StringsToJson(AllowedExternalRoots));
	Result->SetArrayField(TEXT("legacy_source_roots"), StringsToJson(LegacySourceRoots));
	Result->SetArrayField(TEXT("checked_packages"), StringsToJson(PackagePaths));
	Result->SetArrayField(TEXT("violations"), Violations);
	Result->SetNumberField(TEXT("checked_package_count"), PackagePaths.Num());
	Result->SetNumberField(TEXT("dependency_edge_count"), EdgeCount);
	Result->SetNumberField(TEXT("violation_count"), Violations.Num());
	Result->SetBoolField(TEXT("truncated"), bTruncated);
	return FMonolithActionResult::Success(Result);
}
