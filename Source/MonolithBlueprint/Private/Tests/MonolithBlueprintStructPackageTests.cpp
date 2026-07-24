#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "MonolithBlueprintStructActions.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithBlueprintStructPackageTest
{
	static const FString PackageName =
		TEXT("/Game/Tests/Monolith/Blueprint/S_InMemoryPackage");
	static const FString AssetName = TEXT("S_InMemoryPackage");

	static void Cleanup()
	{
		if (UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			if (UObject* Asset = FindObject<UObject>(Package, *AssetName))
			{
				Asset->ClearFlags(RF_Public | RF_Standalone);
				Asset->MarkAsGarbage();
			}
		}

		CollectGarbage(RF_NoFlags);

		const FString Filename = FPackageName::LongPackageNameToFilename(
			PackageName,
			FPackageName::GetAssetPackageExtension());
		IFileManager::Get().Delete(*Filename, false, true, true);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBlueprintStructInMemoryPackageTest,
	"Monolith.Blueprint.StructActions.InMemoryPackage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintStructInMemoryPackageTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithBlueprintStructPackageTest;

	Cleanup();
	ON_SCOPE_EXIT
	{
		Cleanup();
	};

	UPackage* ExistingPackage = CreatePackage(*PackageName);
	TestNotNull(TEXT("test package is pre-created in memory"), ExistingPackage);
	if (!ExistingPackage)
	{
		return false;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("save_path"), PackageName);

	TSharedPtr<FJsonObject> Field = MakeShared<FJsonObject>();
	Field->SetStringField(TEXT("name"), TEXT("Value"));
	Field->SetStringField(TEXT("type"), TEXT("int"));
	Field->SetStringField(TEXT("default_value"), TEXT("0"));
	Params->SetArrayField(TEXT("fields"), {MakeShared<FJsonValueObject>(Field)});

	const FMonolithActionResult Result =
		FMonolithBlueprintStructActions::HandleCreateUserDefinedStruct(Params);

	TestTrue(
		TEXT("struct creation succeeds when CreatePackage reuses an in-memory package"),
		Result.bSuccess);
	TestTrue(
		TEXT("the action keeps using the pre-created package"),
		FindPackage(nullptr, *PackageName) == ExistingPackage);
	TestNotNull(
		TEXT("the struct is created in the reused package"),
		FindObject<UObject>(ExistingPackage, *AssetName));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
