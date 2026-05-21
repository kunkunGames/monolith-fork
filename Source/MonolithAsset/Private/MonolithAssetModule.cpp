#include "MonolithAssetModule.h"

#include "MonolithAssetFindActions.h"
#include "MonolithAssetFontIngestActions.h"
#include "MonolithAssetHygieneActions.h"
#include "MonolithAssetInspectionActions.h"
#include "MonolithAssetLifecycleActions.h"
#include "MonolithAssetTextureIngestActions.h"
#include "MonolithSettings.h"
#include "MonolithToolRegistry.h"

DEFINE_LOG_CATEGORY(LogMonolithAsset);

void FMonolithAssetModule::StartupModule()
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (!Settings || !Settings->bEnableAsset)
	{
		UE_LOG(LogMonolithAsset, Log, TEXT("Monolith - Asset module disabled via settings"));
		return;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Registry.RegisterOwnedActions(TEXT("MonolithAsset"), [](FMonolithToolRegistry& OwnedRegistry)
	{
		MonolithAsset::FTextureIngestActions::Register(OwnedRegistry);
		MonolithAsset::FFontIngestActions::Register(OwnedRegistry);
		FMonolithAssetLifecycleActions::RegisterActions(OwnedRegistry);
		FMonolithAssetHygieneActions::RegisterValidateNamingConventions(OwnedRegistry);
		FMonolithAssetInspectionActions::RegisterActions(OwnedRegistry);
		FMonolithAssetFindActions::RegisterActions(OwnedRegistry);
		FMonolithAssetHygieneActions::RegisterBatchRenameAssets(OwnedRegistry);
	});

	UE_LOG(LogMonolithAsset, Log, TEXT("Monolith - Asset module loaded (%d asset actions)"),
		Registry.GetNamespaceActionCount(TEXT("asset")));
}

void FMonolithAssetModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterOwner(TEXT("MonolithAsset"));
}

IMPLEMENT_MODULE(FMonolithAssetModule, MonolithAsset)
