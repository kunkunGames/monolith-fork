// SPDX-License-Identifier: MIT

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

namespace MonolithCollectionActionsTestDetail
{
	static TSharedPtr<FJsonObject> MakeNamedParams(const FString& Name)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("name"), Name);
		Params->SetStringField(TEXT("share_type"), TEXT("local"));
		return Params;
	}

	static FMonolithActionResult Invoke(
		const FString& Action,
		const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("collection"),
			Action,
			Params);
	}

	static void DeleteIfPresent(const FString& Name)
	{
		TSharedPtr<FJsonObject> Params = MakeNamedParams(Name);
		Params->SetBoolField(TEXT("force"), true);
		Invoke(TEXT("delete_collection"), Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithCollectionRegistrationAndValidationTest,
	"Monolith.Collection.RegistrationAndValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithCollectionRegistrationAndValidationTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithCollectionActionsTestDetail;

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	const TArray<FString> ExpectedActions = {
		TEXT("list_collections"),
		TEXT("get_collection"),
		TEXT("create_collection"),
		TEXT("delete_collection"),
		TEXT("add_assets"),
		TEXT("remove_assets"),
		TEXT("list_assets"),
		TEXT("contains_asset"),
		TEXT("set_dynamic_query"),
		TEXT("get_dynamic_query"),
		TEXT("set_collection_color"),
		TEXT("validate_collection_name"),
		TEXT("create_unique_collection_name")
	};

	TestEqual(
		TEXT("collection namespace exposes exactly the intended action pack"),
		Registry.GetActions(TEXT("collection")).Num(),
		ExpectedActions.Num());
	for (const FString& Action : ExpectedActions)
	{
		TestTrue(
			*FString::Printf(TEXT("collection.%s is registered"), *Action),
			Registry.HasAction(TEXT("collection"), Action));
	}

	auto ExpectInvalidParams = [this](
		const FString& Label,
		const FString& Action,
		const TSharedPtr<FJsonObject>& Params,
		const FString& ExpectedField)
	{
		const FMonolithActionResult Result = Invoke(Action, Params);
		bool bPassed = true;
		bPassed &= TestFalse(*Label, Result.bSuccess);
		bPassed &= TestEqual(
			*FString::Printf(TEXT("%s uses invalid-params"), *Label),
			Result.ErrorCode,
			FMonolithJsonUtils::ErrInvalidParams);
		bPassed &= TestTrue(
			*FString::Printf(TEXT("%s identifies %s"), *Label, *ExpectedField),
			Result.ErrorMessage.Contains(ExpectedField));
		return bPassed;
	};

	bool bPassed = true;

	TSharedPtr<FJsonObject> BadShareType = MakeShared<FJsonObject>();
	BadShareType->SetNumberField(TEXT("share_type"), 7);
	bPassed &= ExpectInvalidParams(
		TEXT("list_collections rejects numeric share_type"),
		TEXT("list_collections"),
		BadShareType,
		TEXT("share_type"));

	TSharedPtr<FJsonObject> BadName = MakeShared<FJsonObject>();
	BadName->SetNumberField(TEXT("name"), 7);
	bPassed &= ExpectInvalidParams(
		TEXT("get_collection rejects numeric name"),
		TEXT("get_collection"),
		BadName,
		TEXT("name"));

	TSharedPtr<FJsonObject> BadStorageMode = MakeNamedParams(TEXT("MonolithValidationOnly"));
	BadStorageMode->SetNumberField(TEXT("storage_mode"), 7);
	bPassed &= ExpectInvalidParams(
		TEXT("create_collection rejects numeric storage_mode"),
		TEXT("create_collection"),
		BadStorageMode,
		TEXT("storage_mode"));

	TSharedPtr<FJsonObject> BadForce = MakeNamedParams(TEXT("MonolithValidationOnly"));
	BadForce->SetStringField(TEXT("force"), TEXT("true"));
	bPassed &= ExpectInvalidParams(
		TEXT("delete_collection rejects string force"),
		TEXT("delete_collection"),
		BadForce,
		TEXT("force"));

	TSharedPtr<FJsonObject> BadAssetPaths = MakeNamedParams(TEXT("MonolithValidationOnly"));
	TArray<TSharedPtr<FJsonValue>> AssetPaths;
	AssetPaths.Add(MakeShared<FJsonValueString>(TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture")));
	AssetPaths.Add(MakeShared<FJsonValueNumber>(7));
	BadAssetPaths->SetArrayField(TEXT("asset_paths"), AssetPaths);
	bPassed &= ExpectInvalidParams(
		TEXT("add_assets rejects non-string array entries"),
		TEXT("add_assets"),
		BadAssetPaths,
		TEXT("asset_paths[1]"));

	TSharedPtr<FJsonObject> BadContainsPath = MakeNamedParams(TEXT("MonolithValidationOnly"));
	BadContainsPath->SetNumberField(TEXT("asset_path"), 7);
	bPassed &= ExpectInvalidParams(
		TEXT("contains_asset rejects numeric asset_path"),
		TEXT("contains_asset"),
		BadContainsPath,
		TEXT("asset_path"));

	TSharedPtr<FJsonObject> EmptyQuery = MakeNamedParams(TEXT("MonolithValidationOnly"));
	EmptyQuery->SetStringField(TEXT("query_text"), TEXT(""));
	bPassed &= ExpectInvalidParams(
		TEXT("set_dynamic_query rejects empty query_text"),
		TEXT("set_dynamic_query"),
		EmptyQuery,
		TEXT("query_text"));

	const FString MissingName = FString::Printf(
		TEXT("MonolithMissing_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const TArray<FString> MissingCollectionActions = {
		TEXT("get_collection"),
		TEXT("delete_collection"),
		TEXT("list_assets"),
		TEXT("get_dynamic_query"),
		TEXT("set_collection_color")
	};
	for (const FString& Action : MissingCollectionActions)
	{
		bPassed &= ExpectInvalidParams(
			FString::Printf(TEXT("%s rejects a missing collection"), *Action),
			Action,
			MakeNamedParams(MissingName),
			TEXT("does not exist"));
	}

	TSharedPtr<FJsonObject> MissingContains = MakeNamedParams(MissingName);
	MissingContains->SetStringField(
		TEXT("asset_path"),
		TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
	bPassed &= ExpectInvalidParams(
		TEXT("contains_asset rejects a missing collection"),
		TEXT("contains_asset"),
		MissingContains,
		TEXT("does not exist"));

	const TArray<FString> MissingMembershipActions = {
		TEXT("add_assets"),
		TEXT("remove_assets")
	};
	for (const FString& Action : MissingMembershipActions)
	{
		TSharedPtr<FJsonObject> MissingMembership = MakeNamedParams(MissingName);
		MissingMembership->SetStringField(
			TEXT("asset_path"),
			TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
		bPassed &= ExpectInvalidParams(
			FString::Printf(TEXT("%s rejects a missing collection"), *Action),
			Action,
			MissingMembership,
			TEXT("does not exist"));
	}

	TSharedPtr<FJsonObject> MissingDynamicQuery = MakeNamedParams(MissingName);
	MissingDynamicQuery->SetStringField(TEXT("query_text"), TEXT("Type=Texture2D"));
	bPassed &= ExpectInvalidParams(
		TEXT("set_dynamic_query rejects a missing collection"),
		TEXT("set_dynamic_query"),
		MissingDynamicQuery,
		TEXT("does not exist"));

	TSharedPtr<FJsonObject> InvalidUniqueBase = MakeShared<FJsonObject>();
	InvalidUniqueBase->SetStringField(TEXT("base_name"), TEXT("Invalid/Collection"));
	InvalidUniqueBase->SetStringField(TEXT("share_type"), TEXT("local"));
	bPassed &= ExpectInvalidParams(
		TEXT("create_unique_collection_name rejects an invalid candidate"),
		TEXT("create_unique_collection_name"),
		InvalidUniqueBase,
		TEXT("base_name"));

	const FString UniqueBase = FString::Printf(
		TEXT("MonolithUnique_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	TSharedPtr<FJsonObject> UniqueParams = MakeShared<FJsonObject>();
	UniqueParams->SetStringField(TEXT("base_name"), UniqueBase);
	UniqueParams->SetStringField(TEXT("share_type"), TEXT("local"));
	const FMonolithActionResult UniqueResult =
		Invoke(TEXT("create_unique_collection_name"), UniqueParams);
	bPassed &= TestTrue(
		TEXT("create_unique_collection_name returns a candidate"),
		UniqueResult.bSuccess && UniqueResult.Result.IsValid());
	if (UniqueResult.bSuccess && UniqueResult.Result.IsValid())
	{
		const FString UniqueName =
			UniqueResult.Result->GetStringField(TEXT("unique_name"));
		if (TestFalse(
			TEXT("create_unique_collection_name returns a non-empty candidate"),
			UniqueName.IsEmpty()))
		{
			bPassed &= TestFalse(
				TEXT("create_unique_collection_name does not create the candidate"),
				Invoke(TEXT("get_collection"), MakeNamedParams(UniqueName)).bSuccess);
		}
		else
		{
			bPassed = false;
		}
	}

	TSharedPtr<FJsonObject> BadColor = MakeNamedParams(TEXT("MonolithValidationOnly"));
	TSharedPtr<FJsonObject> Color = MakeShared<FJsonObject>();
	Color->SetNumberField(TEXT("r"), 1.25);
	Color->SetNumberField(TEXT("g"), 0.25);
	Color->SetNumberField(TEXT("b"), 0.5);
	Color->SetNumberField(TEXT("a"), 1.0);
	BadColor->SetObjectField(TEXT("color"), Color);
	bPassed &= ExpectInvalidParams(
		TEXT("set_collection_color rejects out-of-range channels"),
		TEXT("set_collection_color"),
		BadColor,
		TEXT("color"));

	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithCollectionLocalLifecycleTest,
	"Monolith.Collection.LocalLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithCollectionLocalLifecycleTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithCollectionActionsTestDetail;

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString StaticName = FString::Printf(TEXT("MonolithStatic_%s"), *Suffix);
	const FString DynamicName = FString::Printf(TEXT("MonolithDynamic_%s"), *Suffix);
	const FString CycleAName = FString::Printf(TEXT("MonolithCycleA_%s"), *Suffix);
	const FString CycleBName = FString::Printf(TEXT("MonolithCycleB_%s"), *Suffix);
	const FString AssetPath = TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture");
	bool bStaticExists = false;
	bool bDynamicExists = false;
	bool bCycleAExists = false;
	bool bCycleBExists = false;

	ON_SCOPE_EXIT
	{
		if (bStaticExists)
		{
			DeleteIfPresent(StaticName);
		}
		if (bDynamicExists)
		{
			DeleteIfPresent(DynamicName);
		}
		if (bCycleAExists)
		{
			DeleteIfPresent(CycleAName);
		}
		if (bCycleBExists)
		{
			DeleteIfPresent(CycleBName);
		}
	};

	TSharedPtr<FJsonObject> CreateStaticParams = MakeNamedParams(StaticName);
	CreateStaticParams->SetStringField(TEXT("storage_mode"), TEXT("static"));
	const FMonolithActionResult CreateStatic =
		Invoke(TEXT("create_collection"), CreateStaticParams);
	if (!TestTrue(TEXT("create_collection creates a local static collection"), CreateStatic.bSuccess))
	{
		AddError(CreateStatic.ErrorMessage);
		return false;
	}
	bStaticExists = true;
	if (TestTrue(TEXT("create_collection returns collection details"), CreateStatic.Result.IsValid()))
	{
		TestEqual(
			TEXT("created static collection reports storage mode"),
			CreateStatic.Result->GetStringField(TEXT("storage_mode")),
			FString(TEXT("static")));
	}

	const FMonolithActionResult EmptyList =
		Invoke(TEXT("list_assets"), MakeNamedParams(StaticName));
	TestTrue(TEXT("list_assets succeeds for an existing empty collection"), EmptyList.bSuccess);
	if (TestTrue(TEXT("empty list_assets returns a payload"), EmptyList.Result.IsValid()))
	{
		TestEqual(
			TEXT("empty collection reports zero assets"),
			static_cast<int32>(EmptyList.Result->GetIntegerField(TEXT("count"))),
			0);
	}

	TSharedPtr<FJsonObject> AddParams = MakeNamedParams(StaticName);
	AddParams->SetStringField(TEXT("asset_path"), AssetPath);
	const FMonolithActionResult Add = Invoke(TEXT("add_assets"), AddParams);
	TestTrue(TEXT("add_assets adds an engine asset"), Add.bSuccess);
	if (TestTrue(TEXT("add_assets returns mutation counts"), Add.Result.IsValid()))
	{
		TestEqual(
			TEXT("add_assets reports one added asset"),
			static_cast<int32>(Add.Result->GetIntegerField(TEXT("added"))),
			1);
	}

	TSharedPtr<FJsonObject> ContainsParams = MakeNamedParams(StaticName);
	ContainsParams->SetStringField(TEXT("asset_path"), AssetPath);
	const FMonolithActionResult Contains =
		Invoke(TEXT("contains_asset"), ContainsParams);
	TestTrue(TEXT("contains_asset succeeds"), Contains.bSuccess);
	if (TestTrue(TEXT("contains_asset returns a payload"), Contains.Result.IsValid()))
	{
		TestTrue(
			TEXT("contains_asset sees the added asset"),
			Contains.Result->GetBoolField(TEXT("contains")));
	}

	const FMonolithActionResult ListAssets =
		Invoke(TEXT("list_assets"), MakeNamedParams(StaticName));
	TestTrue(TEXT("list_assets succeeds"), ListAssets.bSuccess);
	if (TestTrue(TEXT("list_assets returns a payload"), ListAssets.Result.IsValid()))
	{
		TestEqual(
			TEXT("list_assets reports one asset"),
			static_cast<int32>(ListAssets.Result->GetIntegerField(TEXT("count"))),
			1);
	}

	TSharedPtr<FJsonObject> SetColorParams = MakeNamedParams(StaticName);
	TSharedPtr<FJsonObject> Color = MakeShared<FJsonObject>();
	Color->SetNumberField(TEXT("r"), 0.1);
	Color->SetNumberField(TEXT("g"), 0.2);
	Color->SetNumberField(TEXT("b"), 0.3);
	Color->SetNumberField(TEXT("a"), 1.0);
	SetColorParams->SetObjectField(TEXT("color"), Color);
	const FMonolithActionResult SetColor =
		Invoke(TEXT("set_collection_color"), SetColorParams);
	TestTrue(TEXT("set_collection_color succeeds"), SetColor.bSuccess);
	if (TestTrue(TEXT("set_collection_color returns collection details"), SetColor.Result.IsValid()))
	{
		TestTrue(TEXT("collection details include the persisted color"), SetColor.Result->HasField(TEXT("color")));
	}

	TSharedPtr<FJsonObject> RemoveParams = MakeNamedParams(StaticName);
	RemoveParams->SetStringField(TEXT("asset_path"), AssetPath);
	const FMonolithActionResult Remove =
		Invoke(TEXT("remove_assets"), RemoveParams);
	TestTrue(TEXT("remove_assets succeeds"), Remove.bSuccess);
	if (TestTrue(TEXT("remove_assets returns mutation counts"), Remove.Result.IsValid()))
	{
		TestEqual(
			TEXT("remove_assets reports one removed asset"),
			static_cast<int32>(Remove.Result->GetIntegerField(TEXT("removed"))),
			1);
	}

	const FMonolithActionResult ContainsAfterRemove =
		Invoke(TEXT("contains_asset"), ContainsParams);
	TestTrue(TEXT("contains_asset succeeds after removal"), ContainsAfterRemove.bSuccess);
	if (TestTrue(TEXT("contains_asset after removal returns a payload"), ContainsAfterRemove.Result.IsValid()))
	{
		TestFalse(
			TEXT("contains_asset no longer sees the removed asset"),
			ContainsAfterRemove.Result->GetBoolField(TEXT("contains")));
	}

	TSharedPtr<FJsonObject> DeleteStaticParams = MakeNamedParams(StaticName);
	const FMonolithActionResult DeleteStatic =
		Invoke(TEXT("delete_collection"), DeleteStaticParams);
	TestTrue(TEXT("delete_collection removes the empty static collection"), DeleteStatic.bSuccess);
	if (DeleteStatic.bSuccess)
	{
		bStaticExists = false;
	}

	TSharedPtr<FJsonObject> CreateDynamicParams = MakeNamedParams(DynamicName);
	CreateDynamicParams->SetStringField(TEXT("storage_mode"), TEXT("dynamic"));
	const FMonolithActionResult CreateDynamic =
		Invoke(TEXT("create_collection"), CreateDynamicParams);
	if (!TestTrue(TEXT("create_collection creates a local dynamic collection"), CreateDynamic.bSuccess))
	{
		AddError(CreateDynamic.ErrorMessage);
		return false;
	}
	bDynamicExists = true;

	TSharedPtr<FJsonObject> SetQueryParams = MakeNamedParams(DynamicName);
	SetQueryParams->SetStringField(TEXT("query_text"), TEXT("Type=Texture2D"));
	const FMonolithActionResult SetQuery =
		Invoke(TEXT("set_dynamic_query"), SetQueryParams);
	TestTrue(TEXT("set_dynamic_query succeeds"), SetQuery.bSuccess);
	if (TestTrue(TEXT("set_dynamic_query returns the persisted query"), SetQuery.Result.IsValid()))
	{
		TestEqual(
			TEXT("dynamic query round-trips"),
			SetQuery.Result->GetStringField(TEXT("query_text")),
			FString(TEXT("Type=Texture2D")));
	}

	const FMonolithActionResult GetQuery =
		Invoke(TEXT("get_dynamic_query"), MakeNamedParams(DynamicName));
	TestTrue(TEXT("get_dynamic_query succeeds"), GetQuery.bSuccess);
	if (TestTrue(TEXT("get_dynamic_query returns a payload"), GetQuery.Result.IsValid()))
	{
		TestEqual(
			TEXT("get_dynamic_query returns the stored text"),
			GetQuery.Result->GetStringField(TEXT("query_text")),
			FString(TEXT("Type=Texture2D")));
	}

	const FMonolithActionResult DynamicDetails =
		Invoke(TEXT("get_collection"), MakeNamedParams(DynamicName));
	TestTrue(
		TEXT("get_collection resolves dynamic membership"),
		DynamicDetails.bSuccess);
	if (TestTrue(
		TEXT("dynamic collection details return a payload"),
		DynamicDetails.Result.IsValid()))
	{
		TestTrue(
			TEXT("dynamic collection details count matching assets"),
			DynamicDetails.Result->GetIntegerField(TEXT("asset_count")) > 0);
		TestEqual(
			TEXT("dynamic collection details expose the saved query"),
			DynamicDetails.Result->GetStringField(TEXT("query_text")),
			FString(TEXT("Type=Texture2D")));
	}

	const FMonolithActionResult DynamicAssets =
		Invoke(TEXT("list_assets"), MakeNamedParams(DynamicName));
	TestTrue(
		TEXT("list_assets resolves dynamic membership"),
		DynamicAssets.bSuccess);
	if (TestTrue(
		TEXT("dynamic list_assets returns a payload"),
		DynamicAssets.Result.IsValid()))
	{
		TestTrue(
			TEXT("dynamic list_assets reports matching assets"),
			DynamicAssets.Result->GetIntegerField(TEXT("count")) > 0);
		const TArray<TSharedPtr<FJsonValue>>& Assets =
			DynamicAssets.Result->GetArrayField(TEXT("assets"));
		const bool bContainsDefaultTexture = Assets.ContainsByPredicate(
			[&AssetPath](const TSharedPtr<FJsonValue>& Value)
			{
				return Value.IsValid()
					&& Value->Type == EJson::String
					&& Value->AsString() == AssetPath;
			});
		TestTrue(
			TEXT("dynamic query includes the engine default texture"),
			bContainsDefaultTexture);
	}

	TSharedPtr<FJsonObject> DynamicContainsParams =
		MakeNamedParams(DynamicName);
	DynamicContainsParams->SetStringField(TEXT("asset_path"), AssetPath);
	const FMonolithActionResult DynamicContains =
		Invoke(TEXT("contains_asset"), DynamicContainsParams);
	TestTrue(
		TEXT("contains_asset resolves dynamic membership"),
		DynamicContains.bSuccess);
	if (TestTrue(
		TEXT("dynamic contains_asset returns a payload"),
		DynamicContains.Result.IsValid()))
	{
		TestTrue(
			TEXT("dynamic query contains the engine default texture"),
			DynamicContains.Result->GetBoolField(TEXT("contains")));
	}

	SetQueryParams->SetStringField(
		TEXT("query_text"),
		TEXT("Path=/Engine/EngineResources"));
	const FMonolithActionResult SetPackagePathQuery =
		Invoke(TEXT("set_dynamic_query"), SetQueryParams);
	TestTrue(
		TEXT("set_dynamic_query accepts a package-folder Path expression"),
		SetPackagePathQuery.bSuccess);
	const FMonolithActionResult PackagePathContains =
		Invoke(TEXT("contains_asset"), DynamicContainsParams);
	TestTrue(
		TEXT("contains_asset evaluates a package-folder Path expression"),
		PackagePathContains.bSuccess);
	if (TestTrue(
		TEXT("package-folder Path result returns a payload"),
		PackagePathContains.Result.IsValid()))
	{
		TestTrue(
			TEXT("Path compares the package folder without the virtual root or asset leaf"),
			PackagePathContains.Result->GetBoolField(TEXT("contains")));
	}

	SetQueryParams->SetStringField(
		TEXT("query_text"),
		TEXT("Path=/All/Engine/EngineResources/DefaultTexture"));
	const FMonolithActionResult SetVirtualPathQuery =
		Invoke(TEXT("set_dynamic_query"), SetQueryParams);
	TestTrue(
		TEXT("set_dynamic_query accepts a virtual-path-shaped expression"),
		SetVirtualPathQuery.bSuccess);
	const FMonolithActionResult VirtualPathContains =
		Invoke(TEXT("contains_asset"), DynamicContainsParams);
	TestTrue(
		TEXT("contains_asset evaluates a virtual-path-shaped expression"),
		VirtualPathContains.bSuccess);
	if (TestTrue(
		TEXT("virtual-path-shaped result returns a payload"),
		VirtualPathContains.Result.IsValid()))
	{
		TestFalse(
			TEXT("Path does not expose the /All virtual root or asset leaf"),
			VirtualPathContains.Result->GetBoolField(TEXT("contains")));
	}

	SetQueryParams->SetStringField(TEXT("query_text"), TEXT("Type=Texture2D"));
	const FMonolithActionResult RestoreTypeQuery =
		Invoke(TEXT("set_dynamic_query"), SetQueryParams);
	TestTrue(
		TEXT("dynamic type query is restored for delete safety"),
		RestoreTypeQuery.bSuccess);

	const FMonolithActionResult DeleteDynamicWithoutForce =
		Invoke(TEXT("delete_collection"), MakeNamedParams(DynamicName));
	TestFalse(
		TEXT("delete_collection rejects a populated dynamic collection without force"),
		DeleteDynamicWithoutForce.bSuccess);
	TestTrue(
		TEXT("dynamic delete rejection explains the non-empty guard"),
		DeleteDynamicWithoutForce.ErrorMessage.Contains(TEXT("non-empty")));

	TSharedPtr<FJsonObject> ForceDeleteDynamicParams =
		MakeNamedParams(DynamicName);
	ForceDeleteDynamicParams->SetBoolField(TEXT("force"), true);
	const FMonolithActionResult DeleteDynamic =
		Invoke(TEXT("delete_collection"), ForceDeleteDynamicParams);
	TestTrue(
		TEXT("delete_collection removes a populated dynamic collection with force"),
		DeleteDynamic.bSuccess);
	if (DeleteDynamic.bSuccess)
	{
		bDynamicExists = false;
	}

	auto CreateDynamicCollection =
		[this](
			const FString& Name,
			bool& bExists)
		{
			TSharedPtr<FJsonObject> Params =
				MonolithCollectionActionsTestDetail::MakeNamedParams(Name);
			Params->SetStringField(TEXT("storage_mode"), TEXT("dynamic"));
			const FMonolithActionResult Result =
				MonolithCollectionActionsTestDetail::Invoke(
					TEXT("create_collection"), Params);
			if (TestTrue(
				*FString::Printf(
					TEXT("create dynamic collection %s"),
					*Name),
				Result.bSuccess))
			{
				bExists = true;
			}
			return Result.bSuccess;
		};
	if (!CreateDynamicCollection(CycleAName, bCycleAExists)
		|| !CreateDynamicCollection(CycleBName, bCycleBExists))
	{
		return false;
	}

	for (const FString* Name : {&CycleAName, &CycleBName})
	{
		TSharedPtr<FJsonObject> QueryParams = MakeNamedParams(*Name);
		QueryParams->SetStringField(TEXT("query_text"), TEXT("Type=Texture2D"));
		TestTrue(
			*FString::Printf(
				TEXT("set a shared-session query for %s"),
				**Name),
			Invoke(TEXT("set_dynamic_query"), QueryParams).bSuccess);
	}

	TSharedPtr<FJsonObject> ListDynamicParams = MakeShared<FJsonObject>();
	ListDynamicParams->SetStringField(TEXT("share_type"), TEXT("local"));
	const FMonolithActionResult SharedDynamicList =
		Invoke(TEXT("list_collections"), ListDynamicParams);
	TestTrue(
		TEXT("list_collections resolves multiple dynamic collections"),
		SharedDynamicList.bSuccess);
	if (TestTrue(
		TEXT("shared dynamic list returns a payload"),
		SharedDynamicList.Result.IsValid()))
	{
		const TArray<TSharedPtr<FJsonValue>>& Collections =
			SharedDynamicList.Result->GetArrayField(TEXT("collections"));
		for (const FString* Name : {&CycleAName, &CycleBName})
		{
			const TSharedPtr<FJsonValue>* MatchingCollection =
				Collections.FindByPredicate(
					[Name](const TSharedPtr<FJsonValue>& Value)
					{
						const TSharedPtr<FJsonObject> Object =
							Value.IsValid() ? Value->AsObject() : nullptr;
						return Object.IsValid()
							&& Object->GetStringField(TEXT("name")) == *Name;
					});
			if (TestNotNull(
				*FString::Printf(
					TEXT("shared list includes %s"),
					**Name),
				MatchingCollection))
			{
				TestTrue(
					*FString::Printf(
						TEXT("shared list resolves assets for %s"),
						**Name),
					(*MatchingCollection)
						->AsObject()
						->GetIntegerField(TEXT("asset_count")) > 0);
			}
		}
	}

	TSharedPtr<FJsonObject> CycleAQuery = MakeNamedParams(CycleAName);
	CycleAQuery->SetStringField(
		TEXT("query_text"),
		FString::Printf(TEXT("Collection=%s"), *CycleBName));
	TSharedPtr<FJsonObject> CycleBQuery = MakeNamedParams(CycleBName);
	CycleBQuery->SetStringField(
		TEXT("query_text"),
		FString::Printf(TEXT("Collection=%s"), *CycleAName));
	TestTrue(
		TEXT("first nested dynamic query is accepted"),
		Invoke(TEXT("set_dynamic_query"), CycleAQuery).bSuccess);
	TestTrue(
		TEXT("second nested dynamic query is accepted"),
		Invoke(TEXT("set_dynamic_query"), CycleBQuery).bSuccess);

	const FMonolithActionResult CyclicDetails =
		Invoke(TEXT("get_collection"), MakeNamedParams(CycleAName));
	TestFalse(
		TEXT("nested dynamic evaluation failures propagate to get_collection"),
		CyclicDetails.bSuccess);
	TestTrue(
		TEXT("cyclic dynamic evaluation reports the source-data error"),
		CyclicDetails.ErrorMessage.Contains(
			TEXT("Cyclic dynamic collection reference")));

	TSharedPtr<FJsonObject> CycleColorParams = MakeNamedParams(CycleAName);
	TSharedPtr<FJsonObject> CycleColor = MakeShared<FJsonObject>();
	CycleColor->SetNumberField(TEXT("r"), 0.4);
	CycleColor->SetNumberField(TEXT("g"), 0.5);
	CycleColor->SetNumberField(TEXT("b"), 0.6);
	CycleColor->SetNumberField(TEXT("a"), 1.0);
	CycleColorParams->SetObjectField(TEXT("color"), CycleColor);
	const FMonolithActionResult CycleColorResult =
		Invoke(TEXT("set_collection_color"), CycleColorParams);
	TestTrue(
		TEXT("set_collection_color succeeds without dynamic membership resolution"),
		CycleColorResult.bSuccess);
	if (TestTrue(
		TEXT("cycle-safe color mutation returns a payload"),
		CycleColorResult.Result.IsValid()))
	{
		TestTrue(
			TEXT("cycle-safe color mutation reports the applied color"),
			CycleColorResult.Result->HasField(TEXT("color")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
