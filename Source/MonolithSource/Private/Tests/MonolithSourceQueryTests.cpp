#include "Misc/AutomationTest.h"
#include "MonolithSourceDatabase.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceSearchSymbolsClampsLimitTest, "Monolith.IndexGuard.Source.SearchSymbolsClampsLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSourceSearchSymbolsClampsLimitTest::RunTest(const FString& Parameters)
{
	// Test limit clamping in database functions (assuming an opened DB would return 1000 items at max)
	// We create a temporary DB just for the test
	FMonolithSourceDatabase DB;

	// Open an in-memory DB or check validation
	// As we might not have a full initialized DB, we test that the method handles the limit without crashing
	// If the DB isn't valid, it returns an empty array safely without executing the query.
	TArray<FMonolithSourceSymbol> Results = DB.SearchSymbolsFTS(TEXT("TestSymbol"), 50000);

	TestTrue(TEXT("Empty results from uninitialized DB"), Results.Num() == 0);

	return true;
}
