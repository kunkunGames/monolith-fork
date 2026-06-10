#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "MonolithToolRegistry.h"
#include "MonolithComboGraphActions.h"
#include "Dom/JsonObject.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_COMBOGRAPH

namespace
{
	const FString CrashguardPackageName = TEXT("/Game/Tests/Monolith/ComboGraph/CG_CrashguardTest");
	const FString CrashguardAssetName = TEXT("CG_CrashguardTest");

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

// Regression test to ensure combo graph asset creations correctly prevent FullyLoad on an in-memory package
// and do not crash or corrupt the asset if an existing in-memory package is reused.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithComboGraphFullyLoadCrashguardTest, "Monolith.Crashguard.ComboGraph.ActionsNoFullyLoad", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithComboGraphFullyLoadCrashguardTest::RunTest(const FString& Parameters)
{
	CleanupCrashguardTestAsset();

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("combograph"), TEXT("create_combo_graph")))
	{
		FMonolithComboGraphActions::RegisterActions(Registry);
	}

	// Setup payload for create_combo_graph
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("save_path"), CrashguardPackageName);

	// Creating package BEFORE running the action simulates the 'in-memory hit path'
	// This exposes the flaw where FullyLoad() would read transient disk state.
	UPackage* ExistingPackage = CreatePackage(*CrashguardPackageName);
	TestNotNull(TEXT("Pre-created package should exist"), ExistingPackage);

	// Call the action
	FMonolithActionResult Result = Registry.ExecuteAction(TEXT("combograph"), TEXT("create_combo_graph"), Payload);

	// In the absence of FullyLoad, this should succeed. If it fails, or if it triggers an assert during FullyLoad (when run locally), the test will fail.
	TestTrue(TEXT("create_combo_graph should handle pre-existing in-memory package safely"), Result.bSuccess);

	CleanupCrashguardTestAsset();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_COMBOGRAPH
