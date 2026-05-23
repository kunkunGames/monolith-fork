#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithMovieRenderQueueActions.h"
#include "MonolithTestSupport.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_MONOLITH_MRQ

#include "Editor.h"
#include "MoviePipelineExecutor.h"
#include "MoviePipelineQueue.h"
#include "MoviePipelineQueueSubsystem.h"

namespace
{
	constexpr TCHAR MovieRenderNamespace[] = TEXT("movie_render");

	class FScopedMRQSaveQueueFixture
	{
	public:
		bool SetUp(FString& OutError)
		{
			if (!GEditor)
			{
				OutError = TEXT("GEditor is not available");
				return false;
			}

			UMoviePipelineQueueSubsystem* Subsystem = GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>();
			if (!Subsystem)
			{
				OutError = TEXT("Movie Render Queue editor subsystem is not available");
				return false;
			}

			Queue = Subsystem->GetQueue();
			if (!Queue)
			{
				OutError = TEXT("Movie Render Queue is not available");
				return false;
			}

			Queue->Modify();
			Queue->DeleteAllJobs();

			UMoviePipelineExecutorJob* Job = Queue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass());
			if (!Job)
			{
				OutError = TEXT("Failed to allocate Movie Render Queue test job");
				return false;
			}

			return true;
		}

		~FScopedMRQSaveQueueFixture()
		{
			if (Queue)
			{
				Queue->Modify();
				Queue->DeleteAllJobs();
			}
		}

	private:
		UMoviePipelineQueue* Queue = nullptr;
	};

	FMonolithActionResult ExecuteMRQSecurityAction(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(MovieRenderNamespace, Action))
		{
			FMonolithMovieRenderQueueActions::RegisterActions(Registry);
		}

		return Registry.ExecuteAction(MovieRenderNamespace, Action, Params);
	}
}

// ---------------------------------------------------------------------------
// FMonolithMovieRenderQueueActions::SaveQueue (Validates ValidatePackagePath)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMRQSecuritySaveQueuePathTest, "Monolith.Security.LevelSequence.MovieRenderQueue.SaveQueue.RejectsMalformedPath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithMRQSecuritySaveQueuePathTest::RunTest(const FString& Parameters)
{
	FMonolithScopedTestNamespace ScopedNamespace(MovieRenderNamespace);
	FMonolithMovieRenderQueueActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("movie_render.save_queue is registered"), FMonolithToolRegistry::Get().HasAction(MovieRenderNamespace, TEXT("save_queue")));

	FScopedMRQSaveQueueFixture QueueFixture;
	FString SetupError;
	if (!QueueFixture.SetUp(SetupError))
	{
		AddError(FString::Printf(TEXT("MRQ SaveQueue path validation fixture setup failed: %s"), *SetupError));
		return false;
	}

	struct FMalformedPathCase
	{
		const TCHAR* Path;
		const TCHAR* ExpectedError;
	};

	const FMalformedPathCase MalformedPaths[] = {
		{ TEXT(""), TEXT("asset_path is required") },
		{ TEXT("//Game/Sequences/BadQueue"), TEXT("Invalid package path") },
		{ TEXT("/Game/Sequences/BadQueue/"), TEXT("Invalid package path") },
		{ TEXT("/Game/Sequences/BadQueue#Invalid"), TEXT("Invalid package path") },
		{ TEXT("../BadQueue"), TEXT("Invalid package path") },
	};

	for (const FMalformedPathCase& Case : MalformedPaths)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Case.Path);

		FMonolithActionResult Result = ExecuteMRQSecurityAction(TEXT("save_queue"), Params);

		TestFalse(*FString::Printf(TEXT("SaveQueue with malformed path '%s' should return Error"), Case.Path), Result.bSuccess);
		TestTrue(
			*FString::Printf(TEXT("SaveQueue malformed path '%s' should report '%s'"), Case.Path, Case.ExpectedError),
			Result.ErrorMessage.Contains(Case.ExpectedError));
		TestFalse(
			*FString::Printf(TEXT("SaveQueue malformed path '%s' should not fail on queue preconditions"), Case.Path),
			Result.ErrorMessage.Contains(TEXT("Movie Render Queue")) || Result.ErrorMessage.Contains(TEXT("empty Movie Render Queue")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_MONOLITH_MRQ
