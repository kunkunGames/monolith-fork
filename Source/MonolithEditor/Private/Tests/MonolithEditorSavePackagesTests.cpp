// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "MonolithEditorActions.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithEditorSavePackagesExternalPrivateReferenceGuardTest,
	"Monolith.Crashguard.MonolithEditor.SavePackagesExternalPrivateReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorSavePackagesExternalPrivateReferenceGuardTest::RunTest(const FString& /*Parameters*/)
{
	const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString TargetPackageName =
		FString::Printf(TEXT("/Game/Tests/Monolith/SaveGuard/Target_%s"), *UniqueSuffix);
	const FString ExternalPackageName =
		FString::Printf(TEXT("/Game/Tests/Monolith/SaveGuard/External_%s"), *UniqueSuffix);

	UPackage* TargetPackage = CreatePackage(*TargetPackageName);
	UPackage* ExternalPackage = CreatePackage(*ExternalPackageName);
	if (!TestNotNull(TEXT("Create target package"), TargetPackage) ||
		!TestNotNull(TEXT("Create external package"), ExternalPackage))
	{
		return false;
	}

	const FString TargetAssetName = FPackageName::GetLongPackageAssetName(TargetPackageName);
	UObjectRedirector* Referencer = NewObject<UObjectRedirector>(
		TargetPackage,
		*TargetAssetName,
		RF_Public | RF_Standalone);
	UObject* PrivateExternalObject = NewObject<UObjectRedirector>(
		ExternalPackage,
		TEXT("PrivateExternalObject"),
		RF_NoFlags);
	UObject* PublicExternalObject = NewObject<UObjectRedirector>(
		ExternalPackage,
		TEXT("PublicExternalObject"),
		RF_Public);
	const FString TargetFilename = FPackageName::LongPackageNameToFilename(
		TargetPackageName,
		FPackageName::GetAssetPackageExtension());
	ON_SCOPE_EXIT
	{
		if (Referencer)
		{
			Referencer->DestinationObject = nullptr;
			Referencer->ClearFlags(RF_Public | RF_Standalone);
			Referencer->MarkAsGarbage();
		}
		if (PrivateExternalObject)
		{
			PrivateExternalObject->MarkAsGarbage();
		}
		if (PublicExternalObject)
		{
			PublicExternalObject->MarkAsGarbage();
		}
		if (TargetPackage)
		{
			TargetPackage->SetDirtyFlag(false);
			TargetPackage->MarkAsGarbage();
		}
		if (ExternalPackage)
		{
			ExternalPackage->SetDirtyFlag(false);
			ExternalPackage->MarkAsGarbage();
		}
	};
	if (!TestNotNull(TEXT("Create referencer"), Referencer) ||
		!TestNotNull(TEXT("Create private external object"), PrivateExternalObject) ||
		!TestNotNull(TEXT("Create public external object"), PublicExternalObject))
	{
		return false;
	}

	TargetPackage->SetDirtyFlag(true);
	ExternalPackage->SetDirtyFlag(true);

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Packages;
	Packages.Add(MakeShared<FJsonValueString>(TargetPackageName));
	Payload->SetArrayField(TEXT("packages"), Packages);

	TestFalse(TEXT("Unique target package must not exist before the save attempt"),
		IFileManager::Get().FileExists(*TargetFilename));

	// UObjectRedirector::DestinationObject is a native custom-serialized field,
	// not a reflected UPROPERTY. A public destination is still a valid import;
	// exercise the same preflight in dry-run mode so the fixture is never written.
	Referencer->DestinationObject = PublicExternalObject;
	Payload->SetBoolField(TEXT("dry_run"), true);
	const FMonolithActionResult PublicReferenceResult = FMonolithEditorActions::HandleSavePackages(Payload);
	TestTrue(TEXT("Public external reference returns a structured dry-run payload"),
		PublicReferenceResult.bSuccess && PublicReferenceResult.Result.IsValid());
	if (PublicReferenceResult.Result.IsValid())
	{
		double PublicFailedValidation = -1.0;
		TestTrue(TEXT("Public external reference exposes failed_validation"),
			PublicReferenceResult.Result->TryGetNumberField(TEXT("failed_validation"), PublicFailedValidation));
		TestEqual(TEXT("Public external reference passes pre-save validation"),
			PublicFailedValidation, 0.0);

		const TArray<TSharedPtr<FJsonValue>>* PublicRows = nullptr;
		if (TestTrue(TEXT("Public external reference exposes one result row"),
			PublicReferenceResult.Result->TryGetArrayField(TEXT("results"), PublicRows) &&
			PublicRows && PublicRows->Num() == 1))
		{
			const TSharedPtr<FJsonObject> PublicRow = (*PublicRows)[0]->AsObject();
			bool bPublicValidationPassed = false;
			bool bWouldSave = false;
			TestTrue(TEXT("Public row exposes validation_passed=true"),
				PublicRow.IsValid() &&
				PublicRow->TryGetBoolField(TEXT("validation_passed"), bPublicValidationPassed));
			TestTrue(TEXT("Public external reference is allowed"), bPublicValidationPassed);
			TestTrue(TEXT("Dirty public-reference package would save"),
				PublicRow.IsValid() && PublicRow->TryGetBoolField(TEXT("would_save"), bWouldSave));
			TestTrue(TEXT("Dry-run reports the dirty package as saveable"), bWouldSave);
		}
	}
	TestFalse(TEXT("Public-reference dry-run writes no package file"),
		IFileManager::Get().FileExists(*TargetFilename));

	// The private destination must be found by the Serialize-based preflight
	// before UPackage::SavePackage enters its own import harvester.
	Referencer->DestinationObject = PrivateExternalObject;
	Payload->SetBoolField(TEXT("dry_run"), false);
	const FMonolithActionResult Result = FMonolithEditorActions::HandleSavePackages(Payload);
	TestTrue(TEXT("Per-package validation failure returns a structured action payload"), Result.bSuccess);
	if (!TestTrue(TEXT("Result payload is valid"), Result.Result.IsValid()))
	{
		return false;
	}

	bool bOk = true;
	TestTrue(TEXT("Result exposes ok"), Result.Result->TryGetBoolField(TEXT("ok"), bOk));
	TestFalse(TEXT("Invalid private reference makes the batch non-ok"), bOk);

	double FailedValidation = 0.0;
	TestTrue(TEXT("Result exposes failed_validation"),
		Result.Result->TryGetNumberField(TEXT("failed_validation"), FailedValidation));
	TestEqual(TEXT("Exactly one package fails pre-save validation"), FailedValidation, 1.0);

	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (!TestTrue(TEXT("Result exposes one per-package row"),
		Result.Result->TryGetArrayField(TEXT("results"), Rows) && Rows && Rows->Num() == 1))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Row = (*Rows)[0]->AsObject();
	if (!TestTrue(TEXT("Per-package row is an object"), Row.IsValid()))
	{
		return false;
	}

	bool bSaved = true;
	bool bValidationPassed = true;
	FString ErrorCode;
	double InvalidReferenceCount = 0.0;
	TestTrue(TEXT("Row exposes saved=false"), Row->TryGetBoolField(TEXT("saved"), bSaved));
	TestFalse(TEXT("Guard skips UPackage::SavePackage"), bSaved);
	TestTrue(TEXT("Row exposes validation_passed=false"),
		Row->TryGetBoolField(TEXT("validation_passed"), bValidationPassed));
	TestFalse(TEXT("External private reference fails validation"), bValidationPassed);
	TestTrue(TEXT("Row exposes a stable error code"),
		Row->TryGetStringField(TEXT("error_code"), ErrorCode));
	TestEqual(TEXT("Error code identifies the rejected reference shape"),
		ErrorCode,
		FString(TEXT("external_private_object_reference")));
	TestTrue(TEXT("Row exposes invalid_reference_count"),
		Row->TryGetNumberField(TEXT("invalid_reference_count"), InvalidReferenceCount));
	TestEqual(TEXT("Exactly one invalid hard reference is reported"), InvalidReferenceCount, 1.0);

	const TArray<TSharedPtr<FJsonValue>>* InvalidReferences = nullptr;
	if (TestTrue(TEXT("Row exposes one structured invalid reference"),
		Row->TryGetArrayField(TEXT("invalid_references"), InvalidReferences) &&
		InvalidReferences && InvalidReferences->Num() == 1))
	{
		const TSharedPtr<FJsonObject> InvalidReference = (*InvalidReferences)[0]->AsObject();
		FString ReferencerPath;
		FString ReferencedPath;
		FString ReferencedPackage;
		TestTrue(TEXT("Invalid reference exposes referencer"),
			InvalidReference.IsValid() &&
			InvalidReference->TryGetStringField(TEXT("referencer"), ReferencerPath));
		TestTrue(TEXT("Referencer path identifies the target asset"),
			ReferencerPath.Contains(TargetPackageName));
		TestTrue(TEXT("Invalid reference exposes referenced_object"),
			InvalidReference.IsValid() &&
			InvalidReference->TryGetStringField(TEXT("referenced_object"), ReferencedPath));
		TestTrue(TEXT("Referenced path identifies the private object"),
			ReferencedPath.Contains(TEXT("PrivateExternalObject")));
		TestTrue(TEXT("Invalid reference exposes referenced_package"),
			InvalidReference.IsValid() &&
			InvalidReference->TryGetStringField(TEXT("referenced_package"), ReferencedPackage));
		TestEqual(TEXT("Referenced package is exact"), ReferencedPackage, ExternalPackageName);
	}

	TestFalse(TEXT("Pre-save rejection leaves no package file on disk"),
		IFileManager::Get().FileExists(*TargetFilename));

	return true;
}
