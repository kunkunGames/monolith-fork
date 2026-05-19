#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithLevelSequenceActions.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelSequenceSavedReplayRejectsUnsafePathTest, "Monolith.ParamGuard.MonolithLevelSequence.SavedReplayRejectsUnsafePath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelSequenceSavedReplayRejectsUnsafePathTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithLevelSequenceActions::RegisterActions(Registry);

	TestTrue(TEXT("level_sequence.get_saved_replay should be registered"), Registry.HasAction(TEXT("level_sequence"), TEXT("get_saved_replay")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("saved_relative_path"), TEXT("D:/OutsideProject/Demo.demo"));

	const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("level_sequence"), TEXT("get_saved_replay"), Params);
	TestFalse(TEXT("Absolute filesystem paths should be rejected"), Result.bSuccess);
	TestTrue(TEXT("Error should name the saved_relative_path guard"), Result.ErrorMessage.Contains(TEXT("saved_relative_path")));

	TSharedPtr<FJsonObject> TraversalParams = MakeShared<FJsonObject>();
	TraversalParams->SetStringField(TEXT("saved_relative_path"), TEXT("Demos/Foo/.."));

	const FMonolithActionResult TraversalResult = Registry.ExecuteAction(TEXT("level_sequence"), TEXT("get_saved_replay"), TraversalParams);
	TestFalse(TEXT("Trailing parent directory segments should be rejected before filesystem lookup"), TraversalResult.bSuccess);
	TestTrue(TEXT("Traversal error should name the saved_relative_path guard"), TraversalResult.ErrorMessage.Contains(TEXT("saved_relative_path")));

	TSharedPtr<FJsonObject> RootParams = MakeShared<FJsonObject>();
	RootParams->SetStringField(TEXT("saved_relative_path"), TEXT("Demos"));

	const FMonolithActionResult RootResult = Registry.ExecuteAction(TEXT("level_sequence"), TEXT("get_saved_replay"), RootParams);
	TestFalse(TEXT("Replay root status rows should not be accepted as a single saved replay"), RootResult.bSuccess);
	TestTrue(TEXT("Root error should name replay row contract"), RootResult.ErrorMessage.Contains(TEXT("replay row")));

	return true;
}
