// SPDX-License-Identifier: MIT
// Regression cover for the indexer's failure exits (issue #114).
//
// A run that bailed out before reaching the end of Run() used to return without
// broadcasting OnComplete. The owning subsystem sets bIsIndexing when it starts a
// run and clears it in that callback, so a silent exit latched the flag: every
// later reindex request was refused with "Indexing already in progress" until the
// editor restarted. Every exit now routes through FMonolithSourceIndexer::CompleteRun.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "MonolithSourceIndexer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithSourceIndexerOpenFailureCompletionTest,
	"Monolith.Source.Indexer.WriterOpenFailureBroadcastsCompletion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSourceIndexerOpenFailureCompletionTest::RunTest(const FString& /*Parameters*/)
{
	// Build a database path that cannot possibly be opened: place it "inside" a
	// regular file, so the directory component is not a directory.
	const FString TestDirectory = FPaths::Combine(
		FPaths::AutomationTransientDir(),
		TEXT("MonolithSourceIndexerFailureTests"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString BlockingFile = FPaths::Combine(TestDirectory, TEXT("NotADirectory"));
	const FString UnopenableDatabasePath = FPaths::Combine(BlockingFile, TEXT("EngineSource.db"));

	IFileManager::Get().MakeDirectory(*TestDirectory, true);
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*TestDirectory, false, true);
	};

	if (!TestTrue(TEXT("blocking file fixture is created"),
		FFileHelper::SaveStringToFile(TEXT("fixture"), *BlockingFile)))
	{
		return false;
	}

	int32 CompletionBroadcastCount = 0;
	int32 CompletionErrors = 0;

	FMonolithSourceIndexer Indexer;
	Indexer.SetDatabasePath(UnopenableDatabasePath);
	Indexer.OnComplete.AddLambda(
		[&CompletionBroadcastCount, &CompletionErrors](int32 /*Files*/, int32 /*Symbols*/, int32 Errors)
		{
			++CompletionBroadcastCount;
			CompletionErrors = Errors;
		});

	// SQLite reports the open failure itself; its wording and count are the
	// engine's business, so those messages are suppressed rather than asserted.
	AddExpectedErrorPlain(
		TEXT("Failed to open database"),
		EAutomationExpectedErrorFlags::Contains,
		/*Occurrences=*/ -1);
	AddExpectedErrorPlain(
		TEXT("OpenForWriting: failed to open/create DB"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	AddExpectedErrorPlain(
		TEXT("Indexer: Failed to open DB for writing"),
		EAutomationExpectedErrorFlags::Contains,
		1);

	TestTrue(TEXT("synchronous run is dispatched"), Indexer.RunSynchronous());

	TestEqual(
		TEXT("writer-open failure broadcasts completion exactly once"),
		CompletionBroadcastCount,
		1);
	TestTrue(
		TEXT("writer-open failure contributes an error"),
		CompletionErrors > 0);
	TestFalse(
		TEXT("writer-open failure clears the running state"),
		Indexer.IsRunning());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
