#include "MonolithGameFeaturesModule.h"

#include "MonolithGameFeatureActions.h"
#include "MonolithSettings.h"
#include "MonolithToolRegistry.h"

DEFINE_LOG_CATEGORY(LogMonolithGameFeatures);

void FMonolithGameFeaturesModule::StartupModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	const bool bEnableInspectionActions = Settings && Settings->bEnableGameFeatureActions;

	FMonolithGameFeatureActions::Register(Registry, bEnableInspectionActions);

	const int32 ActionCount = Registry.GetActions(TEXT("gamefeatures")).Num();
	UE_LOG(LogMonolithGameFeatures, Log, TEXT("MonolithGameFeatures: Loaded (%d actions, inspection=%s)"),
		ActionCount,
		bEnableInspectionActions ? TEXT("enabled") : TEXT("disabled"));
}

void FMonolithGameFeaturesModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("gamefeatures"));
}

IMPLEMENT_MODULE(FMonolithGameFeaturesModule, MonolithGameFeatures)
