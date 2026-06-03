#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/Guid.h"
#include "Modules/ModuleManager.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardImportSystemSpecTest, "Monolith.ParamGuard.Niagara.ImportSystemSpec", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardImportSystemSpecTest::RunTest(const FString& Parameters)
{
    // Test 1: Wrong type for mode (number instead of string)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetObjectField(TEXT("spec"), MakeShared<FJsonObject>());
        Params->SetNumberField(TEXT("mode"), 12345);

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleImportSystemSpec(Params);
        TestFalse(TEXT("ImportSystemSpec should fail gracefully with wrong-type mode"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention mode type issue"), Result.ErrorMessage.Contains(TEXT("mode")));
    }

    // Test 2: Wrong type for template in create_system_from_spec (number instead of string)
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("save_path"), TEXT("/Game/NonExistentSystem"));

        TSharedRef<FJsonObject> SpecObj = MakeShared<FJsonObject>();
        SpecObj->SetNumberField(TEXT("template"), 12345);
        Params->SetObjectField(TEXT("spec"), SpecObj);

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleCreateSystemFromSpec(Params);
        TestFalse(TEXT("CreateSystemFromSpec should fail gracefully with wrong-type template"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention template type issue"), Result.ErrorMessage.Contains(TEXT("template")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardImportSystemSpecMalformedTypeTest, "Monolith.Security.Niagara.ImportSystemSpecMalformedType", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardImportSystemSpecMalformedTypeTest::RunTest(const FString& Parameters)
{
	const FString SystemPath = FString::Printf(TEXT("/Game/MonolithTests/Niagara/ParamGuard/NS_MalformedName_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Short));
	const int32 LastSlash = SystemPath.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	const FString AssetName = LastSlash != INDEX_NONE ? SystemPath.Mid(LastSlash + 1) : SystemPath;

	UPackage* Package = CreatePackage(*SystemPath);
	TestNotNull(TEXT("Test Niagara package should be created"), Package);
	if (!Package)
	{
		return true;
	}

	UNiagaraSystem* System = NewObject<UNiagaraSystem>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	TestNotNull(TEXT("Test Niagara system should be created"), System);
	if (!System)
	{
		return true;
	}

	UNiagaraSystemFactoryNew::InitializeSystem(System, true);
	FAssetRegistryModule::AssetCreated(System);
	Package->MarkPackageDirty();

	TSharedRef<FJsonObject> Spec = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> EmittersArray;
	TSharedRef<FJsonObject> EmitterObj = MakeShared<FJsonObject>();
	EmitterObj->SetStringField(TEXT("asset"), TEXT("/Game/MonolithTests/Niagara/ParamGuard/MissingEmitter"));
	EmitterObj->SetBoolField(TEXT("name"), true);
	EmittersArray.Add(MakeShared<FJsonValueObject>(EmitterObj));
	Spec->SetArrayField(TEXT("emitters"), EmittersArray);

	TArray<TSharedPtr<FJsonValue>> UserParamsArray;
	TSharedRef<FJsonObject> ParamObj = MakeShared<FJsonObject>();
	ParamObj->SetBoolField(TEXT("name"), true);
	ParamObj->SetStringField(TEXT("type"), TEXT("float"));
	UserParamsArray.Add(MakeShared<FJsonValueObject>(ParamObj));
	Spec->SetArrayField(TEXT("user_parameters"), UserParamsArray);

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), SystemPath);
		Params->SetObjectField(TEXT("spec"), Spec);

		FMonolithActionResult Result = FMonolithNiagaraActions::HandleImportSystemSpec(Params);

		TestTrue(TEXT("Overwrite import should return structured result after rejecting malformed emitter name"), Result.bSuccess);
		if (!Result.Result.IsValid())
		{
			AddError(TEXT("Import result JSON object is invalid"));
			return true;
		}

		TestFalse(TEXT("Import result should mark malformed emitter name as failed"), Result.Result->GetBoolField(TEXT("success")));
		TestEqual(TEXT("Malformed emitter and user parameter names should count as failed steps"), Result.Result->GetNumberField(TEXT("failed_steps")), 2.0);
		const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
		if (Result.Result->TryGetArrayField(TEXT("errors"), Errors) && Errors && Errors->Num() > 0)
		{
			bool bFoundEmitterError = false;
			bool bFoundUserParamError = false;
			for (const auto& Err : *Errors)
			{
				FString ErrStr = Err->AsString();
				if (ErrStr.Contains(TEXT("spec.emitters")) && ErrStr.Contains(TEXT("name")))
				{
					bFoundEmitterError = true;
				}
				if (ErrStr.Contains(TEXT("spec.user_parameters")) && ErrStr.Contains(TEXT("name")))
				{
					bFoundUserParamError = true;
				}
			}
			TestTrue(TEXT("Failure should mention malformed emitter name"), bFoundEmitterError);
			TestTrue(TEXT("Failure should mention malformed user parameter name"), bFoundUserParamError);
		}
		else
		{
			AddError(TEXT("Import result should include malformed-name failure details"));
		}
	}

	{
		TSharedRef<FJsonObject> MergeUserParamSpec = MakeShared<FJsonObject>();
		MergeUserParamSpec->SetArrayField(TEXT("user_parameters"), UserParamsArray);

		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), SystemPath);
		Params->SetStringField(TEXT("mode"), TEXT("merge"));
		Params->SetObjectField(TEXT("spec"), MergeUserParamSpec);

		FMonolithActionResult Result = FMonolithNiagaraActions::HandleImportSystemSpec(Params);

		TestFalse(TEXT("Merge import should reject malformed user parameter name"), Result.bSuccess);
		TestTrue(TEXT("Merge import error should mention malformed user parameter name"), Result.ErrorMessage.Contains(TEXT("spec.user_parameters")) && Result.ErrorMessage.Contains(TEXT("name")));
	}

	{
		TArray<TSharedPtr<FJsonValue>> MissingNameUserParamsArray;
		TSharedRef<FJsonObject> MissingNameParamObj = MakeShared<FJsonObject>();
		MissingNameParamObj->SetStringField(TEXT("type"), TEXT("float"));
		MissingNameUserParamsArray.Add(MakeShared<FJsonValueObject>(MissingNameParamObj));

		TSharedRef<FJsonObject> MergeMissingNameSpec = MakeShared<FJsonObject>();
		MergeMissingNameSpec->SetArrayField(TEXT("user_parameters"), MissingNameUserParamsArray);

		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), SystemPath);
		Params->SetStringField(TEXT("mode"), TEXT("merge"));
		Params->SetObjectField(TEXT("spec"), MergeMissingNameSpec);

		FMonolithActionResult Result = FMonolithNiagaraActions::HandleImportSystemSpec(Params);

		TestFalse(TEXT("Merge import should reject missing user parameter name"), Result.bSuccess);
		TestTrue(TEXT("Merge import error should mention missing user parameter name"), Result.ErrorMessage.Contains(TEXT("spec.user_parameters")) && Result.ErrorMessage.Contains(TEXT("name")));
	}

	{
		TSharedRef<FJsonObject> MergeSpec = MakeShared<FJsonObject>();
		MergeSpec->SetArrayField(TEXT("emitters"), EmittersArray);

		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), SystemPath);
		Params->SetStringField(TEXT("mode"), TEXT("merge"));
		Params->SetObjectField(TEXT("spec"), MergeSpec);

		FMonolithActionResult Result = FMonolithNiagaraActions::HandleImportSystemSpec(Params);

		TestFalse(TEXT("Merge import should reject malformed emitter name before filtering"), Result.bSuccess);
		TestTrue(TEXT("Merge import error should mention malformed emitter name"), Result.ErrorMessage.Contains(TEXT("spec.emitters")) && Result.ErrorMessage.Contains(TEXT("name")));
	}

	return true;
}
