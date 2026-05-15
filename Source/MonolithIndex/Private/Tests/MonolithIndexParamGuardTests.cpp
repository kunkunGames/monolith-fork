#include "Misc/AutomationTest.h"
#include "Actions/ProjectGetAssetDetailsAction.h"
#include "Actions/ProjectFindReferencesAction.h"
#include "Actions/ProjectFindByTypeAction.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectIndexParamGuardTest, "Monolith.ParamGuard.ProjectIndex.MalformedInput", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProjectIndexParamGuardTest::RunTest(const FString& Parameters)
{
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

	return true;
}
