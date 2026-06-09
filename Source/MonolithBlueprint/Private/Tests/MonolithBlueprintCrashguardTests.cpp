#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "MonolithBlueprintStructActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	const FString CrashguardPackageName = TEXT("/Game/Tests/Monolith/Blueprint/S_CrashguardTestStruct");
	const FString CrashguardAssetName = TEXT("S_CrashguardTestStruct");

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

// Regression test to ensure structural asset creations correctly prevent FullyLoad on an in-memory package
// and do not crash or corrupt the asset if an existing in-memory package is reused.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintFullyLoadCrashguardTest, "Monolith.Crashguard.Blueprint.StructActionsNoFullyLoad", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintFullyLoadCrashguardTest::RunTest(const FString& Parameters)
{
	CleanupCrashguardTestAsset();
	FMonolithBlueprintStructActions::RegisterActions(FMonolithToolRegistry::Get());

	// Setup payload for create_user_defined_struct
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("save_path"), CrashguardPackageName);

	TArray<TSharedPtr<FJsonValue>> Fields;
	TSharedPtr<FJsonObject> Field = MakeShared<FJsonObject>();
	Field->SetStringField(TEXT("name"), TEXT("DummyField"));
	Field->SetStringField(TEXT("type"), TEXT("int"));
	Fields.Add(MakeShared<FJsonValueObject>(Field));
	Payload->SetArrayField(TEXT("fields"), Fields);

	// Creating package BEFORE running the action simulates the 'in-memory hit path'
	// This exposes the flaw where FullyLoad() would read transient disk state.
	UPackage* ExistingPackage = CreatePackage(*CrashguardPackageName);
	TestNotNull(TEXT("Pre-created package should exist"), ExistingPackage);

	// Call the action
	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("blueprint"), TEXT("create_user_defined_struct"), Payload);

	// In the absence of FullyLoad, this should succeed. If it fails, or if it triggers an assert during FullyLoad (when run locally), the test will fail.
	TestTrue(TEXT("create_user_defined_struct should handle pre-existing in-memory package safely"), Result.bSuccess);

	CleanupCrashguardTestAsset();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
