#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "MonolithBlueprintStructActions.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithBlueprintStructPathValidationTest
{
	using FCreateHandler = FMonolithActionResult (*)(
		const TSharedPtr<FJsonObject>&);

	static bool IsPackageLoaded(const FString& PackageName)
	{
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			if (It->GetName() == PackageName)
			{
				return true;
			}
		}
		return false;
	}

	static void VerifyMalformedPathRejected(
		FAutomationTestBase& Test,
		const FString& Label,
		const FString& PackageName,
		const TSharedPtr<FJsonObject>& Params,
		FCreateHandler Handler)
	{
		Test.TestFalse(
			*FString::Printf(TEXT("%s package does not exist before dispatch"), *Label),
			IsPackageLoaded(PackageName));

		const FMonolithActionResult Result = Handler(Params);
		Test.TestFalse(
			*FString::Printf(TEXT("%s rejects a malformed package path"), *Label),
			Result.bSuccess);
		Test.TestTrue(
			*FString::Printf(TEXT("%s reports the package-path validation error"), *Label),
			Result.ErrorMessage.Contains(TEXT("Invalid package path")));
		Test.TestFalse(
			*FString::Printf(TEXT("%s does not create a package"), *Label),
			IsPackageLoaded(PackageName));
	}

	static TSharedPtr<FJsonObject> MakeStructParams(const FString& SavePath)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("save_path"), SavePath);

		TSharedPtr<FJsonObject> Field = MakeShared<FJsonObject>();
		Field->SetStringField(TEXT("name"), TEXT("Value"));
		Field->SetStringField(TEXT("type"), TEXT("int"));
		Field->SetStringField(TEXT("default_value"), TEXT("0"));
		Params->SetArrayField(
			TEXT("fields"),
			{MakeShared<FJsonValueObject>(Field)});
		return Params;
	}

	static TSharedPtr<FJsonObject> MakeEnumParams(const FString& SavePath)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("save_path"), SavePath);
		Params->SetArrayField(
			TEXT("values"),
			{MakeShared<FJsonValueString>(TEXT("Value"))});
		return Params;
	}

	static TSharedPtr<FJsonObject> MakeDataTableParams(const FString& SavePath)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("save_path"), SavePath);
		Params->SetStringField(TEXT("row_struct"), TEXT("Vector"));
		return Params;
	}

	static TSharedPtr<FJsonObject> MakeDataAssetParams(const FString& SavePath)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("save_path"), SavePath);
		Params->SetStringField(TEXT("class_name"), TEXT("CurveFloat"));
		Params->SetBoolField(TEXT("skip_save"), true);
		return Params;
	}

	static TSharedPtr<FJsonObject> MakeSeedDataAssetParams(
		const FString& SavePath)
	{
		TSharedPtr<FJsonObject> Params = MakeDataAssetParams(SavePath);
		Params->SetObjectField(TEXT("tree"), MakeShared<FJsonObject>());
		return Params;
	}

	static FString MakeUniquePackagePath(const TCHAR* AssetPrefix)
	{
		return FString::Printf(
			TEXT("/Game/Tests/Monolith/Blueprint/%s_%s"),
			AssetPrefix,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}

	static bool CleanupCreatedAsset(const FString& PackageName)
	{
		if (!UEditorAssetLibrary::DoesAssetExist(PackageName))
		{
			return true;
		}

		const bool bDeleted = UEditorAssetLibrary::DeleteAsset(PackageName);
		CollectGarbage(RF_NoFlags);
		return bDeleted && !UEditorAssetLibrary::DoesAssetExist(PackageName);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBlueprintStructMalformedPathTest,
	"Monolith.Blueprint.StructActions.PackagePath.Malformed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintStructMalformedPathTest::RunTest(
	const FString& /*Parameters*/)
{
	using namespace MonolithBlueprintStructPathValidationTest;

	const FString StructPath =
		TEXT("//Game/Tests/Monolith/Blueprint/S_InvalidPath");
	VerifyMalformedPathRejected(
		*this,
		TEXT("create_user_defined_struct"),
		StructPath,
		MakeStructParams(StructPath),
		&FMonolithBlueprintStructActions::HandleCreateUserDefinedStruct);

	const FString EnumPath =
		TEXT("//Game/Tests/Monolith/Blueprint/E_InvalidPath");
	VerifyMalformedPathRejected(
		*this,
		TEXT("create_user_defined_enum"),
		EnumPath,
		MakeEnumParams(EnumPath),
		&FMonolithBlueprintStructActions::HandleCreateUserDefinedEnum);

	const FString DataTablePath =
		TEXT("//Game/Tests/Monolith/Blueprint/DT_InvalidPath");
	VerifyMalformedPathRejected(
		*this,
		TEXT("create_data_table"),
		DataTablePath,
		MakeDataTableParams(DataTablePath),
		&FMonolithBlueprintStructActions::HandleCreateDataTable);

	const FString DataAssetPath =
		TEXT("//Game/Tests/Monolith/Blueprint/DA_InvalidPath");
	VerifyMalformedPathRejected(
		*this,
		TEXT("create_data_asset"),
		DataAssetPath,
		MakeDataAssetParams(DataAssetPath),
		&FMonolithBlueprintStructActions::HandleCreateDataAsset);

	const FString SeedDataAssetPath =
		TEXT("//Game/Tests/Monolith/Blueprint/DA_InvalidSeedPath");
	VerifyMalformedPathRejected(
		*this,
		TEXT("seed_data_asset"),
		SeedDataAssetPath,
		MakeSeedDataAssetParams(SeedDataAssetPath),
		&FMonolithBlueprintStructActions::HandleSeedDataAsset);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBlueprintStructValidPathTest,
	"Monolith.Blueprint.StructActions.PackagePath.Valid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintStructValidPathTest::RunTest(
	const FString& /*Parameters*/)
{
	using namespace MonolithBlueprintStructPathValidationTest;

	// Use the handler's explicit skip_save contract so this fixture never creates
	// a file-system event that can race Asset Registry deletion during teardown.
	const FString PackageName = MakeUniquePackagePath(TEXT("DA_ValidPackagePath"));
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	ON_SCOPE_EXIT
	{
		TestTrue(
			TEXT("the uniquely named test asset is removed through the editor asset API"),
			CleanupCreatedAsset(PackageName));
	};

	const FMonolithActionResult Result =
		FMonolithBlueprintStructActions::HandleCreateDataAsset(
			MakeDataAssetParams(PackageName));

	TestTrue(TEXT("a valid long package name still creates an unsaved test asset"), Result.bSuccess);
	UPackage* Package = FindPackage(nullptr, *PackageName);
	TestNotNull(TEXT("the valid package exists"), Package);
	if (Package)
	{
		TestNotNull(
			TEXT("the valid package contains the requested asset"),
			FindObject<UObject>(Package, *AssetName));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
