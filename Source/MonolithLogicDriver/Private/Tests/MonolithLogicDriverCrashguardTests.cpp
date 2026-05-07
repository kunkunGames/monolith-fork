#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "MonolithLogicDriverAssetActions.h"
#include "MonolithLogicDriverScaffoldActions.h"
#include "MonolithLogicDriverSpecActions.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_LOGICDRIVER

namespace
{
	FMonolithActionResult ExecuteLogicDriverAction(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("logicdriver"), Action))
		{
			FMonolithLogicDriverAssetActions::RegisterActions(Registry);
			FMonolithLogicDriverScaffoldActions::RegisterActions(Registry);
			FMonolithLogicDriverSpecActions::RegisterActions(Registry);
		}

		return Registry.ExecuteAction(TEXT("logicdriver"), Action, Params);
	}

	bool MentionsInvalidPackagePath(const FMonolithActionResult& Result)
	{
		return Result.ErrorMessage.Contains(TEXT("Invalid package path")) ||
			Result.ErrorMessage.Contains(TEXT("//Game/Malformed_Path"));
	}
}

// ------------------------------------------------------------------------------------------------
// Monolith.Crashguard.MonolithLogicDriver.PackagePathValidation
// Validates that malformed logicdriver asset paths correctly return JSON errors
// rather than proceeding to CreatePackage and causing an editor crash/assert.
// ------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLogicDriverCrashguardPackagePathValidation, "Monolith.Crashguard.MonolithLogicDriver.PackagePathValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithLogicDriverCrashguardPackagePathValidation::RunTest(const FString& Parameters)
{
	// 1. Setup invalid payload
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("save_path"), TEXT("//Game/Malformed_Path")); // Double slash is fatal to CreatePackage

	// 2. Test create_state_machine
	{
		FMonolithActionResult Result = ExecuteLogicDriverAction(TEXT("create_state_machine"), Payload);

		TestTrue(TEXT("create_state_machine should fail on malformed path"), !Result.bSuccess);
		TestTrue(TEXT("Error should mention validation failure"), MentionsInvalidPackagePath(Result));
	}

	// 3. Test scaffold_hello_world_sm
	{
		FMonolithActionResult Result = ExecuteLogicDriverAction(TEXT("scaffold_hello_world_sm"), Payload);

		TestTrue(TEXT("scaffold_hello_world_sm should fail on malformed path"), !Result.bSuccess);
		TestTrue(TEXT("Error should mention validation failure"), MentionsInvalidPackagePath(Result));
	}

	// 4. Test build_sm_from_spec
	{
		TSharedPtr<FJsonObject> SpecPayload = MakeShared<FJsonObject>();
		SpecPayload->SetStringField(TEXT("save_path"), TEXT("//Game/Malformed_Path"));
		TSharedPtr<FJsonObject> EmptySpec = MakeShared<FJsonObject>();
		SpecPayload->SetObjectField(TEXT("spec"), EmptySpec);

		FMonolithActionResult Result = ExecuteLogicDriverAction(TEXT("build_sm_from_spec"), SpecPayload);

		TestTrue(TEXT("build_sm_from_spec should fail on malformed path"), !Result.bSuccess);
		TestTrue(TEXT("Error should mention validation failure"), MentionsInvalidPackagePath(Result));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_LOGICDRIVER
