#include "MonolithAssetModule.h"

#include "MonolithAssetFindActions.h"
#include "MonolithAssetFontIngestActions.h"
#include "MonolithAssetHygieneActions.h"
#include "MonolithAssetInspectionActions.h"
#include "MonolithAssetLifecycleActions.h"
#include "MonolithAssetMoveActions.h"
#include "MonolithAssetPackageGraphActions.h"
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
	MonolithAsset::FTextureIngestActions::Register(Registry);
	MonolithAsset::FFontIngestActions::Register(Registry);
	FMonolithAssetLifecycleActions::RegisterActions(Registry);
	FMonolithAssetMoveActions::RegisterActions(Registry);
	FMonolithAssetHygieneActions::RegisterValidateNamingConventions(Registry);
	FMonolithAssetInspectionActions::RegisterActions(Registry);
	FMonolithAssetFindActions::RegisterActions(Registry);
	FMonolithAssetPackageGraphActions::RegisterActions(Registry);
	FMonolithAssetHygieneActions::RegisterBatchRenameAssets(Registry);

	UE_LOG(LogMonolithAsset, Log, TEXT("Monolith - Asset module loaded (%d asset actions)"),
		Registry.GetActions(TEXT("asset")).Num());
}

void FMonolithAssetModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("asset"));
}

IMPLEMENT_MODULE(FMonolithAssetModule, MonolithAsset)
