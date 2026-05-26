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
}

// ------------------------------------------------------------------------------------------------
// Monolith.Crashguard.MonolithLogicDriver.PackagePathValidation
// Validates that malformed logicdriver asset paths correctly return JSON errors
// rather than proceeding to CreatePackage and causing an editor crash/assert.
// ------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLogicDriverCrashguardPackagePathValidation, "Monolith.Crashguard.MonolithLogicDriver.PackagePathValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithLogicDriverCrashguardPackagePathValidation::RunTest(const FString& Parameters)
{
	struct FMalformedPathCase
	{
		const TCHAR* Path;
		const TCHAR* ExpectedError;
		const TCHAR* AltExpectedError; // for create_state_machine's AssetName calculation
	};

	const FMalformedPathCase MalformedPaths[] = {
		{ TEXT(""), TEXT("Missing required param 'save_path'"), TEXT("Missing required param 'save_path'") },
		{ TEXT("//Game/Malformed_Path"), TEXT("Invalid package path"), TEXT("Invalid package path") },
		{ TEXT("Game/Malformed_Path"), TEXT("Invalid package path"), TEXT("Invalid package path") },
		{ TEXT("/Game/Malformed_Path/"), TEXT("Invalid package path"), TEXT("Could not determine asset name from save_path") },
		{ TEXT("/Game/Malformed_Path#1"), TEXT("Invalid package path"), TEXT("Invalid package path") },
	};

	for (const FMalformedPathCase& Case : MalformedPaths)
	{
		// 1. Setup invalid payload
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("save_path"), Case.Path);

		// 2. Test create_state_machine
		{
			FMonolithActionResult Result = ExecuteLogicDriverAction(TEXT("create_state_machine"), Payload);

			TestFalse(*FString::Printf(TEXT("create_state_machine with malformed path '%s' should return Error"), Case.Path), Result.bSuccess);
			TestTrue(
				*FString::Printf(TEXT("create_state_machine malformed path '%s' should report '%s' or '%s'"), Case.Path, Case.ExpectedError, Case.AltExpectedError),
				Result.ErrorMessage.Contains(Case.ExpectedError) || Result.ErrorMessage.Contains(Case.AltExpectedError));
		}

		// 3. Test scaffold_hello_world_sm
		{
			FMonolithActionResult Result = ExecuteLogicDriverAction(TEXT("scaffold_hello_world_sm"), Payload);

			TestFalse(*FString::Printf(TEXT("scaffold_hello_world_sm with malformed path '%s' should return Error"), Case.Path), Result.bSuccess);
			TestTrue(
				*FString::Printf(TEXT("scaffold_hello_world_sm malformed path '%s' should report '%s'"), Case.Path, Case.ExpectedError),
				Result.ErrorMessage.Contains(Case.ExpectedError));
		}

		// 4. Test build_sm_from_spec
		{
			TSharedPtr<FJsonObject> SpecPayload = MakeShared<FJsonObject>();
			SpecPayload->SetStringField(TEXT("save_path"), Case.Path);
			TSharedPtr<FJsonObject> EmptySpec = MakeShared<FJsonObject>();
			SpecPayload->SetObjectField(TEXT("spec"), EmptySpec);

			FMonolithActionResult Result = ExecuteLogicDriverAction(TEXT("build_sm_from_spec"), SpecPayload);

			TestFalse(*FString::Printf(TEXT("build_sm_from_spec with malformed path '%s' should return Error"), Case.Path), Result.bSuccess);
			TestTrue(
				*FString::Printf(TEXT("build_sm_from_spec malformed path '%s' should report '%s'"), Case.Path, Case.ExpectedError),
				Result.ErrorMessage.Contains(Case.ExpectedError));
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_LOGICDRIVER
