#include "MonolithActionJsonBridge.h"

#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"

FString FMonolithActionJsonBridge::MakeErrorEnvelope(const FString& Message, int32 Code)
{
	const TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
	Envelope->SetBoolField(TEXT("success"), false);
	Envelope->SetStringField(TEXT("error"), Message);
	Envelope->SetNumberField(TEXT("code"), Code);
	return FMonolithJsonUtils::Serialize(Envelope);
}

FString FMonolithActionJsonBridge::ExecuteActionAsJson(
	const FString& Namespace,
	const FString& Action,
	const TSharedPtr<FJsonObject>& Params,
	bool& bOutSuccess,
	FString& OutError)
{
	bOutSuccess = false;
	OutError.Reset();

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(Namespace, Action))
	{
		OutError = FString::Printf(
			TEXT("Action '%s.%s' is not registered (registry not yet populated or unknown action)."),
			*Namespace,
			*Action);
		return MakeErrorEnvelope(OutError, FMonolithJsonUtils::ErrMethodNotFound);
	}

	const TSharedPtr<FJsonObject> SafeParams = Params.IsValid() ? Params : MakeShared<FJsonObject>();
	const FMonolithActionResult Result = Registry.ExecuteAction(Namespace, Action, SafeParams);

	if (!Result.bSuccess)
	{
		OutError = Result.ErrorMessage.IsEmpty()
			? FString::Printf(TEXT("Action '%s.%s' failed with no error message."), *Namespace, *Action)
			: Result.ErrorMessage;
		return MakeErrorEnvelope(OutError, Result.ErrorCode);
	}

	if (!Result.Result.IsValid())
	{
		OutError = FString::Printf(
			TEXT("Action '%s.%s' reported success but returned a null result payload."),
			*Namespace,
			*Action);
		return MakeErrorEnvelope(OutError, FMonolithJsonUtils::ErrInternalError);
	}

	bOutSuccess = true;
	return FMonolithJsonUtils::Serialize(Result.Result);
}
