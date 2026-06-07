#include "Actions/ProjectActionRegistration.h"

#include "Actions/AssetCollectionActions.h"
#include "Actions/ProjectCleanupGeneratedAssetsAction.h"
#include "Actions/ProjectDetectChangesAction.h"
#include "Actions/ProjectDiffSnapshotsAction.h"
#include "Actions/ProjectFindByTypeAction.h"
#include "Actions/ProjectFindReferencesAction.h"
#include "Actions/ProjectFindUnusedAction.h"
#include "Actions/ProjectGetAssetDetailsAction.h"
#include "Actions/ProjectGetSavedAssetStateAction.h"
#include "Actions/ProjectGetStatsAction.h"
#include "Actions/ProjectHealthAction.h"
#include "Actions/ProjectImpactRadiusAction.h"
#include "Actions/ProjectListGameplayTagsAction.h"
#include "Actions/ProjectPreMergeCheckAction.h"
#include "Actions/ProjectRefreshAssetsAction.h"
#include "Actions/ProjectRepairCrgCacheAction.h"
#include "Actions/ProjectRepairFtsAction.h"
#include "Actions/ProjectReviewContextAction.h"
#include "Actions/ProjectReviewHotspotsAction.h"
#include "Actions/ProjectRiskScoreAction.h"
#include "Actions/ProjectSearchAction.h"
#include "Actions/ProjectSearchGameplayTagsAction.h"
#include "Actions/ProjectSnapshotAction.h"
#include "MonolithToolRegistry.h"

namespace
{
	template <typename TAction>
	void RegisterProjectAction(FMonolithToolRegistry& Registry)
	{
		Registry.RegisterAction(
			TEXT("project"),
			TAction::GetName(),
			TAction::GetDescription(),
			FMonolithActionHandler::CreateStatic(&TAction::Execute),
			TAction::GetSchema());
	}
}

namespace MonolithIndex
{
	void FProjectActionRegistration::Register(FMonolithToolRegistry& Registry)
	{
		RegisterProjectAction<FProjectSearchAction>(Registry);
		RegisterProjectAction<FProjectFindReferencesAction>(Registry);
		RegisterProjectAction<FProjectFindByTypeAction>(Registry);
		RegisterProjectAction<FProjectGetStatsAction>(Registry);
		RegisterProjectAction<FProjectGetAssetDetailsAction>(Registry);
		RegisterProjectAction<FProjectRefreshAssetsAction>(Registry);
		RegisterProjectAction<FProjectGetSavedAssetStateAction>(Registry);
		RegisterProjectAction<FProjectCleanupGeneratedAssetsAction>(Registry);
		RegisterProjectAction<FProjectListGameplayTagsAction>(Registry);
		RegisterProjectAction<FProjectSearchGameplayTagsAction>(Registry);

		FAssetCollectionActions::Register(Registry);

		RegisterProjectAction<FProjectImpactRadiusAction>(Registry);
		RegisterProjectAction<FProjectHealthAction>(Registry);
		RegisterProjectAction<FProjectRepairFtsAction>(Registry);
		RegisterProjectAction<FProjectRepairCrgCacheAction>(Registry);
		RegisterProjectAction<FProjectRiskScoreAction>(Registry);
		RegisterProjectAction<FProjectDetectChangesAction>(Registry);
		RegisterProjectAction<FProjectFindUnusedAction>(Registry);
		RegisterProjectAction<FProjectPreMergeCheckAction>(Registry);
		RegisterProjectAction<FProjectSnapshotAction>(Registry);
		RegisterProjectAction<FProjectDiffSnapshotsAction>(Registry);
		RegisterProjectAction<FProjectReviewHotspotsAction>(Registry);
		RegisterProjectAction<FProjectReviewContextAction>(Registry);
	}
}
