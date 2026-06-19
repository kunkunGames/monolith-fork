#include "Misc/AutomationTest.h"
#include "Actions/ProjectGetAssetDetailsAction.h"
#include "Actions/ProjectGetSavedAssetStateAction.h"
#include "Actions/ProjectFindReferencesAction.h"
#include "Actions/ProjectFindByTypeAction.h"
#include "Actions/ProjectFindUnusedAction.h"
#include "Actions/ProjectExportAssetTextAction.h"
#include "Dom/JsonObject.h"

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

	return true;
}
