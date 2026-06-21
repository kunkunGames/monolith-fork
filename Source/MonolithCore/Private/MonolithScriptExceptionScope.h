// Copyright Monolith. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/BlueprintExceptionInfo.h" // EBlueprintExceptionType

struct FFrame;
struct FBlueprintExceptionInfo;

/**
 * RAII scope that captures Blueprint / Kismet script exceptions raised on the
 * calling (game) thread for the lifetime of the object.
 *
 * Motivation: a Monolith action handler that drives Blueprint / Kismet work can
 * trigger UKismetSystemLibrary::RaiseScriptError (or an access-none / array-bounds
 * script error) and still return FMonolithActionResult::Success — the script error
 * is routed through FBlueprintCoreDelegates::OnScriptException and would otherwise be
 * silently lost. Wrapping the handler call in this scope lets the dispatcher detect a
 * "success but raised a script error" outcome and surface it as a structured error.
 *
 * Mirrors UE5.8 ToolsetRegistry's FToolCallExceptionHandler
 * (Engine/Plugins/Experimental/ToolsetRegistry .../ToolCallExceptionHandler.h).
 *
 * Pure-C++ handlers are unaffected: with no active script frame RaiseScriptError
 * no-ops, so nothing is captured and existing behaviour is byte-identical.
 *
 * Thread-safety: OnScriptException is a global multicast that may fire from any
 * thread. Capture is guarded by a critical section so a concurrent script error on
 * another thread cannot race the recording array.
 */
class FMonolithScriptExceptionScope
{
public:
	FMonolithScriptExceptionScope();
	~FMonolithScriptExceptionScope();

	FMonolithScriptExceptionScope(const FMonolithScriptExceptionScope&) = delete;
	FMonolithScriptExceptionScope& operator=(const FMonolithScriptExceptionScope&) = delete;
	FMonolithScriptExceptionScope(FMonolithScriptExceptionScope&&) = delete;
	FMonolithScriptExceptionScope& operator=(FMonolithScriptExceptionScope&&) = delete;

	/** True if any error-class (non-debug) script exception was captured. */
	bool HasError() const;

	/** Newline-joined captured error messages (empty string if none). */
	FString GetErrorString() const;

	/**
	 * Pure classification used by the OnScriptException handler and unit tests.
	 * Returns the message to record, or an unset optional for ignored debug/trace
	 * exception types (Breakpoint / Tracepoint / WireTracepoint). When the supplied
	 * Description is empty a stable type name is substituted so the message is never
	 * blank.
	 */
	static TOptional<FString> ClassifyException(EBlueprintExceptionType::Type Type, const FString& Description);

private:
	void HandleScriptException(const UObject* ActiveObject, const FFrame& StackFrame, const FBlueprintExceptionInfo& Info);

	FDelegateHandle Handle;
	mutable FCriticalSection Lock;
	TArray<FString> ErrorMessages;
};
