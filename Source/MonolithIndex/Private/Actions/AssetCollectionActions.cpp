#include "Actions/AssetCollectionActions.h"

#include "AssetRegistry/AssetData.h"
#include "CollectionManagerModule.h"
#include "CollectionManagerTypes.h"
#include "ContentBrowserDataFilter.h"
#include "ContentBrowserDataSubsystem.h"
#include "ContentBrowserItem.h"
#include "ICollectionContainer.h"
#include "ICollectionManager.h"
#include "IContentBrowserDataModule.h"
#include "MonolithParamSchema.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "Misc/TextFilterExpressionEvaluator.h"
#include "Misc/TextFilterUtils.h"
#include "UObject/SoftObjectPath.h"

namespace MonolithCollection
{
	static const TSharedRef<ICollectionContainer>& Container()
	{
		FCollectionManagerModule& Module = FCollectionManagerModule::GetModule();
		ICollectionManager& Manager = Module.Get();
		return Manager.GetProjectCollectionContainer();
	}

	static bool TryParseShareType(const FString& In, ECollectionShareType::Type& OutType, bool bAllowAll = false)
	{
		if (In.IsEmpty() || In.Equals(TEXT("local"), ESearchCase::IgnoreCase))
		{
			OutType = ECollectionShareType::CST_Local;
			return true;
		}
		if (In.Equals(TEXT("private"), ESearchCase::IgnoreCase))
		{
			OutType = ECollectionShareType::CST_Private;
			return true;
		}
		if (In.Equals(TEXT("shared"), ESearchCase::IgnoreCase))
		{
			OutType = ECollectionShareType::CST_Shared;
			return true;
		}
		if (In.Equals(TEXT("system"), ESearchCase::IgnoreCase))
		{
			OutType = ECollectionShareType::CST_System;
			return true;
		}
		if (bAllowAll && In.Equals(TEXT("all"), ESearchCase::IgnoreCase))
		{
			OutType = ECollectionShareType::CST_All;
			return true;
		}
		return false;
	}

	static bool GetStorageMode(const TSharedPtr<FJsonObject>& Params, ECollectionStorageMode::Type& OutMode, FString& OutError)
	{
		FString StorageMode;
		if (Params.IsValid() && Params->HasField(TEXT("storage_mode")))
		{
			if (!Params->HasTypedField<EJson::String>(TEXT("storage_mode")))
			{
				OutError = TEXT("storage_mode must be a string");
				return false;
			}
			StorageMode = Params->GetStringField(TEXT("storage_mode"));
		}
		if (StorageMode.IsEmpty() || StorageMode.Equals(TEXT("static"), ESearchCase::IgnoreCase))
		{
			OutMode = ECollectionStorageMode::Static;
			return true;
		}
		if (StorageMode.Equals(TEXT("dynamic"), ESearchCase::IgnoreCase))
		{
			OutMode = ECollectionStorageMode::Dynamic;
			return true;
		}

		OutError = FString::Printf(TEXT("Invalid storage_mode: %s"), *StorageMode);
		return false;
	}

	static bool GetRecursion(const TSharedPtr<FJsonObject>& Params, ECollectionRecursionFlags::Flags& OutFlags, FString& OutError)
	{
		FString Recursion;
		if (Params.IsValid() && Params->HasField(TEXT("recursive")))
		{
			if (!Params->HasTypedField<EJson::String>(TEXT("recursive")))
			{
				OutError = TEXT("recursive must be a string");
				return false;
			}
			Recursion = Params->GetStringField(TEXT("recursive"));
		}
		if (Recursion.IsEmpty() || Recursion.Equals(TEXT("self"), ESearchCase::IgnoreCase))
		{
			OutFlags = ECollectionRecursionFlags::Self;
			return true;
		}
		if (Recursion.Equals(TEXT("children"), ESearchCase::IgnoreCase))
		{
			OutFlags = ECollectionRecursionFlags::SelfAndChildren;
			return true;
		}
		if (Recursion.Equals(TEXT("parents"), ESearchCase::IgnoreCase))
		{
			OutFlags = ECollectionRecursionFlags::SelfAndParents;
			return true;
		}
		if (Recursion.Equals(TEXT("all"), ESearchCase::IgnoreCase))
		{
			OutFlags = ECollectionRecursionFlags::All;
			return true;
		}

		OutError = FString::Printf(TEXT("Invalid recursive: %s"), *Recursion);
		return false;
	}

	static FString ShareTypeToString(ECollectionShareType::Type Type)
	{
		switch (Type)
		{
		case ECollectionShareType::CST_Local: return TEXT("local");
		case ECollectionShareType::CST_Private: return TEXT("private");
		case ECollectionShareType::CST_Shared: return TEXT("shared");
		case ECollectionShareType::CST_System: return TEXT("system");
		case ECollectionShareType::CST_All: return TEXT("all");
		default: return TEXT("unknown");
		}
	}

	static FString StorageModeToString(ECollectionStorageMode::Type Mode)
	{
		return Mode == ECollectionStorageMode::Dynamic ? TEXT("dynamic") : TEXT("static");
	}

	static bool GetRequiredName(const TSharedPtr<FJsonObject>& Params, FName& OutName, FString& OutError)
	{
		FString Name;
		if (!Params.IsValid() || !Params->HasField(TEXT("name")))
		{
			OutError = TEXT("Missing or empty required param: name");
			return false;
		}
		if (!Params->HasTypedField<EJson::String>(TEXT("name")))
		{
			OutError = TEXT("name must be a string");
			return false;
		}
		Name = Params->GetStringField(TEXT("name"));
		if (Name.IsEmpty())
		{
			OutError = TEXT("Missing or empty required param: name");
			return false;
		}
		OutName = FName(*Name);
		return true;
	}

	static bool GetShareType(const TSharedPtr<FJsonObject>& Params, ECollectionShareType::Type& OutType, FString& OutError, bool bAllowAll = false)
	{
		FString ShareType;
		if (Params.IsValid())
		{
			if (Params->HasField(TEXT("share_type")))
			{
				if (!Params->HasTypedField<EJson::String>(TEXT("share_type")))
				{
					OutError = TEXT("share_type must be a string");
					return false;
				}
				ShareType = Params->GetStringField(TEXT("share_type"));
			}
		}
		if (!TryParseShareType(ShareType, OutType, bAllowAll))
		{
			OutError = FString::Printf(TEXT("Invalid share_type: %s"), *ShareType);
			return false;
		}
		return true;
	}

	static FString NormalizeObjectPath(const FString& InPath)
	{
		FString Path = InPath;
		if (Path.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase))
		{
			FString PackageName;
			if (FPackageName::TryConvertFilenameToLongPackageName(Path, PackageName))
			{
				Path = PackageName;
			}
		}
		if (Path.StartsWith(TEXT("/")) && !Path.Contains(TEXT(".")))
		{
			Path = Path + TEXT(".") + FPackageName::GetLongPackageAssetName(Path);
		}
		return Path;
	}

	static bool ParseAssetPaths(const TSharedPtr<FJsonObject>& Params, TArray<FSoftObjectPath>& OutPaths, FString& OutError)
	{
		FString SinglePath;
		if (Params->HasField(TEXT("asset_path")))
		{
			if (!Params->HasTypedField<EJson::String>(TEXT("asset_path")))
			{
				OutError = TEXT("asset_path must be a string");
				return false;
			}
			SinglePath = Params->GetStringField(TEXT("asset_path"));
			if (SinglePath.IsEmpty())
			{
				OutError = TEXT("asset_path must not be empty");
				return false;
			}
			OutPaths.Add(FSoftObjectPath(NormalizeObjectPath(SinglePath)));
		}

		const TArray<TSharedPtr<FJsonValue>>* PathsArray = nullptr;
		if (Params->HasField(TEXT("asset_paths")))
		{
			if (!Params->TryGetArrayField(TEXT("asset_paths"), PathsArray) || !PathsArray)
			{
				OutError = TEXT("asset_paths must be an array");
				return false;
			}
			for (int32 Index = 0; Index < PathsArray->Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& Value = (*PathsArray)[Index];
				FString Path;
				if (!Value.IsValid() || Value->Type != EJson::String)
				{
					OutError = FString::Printf(TEXT("asset_paths[%d] must be a string"), Index);
					return false;
				}
				Path = Value->AsString();
				if (Path.IsEmpty())
				{
					OutError = FString::Printf(TEXT("asset_paths[%d] must not be empty"), Index);
					return false;
				}
				OutPaths.Add(FSoftObjectPath(NormalizeObjectPath(Path)));
			}
		}

		if (OutPaths.Num() == 0)
		{
			OutError = TEXT("Provide asset_path or non-empty asset_paths");
			return false;
		}
		return true;
	}

	static bool GatherCollectionScope(
		const FCollectionNameType& RootCollection,
		ECollectionRecursionFlags::Flags Recursion,
		TArray<FCollectionNameType>& OutCollections)
	{
		const TSharedRef<ICollectionContainer>& C = Container();
		TSet<FCollectionNameType> Seen;
		Seen.Add(RootCollection);
		OutCollections.Add(RootCollection);

		if ((Recursion & ECollectionRecursionFlags::Parents) != 0)
		{
			FCollectionNameType Current = RootCollection;
			while (const TOptional<FCollectionNameType> Parent =
				C->GetParentCollection(Current.Name, Current.Type))
			{
				if (Seen.Contains(Parent.GetValue()))
				{
					break;
				}
				Seen.Add(Parent.GetValue());
				OutCollections.Add(Parent.GetValue());
				Current = Parent.GetValue();
			}
		}

		if ((Recursion & ECollectionRecursionFlags::Children) != 0)
		{
			TArray<FCollectionNameType> PendingChildren;
			PendingChildren.Add(RootCollection);
			for (int32 CollectionIndex = 0;
				CollectionIndex < PendingChildren.Num();
				++CollectionIndex)
			{
				const FCollectionNameType Current =
					PendingChildren[CollectionIndex];
				TArray<FCollectionNameType> Children;
				C->GetChildCollections(Current.Name, Current.Type, Children);
				for (const FCollectionNameType& Child : Children)
				{
					if (!Seen.Contains(Child))
					{
						Seen.Add(Child);
						OutCollections.Add(Child);
						PendingChildren.Add(Child);
					}
				}
			}
		}

		return OutCollections.Num() > 0;
	}

	struct FDynamicCollectionFilter
	{
		FCollectionNameType Collection;
	};

	class FDynamicCollectionExpressionContext
		: public ITextFilterExpressionContext
	{
	public:
		FDynamicCollectionExpressionContext(
			const FContentBrowserItem& InItem,
			const TArray<FCollectionNameType>& InKnownCollections)
			: Item(InItem)
			, KnownCollections(InKnownCollections)
		{
			AssetDisplayName = Item.GetDisplayName().ToString();

			Item.GetVirtualPath().AppendString(AssetFullPath);
			AssetFullPath.ParseIntoArray(AssetSplitPath, TEXT("/"));
			Item.AppendItemReference(AssetExportTextName);

			FSoftObjectPath CollectionId;
			if (Item.TryGetCollectionId(CollectionId))
			{
				Container()->GetCollectionsContainingObject(
					CollectionId,
					ECollectionShareType::CST_All,
					AssetCollectionNames,
					ECollectionRecursionFlags::SelfAndChildren);
			}
		}

		bool TestDynamicCollection(
			const FCollectionNameType& Collection,
			bool& OutMatches) const
		{
			if (const bool* Cached =
				DynamicMembershipCache.Find(Collection))
			{
				OutMatches = *Cached;
				return true;
			}

			// A dynamic query may reference another dynamic collection.
			// Fail a cycle closed for this asset instead of recursing forever.
			if (ActiveDynamicCollections.Contains(Collection))
			{
				OutMatches = false;
				return true;
			}

			ActiveDynamicCollections.Add(Collection);
			FText ErrorText;
			const bool bSucceeded = Container()->TestDynamicQuery(
				Collection.Name,
				Collection.Type,
				*this,
				OutMatches,
				&ErrorText);
			ActiveDynamicCollections.Remove(Collection);
			if (!bSucceeded)
			{
				EvaluationError = ErrorText.IsEmpty()
					? FString::Printf(
						TEXT("Failed to evaluate dynamic collection '%s'"),
						*Collection.Name.ToString())
					: ErrorText.ToString();
				return false;
			}

			DynamicMembershipCache.Add(Collection, OutMatches);
			return true;
		}

		const FString& GetEvaluationError() const
		{
			return EvaluationError;
		}

		virtual bool TestBasicStringExpression(
			const FTextFilterString& InValue,
			ETextFilterTextComparisonMode InTextComparisonMode)
			const override
		{
			if (InValue.CompareName(
				Item.GetItemName(), InTextComparisonMode)
				|| InValue.CompareFString(
					AssetDisplayName, InTextComparisonMode)
				|| InValue.CompareFString(
					AssetFullPath, InTextComparisonMode)
				|| InValue.CompareFString(
					AssetExportTextName, InTextComparisonMode))
			{
				return true;
			}

			for (const FString& PathPart : AssetSplitPath)
			{
				if (InValue.CompareFString(
					PathPart, InTextComparisonMode))
				{
					return true;
				}
			}

			const FContentBrowserItemDataAttributeValue ClassValue =
				Item.GetItemAttribute(NAME_Class);
			if (ClassValue.IsValid()
				&& InValue.CompareName(
					ClassValue.GetValue<FName>(),
					InTextComparisonMode))
			{
				return true;
			}

			for (const FName& CollectionName : AssetCollectionNames)
			{
				if (InValue.CompareName(
					CollectionName, InTextComparisonMode))
				{
					return true;
				}
			}

			for (const FCollectionNameType& Collection : KnownCollections)
			{
				ECollectionStorageMode::Type StorageMode;
				if (!InValue.CompareName(
						Collection.Name, InTextComparisonMode)
					|| !Container()->GetCollectionStorageMode(
						Collection.Name,
						Collection.Type,
						StorageMode)
					|| StorageMode != ECollectionStorageMode::Dynamic)
				{
					continue;
				}

				bool bMatches = false;
				if (TestDynamicCollection(Collection, bMatches)
					&& bMatches)
				{
					return true;
				}
			}

			return false;
		}

		virtual bool TestComplexExpression(
			const FName& InKey,
			const FTextFilterString& InValue,
			ETextFilterComparisonOperation InComparisonOperation,
			ETextFilterTextComparisonMode InTextComparisonMode)
			const override
		{
			static const FName NameKey(TEXT("Name"));
			static const FName PathKey(TEXT("Path"));
			static const FName ClassKey(TEXT("Class"));
			static const FName TypeKey(TEXT("Type"));
			static const FName CollectionKey(TEXT("Collection"));
			static const FName TagKey(TEXT("Tag"));

			const bool bEqualityOperation =
				InComparisonOperation
					== ETextFilterComparisonOperation::Equal
				|| InComparisonOperation
					== ETextFilterComparisonOperation::NotEqual;
			if (InKey == NameKey)
			{
				if (!bEqualityOperation)
				{
					return false;
				}
				const bool bMatches =
					TextFilterUtils::TestBasicStringExpression(
						Item.GetItemName(),
						InValue,
						InTextComparisonMode);
				return InComparisonOperation
					== ETextFilterComparisonOperation::Equal
					? bMatches
					: !bMatches;
			}

			if (InKey == PathKey)
			{
				if (!bEqualityOperation)
				{
					return false;
				}
				const bool bMatches =
					TextFilterUtils::TestBasicStringExpression(
						Item.GetVirtualPath(),
						InValue,
						InTextComparisonMode)
					|| TextFilterUtils::TestBasicStringExpression(
						AssetFullPath,
						InValue,
						InTextComparisonMode);
				return InComparisonOperation
					== ETextFilterComparisonOperation::Equal
					? bMatches
					: !bMatches;
			}

			if (InKey == ClassKey || InKey == TypeKey)
			{
				if (!bEqualityOperation)
				{
					return false;
				}
				const FContentBrowserItemDataAttributeValue ClassValue =
					Item.GetItemAttribute(NAME_Class);
				const bool bMatches = ClassValue.IsValid()
					&& TextFilterUtils::TestBasicStringExpression(
						ClassValue.GetValue<FName>(),
						InValue,
						InTextComparisonMode);
				return InComparisonOperation
					== ETextFilterComparisonOperation::Equal
					? bMatches
					: !bMatches;
			}

			if (InKey == CollectionKey || InKey == TagKey)
			{
				if (!bEqualityOperation)
				{
					return false;
				}

				bool bMatches = false;
				for (const FName& CollectionName : AssetCollectionNames)
				{
					if (TextFilterUtils::TestBasicStringExpression(
						CollectionName,
						InValue,
						InTextComparisonMode))
					{
						bMatches = true;
						break;
					}
				}

				if (!bMatches)
				{
					for (const FCollectionNameType& Collection
						: KnownCollections)
					{
						ECollectionStorageMode::Type StorageMode;
						if (!TextFilterUtils::TestBasicStringExpression(
								Collection.Name,
								InValue,
								InTextComparisonMode)
							|| !Container()->GetCollectionStorageMode(
								Collection.Name,
								Collection.Type,
								StorageMode)
							|| StorageMode
								!= ECollectionStorageMode::Dynamic)
						{
							continue;
						}

						if (TestDynamicCollection(
								Collection, bMatches)
							&& bMatches)
						{
							break;
						}
					}
				}

				return InComparisonOperation
					== ETextFilterComparisonOperation::Equal
					? bMatches
					: !bMatches;
			}

			const FContentBrowserItemDataAttributeValue AttributeValue =
				Item.GetItemAttribute(InKey);
			return AttributeValue.IsValid()
				&& TextFilterUtils::TestComplexExpression(
					AttributeValue.GetValue<FString>(),
					InValue,
					InComparisonOperation,
					InTextComparisonMode);
		}

	private:
		const FContentBrowserItem& Item;
		const TArray<FCollectionNameType>& KnownCollections;
		FString AssetDisplayName;
		FString AssetFullPath;
		FString AssetExportTextName;
		TArray<FString> AssetSplitPath;
		TArray<FName> AssetCollectionNames;
		mutable TSet<FCollectionNameType> ActiveDynamicCollections;
		mutable TMap<FCollectionNameType, bool> DynamicMembershipCache;
		mutable FString EvaluationError;
	};

	static bool CompileDynamicCollectionFilter(
		const FCollectionNameType& Collection,
		TOptional<FDynamicCollectionFilter>& OutFilter,
		FString& OutError)
	{
		FString QueryText;
		FText QueryError;
		if (!Container()->GetDynamicQueryText(
			Collection.Name, Collection.Type, QueryText, &QueryError))
		{
			OutError = QueryError.IsEmpty()
				? FString::Printf(
					TEXT("Failed to read dynamic query for collection '%s'"),
					*Collection.Name.ToString())
				: QueryError.ToString();
			return false;
		}

		// A newly created dynamic collection has no saved query yet. Treat that
		// explicit unconfigured state as empty instead of letting an empty text
		// filter match every asset in the project.
		if (QueryText.IsEmpty())
		{
			OutFilter.Reset();
			return true;
		}

		FTextFilterExpressionEvaluator QueryValidator(
			ETextFilterExpressionEvaluatorMode::Complex);
		QueryValidator.SetFilterText(FText::FromString(QueryText));
		const FText FilterError = QueryValidator.GetFilterErrorText();
		if (!FilterError.IsEmpty())
		{
			OutError = FString::Printf(
				TEXT("Dynamic query for collection '%s' is invalid: %s"),
				*Collection.Name.ToString(),
				*FilterError.ToString());
			return false;
		}

		OutFilter = FDynamicCollectionFilter{
			Collection};
		return true;
	}

	static bool ResolveCollectionAssets(
		const FCollectionNameType& RootCollection,
		ECollectionRecursionFlags::Flags Recursion,
		TArray<FSoftObjectPath>& OutAssets,
		FString& OutError)
	{
		TArray<FCollectionNameType> CollectionScope;
		GatherCollectionScope(RootCollection, Recursion, CollectionScope);

		TSet<FSoftObjectPath> UniqueAssets;
		TArray<FDynamicCollectionFilter> DynamicFilters;
		for (const FCollectionNameType& Collection : CollectionScope)
		{
			ECollectionStorageMode::Type StorageMode;
			if (!Container()->GetCollectionStorageMode(
				Collection.Name, Collection.Type, StorageMode))
			{
				OutError = FString::Printf(
					TEXT("Failed to read storage mode for collection '%s'"),
					*Collection.Name.ToString());
				return false;
			}

			if (StorageMode == ECollectionStorageMode::Static)
			{
				TArray<FSoftObjectPath> StoredAssets;
				if (!Container()->GetAssetsInCollection(
					Collection.Name,
					Collection.Type,
					StoredAssets,
					ECollectionRecursionFlags::Self))
				{
					OutError = FString::Printf(
						TEXT("Failed to read assets for collection '%s'"),
						*Collection.Name.ToString());
					return false;
				}
				for (const FSoftObjectPath& StoredAsset : StoredAssets)
				{
					UniqueAssets.Add(StoredAsset);
				}
				continue;
			}

			TOptional<FDynamicCollectionFilter> DynamicFilter;
			if (!CompileDynamicCollectionFilter(
				Collection, DynamicFilter, OutError))
			{
				return false;
			}
			if (DynamicFilter.IsSet())
			{
				DynamicFilters.Add(MoveTemp(DynamicFilter.GetValue()));
			}
		}

		if (DynamicFilters.Num() > 0)
		{
			UContentBrowserDataSubsystem* ContentBrowserData =
				IContentBrowserDataModule::Get().GetSubsystem();
			if (!ContentBrowserData)
			{
				OutError = TEXT(
					"Content Browser data subsystem is unavailable; "
					"dynamic collection membership cannot be resolved");
				return false;
			}
			if (!ContentBrowserData->GetActiveDataSources().Contains(
				FName(TEXT("AssetData"))))
			{
				OutError = TEXT(
					"Content Browser AssetData source is inactive; "
					"dynamic collection membership cannot be resolved");
				return false;
			}

			FContentBrowserDataFilter DataFilter;
			DataFilter.bRecursivePaths = true;
			DataFilter.ItemTypeFilter =
				EContentBrowserItemTypeFilter::IncludeFiles;
			DataFilter.ItemCategoryFilter =
				EContentBrowserItemCategoryFilter::IncludeAssets;
			DataFilter.ItemAttributeFilter =
				EContentBrowserItemAttributeFilter::IncludeAll;

			static const FName AllContentRoot(TEXT("/All"));
			TArray<FCollectionNameType> KnownCollections;
			Container()->GetCollections(KnownCollections);
			FString EvaluationError;
			ContentBrowserData->EnumerateItemsUnderPath(
				AllContentRoot,
				DataFilter,
				[&DynamicFilters,
				 &KnownCollections,
				 &UniqueAssets,
				 &EvaluationError](
					FContentBrowserItem&& Item)
				{
					FDynamicCollectionExpressionContext Context(
						Item, KnownCollections);
					bool bMatches = false;
					for (const FDynamicCollectionFilter& Filter
						: DynamicFilters)
					{
						if (!Context.TestDynamicCollection(
							Filter.Collection, bMatches))
						{
							EvaluationError =
								Context.GetEvaluationError();
							return false;
						}
						if (bMatches)
						{
							break;
						}
					}
					if (!bMatches)
					{
						return true;
					}

					FAssetData AssetData;
					if (Item.Legacy_TryGetAssetData(AssetData))
					{
						UniqueAssets.Add(AssetData.GetSoftObjectPath());
					}
					return true;
				});
			if (!EvaluationError.IsEmpty())
			{
				OutError = MoveTemp(EvaluationError);
				return false;
			}
		}

		OutAssets.Reserve(UniqueAssets.Num());
		for (const FSoftObjectPath& Asset : UniqueAssets)
		{
			OutAssets.Add(Asset);
		}
		OutAssets.Sort(
			[](const FSoftObjectPath& Left, const FSoftObjectPath& Right)
			{
				return Left.ToString() < Right.ToString();
			});
		return true;
	}

	static bool CollectionToJson(
		const FCollectionNameType& Collection,
		TSharedPtr<FJsonObject>& OutObject,
		FString& OutError)
	{
		const TSharedRef<ICollectionContainer>& C = Container();
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Collection.Name.ToString());
		Obj->SetStringField(TEXT("share_type"), ShareTypeToString(Collection.Type));
		Obj->SetBoolField(TEXT("read_only"), C->IsReadOnly(Collection.Type));

		ECollectionStorageMode::Type StorageMode;
		if (C->GetCollectionStorageMode(Collection.Name, Collection.Type, StorageMode))
		{
			Obj->SetStringField(TEXT("storage_mode"), StorageModeToString(StorageMode));
		}

		TArray<FSoftObjectPath> Assets;
		if (!ResolveCollectionAssets(
			Collection, ECollectionRecursionFlags::Self, Assets, OutError))
		{
			return false;
		}
		Obj->SetNumberField(TEXT("asset_count"), Assets.Num());
		if (StorageMode == ECollectionStorageMode::Dynamic)
		{
			FString QueryText;
			FText QueryError;
			if (!C->GetDynamicQueryText(
				Collection.Name, Collection.Type, QueryText, &QueryError))
			{
				OutError = QueryError.IsEmpty()
					? TEXT("Failed to read dynamic collection query")
					: QueryError.ToString();
				return false;
			}
			Obj->SetStringField(TEXT("query_text"), QueryText);
		}

		TArray<FCollectionNameType> Children;
		C->GetChildCollections(Collection.Name, Collection.Type, Children);
		Obj->SetNumberField(TEXT("child_count"), Children.Num());

		const TOptional<FCollectionNameType> Parent = C->GetParentCollection(Collection.Name, Collection.Type);
		if (Parent.IsSet())
		{
			Obj->SetStringField(TEXT("parent_name"), Parent->Name.ToString());
			Obj->SetStringField(TEXT("parent_share_type"), ShareTypeToString(Parent->Type));
		}

		TOptional<FLinearColor> Color;
		if (C->GetCollectionColor(Collection.Name, Collection.Type, Color) && Color.IsSet())
		{
			TSharedPtr<FJsonObject> ColorObj = MakeShared<FJsonObject>();
			ColorObj->SetNumberField(TEXT("r"), Color->R);
			ColorObj->SetNumberField(TEXT("g"), Color->G);
			ColorObj->SetNumberField(TEXT("b"), Color->B);
			ColorObj->SetNumberField(TEXT("a"), Color->A);
			Obj->SetObjectField(TEXT("color"), ColorObj);
		}

		OutObject = MoveTemp(Obj);
		return true;
	}

	static FMonolithActionResult MutatingReadOnlyError(ECollectionShareType::Type ShareType)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Collections of share_type '%s' are read-only"), *ShareTypeToString(ShareType)),
			-32602);
	}

	static FMonolithActionResult CollectionNotFoundError(
		FName Name,
		ECollectionShareType::Type ShareType)
	{
		return FMonolithActionResult::Error(
			FString::Printf(
				TEXT("Collection '%s' (%s) does not exist"),
				*Name.ToString(),
				*ShareTypeToString(ShareType)),
			-32602);
	}

	static TOptional<FMonolithActionResult> GetMissingCollectionError(
		FName Name,
		ECollectionShareType::Type ShareType)
	{
		if (!Container()->CollectionExists(Name, ShareType))
		{
			return CollectionNotFoundError(Name, ShareType);
		}
		return TOptional<FMonolithActionResult>();
	}
}

void FAssetCollectionActions::Register(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("collection"), TEXT("list_collections"),
		TEXT("List Content Browser collections, optionally filtered by share_type."),
		FMonolithActionHandler::CreateStatic(&ListCollections),
		FParamSchemaBuilder().Optional(TEXT("share_type"), TEXT("string"), TEXT("local, private, shared, system, or all"), TEXT("all")).Build());

	Registry.RegisterAction(TEXT("collection"), TEXT("get_collection"),
		TEXT("Get Content Browser collection details."),
		FMonolithActionHandler::CreateStatic(&GetCollection),
		FParamSchemaBuilder().Required(TEXT("name"), TEXT("string"), TEXT("Collection name")).Optional(TEXT("share_type"), TEXT("string"), TEXT("local, private, shared, or system"), TEXT("local")).Build());

	Registry.RegisterAction(TEXT("collection"), TEXT("create_collection"),
		TEXT("Create a static or dynamic Content Browser collection."),
		FMonolithActionHandler::CreateStatic(&CreateCollection),
		FParamSchemaBuilder().Required(TEXT("name"), TEXT("string"), TEXT("Collection name")).Optional(TEXT("share_type"), TEXT("string"), TEXT("local, private, shared, or system"), TEXT("local")).Optional(TEXT("storage_mode"), TEXT("string"), TEXT("static or dynamic"), TEXT("static")).Build());

	Registry.RegisterAction(TEXT("collection"), TEXT("delete_collection"),
		TEXT("Delete a Content Browser collection. Non-empty collections require force=true."),
		FMonolithActionHandler::CreateStatic(&DeleteCollection),
		FParamSchemaBuilder().Required(TEXT("name"), TEXT("string"), TEXT("Collection name")).Optional(TEXT("share_type"), TEXT("string"), TEXT("local, private, shared, or system"), TEXT("local")).Optional(TEXT("force"), TEXT("bool"), TEXT("Allow deleting non-empty collection"), TEXT("false")).Build());

	Registry.RegisterAction(TEXT("collection"), TEXT("add_assets"),
		TEXT("Add one or more assets to a static Content Browser collection."),
		FMonolithActionHandler::CreateStatic(&AddAssets),
		FParamSchemaBuilder().Required(TEXT("name"), TEXT("string"), TEXT("Collection name")).Optional(TEXT("share_type"), TEXT("string"), TEXT("local, private, shared, or system"), TEXT("local")).Optional(TEXT("asset_path"), TEXT("string"), TEXT("Single asset path")).Optional(TEXT("asset_paths"), TEXT("array"), TEXT("Asset path array")).Build());

	Registry.RegisterAction(TEXT("collection"), TEXT("remove_assets"),
		TEXT("Remove one or more assets from a static Content Browser collection."),
		FMonolithActionHandler::CreateStatic(&RemoveAssets),
		FParamSchemaBuilder().Required(TEXT("name"), TEXT("string"), TEXT("Collection name")).Optional(TEXT("share_type"), TEXT("string"), TEXT("local, private, shared, or system"), TEXT("local")).Optional(TEXT("asset_path"), TEXT("string"), TEXT("Single asset path")).Optional(TEXT("asset_paths"), TEXT("array"), TEXT("Asset path array")).Build());

	Registry.RegisterAction(TEXT("collection"), TEXT("list_assets"),
		TEXT("List asset paths in a collection."),
		FMonolithActionHandler::CreateStatic(&ListAssets),
		FParamSchemaBuilder().Required(TEXT("name"), TEXT("string"), TEXT("Collection name")).Optional(TEXT("share_type"), TEXT("string"), TEXT("local, private, shared, or system"), TEXT("local")).Optional(TEXT("recursive"), TEXT("string"), TEXT("self, children, parents, or all"), TEXT("self")).Build());

	Registry.RegisterAction(TEXT("collection"), TEXT("contains_asset"),
		TEXT("Check whether a collection contains an asset."),
		FMonolithActionHandler::CreateStatic(&ContainsAsset),
		FParamSchemaBuilder().Required(TEXT("name"), TEXT("string"), TEXT("Collection name")).Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path")).Optional(TEXT("share_type"), TEXT("string"), TEXT("local, private, shared, or system"), TEXT("local")).Optional(TEXT("recursive"), TEXT("string"), TEXT("self, children, parents, or all"), TEXT("self")).Build());

	Registry.RegisterAction(TEXT("collection"), TEXT("set_dynamic_query"),
		TEXT("Set query text for a dynamic collection."),
		FMonolithActionHandler::CreateStatic(&SetDynamicQuery),
		FParamSchemaBuilder().Required(TEXT("name"), TEXT("string"), TEXT("Collection name")).Required(TEXT("query_text"), TEXT("string"), TEXT("Dynamic query text")).Optional(TEXT("share_type"), TEXT("string"), TEXT("local, private, shared, or system"), TEXT("local")).Build());

	Registry.RegisterAction(TEXT("collection"), TEXT("get_dynamic_query"),
		TEXT("Get query text from a dynamic collection."),
		FMonolithActionHandler::CreateStatic(&GetDynamicQuery),
		FParamSchemaBuilder().Required(TEXT("name"), TEXT("string"), TEXT("Collection name")).Optional(TEXT("share_type"), TEXT("string"), TEXT("local, private, shared, or system"), TEXT("local")).Build());

	Registry.RegisterAction(TEXT("collection"), TEXT("set_collection_color"),
		TEXT("Set or clear a collection color. Omit color to clear."),
		FMonolithActionHandler::CreateStatic(&SetCollectionColor),
		FParamSchemaBuilder().Required(TEXT("name"), TEXT("string"), TEXT("Collection name")).Optional(TEXT("share_type"), TEXT("string"), TEXT("local, private, shared, or system"), TEXT("local")).Optional(TEXT("color"), TEXT("object"), TEXT("{r,g,b,a} in 0..1; omit to clear")).Build());

	Registry.RegisterAction(TEXT("collection"), TEXT("validate_collection_name"),
		TEXT("Validate a collection name for a share type."),
		FMonolithActionHandler::CreateStatic(&ValidateCollectionName),
		FParamSchemaBuilder().Required(TEXT("name"), TEXT("string"), TEXT("Collection name")).Optional(TEXT("share_type"), TEXT("string"), TEXT("local, private, shared, system, or all"), TEXT("local")).Build());

	Registry.RegisterAction(TEXT("collection"), TEXT("create_unique_collection_name"),
		TEXT("Generate a valid unique collection name from a base name without creating a collection."),
		FMonolithActionHandler::CreateStatic(&CreateUniqueCollectionName),
		FParamSchemaBuilder().Required(TEXT("base_name"), TEXT("string"), TEXT("Base collection name")).Optional(TEXT("share_type"), TEXT("string"), TEXT("local, private, shared, or system"), TEXT("local")).Build());
}

FMonolithActionResult FAssetCollectionActions::ListCollections(const TSharedPtr<FJsonObject>& Params)
{
	FString ShareTypeText;
	if (Params.IsValid() && Params->HasField(TEXT("share_type")))
	{
		if (!Params->HasTypedField<EJson::String>(TEXT("share_type")))
		{
			return FMonolithActionResult::Error(TEXT("share_type must be a string"), -32602);
		}
		ShareTypeText = Params->GetStringField(TEXT("share_type"));
	}
	const bool bFilter = !ShareTypeText.IsEmpty() && !ShareTypeText.Equals(TEXT("all"), ESearchCase::IgnoreCase);
	ECollectionShareType::Type FilterType = ECollectionShareType::CST_All;
	if (!MonolithCollection::TryParseShareType(ShareTypeText, FilterType, true))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid share_type: %s"), *ShareTypeText), -32602);
	}

	TArray<FCollectionNameType> Collections;
	MonolithCollection::Container()->GetCollections(Collections);

	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FCollectionNameType& Collection : Collections)
	{
		if (bFilter && Collection.Type != FilterType)
		{
			continue;
		}
		TSharedPtr<FJsonObject> CollectionObject;
		FString CollectionError;
		if (!MonolithCollection::CollectionToJson(
			Collection, CollectionObject, CollectionError))
		{
			return FMonolithActionResult::Error(CollectionError, -32603);
		}
		Rows.Add(MakeShared<FJsonValueObject>(CollectionObject));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("collections"), Rows);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FAssetCollectionActions::GetCollection(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	FName Name;
	if (!MonolithCollection::GetRequiredName(Params, Name, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	ECollectionShareType::Type ShareType;
	if (!MonolithCollection::GetShareType(Params, ShareType, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	const TOptional<FMonolithActionResult> MissingCollection =
		MonolithCollection::GetMissingCollectionError(Name, ShareType);
	if (MissingCollection.IsSet())
	{
		return MissingCollection.GetValue();
	}
	TSharedPtr<FJsonObject> CollectionObject;
	if (!MonolithCollection::CollectionToJson(
		FCollectionNameType(Name, ShareType), CollectionObject, Error))
	{
		return FMonolithActionResult::Error(Error, -32603);
	}
	return FMonolithActionResult::Success(CollectionObject);
}

FMonolithActionResult FAssetCollectionActions::CreateCollection(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	FName Name;
	if (!MonolithCollection::GetRequiredName(Params, Name, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	ECollectionShareType::Type ShareType;
	if (!MonolithCollection::GetShareType(Params, ShareType, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	if (MonolithCollection::Container()->IsReadOnly(ShareType))
	{
		return MonolithCollection::MutatingReadOnlyError(ShareType);
	}

	ECollectionStorageMode::Type StorageMode;
	if (!MonolithCollection::GetStorageMode(Params, StorageMode, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	FText ErrorText;
	const bool bSuccess = MonolithCollection::Container()->CreateCollection(Name, ShareType, StorageMode, &ErrorText);
	if (!bSuccess)
	{
		return FMonolithActionResult::Error(ErrorText.IsEmpty() ? TEXT("Failed to create collection") : ErrorText.ToString(), -32603);
	}
	return GetCollection(Params);
}

FMonolithActionResult FAssetCollectionActions::DeleteCollection(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	FName Name;
	if (!MonolithCollection::GetRequiredName(Params, Name, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	ECollectionShareType::Type ShareType;
	if (!MonolithCollection::GetShareType(Params, ShareType, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	bool bForce = false;
	if (Params->HasField(TEXT("force")))
	{
		if (!Params->HasTypedField<EJson::Boolean>(TEXT("force")))
		{
			return FMonolithActionResult::Error(TEXT("force must be a bool"), -32602);
		}
		bForce = Params->GetBoolField(TEXT("force"));
	}
	const TOptional<FMonolithActionResult> MissingCollection =
		MonolithCollection::GetMissingCollectionError(Name, ShareType);
	if (MissingCollection.IsSet())
	{
		return MissingCollection.GetValue();
	}
	if (MonolithCollection::Container()->IsReadOnly(ShareType))
	{
		return MonolithCollection::MutatingReadOnlyError(ShareType);
	}
	TArray<FSoftObjectPath> Assets;
	MonolithCollection::Container()->GetAssetsInCollection(Name, ShareType, Assets);
	if (!bForce && Assets.Num() > 0)
	{
		return FMonolithActionResult::Error(TEXT("Collection is non-empty; pass force=true to delete"), -32602);
	}

	FText ErrorText;
	const bool bSuccess = MonolithCollection::Container()->DestroyCollection(Name, ShareType, &ErrorText);
	if (!bSuccess)
	{
		return FMonolithActionResult::Error(ErrorText.IsEmpty() ? TEXT("Failed to delete collection") : ErrorText.ToString(), -32603);
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("deleted"), true);
	Result->SetStringField(TEXT("name"), Name.ToString());
	Result->SetStringField(TEXT("share_type"), MonolithCollection::ShareTypeToString(ShareType));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FAssetCollectionActions::AddAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	FName Name;
	if (!MonolithCollection::GetRequiredName(Params, Name, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	ECollectionShareType::Type ShareType;
	if (!MonolithCollection::GetShareType(Params, ShareType, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	TArray<FSoftObjectPath> Paths;
	if (!MonolithCollection::ParseAssetPaths(Params, Paths, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	const TOptional<FMonolithActionResult> MissingCollection =
		MonolithCollection::GetMissingCollectionError(Name, ShareType);
	if (MissingCollection.IsSet())
	{
		return MissingCollection.GetValue();
	}
	if (MonolithCollection::Container()->IsReadOnly(ShareType))
	{
		return MonolithCollection::MutatingReadOnlyError(ShareType);
	}
	int32 NumAdded = 0;
	FText ErrorText;
	const bool bSuccess = MonolithCollection::Container()->AddToCollection(Name, ShareType, Paths, &NumAdded, &ErrorText);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), bSuccess);
	Result->SetNumberField(TEXT("requested"), Paths.Num());
	Result->SetNumberField(TEXT("added"), NumAdded);
	Result->SetStringField(TEXT("collection"), Name.ToString());
	if (!bSuccess)
	{
		FMonolithActionResult ErrorResult = FMonolithActionResult::Error(ErrorText.IsEmpty() ? TEXT("Failed to add one or more assets") : ErrorText.ToString(), -32603);
		ErrorResult.WithErrorData(Result);
		return ErrorResult;
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FAssetCollectionActions::RemoveAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	FName Name;
	if (!MonolithCollection::GetRequiredName(Params, Name, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	ECollectionShareType::Type ShareType;
	if (!MonolithCollection::GetShareType(Params, ShareType, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	TArray<FSoftObjectPath> Paths;
	if (!MonolithCollection::ParseAssetPaths(Params, Paths, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	const TOptional<FMonolithActionResult> MissingCollection =
		MonolithCollection::GetMissingCollectionError(Name, ShareType);
	if (MissingCollection.IsSet())
	{
		return MissingCollection.GetValue();
	}
	if (MonolithCollection::Container()->IsReadOnly(ShareType))
	{
		return MonolithCollection::MutatingReadOnlyError(ShareType);
	}
	int32 NumRemoved = 0;
	FText ErrorText;
	const bool bSuccess = MonolithCollection::Container()->RemoveFromCollection(Name, ShareType, Paths, &NumRemoved, &ErrorText);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), bSuccess);
	Result->SetNumberField(TEXT("requested"), Paths.Num());
	Result->SetNumberField(TEXT("removed"), NumRemoved);
	Result->SetStringField(TEXT("collection"), Name.ToString());
	if (!bSuccess)
	{
		FMonolithActionResult ErrorResult = FMonolithActionResult::Error(ErrorText.IsEmpty() ? TEXT("Failed to remove one or more assets") : ErrorText.ToString(), -32603);
		ErrorResult.WithErrorData(Result);
		return ErrorResult;
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FAssetCollectionActions::ListAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	FName Name;
	if (!MonolithCollection::GetRequiredName(Params, Name, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	ECollectionRecursionFlags::Flags Recursion = ECollectionRecursionFlags::Self;
	if (!MonolithCollection::GetRecursion(Params, Recursion, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	TArray<FSoftObjectPath> Assets;
	ECollectionShareType::Type ShareType;
	if (!MonolithCollection::GetShareType(Params, ShareType, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	const TOptional<FMonolithActionResult> MissingCollection =
		MonolithCollection::GetMissingCollectionError(Name, ShareType);
	if (MissingCollection.IsSet())
	{
		return MissingCollection.GetValue();
	}
	if (!MonolithCollection::ResolveCollectionAssets(
		FCollectionNameType(Name, ShareType), Recursion, Assets, Error))
	{
		return FMonolithActionResult::Error(Error, -32603);
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(Assets.Num());
	for (const FSoftObjectPath& Asset : Assets)
	{
		Rows.Add(MakeShared<FJsonValueString>(Asset.ToString()));
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("collection"), Name.ToString());
	Result->SetArrayField(TEXT("assets"), Rows);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FAssetCollectionActions::ContainsAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	FName Name;
	if (!MonolithCollection::GetRequiredName(Params, Name, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	FString AssetPath;
	if (!Params->HasTypedField<EJson::String>(TEXT("asset_path")))
	{
		return FMonolithActionResult::Error(TEXT("asset_path must be a string"), -32602);
	}
	AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: asset_path"), -32602);
	}
	ECollectionRecursionFlags::Flags Recursion = ECollectionRecursionFlags::Self;
	if (!MonolithCollection::GetRecursion(Params, Recursion, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	ECollectionShareType::Type ShareType;
	if (!MonolithCollection::GetShareType(Params, ShareType, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	const TOptional<FMonolithActionResult> MissingCollection =
		MonolithCollection::GetMissingCollectionError(Name, ShareType);
	if (MissingCollection.IsSet())
	{
		return MissingCollection.GetValue();
	}
	TArray<FSoftObjectPath> Assets;
	if (!MonolithCollection::ResolveCollectionAssets(
		FCollectionNameType(Name, ShareType), Recursion, Assets, Error))
	{
		return FMonolithActionResult::Error(Error, -32603);
	}
	const FString NormalizedAssetPath =
		MonolithCollection::NormalizeObjectPath(AssetPath);
	const bool bContains =
		Assets.Contains(FSoftObjectPath(NormalizedAssetPath));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("collection"), Name.ToString());
	Result->SetStringField(TEXT("asset_path"), NormalizedAssetPath);
	Result->SetBoolField(TEXT("contains"), bContains);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FAssetCollectionActions::SetDynamicQuery(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	FName Name;
	if (!MonolithCollection::GetRequiredName(Params, Name, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	FString QueryText;
	if (!Params->HasTypedField<EJson::String>(TEXT("query_text")))
	{
		return FMonolithActionResult::Error(TEXT("query_text must be a string"), -32602);
	}
	QueryText = Params->GetStringField(TEXT("query_text"));
	if (QueryText.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: query_text"), -32602);
	}
	ECollectionShareType::Type ShareType;
	if (!MonolithCollection::GetShareType(Params, ShareType, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	const TOptional<FMonolithActionResult> MissingCollection =
		MonolithCollection::GetMissingCollectionError(Name, ShareType);
	if (MissingCollection.IsSet())
	{
		return MissingCollection.GetValue();
	}
	if (MonolithCollection::Container()->IsReadOnly(ShareType))
	{
		return MonolithCollection::MutatingReadOnlyError(ShareType);
	}
	FText ErrorText;
	const bool bSuccess = MonolithCollection::Container()->SetDynamicQueryText(Name, ShareType, QueryText, &ErrorText);
	if (!bSuccess)
	{
		return FMonolithActionResult::Error(ErrorText.IsEmpty() ? TEXT("Failed to set dynamic query") : ErrorText.ToString(), -32603);
	}
	return GetDynamicQuery(Params);
}

FMonolithActionResult FAssetCollectionActions::GetDynamicQuery(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	FName Name;
	if (!MonolithCollection::GetRequiredName(Params, Name, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	FString QueryText;
	FText ErrorText;
	ECollectionShareType::Type ShareType;
	if (!MonolithCollection::GetShareType(Params, ShareType, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	const TOptional<FMonolithActionResult> MissingCollection =
		MonolithCollection::GetMissingCollectionError(Name, ShareType);
	if (MissingCollection.IsSet())
	{
		return MissingCollection.GetValue();
	}
	const bool bSuccess = MonolithCollection::Container()->GetDynamicQueryText(Name, ShareType, QueryText, &ErrorText);
	if (!bSuccess)
	{
		return FMonolithActionResult::Error(ErrorText.IsEmpty() ? TEXT("Failed to get dynamic query") : ErrorText.ToString(), -32603);
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("collection"), Name.ToString());
	Result->SetStringField(TEXT("query_text"), QueryText);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FAssetCollectionActions::SetCollectionColor(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	FName Name;
	if (!MonolithCollection::GetRequiredName(Params, Name, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	ECollectionShareType::Type ShareType;
	if (!MonolithCollection::GetShareType(Params, ShareType, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	TOptional<FLinearColor> NewColor;
	const TSharedPtr<FJsonObject>* ColorObj = nullptr;
	if (Params->HasField(TEXT("color")))
	{
		if (!Params->HasTypedField<EJson::Object>(TEXT("color"))
			|| !Params->TryGetObjectField(TEXT("color"), ColorObj)
			|| !ColorObj
			|| !ColorObj->IsValid())
		{
			return FMonolithActionResult::Error(TEXT("color must be an object"), -32602);
		}

		double R = 0.0;
		double G = 0.0;
		double B = 0.0;
		double A = 1.0;
		if (!(*ColorObj)->HasTypedField<EJson::Number>(TEXT("r"))
			|| !(*ColorObj)->HasTypedField<EJson::Number>(TEXT("g"))
			|| !(*ColorObj)->HasTypedField<EJson::Number>(TEXT("b")))
		{
			return FMonolithActionResult::Error(TEXT("color must contain numeric r, g, and b fields"), -32602);
		}
		R = (*ColorObj)->GetNumberField(TEXT("r"));
		G = (*ColorObj)->GetNumberField(TEXT("g"));
		B = (*ColorObj)->GetNumberField(TEXT("b"));
		if ((*ColorObj)->HasField(TEXT("a")))
		{
			if (!(*ColorObj)->HasTypedField<EJson::Number>(TEXT("a")))
			{
				return FMonolithActionResult::Error(TEXT("color.a must be numeric"), -32602);
			}
			A = (*ColorObj)->GetNumberField(TEXT("a"));
		}
		if (!FMath::IsFinite(R) || !FMath::IsFinite(G) || !FMath::IsFinite(B) || !FMath::IsFinite(A)
			|| R < 0.0 || R > 1.0
			|| G < 0.0 || G > 1.0
			|| B < 0.0 || B > 1.0
			|| A < 0.0 || A > 1.0)
		{
			return FMonolithActionResult::Error(TEXT("color channels must be finite numbers in the range 0..1"), -32602);
		}
		NewColor = FLinearColor(static_cast<float>(R), static_cast<float>(G), static_cast<float>(B), static_cast<float>(A));
	}
	const TOptional<FMonolithActionResult> MissingCollection =
		MonolithCollection::GetMissingCollectionError(Name, ShareType);
	if (MissingCollection.IsSet())
	{
		return MissingCollection.GetValue();
	}
	if (MonolithCollection::Container()->IsReadOnly(ShareType))
	{
		return MonolithCollection::MutatingReadOnlyError(ShareType);
	}
	FText ErrorText;
	const bool bSuccess = MonolithCollection::Container()->SetCollectionColor(Name, ShareType, NewColor, &ErrorText);
	if (!bSuccess)
	{
		return FMonolithActionResult::Error(ErrorText.IsEmpty() ? TEXT("Failed to set collection color") : ErrorText.ToString(), -32603);
	}
	return GetCollection(Params);
}

FMonolithActionResult FAssetCollectionActions::ValidateCollectionName(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	FName Name;
	if (!MonolithCollection::GetRequiredName(Params, Name, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	FText ErrorText;
	ECollectionShareType::Type ShareType;
	if (!MonolithCollection::GetShareType(Params, ShareType, Error, true))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	const bool bValid = MonolithCollection::Container()->IsValidCollectionName(Name.ToString(), ShareType, &ErrorText);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), Name.ToString());
	Result->SetStringField(TEXT("share_type"), MonolithCollection::ShareTypeToString(ShareType));
	Result->SetBoolField(TEXT("valid"), bValid);
	if (!ErrorText.IsEmpty())
	{
		Result->SetStringField(TEXT("error"), ErrorText.ToString());
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FAssetCollectionActions::CreateUniqueCollectionName(const TSharedPtr<FJsonObject>& Params)
{
	FString BaseName;
	if (!Params.IsValid() || !Params->HasField(TEXT("base_name")))
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: base_name"), -32602);
	}
	if (!Params->HasTypedField<EJson::String>(TEXT("base_name")))
	{
		return FMonolithActionResult::Error(TEXT("base_name must be a string"), -32602);
	}
	BaseName = Params->GetStringField(TEXT("base_name"));
	if (BaseName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: base_name"), -32602);
	}
	FString Error;
	ECollectionShareType::Type ShareType;
	if (!MonolithCollection::GetShareType(Params, ShareType, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	FName UniqueName;
	MonolithCollection::Container()->CreateUniqueCollectionName(FName(*BaseName), ShareType, UniqueName);
	FText ValidationError;
	if (!MonolithCollection::Container()->IsValidCollectionName(
		UniqueName.ToString(),
		ShareType,
		&ValidationError))
	{
		const FString ValidationMessage =
			ValidationError.IsEmpty() ? TEXT("invalid candidate") : ValidationError.ToString();
		return FMonolithActionResult::Error(
			FString::Printf(
				TEXT("base_name cannot produce a valid collection name: %s"),
				*ValidationMessage),
			-32602);
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("base_name"), BaseName);
	Result->SetStringField(TEXT("unique_name"), UniqueName.ToString());
	Result->SetStringField(TEXT("share_type"), MonolithCollection::ShareTypeToString(ShareType));
	return FMonolithActionResult::Success(Result);
}
