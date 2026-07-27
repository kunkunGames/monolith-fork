#include "Actions/AssetCollectionActions.h"

#include "CollectionManagerModule.h"
#include "CollectionManagerTypes.h"
#include "ICollectionContainer.h"
#include "ICollectionManager.h"
#include "MonolithParamSchema.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
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
		if (Params.IsValid() && Params->HasField(TEXT("storage_mode")) && !Params->TryGetStringField(TEXT("storage_mode"), StorageMode))
		{
			OutError = TEXT("storage_mode must be a string");
			return false;
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
		if (Params.IsValid() && Params->HasField(TEXT("recursive")) && !Params->TryGetStringField(TEXT("recursive"), Recursion))
		{
			OutError = TEXT("recursive must be a string");
			return false;
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
		if (!Params->TryGetStringField(TEXT("name"), Name))
		{
			OutError = TEXT("name must be a string");
			return false;
		}
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
			if (Params->HasField(TEXT("share_type")) && !Params->TryGetStringField(TEXT("share_type"), ShareType))
			{
				OutError = TEXT("share_type must be a string");
				return false;
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
			if (!Params->TryGetStringField(TEXT("asset_path"), SinglePath))
			{
				OutError = TEXT("asset_path must be a string");
				return false;
			}
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
				if (!Value.IsValid() || !Value->TryGetString(Path))
				{
					OutError = FString::Printf(TEXT("asset_paths[%d] must be a string"), Index);
					return false;
				}
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

	static TSharedPtr<FJsonObject> CollectionToJson(const FCollectionNameType& Collection)
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
		C->GetAssetsInCollection(Collection.Name, Collection.Type, Assets);
		Obj->SetNumberField(TEXT("asset_count"), Assets.Num());

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

		return Obj;
	}

	static FMonolithActionResult MutatingReadOnlyError(ECollectionShareType::Type ShareType)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Collections of share_type '%s' are read-only"), *ShareTypeToString(ShareType)),
			-32602);
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
		TEXT("Create a unique collection name from a base name."),
		FMonolithActionHandler::CreateStatic(&CreateUniqueCollectionName),
		FParamSchemaBuilder().Required(TEXT("base_name"), TEXT("string"), TEXT("Base collection name")).Optional(TEXT("share_type"), TEXT("string"), TEXT("local, private, shared, or system"), TEXT("local")).Build());
}

FMonolithActionResult FAssetCollectionActions::ListCollections(const TSharedPtr<FJsonObject>& Params)
{
	FString ShareTypeText;
	if (Params.IsValid() && Params->HasField(TEXT("share_type")) && !Params->TryGetStringField(TEXT("share_type"), ShareTypeText))
	{
		return FMonolithActionResult::Error(TEXT("share_type must be a string"), -32602);
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
		Rows.Add(MakeShared<FJsonValueObject>(MonolithCollection::CollectionToJson(Collection)));
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
	if (!MonolithCollection::Container()->CollectionExists(Name, ShareType))
	{
		return FMonolithActionResult::Error(TEXT("Collection does not exist"), -32602);
	}
	return FMonolithActionResult::Success(MonolithCollection::CollectionToJson(FCollectionNameType(Name, ShareType)));
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
	if (MonolithCollection::Container()->IsReadOnly(ShareType))
	{
		return MonolithCollection::MutatingReadOnlyError(ShareType);
	}

	bool bForce = false;
	if (Params->HasField(TEXT("force")) && !Params->TryGetBoolField(TEXT("force"), bForce))
	{
		return FMonolithActionResult::Error(TEXT("force must be a bool"), -32602);
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
	if (MonolithCollection::Container()->IsReadOnly(ShareType))
	{
		return MonolithCollection::MutatingReadOnlyError(ShareType);
	}

	TArray<FSoftObjectPath> Paths;
	if (!MonolithCollection::ParseAssetPaths(Params, Paths, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
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
	if (MonolithCollection::Container()->IsReadOnly(ShareType))
	{
		return MonolithCollection::MutatingReadOnlyError(ShareType);
	}

	TArray<FSoftObjectPath> Paths;
	if (!MonolithCollection::ParseAssetPaths(Params, Paths, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
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
	MonolithCollection::Container()->GetAssetsInCollection(Name, ShareType, Assets, Recursion);

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
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FMonolithActionResult::Error(TEXT("asset_path must be a string"), -32602);
	}
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
	FText ErrorText;
	const bool bContains = MonolithCollection::Container()->IsObjectInCollection(
		FSoftObjectPath(MonolithCollection::NormalizeObjectPath(AssetPath)), Name, ShareType, Recursion, &ErrorText);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("collection"), Name.ToString());
	Result->SetStringField(TEXT("asset_path"), MonolithCollection::NormalizeObjectPath(AssetPath));
	Result->SetBoolField(TEXT("contains"), bContains);
	if (!ErrorText.IsEmpty())
	{
		Result->SetStringField(TEXT("note"), ErrorText.ToString());
	}
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
	if (!Params->TryGetStringField(TEXT("query_text"), QueryText))
	{
		return FMonolithActionResult::Error(TEXT("query_text must be a string"), -32602);
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
	if (MonolithCollection::Container()->IsReadOnly(ShareType))
	{
		return MonolithCollection::MutatingReadOnlyError(ShareType);
	}

	TOptional<FLinearColor> NewColor;
	const TSharedPtr<FJsonObject>* ColorObj = nullptr;
	if (Params->HasField(TEXT("color")))
	{
		if (!Params->TryGetObjectField(TEXT("color"), ColorObj) || !ColorObj || !ColorObj->IsValid())
		{
			return FMonolithActionResult::Error(TEXT("color must be an object"), -32602);
		}

		double R = 0.0;
		double G = 0.0;
		double B = 0.0;
		double A = 1.0;
		if (!(*ColorObj)->TryGetNumberField(TEXT("r"), R)
			|| !(*ColorObj)->TryGetNumberField(TEXT("g"), G)
			|| !(*ColorObj)->TryGetNumberField(TEXT("b"), B))
		{
			return FMonolithActionResult::Error(TEXT("color must contain numeric r, g, and b fields"), -32602);
		}
		if ((*ColorObj)->HasField(TEXT("a")) && !(*ColorObj)->TryGetNumberField(TEXT("a"), A))
		{
			return FMonolithActionResult::Error(TEXT("color.a must be numeric"), -32602);
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
	if (!Params->TryGetStringField(TEXT("base_name"), BaseName))
	{
		return FMonolithActionResult::Error(TEXT("base_name must be a string"), -32602);
	}
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
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("base_name"), BaseName);
	Result->SetStringField(TEXT("unique_name"), UniqueName.ToString());
	Result->SetStringField(TEXT("share_type"), MonolithCollection::ShareTypeToString(ShareType));
	return FMonolithActionResult::Success(Result);
}
