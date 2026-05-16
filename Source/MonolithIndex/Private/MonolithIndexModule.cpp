#include "MonolithIndexModule.h"
#include "MonolithIndexDatabase.h"
#include "MonolithToolRegistry.h"
#include "Actions/ProjectSearchAction.h"
#include "Actions/ProjectFindReferencesAction.h"
#include "Actions/ProjectFindByTypeAction.h"
#include "Actions/ProjectGetStatsAction.h"
#include "Actions/ProjectGetAssetDetailsAction.h"
#include "Actions/ProjectListGameplayTagsAction.h"
#include "Actions/ProjectSearchGameplayTagsAction.h"
#include "Actions/AssetCollectionActions.h"
#include "Actions/ProjectImpactRadiusAction.h"
#include "Actions/ProjectHealthAction.h"
#include "Actions/ProjectRepairFtsAction.h"
#include "Actions/ProjectRiskIndexAction.h"
#include "Actions/ProjectReviewContextAction.h"

#define LOCTEXT_NAMESPACE "FMonolithIndexModule"

void FMonolithIndexModule::StartupModule()
{
	UE_LOG(LogMonolithIndex, Verbose, TEXT("Monolith -- Index module loaded (12 actions, SQLite+FTS5)"));

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	Registry.RegisterAction(TEXT("project"), FProjectSearchAction::GetName(),
		FProjectSearchAction::GetDescription(),
		FMonolithActionHandler::CreateStatic(&FProjectSearchAction::Execute),
		FProjectSearchAction::GetSchema());

	Registry.RegisterAction(TEXT("project"), FProjectFindReferencesAction::GetName(),
		FProjectFindReferencesAction::GetDescription(),
		FMonolithActionHandler::CreateStatic(&FProjectFindReferencesAction::Execute),
		FProjectFindReferencesAction::GetSchema());

	Registry.RegisterAction(TEXT("project"), FProjectFindByTypeAction::GetName(),
		FProjectFindByTypeAction::GetDescription(),
		FMonolithActionHandler::CreateStatic(&FProjectFindByTypeAction::Execute),
		FProjectFindByTypeAction::GetSchema());

	Registry.RegisterAction(TEXT("project"), FProjectGetStatsAction::GetName(),
		FProjectGetStatsAction::GetDescription(),
		FMonolithActionHandler::CreateStatic(&FProjectGetStatsAction::Execute),
		FProjectGetStatsAction::GetSchema());

	Registry.RegisterAction(TEXT("project"), FProjectGetAssetDetailsAction::GetName(),
		FProjectGetAssetDetailsAction::GetDescription(),
		FMonolithActionHandler::CreateStatic(&FProjectGetAssetDetailsAction::Execute),
		FProjectGetAssetDetailsAction::GetSchema());

	Registry.RegisterAction(TEXT("project"), FProjectListGameplayTagsAction::GetName(),
		FProjectListGameplayTagsAction::GetDescription(),
		FMonolithActionHandler::CreateStatic(&FProjectListGameplayTagsAction::Execute),
		FProjectListGameplayTagsAction::GetSchema());

	Registry.RegisterAction(TEXT("project"), FProjectSearchGameplayTagsAction::GetName(),
		FProjectSearchGameplayTagsAction::GetDescription(),
		FMonolithActionHandler::CreateStatic(&FProjectSearchGameplayTagsAction::Execute),
		FProjectSearchGameplayTagsAction::GetSchema());

	FAssetCollectionActions::Register(Registry);

	// CRG-inspired navigation/review surface (additive; existing actions unchanged).
	Registry.RegisterAction(TEXT("project"), FProjectImpactRadiusAction::GetName(),
		FProjectImpactRadiusAction::GetDescription(),
		FMonolithActionHandler::CreateStatic(&FProjectImpactRadiusAction::Execute),
		FProjectImpactRadiusAction::GetSchema());

	Registry.RegisterAction(TEXT("project"), FProjectHealthAction::GetName(),
		FProjectHealthAction::GetDescription(),
		FMonolithActionHandler::CreateStatic(&FProjectHealthAction::Execute),
		FProjectHealthAction::GetSchema());

	Registry.RegisterAction(TEXT("project"), FProjectRepairFtsAction::GetName(),
		FProjectRepairFtsAction::GetDescription(),
		FMonolithActionHandler::CreateStatic(&FProjectRepairFtsAction::Execute),
		FProjectRepairFtsAction::GetSchema());

	Registry.RegisterAction(TEXT("project"), FProjectRiskIndexAction::GetName(),
		FProjectRiskIndexAction::GetDescription(),
		FMonolithActionHandler::CreateStatic(&FProjectRiskIndexAction::Execute),
		FProjectRiskIndexAction::GetSchema());

	Registry.RegisterAction(TEXT("project"), FProjectReviewContextAction::GetName(),
		FProjectReviewContextAction::GetDescription(),
		FMonolithActionHandler::CreateStatic(&FProjectReviewContextAction::Execute),
		FProjectReviewContextAction::GetSchema());
}

void FMonolithIndexModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("project"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("collection"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithIndexModule, MonolithIndex)
