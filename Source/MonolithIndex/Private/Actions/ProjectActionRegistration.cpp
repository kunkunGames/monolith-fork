#include "Actions/ProjectActionRegistration.h"

#include "Actions/AssetCollectionActions.h"
#include "Actions/ProjectCleanupGeneratedAssetsAction.h"
#include "Actions/ProjectDetectChangesAction.h"
#include "Actions/ProjectDiffSnapshotsAction.h"
#include "Actions/ProjectExportAssetTextAction.h"
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
		RegisterProjectAction<FProjectExportAssetTextAction>(Registry);

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

		FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("project"), TEXT("search"),
			{ TEXT("find asset"), TEXT("locate asset"), TEXT("full text search"), TEXT("search assets and blueprints"), TEXT("where is"), TEXT("asset registry search") },
			{ TEXT("find_asset"), TEXT("asset_search"), TEXT("grep assets"), TEXT("lookup asset") },
			{ TEXT("find the BP_Player asset"), TEXT("search the project for Health"), TEXT("which assets mention attack") });
		FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("project"), TEXT("find_references"),
			{ TEXT("what uses this asset"), TEXT("dependencies"), TEXT("referencers"), TEXT("asset dependency graph"), TEXT("who depends on"), TEXT("reverse references") },
			{ TEXT("references"), TEXT("asset_dependencies"), TEXT("reference_viewer"), TEXT("find_dependencies") },
			{ TEXT("what assets reference BP_Enemy"), TEXT("find everything that uses this material"), TEXT("show dependencies of this mesh") });
		FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("project"), TEXT("find_by_type"),
			{ TEXT("list assets of class"), TEXT("all blueprints"), TEXT("all materials"), TEXT("assets by class"), TEXT("enumerate assets"), TEXT("filter by asset type") },
			{ TEXT("list_by_type"), TEXT("assets_of_type"), TEXT("by_class"), TEXT("find_assets_by_class") },
			{ TEXT("list all StaticMesh assets"), TEXT("show every Blueprint in the project"), TEXT("find all Material assets") });
		FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("project"), TEXT("get_asset_details"),
			{ TEXT("inspect asset"), TEXT("asset internals"), TEXT("nodes and variables"), TEXT("deep inspect"), TEXT("asset parameters"), TEXT("show asset contents") },
			{ TEXT("asset_details"), TEXT("describe_asset"), TEXT("inspect_asset"), TEXT("asset_info") },
			{ TEXT("show the details of BP_Player"), TEXT("what variables does this Blueprint have"), TEXT("inspect the nodes in this asset") });
		FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("project"), TEXT("find_unused"),
			{ TEXT("orphan assets"), TEXT("unreferenced assets"), TEXT("dead assets"), TEXT("cleanup candidates"), TEXT("unused content"), TEXT("assets nothing references") },
			{ TEXT("unused_assets"), TEXT("orphans"), TEXT("find_orphans"), TEXT("dead_assets") },
			{ TEXT("find assets nothing references"), TEXT("list orphaned content for cleanup"), TEXT("which assets are unused") });
		FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("project"), TEXT("list_gameplay_tags"),
			{ TEXT("all gameplay tags"), TEXT("tag list"), TEXT("GAS tags"), TEXT("gameplay tag hierarchy"), TEXT("enumerate tags"), TEXT("tags by prefix") },
			{ TEXT("gameplay_tags"), TEXT("list_tags"), TEXT("tag_dump"), TEXT("all_tags") },
			{ TEXT("list every gameplay tag"), TEXT("show gameplay tags under Weapon.Melee"), TEXT("what GAS tags exist") });
	}
}
