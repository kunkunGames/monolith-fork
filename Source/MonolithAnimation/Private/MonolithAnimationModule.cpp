#include "MonolithAnimationModule.h"
#include "MonolithJsonUtils.h"
#include "MonolithAnimationActions.h"
#include "MonolithPoseSearchActions.h"
#include "MonolithControlRigWriteActions.h"
#include "MonolithAbpWriteActions.h"
#include "MonolithAnimLayoutActions.h"
#include "MonolithToolRegistry.h"

#define LOCTEXT_NAMESPACE "FMonolithAnimationModule"

void FMonolithAnimationModule::StartupModule()
{
	FMonolithAnimationActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithPoseSearchActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithControlRigWriteActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithAbpWriteActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithAnimLayoutActions::RegisterActions(FMonolithToolRegistry::Get());
	UE_LOG(LogMonolith, Verbose, TEXT("Monolith — Animation module loaded (90 actions)"));
}

void FMonolithAnimationModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("animation"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("cloth"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("metahuman"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithAnimationModule, MonolithAnimation)
