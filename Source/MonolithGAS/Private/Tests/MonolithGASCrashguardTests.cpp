#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "MonolithGASCueActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	const FString CrashguardPackageName = TEXT("/Game/Tests/Monolith/GAS/GC_CrashguardTestCue");
	const FString CrashguardAssetName = TEXT("GC_CrashguardTestCue");

	void CleanupCrashguardTestAsset()
	{
		if (UPackage* Package = FindPackage(nullptr, *CrashguardPackageName))
		{
			if (UObject* Asset = FindObject<UObject>(Package, *CrashguardAssetName))
			{
				Asset->ClearFlags(RF_Standalone | RF_Public);
				Asset->MarkAsGarbage();
			}
		}

		CollectGarbage(RF_NoFlags);

		const FString PackageFilename = FPackageName::LongPackageNameToFilename(
			CrashguardPackageName,
			FPackageName::GetAssetPackageExtension());
		IFileManager::Get().Delete(*PackageFilename, false, true, true);
	}
}

// Regression test to ensure GAS asset creations correctly prevent FullyLoad on an in-memory package
// and do not crash or corrupt the asset if an existing in-memory package is reused.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithGASFullyLoadCrashguardTest, "Monolith.Crashguard.GAS.CueActionsNoFullyLoad", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGASFullyLoadCrashguardTest::RunTest(const FString& Parameters)
{
	CleanupCrashguardTestAsset();

	if (!FMonolithToolRegistry::Get().HasAction(TEXT("gas"), TEXT("create_gameplay_cue_notify")))
	{
		FMonolithGASCueActions::RegisterActions(FMonolithToolRegistry::Get());
	}

	// Setup payload for create_gameplay_cue_notify
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("save_path"), CrashguardPackageName);
	Payload->SetStringField(TEXT("cue_tag"), TEXT("GameplayCue.Crashguard.Test.Cue"));

	// Creating package BEFORE running the action simulates the 'in-memory hit path'
	// This exposes the flaw where FullyLoad() would read transient disk state.
	UPackage* ExistingPackage = CreatePackage(*CrashguardPackageName);
	TestNotNull(TEXT("Pre-created package should exist"), ExistingPackage);

	// Call the action
	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("gas"), TEXT("create_gameplay_cue_notify"), Payload);

	// EnsureAssetPathFree/GetOrCreatePackage should now reject the existing in-memory package to prevent overwriting
	TestFalse(TEXT("create_gameplay_cue_notify should reject pre-existing in-memory package"), Result.bSuccess);
	TestTrue(TEXT("Error message should mention existing package"), Result.ErrorMessage.Contains(TEXT("already exists in memory")));

	CleanupCrashguardTestAsset();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
