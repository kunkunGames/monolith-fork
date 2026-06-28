#include "Indexers/DomainAssetIndexer.h"
#include "Utility/MonolithSearchValueWriter.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "MonolithSettings.h"
#include "Modules/ModuleManager.h"
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"

namespace
{
	const FString SourceKind = TEXT("domain_asset");
	constexpr int32 MaxRegistryTagValueChars = 1024;

	bool TryGetDomain(const FAssetData& AssetData, FString& OutDomain)
	{
		const FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();
		const FString ClassPath = AssetData.AssetClassPath.ToString();

		if (ClassName.Contains(TEXT("ControlRig"), ESearchCase::IgnoreCase) ||
			ClassPath.Contains(TEXT("ControlRig"), ESearchCase::IgnoreCase))
		{
			OutDomain = TEXT("controlrig");
			return true;
		}

		if (ClassName.Contains(TEXT("RigVM"), ESearchCase::IgnoreCase) ||
			ClassPath.Contains(TEXT("RigVM"), ESearchCase::IgnoreCase))
		{
			OutDomain = TEXT("rigvm");
			return true;
		}

		if (ClassName.Equals(TEXT("StateTree"), ESearchCase::IgnoreCase) ||
			ClassName.EndsWith(TEXT("StateTree"), ESearchCase::IgnoreCase) ||
			ClassPath.Contains(TEXT("StateTree"), ESearchCase::IgnoreCase))
		{
			OutDomain = TEXT("statetree");
			return true;
		}

		if (ClassName.Contains(TEXT("Chooser"), ESearchCase::IgnoreCase) ||
			ClassName.Equals(TEXT("ProxyTable"), ESearchCase::IgnoreCase) ||
			ClassPath.Contains(TEXT("Chooser"), ESearchCase::IgnoreCase))
		{
			OutDomain = TEXT("chooser");
			return true;
		}

		return false;
	}

	FString GetDomainKeywords(const FString& Domain)
	{
		if (Domain == TEXT("controlrig"))
		{
			return TEXT("ControlRig RigVM animation rig graph control hierarchy");
		}
		if (Domain == TEXT("rigvm"))
		{
			return TEXT("RigVM control rig virtual machine graph unit pins");
		}
		if (Domain == TEXT("statetree"))
		{
			return TEXT("StateTree AI state machine states transitions tasks conditions schema");
		}
		if (Domain == TEXT("chooser"))
		{
			return TEXT("Chooser selection table proxy table context result rows columns animation selection");
		}
		return Domain;
	}

	bool ShouldIndexRegistryTag(const FString& Key, const FString& Value, const FString& Domain)
	{
		if (Key.IsEmpty() || Value.IsEmpty())
		{
			return false;
		}

		if (Key.Contains(TEXT("AssetImportData"), ESearchCase::IgnoreCase) ||
			Key.Contains(TEXT("Thumbnail"), ESearchCase::IgnoreCase) ||
			Key.Contains(TEXT("SourceFile"), ESearchCase::IgnoreCase))
		{
			return false;
		}

		static const TCHAR* InterestingKeyTokens[] = {
			TEXT("Class"),
			TEXT("Schema"),
			TEXT("Skeleton"),
			TEXT("Graph"),
			TEXT("Variable"),
			TEXT("Function"),
			TEXT("Rig"),
			TEXT("State"),
			TEXT("Chooser"),
			TEXT("Context"),
			TEXT("Result"),
			TEXT("Root"),
			TEXT("Parameter"),
			TEXT("Tag")
		};

		for (const TCHAR* Token : InterestingKeyTokens)
		{
			if (Key.Contains(Token, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}

		return Value.Contains(TEXT("/Game/"), ESearchCase::IgnoreCase)
			|| Value.Contains(TEXT("/Script/"), ESearchCase::IgnoreCase)
			|| Value.Contains(Domain, ESearchCase::IgnoreCase)
			|| Value.Contains(TEXT("ControlRig"), ESearchCase::IgnoreCase)
			|| Value.Contains(TEXT("RigVM"), ESearchCase::IgnoreCase)
			|| Value.Contains(TEXT("StateTree"), ESearchCase::IgnoreCase)
			|| Value.Contains(TEXT("Chooser"), ESearchCase::IgnoreCase);
	}

	FString ClampRegistryTagValue(const FString& Value)
	{
		if (Value.Len() <= MaxRegistryTagValueChars)
		{
			return Value;
		}
		return Value.Left(MaxRegistryTagValueChars) + TEXT(" ...[truncated]");
	}

	bool IndexOneDomainAsset(const FAssetData& AssetData, FMonolithIndexDatabase& DB, FMonolithSearchValueWriter& SearchValues)
	{
		FString Domain;
		if (!TryGetDomain(AssetData, Domain))
		{
			return false;
		}

		const FString PackagePath = AssetData.PackageName.ToString();
		const int64 AssetId = DB.GetAssetId(PackagePath);
		if (AssetId <= 0)
		{
			return false;
		}

		const FString AssetName = AssetData.AssetName.ToString();
		const FString ObjectPath = AssetData.GetObjectPathString();
		const FString AssetClass = AssetData.AssetClassPath.GetAssetName().ToString();
		const FString AssetClassPath = AssetData.AssetClassPath.ToString();

		auto Add = [&SearchValues, AssetId, &AssetName, &ObjectPath, &AssetClass](const TCHAR* Field, const FString& FieldPath, const FString& Value, const TCHAR* Signal)
		{
			SearchValues.AddValue(AssetId, SourceKind, AssetName, ObjectPath, AssetClass, Field, FieldPath, Value, Signal);
		};

		Add(TEXT("domain"), ObjectPath + TEXT(".domain"), Domain, TEXT("domain_kind"));
		Add(TEXT("asset_class"), ObjectPath + TEXT(".asset_class"), AssetClass, TEXT("domain_class"));
		Add(TEXT("asset_class_path"), ObjectPath + TEXT(".asset_class_path"), AssetClassPath, TEXT("domain_class"));
		Add(TEXT("safe_index_mode"), ObjectPath + TEXT(".safe_index_mode"), TEXT("asset_registry_only no_asset_load"), TEXT("domain_safety"));
		Add(TEXT("domain_keywords"), ObjectPath + TEXT(".domain_keywords"), GetDomainKeywords(Domain), TEXT("domain_keywords"));

		AssetData.TagsAndValues.ForEach([&](TPair<FName, FAssetTagValueRef> TagPair)
		{
			const FString Key = TagPair.Key.ToString();
			const FString Value = TagPair.Value.GetValue();
			if (!ShouldIndexRegistryTag(Key, Value, Domain))
			{
				return;
			}

			Add(*Key, ObjectPath + TEXT(".asset_registry.") + Key, Key + TEXT(" ") + ClampRegistryTagValue(Value), TEXT("asset_registry_tag"));
		});

		return true;
	}

	bool DeleteDomainValuesForAsset(FMonolithIndexDatabase& DB, int64 AssetId)
	{
		FSQLiteDatabase* RawDB = DB.GetRawDatabase();
		if (!RawDB || AssetId <= 0)
		{
			return false;
		}

		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT("DELETE FROM asset_search_values WHERE asset_id = ? AND source_kind = ?;")))
		{
			return false;
		}
		Stmt.SetBindingValueByIndex(1, AssetId);
		Stmt.SetBindingValueByIndex(2, SourceKind);
		return Stmt.Execute();
	}
}

bool FDomainAssetIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	(void)AssetData;
	(void)LoadedAsset;
	(void)AssetId;

	FMonolithSearchValueWriter SearchValues(DB);
	if (!SearchValues.IsEnabled())
	{
		return false;
	}

	DB.DeleteAssetSearchValuesBySourceKind(SourceKind);

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FARFilter Filter;
	for (const FName& ContentPath : UMonolithSettings::GetIndexedContentPaths())
	{
		Filter.PackagePaths.Add(ContentPath);
	}
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	Registry.GetAssets(Filter, Assets);

	int32 IndexedCount = 0;
	for (const FAssetData& Candidate : Assets)
	{
		if (IndexOneDomainAsset(Candidate, DB, SearchValues))
		{
			++IndexedCount;
		}
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("DomainAssetIndexer: indexed %d AssetRegistry-only domain assets"), IndexedCount);
	return true;
}

bool FDomainAssetIndexer::IndexScoped(const TSet<FString>& ChangedPaths, const TSet<FString>& RemovedPaths, FMonolithIndexDatabase& DB)
{
	(void)RemovedPaths;

	if (ChangedPaths.Num() == 0)
	{
		return true;
	}

	FMonolithSearchValueWriter SearchValues(DB);
	if (!SearchValues.IsEnabled())
	{
		return false;
	}

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	DB.BeginTransaction();
	int32 IndexedCount = 0;
	for (const FString& PackagePath : ChangedPaths)
	{
		const int64 AssetId = DB.GetAssetId(PackagePath);
		if (AssetId > 0)
		{
			DeleteDomainValuesForAsset(DB, AssetId);
		}

		TArray<FAssetData> Assets;
		Registry.GetAssetsByPackageName(FName(*PackagePath), Assets);
		for (const FAssetData& Candidate : Assets)
		{
			if (IndexOneDomainAsset(Candidate, DB, SearchValues))
			{
				++IndexedCount;
			}
		}
	}
	DB.CommitTransaction();

	UE_LOG(LogMonolithIndex, Log, TEXT("DomainAssetIndexer: scoped-indexed %d changed domain assets"), IndexedCount);
	return true;
}
