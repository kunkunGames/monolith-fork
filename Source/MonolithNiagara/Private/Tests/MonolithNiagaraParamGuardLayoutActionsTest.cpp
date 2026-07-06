#include "Misc/AutomationTest.h"
#include "MonolithJsonUtils.h"
#include "MonolithNiagaraLayoutActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardLayoutActionsTest, "Monolith.ParamGuard.Niagara.LayoutActions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardLayoutActionsTest::RunTest(const FString& Parameters)
{
	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("asset_path"), 123); // Invalid type (number instead of string)

		FMonolithActionResult Result = FMonolithNiagaraLayoutActions::HandleAutoLayout(Params);
		TestTrue(TEXT("HandleAutoLayout rejects malformed 'asset_path' param"), Result.Type == EMonolithActionResultType::Error);
		TestEqual(TEXT("HandleAutoLayout rejects malformed 'asset_path' param with ErrInvalidParams"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("system_path"), 123); // Invalid type (number instead of string)

		FMonolithActionResult Result = FMonolithNiagaraLayoutActions::HandleAutoLayout(Params);
		TestTrue(TEXT("HandleAutoLayout rejects malformed 'system_path' param"), Result.Type == EMonolithActionResultType::Error);
		TestEqual(TEXT("HandleAutoLayout rejects malformed 'system_path' param with ErrInvalidParams"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Missing.Missing"));
		Params->SetNumberField(TEXT("emitter"), 123); // Invalid type (number instead of string)

		FMonolithActionResult Result = FMonolithNiagaraLayoutActions::HandleAutoLayout(Params);
		TestTrue(TEXT("HandleAutoLayout rejects malformed 'emitter' param"), Result.Type == EMonolithActionResultType::Error);
		TestEqual(TEXT("HandleAutoLayout rejects malformed 'emitter' param with ErrInvalidParams"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Missing.Missing"));
		Params->SetNumberField(TEXT("script_usage"), 123); // Invalid type

		FMonolithActionResult Result = FMonolithNiagaraLayoutActions::HandleAutoLayout(Params);
		TestTrue(TEXT("HandleAutoLayout rejects malformed 'script_usage' param"), Result.Type == EMonolithActionResultType::Error);
		TestEqual(TEXT("HandleAutoLayout rejects malformed 'script_usage' param with ErrInvalidParams"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
	}

	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Missing.Missing"));
		Params->SetNumberField(TEXT("formatter"), 123); // Invalid type

		FMonolithActionResult Result = FMonolithNiagaraLayoutActions::HandleAutoLayout(Params);
		TestTrue(TEXT("HandleAutoLayout rejects malformed 'formatter' param"), Result.Type == EMonolithActionResultType::Error);
		TestEqual(TEXT("HandleAutoLayout rejects malformed 'formatter' param with ErrInvalidParams"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
	}

	{
		// Valid inputs test
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Missing.Missing"));
		Params->SetStringField(TEXT("emitter"), TEXT("MyEmitter"));
		Params->SetStringField(TEXT("script_usage"), TEXT("system"));
		Params->SetStringField(TEXT("formatter"), TEXT("auto"));

		FMonolithActionResult Result = FMonolithNiagaraLayoutActions::HandleAutoLayout(Params);
		// It will fail because the asset doesn't exist, but it should not return ErrInvalidParams
		TestTrue(TEXT("HandleAutoLayout parses valid inputs correctly without returning ErrInvalidParams"), Result.ErrorCode != FMonolithJsonUtils::ErrInvalidParams);
	}

	return true;
}
