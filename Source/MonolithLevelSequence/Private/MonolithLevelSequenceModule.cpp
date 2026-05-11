#include "MonolithLevelSequenceModule.h"
#include "MonolithLevelSequenceActions.h"
#include "MonolithLevelSequenceIndexer.h"
#include "MonolithMovieRenderQueueActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"
#include "MonolithIndexSubsystem.h"
#include "Editor.h"
#include "Misc/CoreDelegates.h"

#define LOCTEXT_NAMESPACE "FMonolithLevelSequenceModule"

DEFINE_LOG_CATEGORY(LogMonolithLevelSequence);

void FMonolithLevelSequenceModule::StartupModule()
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (!Settings) return;

	if (Settings->bEnableLevelSequence)
	{
		FMonolithLevelSequenceActions::RegisterActions(FMonolithToolRegistry::Get());
		FMonolithMovieRenderQueueActions::RegisterActions(FMonolithToolRegistry::Get());
		const int32 ActionCount = FMonolithToolRegistry::Get().GetNamespaceActionCount(TEXT("level_sequence"));
		const int32 MovieRenderActionCount = FMonolithToolRegistry::Get().GetNamespaceActionCount(TEXT("movie_render"));
		UE_LOG(LogMonolithLevelSequence, Log, TEXT("MonolithLevelSequence: Loaded (%d level_sequence actions, %d movie_render actions)"), ActionCount, MovieRenderActionCount);
	}

	if (Settings->bIndexLevelSequences)
	{
		PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddLambda([this]()
		{
			if (GEditor)
			{
				if (UMonolithIndexSubsystem* IndexSS = GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>())
				{
					IndexSS->RegisterIndexer(MakeShared<FLevelSequenceIndexer>());
					UE_LOG(LogMonolithLevelSequence, Log, TEXT("MonolithLevelSequence: Registered FLevelSequenceIndexer into MonolithIndex"));
				}
			}
		});
	}
}

void FMonolithLevelSequenceModule::ShutdownModule()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("level_sequence"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("movie_render"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithLevelSequenceModule, MonolithLevelSequence)
