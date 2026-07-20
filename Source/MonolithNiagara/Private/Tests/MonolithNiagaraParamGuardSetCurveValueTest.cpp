#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardSetCurveValueTest, "Monolith.Niagara.ParamGuard.SetCurveValue", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardSetCurveValueTest::RunTest(const FString& Parameters)
{
	FMonolithNiagaraActions Actions;

	TSharedRef<FJsonObject> EmitterNotString = MakeShared<FJsonObject>();
	EmitterNotString->SetBoolField(TEXT("emitter"), true);

	FMonolithActionResult Result1 = Actions.HandleSetCurveValue(EmitterNotString);
	TestTrue(TEXT("emitter not string should fail"), Result1.bSuccess == false);
	TestTrue(TEXT("emitter error message"), Result1.ErrorMessage.Contains(TEXT("must be a string")));

	TSharedRef<FJsonObject> ModuleNodeNotString = MakeShared<FJsonObject>();
	ModuleNodeNotString->SetStringField(TEXT("emitter"), TEXT("Emit1"));
	ModuleNodeNotString->SetBoolField(TEXT("module_node"), true);

	FMonolithActionResult Result2 = Actions.HandleSetCurveValue(ModuleNodeNotString);
	TestTrue(TEXT("module_node not string should fail"), Result2.bSuccess == false);
	TestTrue(TEXT("module_node error message"), Result2.ErrorMessage.Contains(TEXT("must be a string")));

	TSharedRef<FJsonObject> InputNotString = MakeShared<FJsonObject>();
	InputNotString->SetStringField(TEXT("emitter"), TEXT("Emit1"));
	InputNotString->SetStringField(TEXT("module_node"), TEXT("Mod1"));
	InputNotString->SetBoolField(TEXT("input"), true);

	FMonolithActionResult Result3 = Actions.HandleSetCurveValue(InputNotString);
	TestTrue(TEXT("input not string should fail"), Result3.bSuccess == false);
	TestTrue(TEXT("input error message"), Result3.ErrorMessage.Contains(TEXT("must be a string")));

	TSharedRef<FJsonObject> InvalidKeyTime = MakeShared<FJsonObject>();
	InvalidKeyTime->SetStringField(TEXT("emitter"), TEXT("Emit1"));
	InvalidKeyTime->SetStringField(TEXT("module_node"), TEXT("Mod1"));
	InvalidKeyTime->SetStringField(TEXT("input"), TEXT("Input1"));
	TArray<TSharedPtr<FJsonValue>> Keys;
	TSharedRef<FJsonObject> BadKey = MakeShared<FJsonObject>();
	BadKey->SetStringField(TEXT("time"), TEXT("not_number"));
	BadKey->SetNumberField(TEXT("value"), 1.0);
	Keys.Add(MakeShared<FJsonValueObject>(BadKey));
	InvalidKeyTime->SetArrayField(TEXT("keys"), Keys);

	FMonolithActionResult Result4 = Actions.HandleSetCurveValue(InvalidKeyTime);
	TestTrue(TEXT("key time not number should fail"), Result4.bSuccess == false);
	TestTrue(TEXT("key time error message"), Result4.ErrorMessage.Contains(TEXT("numeric 'time'")));

	return true;
}
