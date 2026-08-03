#include "Misc/AutomationTest.h"
#include "Actions/ProjectGetAssetDetailsAction.h"
#include "Actions/ProjectGetSavedAssetStateAction.h"
#include "Actions/ProjectFindReferencesAction.h"
#include "Actions/ProjectFindByTypeAction.h"
#include "Actions/ProjectFindUnusedAction.h"
#include "Actions/ProjectExportAssetTextAction.h"
#include "Actions/ProjectSearchAction.h"
#include "Actions/ProjectSearchGameplayTagsAction.h"
#include "Actions/ProjectReviewHotspotsAction.h"
#include "Actions/ProjectRiskScoreAction.h"
#include "Dom/JsonObject.h"
#include "MonolithParamSchema.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectIndexParamGuardTest, "Monolith.ParamGuard.ProjectIndex.MalformedInput", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProjectIndexParamGuardTest::RunTest(const FString& Parameters)
{
	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("limit"), TEXT("not_a_number"));
		FMonolithActionResult Result = FProjectFindUnusedAction::Execute(Params);
		TestFalse(TEXT("FindUnused: Reject wrong type for limit"), Result.bSuccess);
		TestEqual(TEXT("FindUnused: Error code for limit"), Result.ErrorCode, -32602);
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("asset_path"), 12345);
		FMonolithActionResult Result = FProjectGetAssetDetailsAction::Execute(Params);
		TestFalse(TEXT("GetAssetDetails: Reject wrong type for asset_path"), Result.bSuccess);
		TestEqual(TEXT("GetAssetDetails: Error code for asset_path"), Result.ErrorCode, -32602);
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("package_path"), 12345);
		FMonolithActionResult Result = FProjectGetAssetDetailsAction::Execute(Params);
		TestFalse(TEXT("GetAssetDetails: Reject wrong type for package_path"), Result.bSuccess);
		TestEqual(TEXT("GetAssetDetails: Error code for package_path"), Result.ErrorCode, -32602);
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("asset_path"), 12345);
		FMonolithActionResult Result = FProjectFindReferencesAction::Execute(Params);
		TestFalse(TEXT("FindReferences: Reject wrong type for asset_path"), Result.bSuccess);
		TestEqual(TEXT("FindReferences: Error code for asset_path"), Result.ErrorCode, -32602);
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("package_path"), 12345);
		FMonolithActionResult Result = FProjectFindReferencesAction::Execute(Params);
		TestFalse(TEXT("FindReferences: Reject wrong type for package_path"), Result.bSuccess);
		TestEqual(TEXT("FindReferences: Error code for package_path"), Result.ErrorCode, -32602);
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("asset_type"), 12345);
		FMonolithActionResult Result = FProjectFindByTypeAction::Execute(Params);
		TestFalse(TEXT("FindByType: Reject wrong type for asset_type"), Result.bSuccess);
		TestEqual(TEXT("FindByType: Error code for asset_type"), Result.ErrorCode, -32602);
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("asset_class"), 12345);
		FMonolithActionResult Result = FProjectFindByTypeAction::Execute(Params);
		TestFalse(TEXT("FindByType: Reject wrong type for asset_class"), Result.bSuccess);
		TestEqual(TEXT("FindByType: Error code for asset_class"), Result.ErrorCode, -32602);
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("asset_path"), 12345);
		FMonolithActionResult Result = FProjectGetSavedAssetStateAction::Execute(Params);
		TestFalse(TEXT("GetSavedAssetState: Reject wrong type for asset_path"), Result.bSuccess);
		TestEqual(TEXT("GetSavedAssetState: Error code for asset_path"), Result.ErrorCode, -32602);
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("package_path"), 12345);
		FMonolithActionResult Result = FProjectGetSavedAssetStateAction::Execute(Params);
		TestFalse(TEXT("GetSavedAssetState: Reject wrong type for package_path"), Result.bSuccess);
		TestEqual(TEXT("GetSavedAssetState: Error code for package_path"), Result.ErrorCode, -32602);
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT(""));
		Params->SetStringField(TEXT("package_path"), TEXT("/Game/Foo"));
		FMonolithActionResult Result = FProjectGetAssetDetailsAction::Execute(Params);
		// It might fail to find the asset, but it shouldn't fail with -32602 (invalid params)
		TestNotEqual(TEXT("GetAssetDetails: Fallback to package_path when asset_path is empty"), Result.ErrorCode, -32602);
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("asset_path"), 12345);
		FMonolithActionResult Result = FProjectExportAssetTextAction::Execute(Params);
		TestFalse(TEXT("ExportAssetText: Reject wrong type for asset_path"), Result.bSuccess);
		TestEqual(TEXT("ExportAssetText: Error code for asset_path"), Result.ErrorCode, -32602);
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Foo"));
		Params->SetNumberField(TEXT("object_filter"), 12345);
		FMonolithActionResult Result = FProjectExportAssetTextAction::Execute(Params);
		TestFalse(TEXT("ExportAssetText: Reject wrong type for object_filter"), Result.bSuccess);
		TestEqual(TEXT("ExportAssetText: Error code for object_filter"), Result.ErrorCode, -32602);
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Foo"));
		Params->SetNumberField(TEXT("grep_pattern"), 12345);
		FMonolithActionResult Result = FProjectExportAssetTextAction::Execute(Params);
		TestFalse(TEXT("ExportAssetText: Reject wrong type for grep_pattern"), Result.bSuccess);
		TestEqual(TEXT("ExportAssetText: Error code for grep_pattern"), Result.ErrorCode, -32602);
	}


	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("query"), TEXT("test"));
		Params->SetNumberField(TEXT("asset_class"), 12345);
		FMonolithActionResult Result = FProjectSearchAction::Execute(Params);
		TestFalse(TEXT("Search: Reject wrong type for asset_class"), Result.bSuccess);
		TestEqual(TEXT("Search: Error code for asset_class"), Result.ErrorCode, -32602);
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("query"), TEXT("test"));
		Params->SetNumberField(TEXT("path_filter"), 12345);
		FMonolithActionResult Result = FProjectSearchAction::Execute(Params);
		TestFalse(TEXT("Search: Reject wrong type for path_filter"), Result.bSuccess);
		TestEqual(TEXT("Search: Error code for path_filter"), Result.ErrorCode, -32602);
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("limit"), TEXT("not_a_number"));
		FMonolithActionResult Result = FProjectReviewHotspotsAction::Execute(Params);
		TestFalse(TEXT("ReviewHotspots: Reject wrong type for limit"), Result.bSuccess);
		TestEqual(TEXT("ReviewHotspots: Error code for limit"), Result.ErrorCode, -32602);
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("min_lines"), TEXT("not_a_number"));
		FMonolithActionResult Result = FProjectReviewHotspotsAction::Execute(Params);
		TestFalse(TEXT("ReviewHotspots: Reject wrong type for min_lines"), Result.bSuccess);
		TestEqual(TEXT("ReviewHotspots: Error code for min_lines"), Result.ErrorCode, -32602);
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 0);
		FMonolithActionResult Result = FProjectRiskScoreAction::Execute(Params);
		TestTrue(TEXT("RiskScore: limit 0 preserves the default sentinel"), Result.bSuccess);
		if (Result.bSuccess && Result.Result.IsValid())
		{
			const TSharedPtr<FJsonObject>* Limits = nullptr;
			TestTrue(TEXT("RiskScore: limits object present"),
				Result.Result->TryGetObjectField(TEXT("limits"), Limits) && Limits);
			if (Limits)
			{
				int32 EffectiveLimit = -1;
				(*Limits)->TryGetNumberField(TEXT("limit"), EffectiveLimit);
				TestEqual(TEXT("RiskScore: limit 0 uses the documented default"), EffectiveLimit, 20);
			}
		}
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 0);
		Params->SetNumberField(TEXT("min_lines"), 0);
		FMonolithActionResult Result = FProjectReviewHotspotsAction::Execute(Params);
		TestTrue(TEXT("ReviewHotspots: non-positive values preserve default sentinels"), Result.bSuccess);
		if (Result.bSuccess && Result.Result.IsValid())
		{
			const TSharedPtr<FJsonObject>* Limits = nullptr;
			TestTrue(TEXT("ReviewHotspots: limits object present"),
				Result.Result->TryGetObjectField(TEXT("limits"), Limits) && Limits);
			if (Limits)
			{
				int32 EffectiveLimit = -1;
				int32 EffectiveMinLines = -1;
				(*Limits)->TryGetNumberField(TEXT("limit"), EffectiveLimit);
				(*Limits)->TryGetNumberField(TEXT("min_lines"), EffectiveMinLines);
				TestEqual(TEXT("ReviewHotspots: limit 0 uses the documented default"), EffectiveLimit, 50);
				TestEqual(TEXT("ReviewHotspots: min_lines 0 uses the documented default"), EffectiveMinLines, 100);
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectIndexSearchQueryAliasTest,
	"Monolith.Registry.ProjectIndex.SearchQueryAlias",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProjectIndexSearchQueryAliasTest::RunTest(const FString& Parameters)
{
	auto TestSchema = [this](const TCHAR* Label, const TSharedPtr<FJsonObject>& Schema)
	{
		TestTrue(FString::Printf(TEXT("%s schema exists"), Label), Schema.IsValid());
		if (!Schema.IsValid())
		{
			return;
		}

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("q"), TEXT("Health"));

		FString Collision;
		const bool bResult = FMonolithParamSchema::ApplyAliases(Schema, Params, Collision);

		TestTrue(FString::Printf(TEXT("%s q alias applies"), Label), bResult);
		TestTrue(FString::Printf(TEXT("%s query created from q"), Label), Params->HasField(TEXT("query")));
		TestEqual(FString::Printf(TEXT("%s query value matches q"), Label),
			Params->GetStringField(TEXT("query")),
			FString(TEXT("Health")));
	};

	TestSchema(TEXT("project.search"), FProjectSearchAction::GetSchema());
	TestSchema(TEXT("project.search_gameplay_tags"), FProjectSearchGameplayTagsAction::GetSchema());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectIndexSchemaAliasTest, "Monolith.Registry.Index.SchemaRequiresPackagePathAlias", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProjectIndexSchemaAliasTest::RunTest(const FString& Parameters)
{
	auto ValidateAlias = [this](const FString& ActionName, TSharedPtr<FJsonObject> Schema)
	{
		if (!Schema.IsValid())
		{
			AddError(FString::Printf(TEXT("Action schema %s is invalid"), *ActionName));
			return;
		}

		const TSharedPtr<FJsonObject>* AssetPathParam = nullptr;
		if (!Schema->TryGetObjectField(TEXT("asset_path"), AssetPathParam)
			|| !AssetPathParam
			|| !AssetPathParam->IsValid())
		{
			AddError(FString::Printf(TEXT("Action schema %s has no asset_path param"), *ActionName));
			return;
		}

		bool bHasPackagePathAlias = false;
		const TArray<TSharedPtr<FJsonValue>>* AliasesArray = nullptr;
		if ((*AssetPathParam)->TryGetArrayField(TEXT("aliases"), AliasesArray) && AliasesArray)
		{
			for (const TSharedPtr<FJsonValue>& AliasVal : *AliasesArray)
			{
				if (AliasVal.IsValid() && AliasVal->AsString() == TEXT("package_path"))
				{
					bHasPackagePathAlias = true;
					break;
				}
			}
		}

		TestTrue(FString::Printf(TEXT("%s param exists"), *ActionName), AssetPathParam->IsValid());
		TestTrue(FString::Printf(TEXT("%s has package_path alias"), *ActionName), bHasPackagePathAlias);
	};

	ValidateAlias(TEXT("get_asset_details"), FProjectGetAssetDetailsAction::GetSchema());
	ValidateAlias(TEXT("find_references"), FProjectFindReferencesAction::GetSchema());
	ValidateAlias(TEXT("get_saved_asset_state"), FProjectGetSavedAssetStateAction::GetSchema());

	return true;
}
