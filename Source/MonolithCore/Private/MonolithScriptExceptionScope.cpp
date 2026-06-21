// Copyright Monolith. All Rights Reserved.

#include "MonolithScriptExceptionScope.h"

#include "UObject/Script.h"   // FBlueprintCoreDelegates::OnScriptException
#include "UObject/Stack.h"    // FFrame

namespace
{
	/** Stable, human-readable name for an exception type used when no description is supplied. */
	FString ExceptionTypeName(EBlueprintExceptionType::Type Type)
	{
		switch (Type)
		{
		case EBlueprintExceptionType::AccessViolation: return TEXT("AccessViolation");
		case EBlueprintExceptionType::InfiniteLoop:    return TEXT("InfiniteLoop");
		case EBlueprintExceptionType::NonFatalError:   return TEXT("NonFatalError");
		case EBlueprintExceptionType::FatalError:      return TEXT("FatalError");
		case EBlueprintExceptionType::AbortExecution:  return TEXT("AbortExecution");
		case EBlueprintExceptionType::UserRaisedError: return TEXT("UserRaisedError");
		default:                                       return TEXT("ScriptError");
		}
	}
}

FMonolithScriptExceptionScope::FMonolithScriptExceptionScope()
{
	Handle = FBlueprintCoreDelegates::OnScriptException.AddRaw(this, &FMonolithScriptExceptionScope::HandleScriptException);
}

FMonolithScriptExceptionScope::~FMonolithScriptExceptionScope()
{
	FBlueprintCoreDelegates::OnScriptException.Remove(Handle);
}

bool FMonolithScriptExceptionScope::HasError() const
{
	FScopeLock ScopeLock(&Lock);
	return ErrorMessages.Num() > 0;
}

FString FMonolithScriptExceptionScope::GetErrorString() const
{
	FScopeLock ScopeLock(&Lock);
	return FString::Join(ErrorMessages, TEXT("\n"));
}

TOptional<FString> FMonolithScriptExceptionScope::ClassifyException(EBlueprintExceptionType::Type Type, const FString& Description)
{
	// Debug / trace events are not errors — every tracepoint raises one of these.
	switch (Type)
	{
	case EBlueprintExceptionType::Breakpoint:
	case EBlueprintExceptionType::Tracepoint:
	case EBlueprintExceptionType::WireTracepoint:
		return TOptional<FString>();
	default:
		break;
	}

	const FString Trimmed = Description.TrimStartAndEnd();
	return Trimmed.IsEmpty() ? ExceptionTypeName(Type) : Trimmed;
}

void FMonolithScriptExceptionScope::HandleScriptException(const UObject* /*ActiveObject*/, const FFrame& /*StackFrame*/, const FBlueprintExceptionInfo& Info)
{
	const TOptional<FString> Message = ClassifyException(Info.GetType(), Info.GetDescription().ToString());
	if (!Message.IsSet())
	{
		return;
	}

	FScopeLock ScopeLock(&Lock);
	ErrorMessages.Add(Message.GetValue());
}
