#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithJsonUtils.h"
#include "MonolithNiagaraActions.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraGetModuleInputValueParamTest, "Monolith.ParamGuard.Niagara.GetModuleInputValue", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraGetModuleInputValueParamTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	// Test 1: Missing emitter
	Params->SetStringField(TEXT("module_node"), TEXT("some_node"));
	Params->SetStringField(TEXT("input"), TEXT("some_input"));
	FMonolithActionResult Result = FMonolithNiagaraActions::HandleGetModuleInputValue(Params);
	TestFalse(TEXT("Should fail when emitter is missing"), Result.bSuccess);
	TestEqual(TEXT("Should return InvalidParams error code"), static_cast<int32>(Result.ErrorCode), FMonolithJsonUtils::ErrInvalidParams);
	TestTrue(TEXT("Error message should mention emitter"), Result.ErrorMessage.Contains(TEXT("emitter")));

	// Test 2: Invalid emitter type
	Params->SetNumberField(TEXT("emitter"), 123);
	Result = FMonolithNiagaraActions::HandleGetModuleInputValue(Params);
	TestFalse(TEXT("Should fail when emitter is a number"), Result.bSuccess);
	TestEqual(TEXT("Should return InvalidParams error code"), static_cast<int32>(Result.ErrorCode), FMonolithJsonUtils::ErrInvalidParams);

	// Test 3: Missing module_node
	Params->SetStringField(TEXT("emitter"), TEXT("some_emitter"));
	Params->RemoveField(TEXT("module_node"));
	Result = FMonolithNiagaraActions::HandleGetModuleInputValue(Params);
	TestFalse(TEXT("Should fail when module_node is missing"), Result.bSuccess);
	TestEqual(TEXT("Should return InvalidParams error code"), static_cast<int32>(Result.ErrorCode), FMonolithJsonUtils::ErrInvalidParams);
	TestTrue(TEXT("Error message should mention module_node"), Result.ErrorMessage.Contains(TEXT("module_node")));

	// Test 4: Missing input
	Params->SetStringField(TEXT("module_node"), TEXT("some_node"));
	Params->RemoveField(TEXT("input"));
	Result = FMonolithNiagaraActions::HandleGetModuleInputValue(Params);
	TestFalse(TEXT("Should fail when input is missing"), Result.bSuccess);
	TestEqual(TEXT("Should return InvalidParams error code"), static_cast<int32>(Result.ErrorCode), FMonolithJsonUtils::ErrInvalidParams);
	TestTrue(TEXT("Error message should mention input"), Result.ErrorMessage.Contains(TEXT("input")));

	return true;
}
