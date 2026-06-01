#include "MonolithPoseSearchActions.h"
#include "MonolithAssetUtils.h"
#include "MonolithPackagePathValidator.h"
#include "MonolithParamSchema.h"

#include "PoseSearch/PoseSearchDatabase.h"
#include "PoseSearch/PoseSearchSchema.h"
#include "PoseSearch/PoseSearchFeatureChannel.h"
#include "PoseSearch/PoseSearchFeatureChannel_Position.h"
#include "PoseSearch/PoseSearchFeatureChannel_Velocity.h"
#include "PoseSearch/PoseSearchFeatureChannel_Heading.h"
#include "PoseSearch/PoseSearchFeatureChannel_Pose.h"
#include "PoseSearch/PoseSearchFeatureChannel_Trajectory.h"
#include "PoseSearch/PoseSearchFeatureChannel_Phase.h"
#include "PoseSearch/PoseSearchFeatureChannel_Distance.h"
#include "PoseSearch/PoseSearchFeatureChannel_Curve.h"
#include "PoseSearch/PoseSearchFeatureChannel_Group.h"
#include "PoseSearch/PoseSearchIndex.h"
#include "PoseSearch/PoseSearchDerivedData.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/UnrealType.h"
#include "Editor.h"
#include "Modules/ModuleManager.h"

namespace
{
	int32 ClampClothLimit(double LimitValue)
	{
		return FMath::Clamp(static_cast<int32>(LimitValue), 1, 500);
	}

	bool IsClothLikeAssetClass(const FAssetData& AssetData)
	{
		const FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();
		const FString ClassPath = AssetData.AssetClassPath.ToString();
		return ClassPath.Contains(TEXT("Cloth"))
			|| ClassPath.Contains(TEXT("Clothing"))
			|| ClassPath.Contains(TEXT("Outfit"))
			|| ClassName.Contains(TEXT("Cloth"))
			|| ClassName.Contains(TEXT("Clothing"))
			|| ClassName.Contains(TEXT("Outfit"));
	}

	TSharedPtr<FJsonObject> MakeClothModuleStatus(const TCHAR* ModuleName)
	{
		FModuleManager& ModuleManager = FModuleManager::Get();
		auto Status = MakeShared<FJsonObject>();
		Status->SetStringField(TEXT("name"), ModuleName);
		Status->SetBoolField(TEXT("exists"), ModuleManager.ModuleExists(ModuleName));
		Status->SetBoolField(TEXT("loaded"), ModuleManager.IsModuleLoaded(ModuleName));
		return Status;
	}
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void FMonolithPoseSearchActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("animation"), TEXT("get_pose_search_schema"),
		TEXT("Get PoseSearch schema configuration including skeleton, sample rate, and channels"),
		FMonolithActionHandler::CreateStatic(&HandleGetPoseSearchSchema),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PoseSearchSchema asset path"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("get_pose_search_database"),
		TEXT("Get PoseSearch database contents including schema reference and animation asset list"),
		FMonolithActionHandler::CreateStatic(&HandleGetPoseSearchDatabase),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PoseSearchDatabase asset path"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("add_database_sequence"),
		TEXT("Add an animation asset to a PoseSearch database"),
		FMonolithActionHandler::CreateStatic(&HandleAddDatabaseSequence),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PoseSearchDatabase asset path"))
			.RequiredAssetPath(TEXT("anim_path"), TEXT("Animation asset to add"))
			.Optional(TEXT("enabled"), TEXT("boolean"), TEXT("Enable for search (default true)"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("remove_database_sequence"),
		TEXT("Remove an animation asset from a PoseSearch database by index"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveDatabaseSequence),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PoseSearchDatabase asset path"))
			.Required(TEXT("sequence_index"), TEXT("integer"), TEXT("Index of the animation asset to remove"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("get_database_stats"),
		TEXT("Get PoseSearch database statistics including sequence count, schema, and search index info"),
		FMonolithActionHandler::CreateStatic(&HandleGetDatabaseStats),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PoseSearchDatabase asset path"))
			.Build());

	// Wave 11 — PoseSearch Creation
	Registry.RegisterAction(TEXT("animation"), TEXT("create_pose_search_schema"),
		TEXT("Create a new PoseSearch schema with skeleton and default channels"),
		FMonolithActionHandler::CreateStatic(&HandleCreatePoseSearchSchema),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path for the new schema (e.g. /Game/PoseSearch/PS_Schema)"))
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton asset path"))
			.Optional(TEXT("sample_rate"), TEXT("integer"), TEXT("Sample rate in Hz (default: 30)"))
			.Optional(TEXT("add_default_channels"), TEXT("boolean"), TEXT("Add default Pose+Trajectory channels (default: true)"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("create_pose_search_database"),
		TEXT("Create a new PoseSearch database and assign a schema"),
		FMonolithActionHandler::CreateStatic(&HandleCreatePoseSearchDatabase),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path for the new database"))
			.RequiredAssetPath(TEXT("schema_path"), TEXT("PoseSearchSchema asset path to assign"))
			.Build());

	// Wave 14 — PoseSearch Writes
	Registry.RegisterAction(TEXT("animation"), TEXT("set_database_sequence_properties"),
		TEXT("Set per-entry properties on a PoseSearch database animation asset (enabled, mirror, sampling range)"),
		FMonolithActionHandler::CreateStatic(&HandleSetDatabaseSequenceProperties),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PoseSearchDatabase asset path"))
			.Required(TEXT("sequence_index"), TEXT("integer"), TEXT("Index of the animation entry"))
			.Optional(TEXT("enabled"), TEXT("boolean"), TEXT("Enable/disable for search"))
			.Optional(TEXT("mirror_option"), TEXT("string"), TEXT("UnmirroredOnly, MirroredOnly, or UnmirroredAndMirrored"))
			.Optional(TEXT("sampling_range_start"), TEXT("number"), TEXT("Start of sampling range in seconds (0=full)"))
			.Optional(TEXT("sampling_range_end"), TEXT("number"), TEXT("End of sampling range in seconds (0=full)"))
			.Optional(TEXT("disable_reselection"), TEXT("boolean"), TEXT("Prevent reselection of poses from same asset"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("add_schema_channel"),
		TEXT("Add a feature channel to a PoseSearch schema (Position, Velocity, Heading, Pose, Trajectory, Phase, Distance, Curve, Group)"),
		FMonolithActionHandler::CreateStatic(&HandleAddSchemaChannel),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PoseSearchSchema asset path"))
			.Required(TEXT("channel_type"), TEXT("string"), TEXT("Channel type: Position, Velocity, Heading, Pose, Trajectory, Phase, Distance, Curve, Group"))
			.Optional(TEXT("weight"), TEXT("number"), TEXT("Channel weight (default 1.0)"))
			.Optional(TEXT("bone"), TEXT("string"), TEXT("Bone name for Position/Velocity/Heading channels"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("remove_schema_channel"),
		TEXT("Remove a feature channel from a PoseSearch schema by index (from authored channels)"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveSchemaChannel),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PoseSearchSchema asset path"))
			.Required(TEXT("channel_index"), TEXT("integer"), TEXT("Index into the authored Channels array"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("set_channel_weight"),
		TEXT("Set the weight on a PoseSearch schema channel by index"),
		FMonolithActionHandler::CreateStatic(&HandleSetChannelWeight),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PoseSearchSchema asset path"))
			.Required(TEXT("channel_index"), TEXT("integer"), TEXT("Index into the authored Channels array"))
			.Required(TEXT("weight"), TEXT("number"), TEXT("New weight value"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("rebuild_pose_search_index"),
		TEXT("Trigger async rebuild of a PoseSearch database search index"),
		FMonolithActionHandler::CreateStatic(&HandleRebuildPoseSearchIndex),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PoseSearchDatabase asset path"))
			.Optional(TEXT("wait"), TEXT("boolean"), TEXT("Block until rebuild completes (default false)"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("set_database_search_mode"),
		TEXT("Configure search algorithm and cost bias settings on a PoseSearch database"),
		FMonolithActionHandler::CreateStatic(&HandleSetDatabaseSearchMode),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PoseSearchDatabase asset path"))
			.Optional(TEXT("pose_search_mode"), TEXT("string"), TEXT("BruteForce, PCAKDTree, VPTree, or EventOnly"))
			.Optional(TEXT("kd_tree_query_num_neighbors"), TEXT("integer"), TEXT("Number of KNN neighbors (1-600, default 200)"))
			.Optional(TEXT("number_of_principal_components"), TEXT("integer"), TEXT("PCA dimensions (1-64, default 4)"))
			.Optional(TEXT("kd_tree_max_leaf_size"), TEXT("integer"), TEXT("KDTree max leaf size (1-256, default 16)"))
			.Optional(TEXT("continuing_pose_cost_bias"), TEXT("number"), TEXT("Cost bias for continuing current pose"))
			.Optional(TEXT("base_cost_bias"), TEXT("number"), TEXT("Base cost bias applied to all poses"))
			.Optional(TEXT("looping_cost_bias"), TEXT("number"), TEXT("Cost bias for looping animations"))
			.Optional(TEXT("continuing_interaction_cost_bias"), TEXT("number"), TEXT("Cost bias for continuing interaction poses"))
			.Build());

	Registry.RegisterAction(TEXT("cloth"), TEXT("get_status"),
		TEXT("Report read-only Chaos Cloth/Outfit workflow support without hard Chaos Outfit dependencies."),
		FMonolithActionHandler::CreateStatic(&HandleGetClothStatus),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("cloth"), TEXT("list_clothing_assets"),
		TEXT("List cloth/clothing/outfit-like assets under /Game using AssetRegistry metadata only. Does not load vertex data, weight maps, or mutate assets."),
		FMonolithActionHandler::CreateStatic(&HandleListClothingAssets),
		FParamSchemaBuilder()
			.Optional(TEXT("package_path"), TEXT("string"), TEXT("Content package path under /Game"), TEXT("/Game"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum assets to return, clamped to 1..500"), TEXT("100"))
			.Build());
}

// ---------------------------------------------------------------------------
// cloth.get_status / cloth.list_clothing_assets
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleGetClothStatus(const TSharedPtr<FJsonObject>& Params)
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("cloth"));
	Result->SetStringField(TEXT("domain"), TEXT("cloth_outfit_discovery"));
	Result->SetStringField(TEXT("mode"), TEXT("read_only"));
	Result->SetBoolField(TEXT("hard_dependency"), false);

	TArray<TSharedPtr<FJsonValue>> BaseModules;
	BaseModules.Add(MakeShared<FJsonValueObject>(MakeClothModuleStatus(TEXT("ChaosCloth"))));
	BaseModules.Add(MakeShared<FJsonValueObject>(MakeClothModuleStatus(TEXT("ChaosClothAsset"))));
	BaseModules.Add(MakeShared<FJsonValueObject>(MakeClothModuleStatus(TEXT("ChaosClothAssetEditor"))));
	BaseModules.Add(MakeShared<FJsonValueObject>(MakeClothModuleStatus(TEXT("ClothingSystemRuntimeCommon"))));
	Result->SetArrayField(TEXT("base_cloth_modules"), BaseModules);

	TArray<TSharedPtr<FJsonValue>> OutfitModules;
	OutfitModules.Add(MakeShared<FJsonValueObject>(MakeClothModuleStatus(TEXT("ChaosOutfit"))));
	OutfitModules.Add(MakeShared<FJsonValueObject>(MakeClothModuleStatus(TEXT("ChaosOutfitAsset"))));
	OutfitModules.Add(MakeShared<FJsonValueObject>(MakeClothModuleStatus(TEXT("ChaosOutfitAssetEditor"))));
	Result->SetArrayField(TEXT("outfit_modules"), OutfitModules);

	TArray<TSharedPtr<FJsonValue>> ImplementedActions;
	ImplementedActions.Add(MakeShared<FJsonValueString>(TEXT("cloth.get_status")));
	ImplementedActions.Add(MakeShared<FJsonValueString>(TEXT("cloth.list_clothing_assets")));
	Result->SetArrayField(TEXT("implemented_actions"), ImplementedActions);

	TArray<TSharedPtr<FJsonValue>> FutureActions;
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("cloth.list_clothing_bindings")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("cloth.get_clothing_sim_mesh_info")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("cloth.get_clothing_vertices")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("cloth.get_weight_map")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("cloth.set_weight_map")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("cloth.find_closest_outfit_size")));
	Result->SetArrayField(TEXT("future_optional_actions"), FutureActions);

	TArray<TSharedPtr<FJsonValue>> Notes;
	Notes.Add(MakeShared<FJsonValueString>(TEXT("This first milestone uses module reflection plus AssetRegistry metadata only; it does not load cloth assets, vertices, weight maps, or Chaos Outfit APIs.")));
	Notes.Add(MakeShared<FJsonValueString>(TEXT("Vertex/weight-map reads and all writes remain future work with paging, value validation, confirm=true, and dirty package reporting.")));
	Result->SetArrayField(TEXT("notes"), Notes);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithPoseSearchActions::HandleListClothingAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString PackagePath = TEXT("/Game");
	Params->TryGetStringField(TEXT("package_path"), PackagePath);
	if (!PackagePath.StartsWith(TEXT("/Game")))
	{
		return FMonolithActionResult::Error(TEXT("package_path must be under /Game"));
	}

	double LimitValue = 100.0;
	Params->TryGetNumberField(TEXT("limit"), LimitValue);
	const int32 Limit = ClampClothLimit(LimitValue);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*PackagePath));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 MatchedCount = 0;
	TMap<FString, int32> ClassCounts;

	for (const FAssetData& AssetData : Assets)
	{
		if (!IsClothLikeAssetClass(AssetData))
		{
			continue;
		}

		MatchedCount++;
		const FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();
		ClassCounts.FindOrAdd(ClassName)++;

		if (Rows.Num() >= Limit)
		{
			continue;
		}

		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("object_path"), AssetData.GetObjectPathString());
		Row->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
		Row->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
		Row->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		Row->SetStringField(TEXT("asset_class"), ClassName);
		Row->SetStringField(TEXT("asset_class_path"), AssetData.AssetClassPath.ToString());
		Row->SetBoolField(TEXT("loaded"), AssetData.IsAssetLoaded());
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	auto CountsJson = MakeShared<FJsonObject>();
	for (const TPair<FString, int32>& Pair : ClassCounts)
	{
		CountsJson->SetNumberField(Pair.Key, Pair.Value);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("cloth"));
	Result->SetStringField(TEXT("domain"), TEXT("cloth_outfit_discovery"));
	Result->SetStringField(TEXT("package_path"), PackagePath);
	Result->SetNumberField(TEXT("matched_count"), MatchedCount);
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetBoolField(TEXT("truncated"), MatchedCount > Rows.Num());
	Result->SetObjectField(TEXT("class_counts"), CountsJson);
	Result->SetArrayField(TEXT("assets"), Rows);
	return FMonolithActionResult::Success(Result);
}

// ---------------------------------------------------------------------------
// get_pose_search_schema
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleGetPoseSearchSchema(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)) return FMonolithActionResult::Error(TEXT("Parameter 'asset_path' must be a string"));

	UPoseSearchSchema* Schema = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchSchema>(AssetPath);
	if (!Schema)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchSchema not found: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("sample_rate"), Schema->SampleRate);

	// Skeletons
	const TArray<FPoseSearchRoledSkeleton>& RoledSkeletons = Schema->GetRoledSkeletons();
	if (RoledSkeletons.Num() > 0)
	{
		const FPoseSearchRoledSkeleton& DefaultSkeleton = RoledSkeletons[0];
		if (DefaultSkeleton.Skeleton)
		{
			Root->SetStringField(TEXT("skeleton"), DefaultSkeleton.Skeleton->GetPathName());
		}

		// All roled skeletons
		TArray<TSharedPtr<FJsonValue>> SkeletonArray;
		SkeletonArray.Reserve(RoledSkeletons.Num());
		for (int32 i = 0; i < RoledSkeletons.Num(); ++i)
		{
			TSharedPtr<FJsonObject> SkelObj = MakeShared<FJsonObject>();
			SkelObj->SetNumberField(TEXT("index"), i);
			SkelObj->SetStringField(TEXT("role"), RoledSkeletons[i].Role.ToString());
			if (RoledSkeletons[i].Skeleton)
			{
				SkelObj->SetStringField(TEXT("skeleton"), RoledSkeletons[i].Skeleton->GetPathName());
			}
			// MirrorDataTable omitted — forward-declared type, not worth including full header
			SkeletonArray.Add(MakeShared<FJsonValueObject>(SkelObj));
		}
		Root->SetArrayField(TEXT("skeletons"), SkeletonArray);
	}

	// Channels
	TConstArrayView<TObjectPtr<UPoseSearchFeatureChannel>> Channels = Schema->GetChannels();
	TArray<TSharedPtr<FJsonValue>> ChannelArray;
	ChannelArray.Reserve(Channels.Num());
	for (int32 i = 0; i < Channels.Num(); ++i)
	{
		const UPoseSearchFeatureChannel* Channel = Channels[i];
		if (!Channel) continue;

		TSharedPtr<FJsonObject> ChObj = MakeShared<FJsonObject>();
		ChObj->SetNumberField(TEXT("index"), i);
		ChObj->SetStringField(TEXT("type"), Channel->GetClass()->GetName());
		ChObj->SetNumberField(TEXT("cardinality"), Channel->GetChannelCardinality());
		ChObj->SetNumberField(TEXT("data_offset"), Channel->GetChannelDataOffset());
		ChannelArray.Add(MakeShared<FJsonValueObject>(ChObj));
	}
	Root->SetArrayField(TEXT("channels"), ChannelArray);
	Root->SetNumberField(TEXT("schema_cardinality"), Schema->SchemaCardinality);

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// get_pose_search_database
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleGetPoseSearchDatabase(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)) return FMonolithActionResult::Error(TEXT("Parameter 'asset_path' must be a string"));

	UPoseSearchDatabase* Database = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchDatabase>(AssetPath);
	if (!Database)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchDatabase not found: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	// Schema reference
	if (Database->Schema)
	{
		Root->SetStringField(TEXT("schema"), Database->Schema->GetPathName());
	}

	// Animation assets
	const int32 NumAssets = Database->GetNumAnimationAssets();
	Root->SetNumberField(TEXT("sequence_count"), NumAssets);

	TArray<TSharedPtr<FJsonValue>> SeqArray;
	SeqArray.Reserve(NumAssets);
	for (int32 i = 0; i < NumAssets; ++i)
	{
		const FPoseSearchDatabaseAnimationAsset* AnimAsset = Database->GetDatabaseAnimationAsset(i);
		if (!AnimAsset) continue;

		TSharedPtr<FJsonObject> SeqObj = MakeShared<FJsonObject>();
		SeqObj->SetNumberField(TEXT("index"), i);

		if (UObject* Asset = AnimAsset->GetAnimationAsset())
		{
			SeqObj->SetStringField(TEXT("animation"), Asset->GetPathName());
			SeqObj->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());
		}

#if WITH_EDITORONLY_DATA
		SeqObj->SetBoolField(TEXT("enabled"), AnimAsset->IsEnabled());
		FFloatInterval Range = AnimAsset->GetSamplingRange();
		SeqObj->SetNumberField(TEXT("sampling_range_start"), Range.Min);
		SeqObj->SetNumberField(TEXT("sampling_range_end"), Range.Max);
		SeqObj->SetStringField(TEXT("mirror_option"),
			AnimAsset->GetMirrorOption() == EPoseSearchMirrorOption::UnmirroredOnly ? TEXT("UnmirroredOnly") :
			AnimAsset->GetMirrorOption() == EPoseSearchMirrorOption::MirroredOnly ? TEXT("MirroredOnly") :
			TEXT("UnmirroredAndMirrored"));
#endif

		SeqArray.Add(MakeShared<FJsonValueObject>(SeqObj));
	}
	Root->SetArrayField(TEXT("sequences"), SeqArray);

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// add_database_sequence
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleAddDatabaseSequence(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)) return FMonolithActionResult::Error(TEXT("Parameter 'asset_path' must be a string"));
	FString AnimPath;
	if (!Params->TryGetStringField(TEXT("anim_path"), AnimPath)) return FMonolithActionResult::Error(TEXT("Parameter 'anim_path' must be a string"));
	bool bEnabled = true;
	Params->TryGetBoolField(TEXT("enabled"), bEnabled);

	UPoseSearchDatabase* Database = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchDatabase>(AssetPath);
	if (!Database)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchDatabase not found: %s"), *AssetPath));

	UObject* AnimAsset = FMonolithAssetUtils::LoadAssetByPath<UObject>(AnimPath);
	if (!AnimAsset)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Animation asset not found: %s"), *AnimPath));

	// Check if already in database
	if (Database->Contains(AnimAsset))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Animation already in database: %s"), *AnimPath));

	GEditor->BeginTransaction(FText::FromString(TEXT("Add PoseSearch Database Animation")));
	Database->Modify();

	FPoseSearchDatabaseAnimationAsset NewEntry;
	NewEntry.AnimAsset = AnimAsset;
#if WITH_EDITORONLY_DATA
	NewEntry.bEnabled = bEnabled;
#endif

	const int32 IndexBefore = Database->GetNumAnimationAssets();
	Database->AddAnimationAsset(NewEntry);

	GEditor->EndTransaction();
	Database->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("index"), IndexBefore);
	Root->SetStringField(TEXT("animation"), AnimAsset->GetPathName());
	Root->SetBoolField(TEXT("enabled"), bEnabled);
	Root->SetNumberField(TEXT("new_count"), Database->GetNumAnimationAssets());
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// remove_database_sequence
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleRemoveDatabaseSequence(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)) return FMonolithActionResult::Error(TEXT("Parameter 'asset_path' must be a string"));
	double TempSeqIdx;
	if (!Params->TryGetNumberField(TEXT("sequence_index"), TempSeqIdx)) return FMonolithActionResult::Error(TEXT("Parameter 'sequence_index' must be a number"));
	int32 SequenceIndex = static_cast<int32>(TempSeqIdx);

	UPoseSearchDatabase* Database = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchDatabase>(AssetPath);
	if (!Database)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchDatabase not found: %s"), *AssetPath));

	const int32 NumAssets = Database->GetNumAnimationAssets();
	if (SequenceIndex < 0 || SequenceIndex >= NumAssets)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid sequence index: %d (database has %d entries)"), SequenceIndex, NumAssets));

	// Capture info before removal
	FString RemovedAnimPath;
	if (const FPoseSearchDatabaseAnimationAsset* AnimAsset = Database->GetDatabaseAnimationAsset(SequenceIndex))
	{
		if (UObject* Asset = AnimAsset->GetAnimationAsset())
		{
			RemovedAnimPath = Asset->GetPathName();
		}
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Remove PoseSearch Database Animation")));
	Database->Modify();

	Database->RemoveAnimationAssetAt(SequenceIndex);

	GEditor->EndTransaction();
	Database->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("removed_index"), SequenceIndex);
	Root->SetStringField(TEXT("removed_animation"), RemovedAnimPath);
	Root->SetNumberField(TEXT("remaining_count"), Database->GetNumAnimationAssets());
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// get_database_stats
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleGetDatabaseStats(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)) return FMonolithActionResult::Error(TEXT("Parameter 'asset_path' must be a string"));

	UPoseSearchDatabase* Database = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchDatabase>(AssetPath);
	if (!Database)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchDatabase not found: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("sequence_count"), Database->GetNumAnimationAssets());

	// Schema
	if (Database->Schema)
	{
		Root->SetStringField(TEXT("schema"), Database->Schema->GetPathName());
		Root->SetNumberField(TEXT("sample_rate"), Database->Schema->SampleRate);
		Root->SetNumberField(TEXT("schema_cardinality"), Database->Schema->SchemaCardinality);
	}

	// Search index stats
	const UE::PoseSearch::FSearchIndex& SearchIndex = Database->GetSearchIndex();
	const int32 NumPoses = SearchIndex.GetNumPoses();
	Root->SetNumberField(TEXT("total_pose_count"), NumPoses);
	Root->SetBoolField(TEXT("is_valid"), NumPoses > 0);

	// Search mode
	FString SearchModeStr;
	switch (Database->PoseSearchMode)
	{
	case EPoseSearchMode::BruteForce: SearchModeStr = TEXT("BruteForce"); break;
	case EPoseSearchMode::PCAKDTree: SearchModeStr = TEXT("PCAKDTree"); break;
	case EPoseSearchMode::VPTree: SearchModeStr = TEXT("VPTree"); break;
	case EPoseSearchMode::EventOnly: SearchModeStr = TEXT("EventOnly"); break;
	default: SearchModeStr = TEXT("Unknown"); break;
	}
	Root->SetStringField(TEXT("search_mode"), SearchModeStr);

	// Cost biases
	Root->SetNumberField(TEXT("continuing_pose_cost_bias"), Database->ContinuingPoseCostBias);
	Root->SetNumberField(TEXT("base_cost_bias"), Database->BaseCostBias);
	Root->SetNumberField(TEXT("looping_cost_bias"), Database->LoopingCostBias);

	// Tags
	if (Database->Tags.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> TagArray;
		TagArray.Reserve(Database->Tags.Num());
		for (const FName& Tag : Database->Tags)
		{
			TagArray.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		}
		Root->SetArrayField(TEXT("tags"), TagArray);
	}

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// create_pose_search_schema
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleCreatePoseSearchSchema(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)) return FMonolithActionResult::Error(TEXT("Parameter 'asset_path' must be a string"));
	FString SkeletonPath;
	if (!Params->TryGetStringField(TEXT("skeleton_path"), SkeletonPath)) return FMonolithActionResult::Error(TEXT("Parameter 'skeleton_path' must be a string"));

	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(SkeletonPath);
	if (!Skeleton) return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton not found: %s"), *SkeletonPath));

	FString AssetName;
	int32 LastSlash;
	if (!AssetPath.FindLastChar('/', LastSlash) || LastSlash == AssetPath.Len() - 1)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid asset path: %s"), *AssetPath));
	AssetName = AssetPath.Mid(LastSlash + 1);

	if (FMonolithAssetUtils::LoadAssetByPath<UObject>(AssetPath))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));

	if (const FString ValidationError = MonolithCore::ValidatePackagePath(AssetPath); !ValidationError.IsEmpty())
	{
		return FMonolithActionResult::Error(ValidationError);
	}

	UPackage* Pkg = CreatePackage(*AssetPath);
	if (!Pkg) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at '%s'"), *AssetPath));

	UPoseSearchSchema* Schema = NewObject<UPoseSearchSchema>(Pkg, FName(*AssetName), RF_Public | RF_Standalone);
	if (!Schema) return FMonolithActionResult::Error(TEXT("Failed to create UPoseSearchSchema object"));

	// Add skeleton (public API)
	Schema->AddSkeleton(Skeleton);

	// Set sample rate if provided
	double SampleRateVal = 0.0;
	if (Params->TryGetNumberField(TEXT("sample_rate"), SampleRateVal))
	{
		Schema->SampleRate = static_cast<int32>(SampleRateVal);
	}

	// Add default channels (Pose + Trajectory) unless explicitly disabled
	bool bAddDefaults = true;
	Params->TryGetBoolField(TEXT("add_default_channels"), bAddDefaults);
	if (bAddDefaults)
	{
		Schema->AddDefaultChannels();
	}

	FAssetRegistryModule::AssetCreated(Schema);
	Pkg->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), Schema->GetPathName());
	Root->SetStringField(TEXT("asset_name"), AssetName);
	Root->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	Root->SetNumberField(TEXT("sample_rate"), Schema->SampleRate);
	Root->SetBoolField(TEXT("default_channels_added"), bAddDefaults);

	// Report channel count
	TConstArrayView<TObjectPtr<UPoseSearchFeatureChannel>> Channels = Schema->GetChannels();
	Root->SetNumberField(TEXT("channel_count"), Channels.Num());

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// create_pose_search_database
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleCreatePoseSearchDatabase(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)) return FMonolithActionResult::Error(TEXT("Parameter 'asset_path' must be a string"));
	FString SchemaPath;
	if (!Params->TryGetStringField(TEXT("schema_path"), SchemaPath)) return FMonolithActionResult::Error(TEXT("Parameter 'schema_path' must be a string"));

	UPoseSearchSchema* Schema = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchSchema>(SchemaPath);
	if (!Schema) return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchSchema not found: %s"), *SchemaPath));

	FString AssetName;
	int32 LastSlash;
	if (!AssetPath.FindLastChar('/', LastSlash) || LastSlash == AssetPath.Len() - 1)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid asset path: %s"), *AssetPath));
	AssetName = AssetPath.Mid(LastSlash + 1);

	if (FMonolithAssetUtils::LoadAssetByPath<UObject>(AssetPath))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));

	if (const FString ValidationError = MonolithCore::ValidatePackagePath(AssetPath); !ValidationError.IsEmpty())
	{
		return FMonolithActionResult::Error(ValidationError);
	}

	UPackage* Pkg = CreatePackage(*AssetPath);
	if (!Pkg) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at '%s'"), *AssetPath));

	UPoseSearchDatabase* Database = NewObject<UPoseSearchDatabase>(Pkg, FName(*AssetName), RF_Public | RF_Standalone);
	if (!Database) return FMonolithActionResult::Error(TEXT("Failed to create UPoseSearchDatabase object"));

	Database->Schema = Schema;

	FAssetRegistryModule::AssetCreated(Database);
	Pkg->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), Database->GetPathName());
	Root->SetStringField(TEXT("asset_name"), AssetName);
	Root->SetStringField(TEXT("schema"), Schema->GetPathName());
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Helpers — Schema channel reflection access
// ---------------------------------------------------------------------------

/** Access the private 'Channels' UPROPERTY on UPoseSearchSchema via reflection.
  * Returns nullptr if the property is not found (should never happen). */
static FArrayProperty* GetSchemaChannelsProperty()
{
	static FArrayProperty* CachedProp = nullptr;
	if (!CachedProp)
	{
		for (TFieldIterator<FArrayProperty> It(UPoseSearchSchema::StaticClass()); It; ++It)
		{
			if (It->GetName() == TEXT("Channels"))
			{
				CachedProp = *It;
				break;
			}
		}
	}
	return CachedProp;
}

/** Get a mutable pointer to the authored Channels TArray on a schema instance. */
static TArray<TObjectPtr<UPoseSearchFeatureChannel>>* GetSchemaChannelsMutable(UPoseSearchSchema* Schema)
{
	FArrayProperty* ArrayProp = GetSchemaChannelsProperty();
	if (!ArrayProp) return nullptr;
	return ArrayProp->ContainerPtrToValuePtr<TArray<TObjectPtr<UPoseSearchFeatureChannel>>>(Schema);
}

/** Trigger PostEditChangeProperty on the schema to call Finalize() internally. */
static void TriggerSchemaFinalize(UPoseSearchSchema* Schema)
{
#if WITH_EDITOR
	FPropertyChangedEvent Evt(GetSchemaChannelsProperty(), EPropertyChangeType::ValueSet);
	Schema->PostEditChangeProperty(Evt);
#endif
}

/** Map a channel type string to a UClass. Returns nullptr if unrecognized. */
static UClass* ResolveChannelClass(const FString& TypeStr)
{
	if (TypeStr.Equals(TEXT("Position"), ESearchCase::IgnoreCase)) return UPoseSearchFeatureChannel_Position::StaticClass();
	if (TypeStr.Equals(TEXT("Velocity"), ESearchCase::IgnoreCase)) return UPoseSearchFeatureChannel_Velocity::StaticClass();
	if (TypeStr.Equals(TEXT("Heading"), ESearchCase::IgnoreCase))  return UPoseSearchFeatureChannel_Heading::StaticClass();
	if (TypeStr.Equals(TEXT("Pose"), ESearchCase::IgnoreCase))     return UPoseSearchFeatureChannel_Pose::StaticClass();
	if (TypeStr.Equals(TEXT("Trajectory"), ESearchCase::IgnoreCase)) return UPoseSearchFeatureChannel_Trajectory::StaticClass();
	if (TypeStr.Equals(TEXT("Phase"), ESearchCase::IgnoreCase))    return UPoseSearchFeatureChannel_Phase::StaticClass();
	if (TypeStr.Equals(TEXT("Distance"), ESearchCase::IgnoreCase)) return UPoseSearchFeatureChannel_Distance::StaticClass();
	if (TypeStr.Equals(TEXT("Curve"), ESearchCase::IgnoreCase))    return UPoseSearchFeatureChannel_Curve::StaticClass();
	if (TypeStr.Equals(TEXT("Group"), ESearchCase::IgnoreCase))    return UPoseSearchFeatureChannel_Group::StaticClass();
	return nullptr;
}

// ---------------------------------------------------------------------------
// set_database_sequence_properties — Wave 14
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleSetDatabaseSequenceProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)) return FMonolithActionResult::Error(TEXT("Parameter 'asset_path' must be a string"));
	double TempSeqIdx;
	if (!Params->TryGetNumberField(TEXT("sequence_index"), TempSeqIdx)) return FMonolithActionResult::Error(TEXT("Parameter 'sequence_index' must be a number"));
	int32 SeqIndex = static_cast<int32>(TempSeqIdx);

	UPoseSearchDatabase* Database = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchDatabase>(AssetPath);
	if (!Database)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchDatabase not found: %s"), *AssetPath));

	const int32 NumAssets = Database->GetNumAnimationAssets();
	if (SeqIndex < 0 || SeqIndex >= NumAssets)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid sequence_index %d (database has %d entries)"), SeqIndex, NumAssets));

	FPoseSearchDatabaseAnimationAsset* Entry = Database->GetMutableDatabaseAnimationAsset(SeqIndex);
	if (!Entry)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to get mutable entry at index %d"), SeqIndex));

	GEditor->BeginTransaction(FText::FromString(TEXT("Set PoseSearch Database Sequence Properties")));
	Database->Modify();

#if WITH_EDITORONLY_DATA
	bool bEntryEnabled = false;
	if (Params->TryGetBoolField(TEXT("enabled"), bEntryEnabled))
	{
		Entry->SetIsEnabled(bEntryEnabled);
	}

	bool bDisableReselection = false;
	if (Params->TryGetBoolField(TEXT("disable_reselection"), bDisableReselection))
	{
		Entry->SetDisableReselection(bDisableReselection);
	}

	FString MirrorStr;
	if (Params->TryGetStringField(TEXT("mirror_option"), MirrorStr))
	{
		if (MirrorStr.Equals(TEXT("UnmirroredOnly"), ESearchCase::IgnoreCase))
			Entry->MirrorOption = EPoseSearchMirrorOption::UnmirroredOnly;
		else if (MirrorStr.Equals(TEXT("MirroredOnly"), ESearchCase::IgnoreCase))
			Entry->MirrorOption = EPoseSearchMirrorOption::MirroredOnly;
		else if (MirrorStr.Equals(TEXT("UnmirroredAndMirrored"), ESearchCase::IgnoreCase))
			Entry->MirrorOption = EPoseSearchMirrorOption::UnmirroredAndMirrored;
		else
		{
			GEditor->EndTransaction();
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid mirror_option: '%s'. Use UnmirroredOnly, MirroredOnly, or UnmirroredAndMirrored"), *MirrorStr));
		}
	}

	double RangeStartVal = 0.0;
	bool bHasStart = Params->TryGetNumberField(TEXT("sampling_range_start"), RangeStartVal);
	double RangeEndVal = 0.0;
	bool bHasEnd = Params->TryGetNumberField(TEXT("sampling_range_end"), RangeEndVal);

	if (bHasStart || bHasEnd)
	{
		FFloatInterval CurrentRange = Entry->GetSamplingRange();
		float Start = bHasStart ? static_cast<float>(RangeStartVal) : CurrentRange.Min;
		float End = bHasEnd ? static_cast<float>(RangeEndVal) : CurrentRange.Max;
		Entry->SetSamplingRange(FFloatInterval(Start, End));
	}
#endif // WITH_EDITORONLY_DATA

	GEditor->EndTransaction();
	Database->MarkPackageDirty();

	// Build response — read back current state
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("sequence_index"), SeqIndex);

	if (UObject* AnimAsset = Entry->GetAnimationAsset())
	{
		Root->SetStringField(TEXT("animation"), AnimAsset->GetPathName());
	}

#if WITH_EDITORONLY_DATA
	Root->SetBoolField(TEXT("enabled"), Entry->IsEnabled());
	Root->SetBoolField(TEXT("disable_reselection"), Entry->IsDisableReselection());

	FString MirrorOptionOut;
	switch (Entry->GetMirrorOption())
	{
	case EPoseSearchMirrorOption::UnmirroredOnly:      MirrorOptionOut = TEXT("UnmirroredOnly"); break;
	case EPoseSearchMirrorOption::MirroredOnly:        MirrorOptionOut = TEXT("MirroredOnly"); break;
	case EPoseSearchMirrorOption::UnmirroredAndMirrored: MirrorOptionOut = TEXT("UnmirroredAndMirrored"); break;
	default:                                           MirrorOptionOut = TEXT("Unknown"); break;
	}
	Root->SetStringField(TEXT("mirror_option"), MirrorOptionOut);

	FFloatInterval Range = Entry->GetSamplingRange();
	Root->SetNumberField(TEXT("sampling_range_start"), Range.Min);
	Root->SetNumberField(TEXT("sampling_range_end"), Range.Max);
#endif

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// add_schema_channel — Wave 14
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleAddSchemaChannel(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)) return FMonolithActionResult::Error(TEXT("Parameter 'asset_path' must be a string"));
	FString ChannelType;
	if (!Params->TryGetStringField(TEXT("channel_type"), ChannelType)) return FMonolithActionResult::Error(TEXT("Parameter 'channel_type' must be a string"));

	UPoseSearchSchema* Schema = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchSchema>(AssetPath);
	if (!Schema)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchSchema not found: %s"), *AssetPath));

	UClass* ChannelClass = ResolveChannelClass(ChannelType);
	if (!ChannelClass)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown channel type: '%s'. Supported: Position, Velocity, Heading, Pose, Trajectory, Phase, Distance, Curve, Group"), *ChannelType));

	GEditor->BeginTransaction(FText::FromString(TEXT("Add PoseSearch Schema Channel")));
	Schema->Modify();

	// Create the channel outered to the schema
	UPoseSearchFeatureChannel* Channel = NewObject<UPoseSearchFeatureChannel>(Schema, ChannelClass, NAME_None, RF_Transactional);
	if (!Channel)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create channel of type %s"), *ChannelType));
	}

	// Set weight if provided (Weight is WITH_EDITORONLY_DATA, exists on all channel types as a float UPROPERTY)
#if WITH_EDITORONLY_DATA
	double WeightVal = 0.0;
	if (Params->TryGetNumberField(TEXT("weight"), WeightVal))
	{
		float Weight = static_cast<float>(WeightVal);
		FProperty* WeightProp = Channel->GetClass()->FindPropertyByName(TEXT("Weight"));
		if (WeightProp)
		{
			void* WeightPtr = WeightProp->ContainerPtrToValuePtr<void>(Channel);
			FString WeightStr = FString::SanitizeFloat(Weight);
			WeightProp->ImportText_Direct(*WeightStr, WeightPtr, Channel, PPF_None);
		}
	}

	// Set bone reference if provided (for Position, Velocity, Heading channels)
	FString BoneName;
	if (Params->TryGetStringField(TEXT("bone"), BoneName))
	{
		FProperty* BoneProp = Channel->GetClass()->FindPropertyByName(TEXT("Bone"));
		if (BoneProp)
		{
			// FBoneReference is a struct with BoneName field — import using UE text format
			void* BonePtr = BoneProp->ContainerPtrToValuePtr<void>(Channel);
			FString BoneImportStr = FString::Printf(TEXT("(BoneName=\"%s\")"), *BoneName);
			BoneProp->ImportText_Direct(*BoneImportStr, BonePtr, Channel, PPF_None);
		}
	}
#endif // WITH_EDITORONLY_DATA

	// Use the public AddChannel API
	Schema->AddChannel(Channel);

	// Trigger Finalize() via PostEditChangeProperty
	TriggerSchemaFinalize(Schema);

	GEditor->EndTransaction();
	Schema->MarkPackageDirty();

	// Count authored channels for response
	TArray<TObjectPtr<UPoseSearchFeatureChannel>>* AuthoredChannels = GetSchemaChannelsMutable(Schema);
	int32 AuthoredCount = AuthoredChannels ? AuthoredChannels->Num() : -1;

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("channel_type"), ChannelType);
	Root->SetStringField(TEXT("channel_class"), Channel->GetClass()->GetName());
	Root->SetNumberField(TEXT("authored_channel_count"), AuthoredCount);
	Root->SetNumberField(TEXT("schema_cardinality"), Schema->SchemaCardinality);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// remove_schema_channel — Wave 14
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleRemoveSchemaChannel(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)) return FMonolithActionResult::Error(TEXT("Parameter 'asset_path' must be a string"));
	double TempChanIdx;
	if (!Params->TryGetNumberField(TEXT("channel_index"), TempChanIdx)) return FMonolithActionResult::Error(TEXT("Parameter 'channel_index' must be a number"));
	int32 ChannelIndex = static_cast<int32>(TempChanIdx);

	UPoseSearchSchema* Schema = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchSchema>(AssetPath);
	if (!Schema)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchSchema not found: %s"), *AssetPath));

	// Access the private Channels array via reflection
	TArray<TObjectPtr<UPoseSearchFeatureChannel>>* Channels = GetSchemaChannelsMutable(Schema);
	if (!Channels)
		return FMonolithActionResult::Error(TEXT("Failed to access Channels property on schema via reflection"));

	if (ChannelIndex < 0 || ChannelIndex >= Channels->Num())
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid channel_index %d (schema has %d authored channels)"), ChannelIndex, Channels->Num()));

	// Capture info before removal
	FString RemovedClass = (*Channels)[ChannelIndex] ? (*Channels)[ChannelIndex]->GetClass()->GetName() : TEXT("null");

	GEditor->BeginTransaction(FText::FromString(TEXT("Remove PoseSearch Schema Channel")));
	Schema->Modify();

	Channels->RemoveAt(ChannelIndex);

	// Trigger Finalize() via PostEditChangeProperty
	TriggerSchemaFinalize(Schema);

	GEditor->EndTransaction();
	Schema->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("removed_index"), ChannelIndex);
	Root->SetStringField(TEXT("removed_class"), RemovedClass);
	Root->SetNumberField(TEXT("remaining_channel_count"), Channels->Num());
	Root->SetNumberField(TEXT("schema_cardinality"), Schema->SchemaCardinality);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// set_channel_weight — Wave 14
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleSetChannelWeight(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)) return FMonolithActionResult::Error(TEXT("Parameter 'asset_path' must be a string"));
	double TempChanIdx;
	if (!Params->TryGetNumberField(TEXT("channel_index"), TempChanIdx)) return FMonolithActionResult::Error(TEXT("Parameter 'channel_index' must be a number"));
	int32 ChannelIndex = static_cast<int32>(TempChanIdx);
	double TempWeight;
	if (!Params->TryGetNumberField(TEXT("weight"), TempWeight)) return FMonolithActionResult::Error(TEXT("Parameter 'weight' must be a number"));
	float NewWeight = static_cast<float>(TempWeight);

	UPoseSearchSchema* Schema = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchSchema>(AssetPath);
	if (!Schema)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchSchema not found: %s"), *AssetPath));

	// Access authored channels via reflection
	TArray<TObjectPtr<UPoseSearchFeatureChannel>>* Channels = GetSchemaChannelsMutable(Schema);
	if (!Channels)
		return FMonolithActionResult::Error(TEXT("Failed to access Channels property on schema via reflection"));

	if (ChannelIndex < 0 || ChannelIndex >= Channels->Num())
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid channel_index %d (schema has %d authored channels)"), ChannelIndex, Channels->Num()));

	UPoseSearchFeatureChannel* Channel = (*Channels)[ChannelIndex];
	if (!Channel)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Channel at index %d is null"), ChannelIndex));

	// Weight is WITH_EDITORONLY_DATA — use reflection to set it on any channel type
#if WITH_EDITORONLY_DATA
	FProperty* WeightProp = Channel->GetClass()->FindPropertyByName(TEXT("Weight"));
	if (!WeightProp)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Channel class %s has no Weight property"), *Channel->GetClass()->GetName()));

	void* WeightPtr = WeightProp->ContainerPtrToValuePtr<void>(Channel);

	// Read old weight
	FString OldWeightStr;
	WeightProp->ExportText_Direct(OldWeightStr, WeightPtr, WeightPtr, Channel, PPF_None);

	GEditor->BeginTransaction(FText::FromString(TEXT("Set PoseSearch Channel Weight")));
	Schema->Modify();

	FString WeightStr = FString::SanitizeFloat(NewWeight);
	WeightProp->ImportText_Direct(*WeightStr, WeightPtr, Channel, PPF_None);

	// Trigger Finalize()
	TriggerSchemaFinalize(Schema);

	GEditor->EndTransaction();
	Schema->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("channel_index"), ChannelIndex);
	Root->SetStringField(TEXT("channel_class"), Channel->GetClass()->GetName());
	Root->SetStringField(TEXT("old_weight"), OldWeightStr);
	Root->SetNumberField(TEXT("new_weight"), NewWeight);
	return FMonolithActionResult::Success(Root);
#else
	return FMonolithActionResult::Error(TEXT("Channel weights are only available in editor builds (WITH_EDITORONLY_DATA)"));
#endif
}

// ---------------------------------------------------------------------------
// rebuild_pose_search_index — Wave 14
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleRebuildPoseSearchIndex(const TSharedPtr<FJsonObject>& Params)
{
#if WITH_EDITOR
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)) return FMonolithActionResult::Error(TEXT("Parameter 'asset_path' must be a string"));

	UPoseSearchDatabase* Database = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchDatabase>(AssetPath);
	if (!Database)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchDatabase not found: %s"), *AssetPath));

	bool bWait = false;
	Params->TryGetBoolField(TEXT("wait"), bWait);

	using namespace UE::PoseSearch;
	ERequestAsyncBuildFlag Flag = ERequestAsyncBuildFlag::NewRequest;
	if (bWait)
	{
		Flag |= ERequestAsyncBuildFlag::WaitForCompletion;
	}

	EAsyncBuildIndexResult Result = FAsyncPoseSearchDatabasesManagement::RequestAsyncBuildIndex(Database, Flag);

	FString ResultStr;
	switch (Result)
	{
	case EAsyncBuildIndexResult::InProgress: ResultStr = TEXT("InProgress"); break;
	case EAsyncBuildIndexResult::Success:    ResultStr = TEXT("Success"); break;
	case EAsyncBuildIndexResult::Failed:     ResultStr = TEXT("Failed"); break;
	default:                                 ResultStr = TEXT("Unknown"); break;
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("result"), ResultStr);
	Root->SetBoolField(TEXT("waited"), bWait);

	// Report current index stats
	const FSearchIndex& SearchIndex = Database->GetSearchIndex();
	Root->SetNumberField(TEXT("total_poses"), SearchIndex.GetNumPoses());

	return FMonolithActionResult::Success(Root);
#else
	return FMonolithActionResult::Error(TEXT("rebuild_pose_search_index is only available in editor builds"));
#endif
}

// ---------------------------------------------------------------------------
// set_database_search_mode — Wave 14
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleSetDatabaseSearchMode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)) return FMonolithActionResult::Error(TEXT("Parameter 'asset_path' must be a string"));

	UPoseSearchDatabase* Database = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchDatabase>(AssetPath);
	if (!Database)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchDatabase not found: %s"), *AssetPath));

	GEditor->BeginTransaction(FText::FromString(TEXT("Set PoseSearch Database Search Mode")));
	Database->Modify();

	// Search mode
	FString ModeStr;
	if (Params->TryGetStringField(TEXT("pose_search_mode"), ModeStr))
	{
		if (ModeStr.Equals(TEXT("BruteForce"), ESearchCase::IgnoreCase))
			Database->PoseSearchMode = EPoseSearchMode::BruteForce;
		else if (ModeStr.Equals(TEXT("PCAKDTree"), ESearchCase::IgnoreCase))
			Database->PoseSearchMode = EPoseSearchMode::PCAKDTree;
		else if (ModeStr.Equals(TEXT("VPTree"), ESearchCase::IgnoreCase))
			Database->PoseSearchMode = EPoseSearchMode::VPTree;
		else if (ModeStr.Equals(TEXT("EventOnly"), ESearchCase::IgnoreCase))
			Database->PoseSearchMode = EPoseSearchMode::EventOnly;
		else
		{
			GEditor->EndTransaction();
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid pose_search_mode: '%s'. Use BruteForce, PCAKDTree, VPTree, or EventOnly"), *ModeStr));
		}
	}

	// KDTree neighbors (not editor-only)
	double NeighborsVal = 0.0;
	if (Params->TryGetNumberField(TEXT("kd_tree_query_num_neighbors"), NeighborsVal))
	{
		Database->KDTreeQueryNumNeighbors = FMath::Clamp(
			static_cast<int32>(NeighborsVal), 1, 600);
	}

#if WITH_EDITORONLY_DATA
	// PCA components (editor-only)
	double ComponentsVal = 0.0;
	if (Params->TryGetNumberField(TEXT("number_of_principal_components"), ComponentsVal))
	{
		Database->NumberOfPrincipalComponents = FMath::Clamp(
			static_cast<int32>(ComponentsVal), 1, 64);
	}

	// KDTree max leaf size (editor-only)
	double LeafSizeVal = 0.0;
	if (Params->TryGetNumberField(TEXT("kd_tree_max_leaf_size"), LeafSizeVal))
	{
		Database->KDTreeMaxLeafSize = FMath::Clamp(
			static_cast<int32>(LeafSizeVal), 1, 256);
	}
#endif // WITH_EDITORONLY_DATA

	// Cost biases (runtime-accessible)
	double BiasVal1 = 0.0;
	if (Params->TryGetNumberField(TEXT("continuing_pose_cost_bias"), BiasVal1))
	{
		Database->ContinuingPoseCostBias = static_cast<float>(BiasVal1);
	}
	double BiasVal2 = 0.0;
	if (Params->TryGetNumberField(TEXT("base_cost_bias"), BiasVal2))
	{
		Database->BaseCostBias = static_cast<float>(BiasVal2);
	}
	double BiasVal3 = 0.0;
	if (Params->TryGetNumberField(TEXT("looping_cost_bias"), BiasVal3))
	{
		Database->LoopingCostBias = static_cast<float>(BiasVal3);
	}
	double BiasVal4 = 0.0;
	if (Params->TryGetNumberField(TEXT("continuing_interaction_cost_bias"), BiasVal4))
	{
		Database->ContinuingInteractionCostBias = static_cast<float>(BiasVal4);
	}

	GEditor->EndTransaction();
	Database->MarkPackageDirty();

	// Build response — echo back current values
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	FString ResponseModeStr;
	switch (Database->PoseSearchMode)
	{
	case EPoseSearchMode::BruteForce: ResponseModeStr = TEXT("BruteForce"); break;
	case EPoseSearchMode::PCAKDTree:  ResponseModeStr = TEXT("PCAKDTree"); break;
	case EPoseSearchMode::VPTree:     ResponseModeStr = TEXT("VPTree"); break;
	case EPoseSearchMode::EventOnly:  ResponseModeStr = TEXT("EventOnly"); break;
	default:                          ResponseModeStr = TEXT("Unknown"); break;
	}
	Root->SetStringField(TEXT("pose_search_mode"), ResponseModeStr);
	Root->SetNumberField(TEXT("kd_tree_query_num_neighbors"), Database->KDTreeQueryNumNeighbors);
#if WITH_EDITORONLY_DATA
	Root->SetNumberField(TEXT("number_of_principal_components"), Database->NumberOfPrincipalComponents);
	Root->SetNumberField(TEXT("kd_tree_max_leaf_size"), Database->KDTreeMaxLeafSize);
#endif
	Root->SetNumberField(TEXT("continuing_pose_cost_bias"), Database->ContinuingPoseCostBias);
	Root->SetNumberField(TEXT("base_cost_bias"), Database->BaseCostBias);
	Root->SetNumberField(TEXT("looping_cost_bias"), Database->LoopingCostBias);
	Root->SetNumberField(TEXT("continuing_interaction_cost_bias"), Database->ContinuingInteractionCostBias);

	return FMonolithActionResult::Success(Root);
}
