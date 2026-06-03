#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithActionJsonBridge.h"
#include "MonolithJsonUtils.h"
#include "MonolithTestSupport.h"
#include "MonolithToolRegistry.h"

namespace
{
	const TCHAR* BridgeTestNamespace = TEXT("actionjsonbridgetest");

	void RegisterBridgeTestActions(FMonolithToolRegistry& Registry)
	{
		Registry.RegisterAction(
			BridgeTestNamespace,
			TEXT("success_action"),
			TEXT("Action JSON bridge success test action."),
			FMonolithActionHandler::CreateLambda([](const TSharedPtr<FJsonObject>&)
			{
				TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("status"), TEXT("ok"));
				return FMonolithActionResult::Success(Result);
			}));

		Registry.RegisterAction(
			BridgeTestNamespace,
			TEXT("error_action"),
			TEXT("Action JSON bridge error test action."),
			FMonolithActionHandler::CreateLambda([](const TSharedPtr<FJsonObject>&)
			{
				return FMonolithActionResult::Error(TEXT("bad params"), FMonolithJsonUtils::ErrInvalidParams);
			}));

		Registry.RegisterAction(
			BridgeTestNamespace,
			TEXT("null_result_action"),
			TEXT("Action JSON bridge null-result test action."),
			FMonolithActionHandler::CreateLambda([](const TSharedPtr<FJsonObject>&)
			{
				FMonolithActionResult Result;
				Result.bSuccess = true;
				return Result;
			}));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithActionJsonBridgeContractTest,
	"Monolith.Core.ActionJsonBridge.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithActionJsonBridgeContractTest::RunTest(const FString& Parameters)
{
	FMonolithScopedTestNamespace ScopedNamespace(BridgeTestNamespace);
	RegisterBridgeTestActions(FMonolithToolRegistry::Get());

	bool bSuccess = false;
	FString Error;

	const FString SuccessJson = FMonolithActionJsonBridge::ExecuteActionAsJson(
		BridgeTestNamespace,
		TEXT("success_action"),
		MakeShared<FJsonObject>(),
		bSuccess,
		Error);
	TestTrue(TEXT("success action reports success"), bSuccess);
	TestTrue(TEXT("success action leaves error empty"), Error.IsEmpty());
	TSharedPtr<FJsonObject> SuccessObject = FMonolithJsonUtils::Parse(SuccessJson);
	TestTrue(TEXT("success action returns parseable JSON"), SuccessObject.IsValid());
	if (SuccessObject.IsValid())
	{
		TestEqual(TEXT("success action serializes raw result object"), SuccessObject->GetStringField(TEXT("status")), TEXT("ok"));
	}

	const FString MissingJson = FMonolithActionJsonBridge::ExecuteActionAsJson(
		BridgeTestNamespace,
		TEXT("missing_action"),
		MakeShared<FJsonObject>(),
		bSuccess,
		Error);
	TestFalse(TEXT("missing action reports failure"), bSuccess);
	TestTrue(TEXT("missing action error mentions registry"), Error.Contains(TEXT("not registered")));
	TSharedPtr<FJsonObject> MissingObject = FMonolithJsonUtils::Parse(MissingJson);
	TestTrue(TEXT("missing action returns parseable JSON"), MissingObject.IsValid());
	if (MissingObject.IsValid())
	{
		TestFalse(TEXT("missing action envelope success=false"), MissingObject->GetBoolField(TEXT("success")));
		TestEqual(TEXT("missing action error code"), MissingObject->GetNumberField(TEXT("code")), static_cast<double>(FMonolithJsonUtils::ErrMethodNotFound));
	}

	const FString ErrorJson = FMonolithActionJsonBridge::ExecuteActionAsJson(
		BridgeTestNamespace,
		TEXT("error_action"),
		MakeShared<FJsonObject>(),
		bSuccess,
		Error);
	TestFalse(TEXT("handler error reports failure"), bSuccess);
	TestEqual(TEXT("handler error copied to OutError"), Error, TEXT("bad params"));
	TSharedPtr<FJsonObject> ErrorObject = FMonolithJsonUtils::Parse(ErrorJson);
	TestTrue(TEXT("handler error returns parseable JSON"), ErrorObject.IsValid());
	if (ErrorObject.IsValid())
	{
		TestFalse(TEXT("handler error envelope success=false"), ErrorObject->GetBoolField(TEXT("success")));
		TestEqual(TEXT("handler error code"), ErrorObject->GetNumberField(TEXT("code")), static_cast<double>(FMonolithJsonUtils::ErrInvalidParams));
	}

	const FString NullJson = FMonolithActionJsonBridge::ExecuteActionAsJson(
		BridgeTestNamespace,
		TEXT("null_result_action"),
		MakeShared<FJsonObject>(),
		bSuccess,
		Error);
	TestFalse(TEXT("null result reports failure"), bSuccess);
	TestTrue(TEXT("null result error explains payload"), Error.Contains(TEXT("null result payload")));
	TSharedPtr<FJsonObject> NullObject = FMonolithJsonUtils::Parse(NullJson);
	TestTrue(TEXT("null result returns parseable JSON"), NullObject.IsValid());
	if (NullObject.IsValid())
	{
		TestFalse(TEXT("null result envelope success=false"), NullObject->GetBoolField(TEXT("success")));
		TestEqual(TEXT("null result error code"), NullObject->GetNumberField(TEXT("code")), static_cast<double>(FMonolithJsonUtils::ErrInternalError));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
