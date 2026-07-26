#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithSourceIndexer.h"
#include "MonolithSourceSubsystem.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithSourceIndexerOpenFailureCompletionTest,
	"Monolith.Activation.SourceWriterOpenFailureRecovers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSourceIndexerOpenFailureCompletionTest::RunTest(
	const FString& /*Parameters*/)
{
	const FString TestDirectory = FPaths::Combine(
		FPaths::ProjectIntermediateDir(),
		TEXT("MonolithSourceIndexerFailureTests"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString BlockingFile = FPaths::Combine(TestDirectory, TEXT("NotADirectory"));
	const FString UnopenableDatabasePath =
		FPaths::Combine(BlockingFile, TEXT("EngineSource.db"));

	IFileManager::Get().MakeDirectory(*TestDirectory, true);
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*TestDirectory, false, true);
	};

	if (!TestTrue(
			TEXT("blocking file fixture is created"),
			FFileHelper::SaveStringToFile(TEXT("fixture"), *BlockingFile)))
	{
		return false;
	}

	int32 CompletionBroadcastCount = 0;
	int32 CompletionErrors = 0;
	FMonolithSourceIndexer Indexer;
	Indexer.SetDatabasePath(UnopenableDatabasePath);
	Indexer.OnComplete.AddLambda(
		[&CompletionBroadcastCount, &CompletionErrors](
			int32 /*Files*/,
			int32 /*Symbols*/,
			int32 Errors)
		{
			++CompletionBroadcastCount;
			CompletionErrors = Errors;
		});

	AddExpectedError(
		TEXT("Failed to open database"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	AddExpectedError(
		TEXT("OpenForWriting: failed to open/create DB"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	AddExpectedError(
		TEXT("Indexer: Failed to open DB for writing"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	TestTrue(
		TEXT("synchronous writer attempt is dispatched"),
		Indexer.RunSynchronous());

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithProcessLocalReindexRejectionTest,
	"Monolith.Activation.ProcessLocalReindexRejection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithProcessLocalReindexRejectionTest::RunTest(
	const FString& /*Parameters*/)
{
	UMonolithIndexSubsystem* ProjectIndex =
		NewObject<UMonolithIndexSubsystem>(GetTransientPackage());
	if (!TestNotNull(
		TEXT("transient project-index subsystem is created"),
		ProjectIndex))
	{
		return false;
	}
	TestFalse(
		TEXT("project Settings re-index eligibility observes process-local stop"),
		ProjectIndex->CanAcceptIndexRequest());

	UFunction* StartFullIndexFunction =
		UMonolithIndexSubsystem::StaticClass()->FindFunctionByName(
			TEXT("StartFullIndex"));
	if (!TestNotNull(
		TEXT("StartFullIndex remains reflectively dispatchable"),
		StartFullIndexFunction))
	{
		return false;
	}

	struct
	{
		bool ReturnValue = true;
	} ProjectParams;
	AddExpectedError(
		TEXT("Project indexing is disabled"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	ProjectIndex->ProcessEvent(StartFullIndexFunction, &ProjectParams);
	TestFalse(
		TEXT("reflective project re-index reports process-local rejection"),
		ProjectParams.ReturnValue);

	UMonolithSourceSubsystem* SourceIndex =
		NewObject<UMonolithSourceSubsystem>(GetTransientPackage());
	if (!TestNotNull(
		TEXT("transient source-index subsystem is created"),
		SourceIndex))
	{
		return false;
	}
	TestFalse(
		TEXT("source Settings re-index eligibility observes process-local stop"),
		SourceIndex->CanAcceptIndexRequest());

	AddExpectedError(
		TEXT("Source indexing is disabled"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	TestFalse(
		TEXT("source re-index reports process-local rejection"),
		SourceIndex->TriggerReindex());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
