#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "MonolithJsonUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardSetParameterDefaultTest, "Monolith.Niagara.ParamGuard.SetParameterDefault", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardSetParameterDefaultTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/MissingSystem"));
	Params->SetStringField(TEXT("parameter"), TEXT("User.TestVec3"));

	// Malformed value missing numeric 'x'
	TSharedRef<FJsonObject> MalformedValue = MakeShared<FJsonObject>();
	MalformedValue->SetStringField(TEXT("x"), TEXT("not a number"));
	MalformedValue->SetNumberField(TEXT("y"), 0.0);
	MalformedValue->SetNumberField(TEXT("z"), 0.0);

	Params->SetObjectField(TEXT("value"), MalformedValue);

	FMonolithActionResult Result = FMonolithNiagaraActions::HandleSetParameterDefault(Params);

	// Ensure we correctly rejected the malformed input instead of asserting
	TestFalse(TEXT("Malformed parameter should fail"), Result.bSuccess);

	// Test a type where the value is completely wrong (e.g., Vec4 instead of color, etc.),
	// but the best way to verify our TryGetNumberField doesn't crash is passing
	// a completely malformed json object.
	TSharedRef<FJsonObject> MissingFieldsValue = MakeShared<FJsonObject>();
	Params->SetObjectField(TEXT("value"), MissingFieldsValue);
	Result = FMonolithNiagaraActions::HandleSetParameterDefault(Params);
	TestFalse(TEXT("Empty parameter value object should fail gracefully"), Result.bSuccess);

	return true;
}
