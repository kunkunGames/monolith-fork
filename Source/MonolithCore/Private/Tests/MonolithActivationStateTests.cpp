#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "MonolithActivationState.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithActivationStatePersistenceTest,
	"Monolith.Activation.PersistentState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithActivationStatePersistenceTest::RunTest(const FString& Parameters)
{
	const FString TestDirectory = FPaths::Combine(
		FPaths::ProjectIntermediateDir(),
		TEXT("MonolithActivationStateTests"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString TestFile = FPaths::Combine(TestDirectory, TEXT("Activation.ini"));

	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*TestDirectory, false, true);
	};

	FMonolithActivationSnapshot Snapshot =
		FMonolithActivationState::LoadFromFileForTests(TestFile);
	TestTrue(TEXT("missing state defaults server to enabled"), Snapshot.bServerEnabled);
	TestTrue(TEXT("missing state defaults indexing to enabled"), Snapshot.bIndexingEnabled);

	FString Error;
	TestTrue(
		TEXT("server deactivation writes a new state file"),
		FMonolithActivationState::SetServerEnabledInFileForTests(TestFile, false, &Error));
	TestTrue(TEXT("server deactivation write error is empty"), Error.IsEmpty());

	Snapshot = FMonolithActivationState::LoadFromFileForTests(TestFile);
	TestFalse(TEXT("server deactivation persists"), Snapshot.bServerEnabled);
	TestTrue(TEXT("server write preserves default-on indexing state"), Snapshot.bIndexingEnabled);

	TestTrue(
		TEXT("indexing deactivation updates the same state file"),
		FMonolithActivationState::SetIndexingEnabledInFileForTests(TestFile, false, &Error));
	Snapshot = FMonolithActivationState::LoadFromFileForTests(TestFile);
	TestFalse(TEXT("indexing deactivation persists"), Snapshot.bIndexingEnabled);
	TestFalse(TEXT("indexing write preserves server state"), Snapshot.bServerEnabled);

	TestTrue(
		TEXT("server activation persists independently"),
		FMonolithActivationState::SetServerEnabledInFileForTests(TestFile, true, &Error));
	Snapshot = FMonolithActivationState::LoadFromFileForTests(TestFile);
	TestTrue(TEXT("server activation persists"), Snapshot.bServerEnabled);
	TestFalse(TEXT("server activation preserves indexing state"), Snapshot.bIndexingEnabled);

	const FString InvalidContents =
		TEXT("[Monolith.Activation]\n")
		TEXT("ServerEnabled=not-a-bool\n")
		TEXT("IndexingEnabled=True\n");
	TestTrue(
		TEXT("invalid-state fixture writes"),
		FFileHelper::SaveStringToFile(
			InvalidContents,
			*TestFile,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));

	AddExpectedError(
		TEXT("Monolith activation state contains invalid ServerEnabled"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	Snapshot = FMonolithActivationState::LoadFromFileForTests(TestFile);
	TestFalse(TEXT("malformed server value fails closed"), Snapshot.bServerEnabled);
	TestTrue(TEXT("valid indexing value still loads"), Snapshot.bIndexingEnabled);

	return true;
}

#endif
