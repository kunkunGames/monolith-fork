#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "MonolithJsonUtils.h"
#include "MonolithNiagaraActions.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraAddModuleParamGuardTest, "Monolith.ParamGuard.Niagara.AddModule", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraAddModuleParamGuardTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	auto MakeValidParams = []()
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/MonolithTests/MissingSystem"));
		Params->SetStringField(TEXT("emitter"), TEXT("TestEmitter"));
		Params->SetStringField(TEXT("usage"), TEXT("particle_spawn"));
		Params->SetStringField(TEXT("module_script"), TEXT("/Game/MonolithTests/MissingModule"));
		return Params;
	};

	enum class EInvalidStringCase : uint8
	{
		Missing,
		WrongType,
		Empty,
		Whitespace
	};

	struct FInvalidStringCase
	{
		EInvalidStringCase Type;
		const TCHAR* Label;
	};

	const TCHAR* RequiredFields[] = { TEXT("emitter"), TEXT("usage"), TEXT("module_script") };
	const FInvalidStringCase InvalidCases[] =
	{
		{ EInvalidStringCase::Missing, TEXT("missing") },
		{ EInvalidStringCase::WrongType, TEXT("wrong type") },
		{ EInvalidStringCase::Empty, TEXT("empty") },
		{ EInvalidStringCase::Whitespace, TEXT("whitespace-only") }
	};

	bool bAllPassed = true;
	for (const TCHAR* Field : RequiredFields)
	{
		for (const FInvalidStringCase& InvalidCase : InvalidCases)
		{
			TSharedPtr<FJsonObject> Params = MakeValidParams();
			switch (InvalidCase.Type)
			{
			case EInvalidStringCase::Missing:
				Params->RemoveField(Field);
				break;
			case EInvalidStringCase::WrongType:
				Params->SetNumberField(Field, 1.0);
				break;
			case EInvalidStringCase::Empty:
				Params->SetStringField(Field, TEXT(""));
				break;
			case EInvalidStringCase::Whitespace:
				Params->SetStringField(Field, TEXT("  \t "));
				break;
			}

			const FMonolithActionResult Result = FMonolithNiagaraActions::HandleAddModule(Params);
			const FString Context = FString::Printf(TEXT("%s %s"), Field, InvalidCase.Label);
			bAllPassed &= TestFalse(*FString::Printf(TEXT("%s should fail"), *Context), Result.bSuccess);
			bAllPassed &= TestEqual(*FString::Printf(TEXT("%s should be invalid params"), *Context), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
			bAllPassed &= TestTrue(*FString::Printf(TEXT("%s should name the invalid field"), *Context), Result.ErrorMessage.Contains(Field));
		}
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	const bool bHasAddModule = Registry.HasAction(TEXT("niagara"), TEXT("add_module"));
	bAllPassed &= TestTrue(TEXT("niagara.add_module should be registered"), bHasAddModule);
	if (bHasAddModule)
	{
		TSharedPtr<FJsonObject> RegistryParams = MakeShared<FJsonObject>();
		RegistryParams->SetStringField(TEXT("asset_path"), TEXT("/Game/MonolithTests/MissingSystem"));
		const FMonolithActionResult RegistryResult = Registry.ExecuteAction(TEXT("niagara"), TEXT("add_module"), RegistryParams);
		bAllPassed &= TestFalse(TEXT("Registry should reject missing required params"), RegistryResult.bSuccess);
		bAllPassed &= TestEqual(TEXT("Registry missing-required error code"), RegistryResult.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
		bAllPassed &= TestTrue(TEXT("Registry missing-required message"), RegistryResult.ErrorMessage.Contains(TEXT("Missing required param(s)")));
		bAllPassed &= TestTrue(TEXT("Registry missing-required error data"), RegistryResult.ErrorData.IsValid());
		if (RegistryResult.ErrorData.IsValid())
		{
			FString FailureCause;
			const bool bHasFailureCause = RegistryResult.ErrorData->TryGetStringField(TEXT("failure_cause"), FailureCause);
			bAllPassed &= TestTrue(TEXT("Registry should provide a failure cause"), bHasFailureCause);
			if (bHasFailureCause)
			{
				bAllPassed &= TestEqual(TEXT("Registry failure cause"), FailureCause, FString(TEXT("missing_required_param")));
			}
		}
	}

	return bAllPassed;
}
