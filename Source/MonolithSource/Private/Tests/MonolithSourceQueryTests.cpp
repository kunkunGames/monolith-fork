#include "Misc/AutomationTest.h"
#include "MonolithSourceDatabase.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceSearchSymbolsClampsLimitTest, "Monolith.IndexGuard.Source.SearchSymbolsClampsLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSourceSearchSymbolsClampsLimitTest::RunTest(const FString& Parameters)
{
	const FString DbPath = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithSourceQuery"), TEXT(".sqlite"));
	FMonolithSourceDatabase DB;

	TestTrue(TEXT("Temporary DB opens for writing"), DB.OpenForWriting(DbPath));
	TestTrue(TEXT("Temporary DB creates schema"), DB.CreateTablesIfNeeded());

	const int64 ModuleId = DB.InsertModule(TEXT("TestModule"), TEXT("/tmp/TestModule"), TEXT("Runtime"));
	const int64 FileId = DB.InsertFile(TEXT("/tmp/TestModule/Test.cpp"), ModuleId, TEXT("cpp"), 1, 0.0);
	TestTrue(TEXT("Test module inserted"), ModuleId != 0);
	TestTrue(TEXT("Test file inserted"), FileId != 0);

	for (int32 Index = 0; Index < 1100; ++Index)
	{
		const FString QualifiedName = FString::Printf(TEXT("TestModule::TestSymbol%d"), Index);
		DB.InsertSymbol(TEXT("TestSymbol"), QualifiedName, TEXT("function"), FileId, Index + 1, Index + 1, 0, TEXT("public"), TEXT("void TestSymbol()"), TEXT(""), false);
	}

	TArray<FMonolithSourceSymbol> Results = DB.SearchSymbolsFTS(TEXT("TestSymbol"), 50000);

	TestEqual(TEXT("Huge FTS limit is clamped to 1000"), Results.Num(), 1000);

	DB.Close();
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*DbPath);

	return true;
}
