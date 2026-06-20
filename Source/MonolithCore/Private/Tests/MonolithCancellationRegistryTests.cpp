#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithCancellationRegistry.h"
#include "MonolithExecutionContext.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithCancellationRegistryTest,
	"Monolith.Core.Cancellation.Registry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithCancellationRegistryTest::RunTest(const FString& Parameters)
{
	FMonolithCancellationRegistry& Registry = FMonolithCancellationRegistry::Get();
	Registry.ResetForTests();

	// Cancelling an unknown request returns false and reports nothing.
	TestFalse(TEXT("Cancel of unregistered request returns false"), Registry.RequestCancellation(TEXT("7"), TEXT("x")));
	TestFalse(TEXT("Unregistered request is not cancelled"), Registry.IsCancellationRequested(TEXT("7")));

	// Register, then cancel from a notional second call (cross-thread path).
	Registry.Register(TEXT("7"));
	TestEqual(TEXT("One active request"), Registry.GetActiveCount(), 1);
	TestFalse(TEXT("Not cancelled before request"), Registry.IsCancellationRequested(TEXT("7")));
	TestTrue(TEXT("Cancel of in-flight request returns true"), Registry.RequestCancellation(TEXT("7"), TEXT("user requested")));
	TestTrue(TEXT("In-flight request now cancelled"), Registry.IsCancellationRequested(TEXT("7")));

	// A different id is unaffected.
	TestFalse(TEXT("Other id not cancelled"), Registry.IsCancellationRequested(TEXT("8")));

	// Unregister clears state, including any cancelled flag.
	Registry.Unregister(TEXT("7"));
	TestEqual(TEXT("No active requests after unregister"), Registry.GetActiveCount(), 0);
	TestFalse(TEXT("Cancelled state cleared after unregister"), Registry.IsCancellationRequested(TEXT("7")));

	// Empty / sentinel ids are never tracked.
	Registry.Register(FString());
	Registry.Register(TEXT("notification"));
	Registry.Register(TEXT("unknown"));
	TestEqual(TEXT("Empty/sentinel ids are not tracked"), Registry.GetActiveCount(), 0);

	// RAII registration scopes the lifetime and observes cancellation.
	{
		FScopedMonolithCancellationRegistration Scope(TEXT("42"));
		TestEqual(TEXT("RAII scope registers"), Registry.GetActiveCount(), 1);
		TestFalse(TEXT("RAII not cancelled initially"), Scope.IsCancellationRequested());
		TestTrue(TEXT("Cancel reaches the RAII-registered id"), Registry.RequestCancellation(TEXT("42"), TEXT("stop")));
		TestTrue(TEXT("RAII observes cancellation"), Scope.IsCancellationRequested());
	}
	TestEqual(TEXT("RAII scope unregisters on destruction"), Registry.GetActiveCount(), 0);

	// A non-addressable RAII registration is inert.
	{
		FScopedMonolithCancellationRegistration NotificationScope(TEXT("notification"));
		TestEqual(TEXT("Sentinel RAII registers nothing"), Registry.GetActiveCount(), 0);
		TestFalse(TEXT("Sentinel RAII never cancellable"), NotificationScope.IsCancellationRequested());
	}

	Registry.ResetForTests();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithCancellationIdNormalizationTest,
	"Monolith.Core.Cancellation.IdNormalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithCancellationIdNormalizationTest::RunTest(const FString& Parameters)
{
	// A JSON-RPC id sent as a number must normalize to the same key as the
	// equivalent string id, so a tools/call (number id) and its
	// notifications/cancelled (string requestId, or vice versa) correlate.
	const TSharedPtr<FJsonValue> NumberId = MakeShared<FJsonValueNumber>(42);
	const TSharedPtr<FJsonValue> StringId = MakeShared<FJsonValueString>(TEXT("42"));
	const FString NumberKey = FMonolithExecutionContext::JsonRpcIdToString(NumberId);
	const FString StringKey = FMonolithExecutionContext::JsonRpcIdToString(StringId);
	TestEqual(TEXT("Numeric id normalizes without a .0 artifact"), NumberKey, FString(TEXT("42")));
	TestEqual(TEXT("Number and string ids of equal value share one key"), NumberKey, StringKey);

	// End-to-end: register under the numeric-id key, cancel via the string-id key.
	FMonolithCancellationRegistry& Registry = FMonolithCancellationRegistry::Get();
	Registry.ResetForTests();
	Registry.Register(NumberKey);
	TestTrue(TEXT("Cancel via the string-id key reaches the number-registered request"),
		Registry.RequestCancellation(StringKey, TEXT("cross-type")));
	TestTrue(TEXT("Number-registered request observes the cancellation"),
		Registry.IsCancellationRequested(NumberKey));
	Registry.ResetForTests();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
