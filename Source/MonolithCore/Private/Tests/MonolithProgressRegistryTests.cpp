#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithProgressRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithProgressRegistryTest,
	"Monolith.Core.Progress.Registry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithProgressRegistryTest::RunTest(const FString& Parameters)
{
	FMonolithProgressRegistry& Registry = FMonolithProgressRegistry::Get();
	Registry.ResetForTests();

	// Reporting against an unregistered token is ignored (no unbounded growth).
	Registry.Report(TEXT("p1"), 5, 10, TEXT("early"));
	TestFalse(TEXT("Unregistered token is not active"), Registry.IsActive(TEXT("p1")));
	TestEqual(TEXT("No active progress before register"), Registry.GetActiveCount(), 0);

	// Register, then report twice — the latest wins, update_count accumulates.
	Registry.Register(TEXT("p1"));
	TestTrue(TEXT("Registered token is active"), Registry.IsActive(TEXT("p1")));
	Registry.Report(TEXT("p1"), 3, 10, TEXT("step 3"));
	Registry.Report(TEXT("p1"), 7, 10, TEXT("step 7"));

	TSharedPtr<FJsonObject> Active = Registry.GetActiveJson();
	TestTrue(TEXT("Active json exists"), Active.IsValid());
	if (Active.IsValid())
	{
		TestEqual(TEXT("One active progress entry"), static_cast<int32>(Active->GetNumberField(TEXT("count"))), 1);
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		TestTrue(TEXT("active_progress array exists"), Active->TryGetArrayField(TEXT("active_progress"), Items));
		if (Items && Items->Num() == 1)
		{
			const TSharedPtr<FJsonObject>* Row = nullptr;
			TestTrue(TEXT("Row is object"), (*Items)[0]->TryGetObject(Row));
			if (Row && Row->IsValid())
			{
				TestEqual(TEXT("Token matches"), (*Row)->GetStringField(TEXT("progress_token")), FString(TEXT("p1")));
				TestEqual(TEXT("Latest progress is 7"), (*Row)->GetNumberField(TEXT("progress")), 7.0);
				TestEqual(TEXT("Total is 10"), (*Row)->GetNumberField(TEXT("total")), 10.0);
				TestEqual(TEXT("Message is the latest"), (*Row)->GetStringField(TEXT("message")), FString(TEXT("step 7")));
				TestEqual(TEXT("Update count is 2"), static_cast<int32>((*Row)->GetNumberField(TEXT("update_count"))), 2);
			}
		}
	}

	// Unknown-total (Total < 0) and second concurrent token.
	Registry.Register(TEXT("p2"));
	Registry.Report(TEXT("p2"), 1, -1, FString());
	TestEqual(TEXT("Two active entries"), Registry.GetActiveCount(), 2);

	// Unregister clears state.
	Registry.Unregister(TEXT("p1"));
	Registry.Unregister(TEXT("p2"));
	TestEqual(TEXT("No active progress after unregister"), Registry.GetActiveCount(), 0);

	// Empty token is inert.
	Registry.Register(FString());
	TestEqual(TEXT("Empty token not tracked"), Registry.GetActiveCount(), 0);

	// Duplicate-token refcounting: two concurrent registrations share one entry;
	// the first Unregister must NOT drop the still-active survivor.
	Registry.Register(TEXT("dup"));
	Registry.Register(TEXT("dup"));
	TestEqual(TEXT("Duplicate token is one entry"), Registry.GetActiveCount(), 1);
	Registry.Unregister(TEXT("dup"));
	TestTrue(TEXT("Survivor still active after one unregister"), Registry.IsActive(TEXT("dup")));
	Registry.Report(TEXT("dup"), 3, 4, TEXT("survivor"));
	TestEqual(TEXT("Still one entry while survivor runs"), Registry.GetActiveCount(), 1);
	Registry.Unregister(TEXT("dup"));
	TestEqual(TEXT("Entry removed after the last unregister"), Registry.GetActiveCount(), 0);

	// RAII registration scopes the lifetime.
	{
		FScopedMonolithProgressRegistration Scope(TEXT("p3"));
		TestEqual(TEXT("RAII registers"), Registry.GetActiveCount(), 1);
		Registry.Report(TEXT("p3"), 50, 100, TEXT("half"));
		TestTrue(TEXT("RAII token active"), Registry.IsActive(TEXT("p3")));
	}
	TestEqual(TEXT("RAII unregisters on destruction"), Registry.GetActiveCount(), 0);

	Registry.ResetForTests();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
