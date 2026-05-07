#include "MonolithLogicDriverModule.h"
#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"
#include "MonolithLogicDriverAssetActions.h"
#include "MonolithLogicDriverGraphActions.h"
#include "MonolithLogicDriverNodeActions.h"
#include "MonolithLogicDriverRuntimeActions.h"
#include "MonolithLogicDriverSpecActions.h"
#include "MonolithLogicDriverScaffoldActions.h"
#include "MonolithLogicDriverDiscoveryActions.h"
#include "MonolithLogicDriverComponentActions.h"
#include "MonolithLogicDriverTextGraphActions.h"
#if WITH_LOGICDRIVER
#include "MonolithLogicDriverIndexer.h"
#include "MonolithIndexSubsystem.h"
#endif
#include "Editor.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMonolithLogicDriver, Log, All);
DEFINE_LOG_CATEGORY(LogMonolithLogicDriver);

void FMonolithLogicDriverModule::StartupModule()
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (!Settings || !Settings->bEnableLogicDriver)
	{
		UE_LOG(LogMonolithLogicDriver, Log,
			TEXT("MonolithLogicDriver: LogicDriver integration disabled in settings"));
		return;
	}

#if WITH_LOGICDRIVER
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithLogicDriverAssetActions::RegisterActions(Registry);
	FMonolithLogicDriverGraphActions::RegisterActions(Registry);
	FMonolithLogicDriverNodeActions::RegisterActions(Registry);
	FMonolithLogicDriverRuntimeActions::RegisterActions(Registry);
	FMonolithLogicDriverSpecActions::RegisterActions(Registry);
	FMonolithLogicDriverScaffoldActions::RegisterActions(Registry);
	FMonolithLogicDriverDiscoveryActions::RegisterActions(Registry);
	FMonolithLogicDriverComponentActions::RegisterActions(Registry);
	FMonolithLogicDriverTextGraphActions::RegisterActions(Registry);

	PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddLambda([this]()
	{
		if (GEditor)
		{
			if (UMonolithIndexSubsystem* IndexSS = GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>())
			{
				IndexSS->RegisterIndexer(MakeShared<FStateMachineIndexer>());
				UE_LOG(LogMonolithLogicDriver, Log, TEXT("MonolithLogicDriver: Registered FStateMachineIndexer into MonolithIndex"));
			}
		}
	});

	int32 ActionCount = Registry.GetNamespaceActionCount(TEXT("logicdriver"));
	UE_LOG(LogMonolithLogicDriver, Log,
		TEXT("MonolithLogicDriver: Loaded (%d actions)"), ActionCount);
#else
	UE_LOG(LogMonolithLogicDriver, Log,
		TEXT("MonolithLogicDriver: Logic Driver Pro not found at compile time, bridge inactive"));
#endif
}

void FMonolithLogicDriverModule::ShutdownModule()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("logicdriver"));
}

IMPLEMENT_MODULE(FMonolithLogicDriverModule, MonolithLogicDriver)
