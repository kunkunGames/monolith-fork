#include "MonolithLevelSequenceModule.h"
#include "MonolithLevelSequenceActions.h"
#include "MonolithLevelSequenceIndexer.h"
#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"
#include "MonolithIndexSubsystem.h"
#include "Editor.h"
#include "Misc/CoreDelegates.h"

#define LOCTEXT_NAMESPACE "FMonolithLevelSequenceModule"

DEFINE_LOG_CATEGORY(LogMonolithLevelSequence);

namespace
{
	FSimpleMulticastDelegate& GetMonolithLevelSequencePostEngineInitDelegate()
	{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
		return FCoreDelegates::GetOnPostEngineInit();
#else
		return FCoreDelegates::OnPostEngineInit;
#endif
	}
}

void FMonolithLevelSequenceModule::StartupModule()
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (!Settings) return;

	if (Settings->bEnableLevelSequence)
	{
		FMonolithLevelSequenceActions::RegisterActions(FMonolithToolRegistry::Get());
		const int32 ActionCount = FMonolithToolRegistry::Get().GetActions(TEXT("level_sequence")).Num();
		UE_LOG(LogMonolithLevelSequence, Log, TEXT("MonolithLevelSequence: Loaded (%d actions)"), ActionCount);
	}

	if (Settings->bIndexLevelSequences)
	{
		PostEngineInitHandle = GetMonolithLevelSequencePostEngineInitDelegate().AddLambda([this]()
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
		GetMonolithLevelSequencePostEngineInitDelegate().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("level_sequence"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithLevelSequenceModule, MonolithLevelSequence)
