#include "MonolithAbpWriteActions.h"
#include "MonolithAssetUtils.h"
#include "MonolithParamSchema.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/AnimationAsset.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_AssetPlayerBase.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimGraphNode_TwoWayBlend.h"
#include "AnimGraphNode_BlendListByBool.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_TwoBoneIK.h"
#include "AnimGraphNode_ModifyBone.h"
#include "AnimGraphNode_LocalToComponentSpace.h"
#include "AnimGraphNode_ComponentToLocalSpace.h"
#include "K2Node_VariableGet.h"
#include "BoneControllers/AnimNode_SkeletalControlBase.h"
#include "Engine/MemberReference.h"
#include "AnimationGraph.h"
#include "AnimationStateGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationGraphSchema.h"
#include "AnimStateNode.h"
#include "EdGraphSchema_K2_Actions.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectIterator.h"

// PoseSearchEditor module — provides UAnimGraphNode_MotionMatching
#include "AnimGraphNode_MotionMatching.h"
// PoseSearchEditor module — provides UAnimGraphNode_PoseSearchHistoryCollector (Sprint 4 MM graph)
#include "AnimGraphNode_PoseSearchHistoryCollector.h"
// AnimGraph module — provides UAnimGraphNode_Inertialization (Sprint 4 alias)
#include "AnimGraphNode_Inertialization.h"
// PoseSearch runtime — UPoseSearchDatabase (build_motion_matching_node Database write)
#include "PoseSearch/PoseSearchDatabase.h"
#include "Animation/BoneReference.h"
// BlendStackEditor module — UAnimGraphNode_BlendStack_Base (BoundGraph-node spawn fix)
#include "AnimGraphNode_BlendStack.h"
// FGraphNodeCreator — pristine node spawn for BoundGraph-owning nodes
#include "EdGraph/EdGraph.h"

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void FMonolithAbpWriteActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// --- add_anim_graph_node ---
	Registry.RegisterAction(TEXT("animation"), TEXT("add_anim_graph_node"),
		TEXT("Place an animation graph node in a state or the main AnimGraph. node_type accepts built-in aliases; node_class accepts any loaded non-abstract UAnimGraphNode_Base subclass by path or name."),
		FMonolithActionHandler::CreateStatic(&HandleAddAnimGraphNode),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Optional(TEXT("node_type"), TEXT("string"), TEXT("Alias or UAnimGraphNode_Base class path/name. Aliases: SequencePlayer, BlendSpacePlayer, TwoWayBlend, BlendListByBool, LayeredBoneBlend, MotionMatching, TwoBoneIK, ModifyBone, LocalToComponentSpace, ComponentToLocalSpace"))
			.Optional(TEXT("node_class"), TEXT("string"), TEXT("UAnimGraphNode_Base subclass path or name, e.g. /Script/AnimGraph.AnimGraphNode_TwoBoneIK. Use this instead of node_type when not using an alias."))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Target graph name — 'AnimGraph' for top-level, or a state name for state inner graphs (default: AnimGraph)"), TEXT("AnimGraph"))
			.Optional(TEXT("state_name"), TEXT("string"), TEXT("State name — if set, node is placed inside this state's inner graph (searched within the state machine found via graph_name if graph_name is a SM name, otherwise searches all SMs)"))
			.Optional(TEXT("position_x"), TEXT("number"), TEXT("Node X position (default: 200)"), TEXT("200"))
			.Optional(TEXT("position_y"), TEXT("number"), TEXT("Node Y position (default: 0)"), TEXT("0"))
			.Optional(TEXT("anim_asset"), TEXT("string"), TEXT("Animation/BlendSpace asset path — for SequencePlayer and BlendSpacePlayer nodes"))
			.Optional(TEXT("ik_bone"), TEXT("string"), TEXT("TwoBoneIK only: end-of-chain bone name (e.g. 'hand_l')"))
			.Optional(TEXT("effector_space"), TEXT("string"), TEXT("TwoBoneIK only: EffectorLocationSpace — WorldSpace, ComponentSpace (default), ParentBoneSpace, BoneSpace"))
			.Optional(TEXT("joint_target_space"), TEXT("string"), TEXT("TwoBoneIK only: JointTargetLocationSpace — WorldSpace, ComponentSpace (default), ParentBoneSpace, BoneSpace"))
			.Optional(TEXT("bone_to_modify"), TEXT("string"), TEXT("ModifyBone only: bone to modify (e.g. 'spine_01')"))
			.Optional(TEXT("expose_pins"), TEXT("array"), TEXT("Names of optional properties to expose as input pins (e.g. ['EffectorLocation','JointTargetLocation','Alpha']). TwoBoneIK exposes these three by default."))
			.Build());

	// --- connect_anim_graph_pins ---
	Registry.RegisterAction(TEXT("animation"), TEXT("connect_anim_graph_pins"),
		TEXT("Wire two node pins together in an ABP anim graph. Use after add_anim_graph_node to connect pose outputs to inputs."),
		FMonolithActionHandler::CreateStatic(&HandleConnectAnimGraphPins),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("source_node"), TEXT("string"), TEXT("Source node name (UObject name from add_anim_graph_node response, or class-based like AnimGraphNode_SequencePlayer_0)"))
			.Required(TEXT("source_pin"), TEXT("string"), TEXT("Source pin name, e.g. 'Pose' (output pin)"))
			.Required(TEXT("target_node"), TEXT("string"), TEXT("Target node name"))
			.Required(TEXT("target_pin"), TEXT("string"), TEXT("Target pin name, e.g. 'Result', 'A', 'B', 'BlendPose_0'"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph name to search in (default: searches all graphs)"))
			.Optional(TEXT("state_name"), TEXT("string"), TEXT("State name to search in — narrows to a specific state's inner graph"))
			.Optional(TEXT("compile"), TEXT("bool"), TEXT("Compile ABP after wiring (default: true)"), TEXT("true"))
			.Build());

	// --- set_state_animation ---
	Registry.RegisterAction(TEXT("animation"), TEXT("set_state_animation"),
		TEXT("High-level shortcut: set which animation a state plays by spawning the right player node and wiring it to the state result. Handles SequencePlayer vs BlendSpacePlayer automatically."),
		FMonolithActionHandler::CreateStatic(&HandleSetStateAnimation),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("machine_name"), TEXT("string"), TEXT("State machine name (as shown in get_state_machines)"))
			.Required(TEXT("state_name"), TEXT("string"), TEXT("State name to set animation for"))
			.RequiredAssetPath(TEXT("anim_asset_path"), TEXT("AnimSequence or BlendSpace asset path"))
			.Optional(TEXT("loop"), TEXT("bool"), TEXT("Set loop flag on the player node"), TEXT("false"))
			.Optional(TEXT("clear_existing"), TEXT("bool"), TEXT("Remove existing animation nodes wired to the state result (default: true)"), TEXT("true"))
			.Build());

	// --- add_variable_get ---
	Registry.RegisterAction(TEXT("animation"), TEXT("add_variable_get"),
		TEXT("Place a variable Get node (K2Node_VariableGet) in the AnimGraph — used to drive AnimGraph pins from AnimInstance members."),
		FMonolithActionHandler::CreateStatic(&HandleAddVariableGet),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("variable_name"), TEXT("string"), TEXT("Variable name as exposed on the AnimInstance (C++ UPROPERTY or BP variable)"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Target graph name (default: AnimGraph)"), TEXT("AnimGraph"))
			.Optional(TEXT("state_name"), TEXT("string"), TEXT("Optional state name to scope the search to a state inner graph"))
			.Optional(TEXT("position_x"), TEXT("number"), TEXT("Node X position (default: 0)"), TEXT("0"))
			.Optional(TEXT("position_y"), TEXT("number"), TEXT("Node Y position (default: 0)"), TEXT("0"))
			.Build());

	// --- set_anim_graph_node_property ---
	// Mutates a property on the *source* UAnimGraphNode's inner FAnimNode struct.
	// Writing via blueprint.set_cdo_property is wiped on compile because the AnimBP's
	// CDO is regenerated from the graph nodes — this action edits the authoritative
	// source so the change persists.
	Registry.RegisterAction(TEXT("animation"), TEXT("set_anim_graph_node_property"),
		TEXT("Mutate a property on an existing anim graph node's internal FAnimNode struct (e.g. ModifyBone.BoneToModify.BoneName, ModifyBone.RotationMode, TwoBoneIK.EffectorLocationSpace). Persists across compile — writes to the source UAnimGraphNode, not the CDO."),
		FMonolithActionHandler::CreateStatic(&HandleSetAnimGraphNodeProperty),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("Node UObject name (e.g. 'AnimGraphNode_ModifyBone_7') — same id surfaced by get_graph_summary / add_anim_graph_node response"))
			.Required(TEXT("property_path"), TEXT("string"), TEXT("Dotted property path inside the node's inner FAnimNode struct (e.g. 'BoneToModify.BoneName', 'RotationMode', 'EffectorLocationSpace', 'Alpha'). Do NOT prefix with 'Node.'."))
			.Required(TEXT("value"), TEXT("string"), TEXT("Value as text — same format as ImportText in the Details panel. Enums: bare name (e.g. 'BMM_Additive', 'BCS_ComponentSpace'). FName: bare name. Struct: '(Field=Value,...)'."))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph name to scope the search (default: searches all graphs)"))
			.Optional(TEXT("state_name"), TEXT("string"), TEXT("State name to narrow the search to a specific state's inner graph"))
			.Build());

	// --- configure_pose_history_node (Sprint 4.2) ---
	Registry.RegisterAction(TEXT("animation"), TEXT("configure_pose_history_node"),
		TEXT("Configure a Pose History (PoseSearchHistoryCollector) anim graph node's FAnimNode_PoseSearchHistoryCollector_Base properties for Motion Matching. Trajectory is generated via bGenerateTrajectory (UE 5.7 has no CharacterTrajectoryComponent)."),
		FMonolithActionHandler::CreateStatic(&FMonolithAbpWriteActions::HandleConfigurePoseHistoryNode),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("abp_path"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("Pose History node UObject name (from add_anim_graph_node / get_graph_summary)"))
			.Optional(TEXT("generate_trajectory"), TEXT("bool"), TEXT("bGenerateTrajectory — node generates trajectory from TrajectoryData instead of an input trajectory"))
			.Optional(TEXT("pose_count"), TEXT("number"), TEXT("PoseCount — max stored poses (ClampMin 2)"))
			.Optional(TEXT("sampling_interval"), TEXT("number"), TEXT("SamplingInterval — seconds between collected poses (0 = every update)"))
			.Optional(TEXT("collected_bones"), TEXT("array"), TEXT("CollectedBones — bone names to collect (written as FBoneReference array)"))
			.Optional(TEXT("trajectory_history_count"), TEXT("number"), TEXT("TrajectoryHistoryCount — past trajectory samples (ClampMin 2; used when generate_trajectory)"))
			.Optional(TEXT("trajectory_prediction_count"), TEXT("number"), TEXT("TrajectoryPredictionCount — future trajectory samples (ClampMin 2; used when generate_trajectory)"))
			.Build());

	// --- configure_motion_matching_node (Sprint 4.3) ---
	Registry.RegisterAction(TEXT("animation"), TEXT("configure_motion_matching_node"),
		TEXT("Configure a Motion Matching anim graph node's FAnimNode_MotionMatching + FAnimNode_BlendStack_Standalone base properties. PoseJumpThresholdTime is an FFloatInterval (min/max)."),
		FMonolithActionHandler::CreateStatic(&FMonolithAbpWriteActions::HandleConfigureMotionMatchingNode),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("abp_path"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("node_id"), TEXT("string"), TEXT("Motion Matching node UObject name (from add_anim_graph_node / get_graph_summary)"))
			.Optional(TEXT("blend_time"), TEXT("number"), TEXT("BlendTime — seconds to blend out to the new pose"))
			.Optional(TEXT("pose_jump_threshold_min"), TEXT("number"), TEXT("PoseJumpThresholdTime.Min (FFloatInterval)"))
			.Optional(TEXT("pose_jump_threshold_max"), TEXT("number"), TEXT("PoseJumpThresholdTime.Max (FFloatInterval)"))
			.Optional(TEXT("search_throttle"), TEXT("number"), TEXT("SearchThrottleTime — min seconds between searches"))
			.Optional(TEXT("use_inertial_blend"), TEXT("bool"), TEXT("bUseInertialBlend — requires an Inertialization node downstream"))
			.Optional(TEXT("should_filter_notifies"), TEXT("bool"), TEXT("bShouldFilterNotifies — on the BlendStack base struct"))
			.Optional(TEXT("notify_recency_timeout"), TEXT("number"), TEXT("NotifyRecencyTimeOut — on the BlendStack base struct"))
			.Optional(TEXT("max_active_blends"), TEXT("number"), TEXT("MaxActiveBlends — on the BlendStack base struct (0 = inertialization-only)"))
			.Build());

	// --- build_motion_matching_node (Sprint 4.4 — COMPOSITE) ---
	Registry.RegisterAction(TEXT("animation"), TEXT("build_motion_matching_node"),
		TEXT("Composite: spawn a Pose History + Motion Matching node in the AnimGraph, wire History pose-out -> MM pose-in, assign the MM Database, apply sensible MM/history defaults, and compile. Optionally set chooser-driven DB selection."),
		FMonolithActionHandler::CreateStatic(&FMonolithAbpWriteActions::HandleBuildMotionMatchingNode),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("abp_path"), TEXT("Animation Blueprint asset path"))
			.RequiredAssetPath(TEXT("database_path"), TEXT("UPoseSearchDatabase asset path assigned to the MM node's Database"))
			.Optional(TEXT("chooser_path"), TEXT("string"), TEXT("Optional UChooserTable asset path for chooser-driven database selection (best-effort; Database is always set as fallback)"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("get_anim_graph_output_connection"),
		TEXT("READ-ONLY: report whether the AnimGraph's Output Pose (UAnimGraphNode_Root 'Result' input) "
			 "is driven, and by which node/pin. Verifies the graph actually produces a final pose — the "
			 "check that would have caught an unwired Motion Matching graph."),
		FMonolithActionHandler::CreateStatic(&FMonolithAbpWriteActions::HandleGetAnimGraphOutputConnection),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("abp_path"), TEXT("Animation Blueprint asset path"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Graph to inspect (default the main AnimGraph)"), TEXT("AnimGraph"))
			.Build());

	// --- build_foot_ik_pass (Pack C — COMPOSITE) ---
	Registry.RegisterAction(TEXT("animation"), TEXT("build_foot_ik_pass"),
		TEXT("Composite: insert a lightweight foot IK pass (two TwoBoneIK nodes on the foot IK bones + a pelvis ModifyBone vertical offset) into the post-MM pose chain. Splices between whatever currently drives the Output Pose (e.g. Inertialization) and the Output Pose node. Foot IK alphas are gated by the contact_l/contact_r curves where supplied. Returns entry/exit node names for further splicing."),
		FMonolithActionHandler::CreateStatic(&FMonolithAbpWriteActions::HandleBuildFootIkPass),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("abp_path"), TEXT("Animation Blueprint asset path"))
			.Required(TEXT("left_foot_bone"), TEXT("string"), TEXT("Left foot IK end-of-chain bone (e.g. 'ik_foot_l')"))
			.Required(TEXT("right_foot_bone"), TEXT("string"), TEXT("Right foot IK end-of-chain bone (e.g. 'ik_foot_r')"))
			.Required(TEXT("pelvis_bone"), TEXT("string"), TEXT("Pelvis bone for the vertical (Z) root/pelvis offset (e.g. 'pelvis')"))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Target graph (default: AnimGraph)"), TEXT("AnimGraph"))
			.Optional(TEXT("left_contact_curve"), TEXT("string"), TEXT("Anim curve gating left-foot IK alpha (default: contact_l)"), TEXT("contact_l"))
			.Optional(TEXT("right_contact_curve"), TEXT("string"), TEXT("Anim curve gating right-foot IK alpha (default: contact_r)"), TEXT("contact_r"))
			.Build());

	// --- assign_post_process_anim_rig (Pack C) ---
	Registry.RegisterAction(TEXT("animation"), TEXT("assign_post_process_anim_rig"),
		TEXT("Set a skeletal mesh asset's PostProcessAnimBlueprint (post-process anim instance class) — runs after the main anim instance, before physics. Use to attach a post-process IK/Control-Rig ABP as an alternative to in-graph foot IK. Writes via USkeletalMesh::SetPostProcessAnimBlueprint."),
		FMonolithActionHandler::CreateStatic(&FMonolithAbpWriteActions::HandleAssignPostProcessAnimRig),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("mesh_path"), TEXT("USkeletalMesh asset path to assign the post-process ABP on"))
			.RequiredAssetPath(TEXT("post_process_abp_path"), TEXT("Post-process Animation Blueprint asset (its generated UAnimInstance class is assigned). Empty/None clears the assignment."))
			.Build());
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace
{

/** Map a user-facing node type alias to UClass. Returns nullptr on unknown type. */
UClass* ResolveNodeTypeAlias(const FString& NodeType)
{
	if (NodeType.Equals(TEXT("SequencePlayer"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_SequencePlayer::StaticClass();
	if (NodeType.Equals(TEXT("BlendSpacePlayer"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_BlendSpacePlayer::StaticClass();
	if (NodeType.Equals(TEXT("TwoWayBlend"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_TwoWayBlend::StaticClass();
	if (NodeType.Equals(TEXT("BlendListByBool"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_BlendListByBool::StaticClass();
	if (NodeType.Equals(TEXT("LayeredBoneBlend"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_LayeredBoneBlend::StaticClass();
	if (NodeType.Equals(TEXT("MotionMatching"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_MotionMatching::StaticClass();
	// Sprint 4 (4.1): PoseHistory node — UE 5.7 class is UAnimGraphNode_PoseSearchHistoryCollector,
	// NOT the old UAnimGraphNode_PoseHistory (gotcha #2).
	if (NodeType.Equals(TEXT("pose_history"), ESearchCase::IgnoreCase)
		|| NodeType.Equals(TEXT("PoseHistory"), ESearchCase::IgnoreCase)
		|| NodeType.Equals(TEXT("PoseSearchHistoryCollector"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_PoseSearchHistoryCollector::StaticClass();
	if (NodeType.Equals(TEXT("inertialization"), ESearchCase::IgnoreCase)
		|| NodeType.Equals(TEXT("Inertialization"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_Inertialization::StaticClass();
	if (NodeType.Equals(TEXT("TwoBoneIK"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_TwoBoneIK::StaticClass();
	if (NodeType.Equals(TEXT("ModifyBone"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_ModifyBone::StaticClass();
	if (NodeType.Equals(TEXT("LocalToComponentSpace"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_LocalToComponentSpace::StaticClass();
	if (NodeType.Equals(TEXT("ComponentToLocalSpace"), ESearchCase::IgnoreCase))
		return UAnimGraphNode_ComponentToLocalSpace::StaticClass();
	return nullptr;
}

FString CleanClassSpecifier(FString ClassSpecifier)
{
	ClassSpecifier.TrimStartAndEndInline();

	const TCHAR* Prefixes[] =
	{
		TEXT("Class'"),
		TEXT("BlueprintGeneratedClass'")
	};

	for (const TCHAR* Prefix : Prefixes)
	{
		if (ClassSpecifier.StartsWith(Prefix) && ClassSpecifier.EndsWith(TEXT("'")))
		{
			const int32 PrefixLen = FCString::Strlen(Prefix);
			ClassSpecifier = ClassSpecifier.Mid(PrefixLen, ClassSpecifier.Len() - PrefixLen - 1);
			ClassSpecifier.TrimStartAndEndInline();
			break;
		}
	}

	return ClassSpecifier;
}

void AddUniqueClassLookupCandidate(TArray<FString>& Candidates, const FString& Candidate)
{
	const FString CleanCandidate = CleanClassSpecifier(Candidate);
	if (!CleanCandidate.IsEmpty())
	{
		Candidates.AddUnique(CleanCandidate);
	}
}

TArray<FString> BuildClassLookupCandidates(const FString& RawClassSpecifier)
{
	TArray<FString> Candidates;
	const FString ClassSpecifier = CleanClassSpecifier(RawClassSpecifier);
	AddUniqueClassLookupCandidate(Candidates, ClassSpecifier);

	if (ClassSpecifier.StartsWith(TEXT("/Script/")))
	{
		int32 DotIndex = INDEX_NONE;
		if (ClassSpecifier.FindLastChar(TEXT('.'), DotIndex)
			&& DotIndex + 2 < ClassSpecifier.Len()
			&& ClassSpecifier[DotIndex + 1] == TEXT('U'))
		{
			AddUniqueClassLookupCandidate(Candidates, ClassSpecifier.Left(DotIndex + 1) + ClassSpecifier.Mid(DotIndex + 2));
		}
	}
	else if (!ClassSpecifier.Contains(TEXT("/")) && ClassSpecifier.Contains(TEXT(".")))
	{
		AddUniqueClassLookupCandidate(Candidates, TEXT("/Script/") + ClassSpecifier);
		int32 DotIndex = INDEX_NONE;
		if (ClassSpecifier.FindLastChar(TEXT('.'), DotIndex)
			&& DotIndex + 2 < ClassSpecifier.Len()
			&& ClassSpecifier[DotIndex + 1] == TEXT('U'))
		{
			AddUniqueClassLookupCandidate(Candidates, TEXT("/Script/") + ClassSpecifier.Left(DotIndex + 1) + ClassSpecifier.Mid(DotIndex + 2));
		}
	}
	else if (!ClassSpecifier.Contains(TEXT("/")) && !ClassSpecifier.Contains(TEXT(".")))
	{
		if ((ClassSpecifier.StartsWith(TEXT("U")) || ClassSpecifier.StartsWith(TEXT("A"))) && ClassSpecifier.Len() > 1)
		{
			AddUniqueClassLookupCandidate(Candidates, ClassSpecifier.Mid(1));
		}
		else
		{
			AddUniqueClassLookupCandidate(Candidates, TEXT("U") + ClassSpecifier);
			AddUniqueClassLookupCandidate(Candidates, TEXT("A") + ClassSpecifier);
		}
	}

	return Candidates;
}

void AddUniqueClassMatch(TArray<UClass*>& Matches, UClass* Match)
{
	if (Match)
	{
		Matches.AddUnique(Match);
	}
}

TArray<UClass*> FindLoadedClassesBySpecifier(const TArray<FString>& Candidates)
{
	TArray<UClass*> Matches;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* CandidateClass = *It;
		if (!CandidateClass)
		{
			continue;
		}

		const FString ClassName = CandidateClass->GetName();
		const FString PathName = CandidateClass->GetPathName();
		const FString FullName = CandidateClass->GetFullName();
		for (const FString& Candidate : Candidates)
		{
			if (ClassName.Equals(Candidate, ESearchCase::IgnoreCase)
				|| PathName.Equals(Candidate, ESearchCase::IgnoreCase)
				|| FullName.Equals(Candidate, ESearchCase::IgnoreCase))
			{
				AddUniqueClassMatch(Matches, CandidateClass);
			}
		}
	}

	return Matches;
}

TArray<UClass*> ResolveClassSpecifier(const FString& RawClassSpecifier)
{
	TArray<UClass*> Matches;
	const TArray<FString> Candidates = BuildClassLookupCandidates(RawClassSpecifier);

	for (const FString& Candidate : Candidates)
	{
		if (Candidate.Contains(TEXT("/")) || Candidate.Contains(TEXT(".")))
		{
			if (UClass* LoadedClass = LoadClass<UObject>(nullptr, *Candidate))
			{
				AddUniqueClassMatch(Matches, LoadedClass);
			}

			const FSoftClassPath SoftClassPath(Candidate);
			if (UClass* ResolvedClass = SoftClassPath.ResolveClass())
			{
				AddUniqueClassMatch(Matches, ResolvedClass);
			}
		}
	}

	if (Matches.Num() == 0)
	{
		Matches = FindLoadedClassesBySpecifier(Candidates);
	}

	return Matches;
}

FString DescribeClassMatches(const TArray<UClass*>& Matches)
{
	TArray<FString> Paths;
	for (const UClass* Match : Matches)
	{
		if (Match)
		{
			Paths.Add(Match->GetPathName());
		}
	}
	return FString::Join(Paths, TEXT(", "));
}

UClass* ResolveAnimGraphNodeClass(const FString& NodeType, const FString& NodeClassSpecifier, FString& OutError)
{
	const FString CleanNodeType = CleanClassSpecifier(NodeType);
	const FString CleanNodeClassSpecifier = CleanClassSpecifier(NodeClassSpecifier);

	if (!CleanNodeType.IsEmpty() && !CleanNodeClassSpecifier.IsEmpty())
	{
		OutError = TEXT("Specify either node_type or node_class, not both. node_type is for built-in aliases or legacy class strings; node_class is for explicit UAnimGraphNode_Base class paths/names.");
		return nullptr;
	}

	if (CleanNodeType.IsEmpty() && CleanNodeClassSpecifier.IsEmpty())
	{
		OutError = TEXT("Missing required parameter: provide node_type alias or node_class UAnimGraphNode_Base class path/name.");
		return nullptr;
	}

	const FString RequestedClass = !CleanNodeClassSpecifier.IsEmpty() ? CleanNodeClassSpecifier : CleanNodeType;
	UClass* NodeClass = CleanNodeClassSpecifier.IsEmpty() ? ResolveNodeTypeAlias(CleanNodeType) : nullptr;
	if (!NodeClass)
	{
		TArray<UClass*> ClassMatches = ResolveClassSpecifier(RequestedClass);
		TArray<UClass*> SpawnableMatches;
		for (UClass* ClassMatch : ClassMatches)
		{
			if (ClassMatch
				&& ClassMatch->IsChildOf(UAnimGraphNode_Base::StaticClass())
				&& !ClassMatch->HasAnyClassFlags(CLASS_Abstract))
			{
				SpawnableMatches.AddUnique(ClassMatch);
			}
		}

		if (SpawnableMatches.Num() > 1)
		{
			OutError = FString::Printf(
				TEXT("Ambiguous anim graph node class '%s'. Matches: %s. Use a full class path such as '/Script/Module.AnimGraphNode_Name'."),
				*RequestedClass,
				*DescribeClassMatches(SpawnableMatches));
			return nullptr;
		}

		if (SpawnableMatches.Num() == 1)
		{
			NodeClass = SpawnableMatches[0];
		}
		else if (ClassMatches.Num() == 1)
		{
			NodeClass = ClassMatches[0];
		}
	}

	if (!NodeClass)
	{
		OutError = FString::Printf(
			TEXT("Anim graph node class not found for '%s'. Use an alias [%s], a loaded class name like 'AnimGraphNode_TwoBoneIK', or a full class path like '/Script/AnimGraph.AnimGraphNode_TwoBoneIK'. If this is a plugin node, make sure its editor module is loaded."),
			*RequestedClass,
			TEXT("SequencePlayer, BlendSpacePlayer, TwoWayBlend, BlendListByBool, LayeredBoneBlend, MotionMatching, TwoBoneIK, ModifyBone, LocalToComponentSpace, ComponentToLocalSpace"));
		return nullptr;
	}

	if (!NodeClass->IsChildOf(UAnimGraphNode_Base::StaticClass()))
	{
		OutError = FString::Printf(
			TEXT("Resolved class '%s' (%s) is not a child of UAnimGraphNode_Base; add_anim_graph_node can only spawn editor AnimGraph node classes."),
			*NodeClass->GetName(),
			*NodeClass->GetPathName());
		return nullptr;
	}

	if (NodeClass->HasAnyClassFlags(CLASS_Abstract))
	{
		OutError = FString::Printf(
			TEXT("Resolved class '%s' (%s) is abstract and cannot be spawned. Use a concrete UAnimGraphNode_Base subclass."),
			*NodeClass->GetName(),
			*NodeClass->GetPathName());
		return nullptr;
	}

	return NodeClass;
}

/** Parse a bone-control-space string. Defaults to ComponentSpace when missing/unrecognized. */
EBoneControlSpace ParseBoneControlSpace(const FString& Str, EBoneControlSpace Default = BCS_ComponentSpace)
{
	if (Str.Equals(TEXT("WorldSpace"), ESearchCase::IgnoreCase))      return BCS_WorldSpace;
	if (Str.Equals(TEXT("ComponentSpace"), ESearchCase::IgnoreCase))  return BCS_ComponentSpace;
	if (Str.Equals(TEXT("ParentBoneSpace"), ESearchCase::IgnoreCase)) return BCS_ParentBoneSpace;
	if (Str.Equals(TEXT("BoneSpace"), ESearchCase::IgnoreCase))       return BCS_BoneSpace;
	return Default;
}

/** Find a state machine graph by its display title (same lookup as Wave 10 add_state_to_machine). */
UAnimationStateMachineGraph* FindSMGraphByName(UAnimBlueprint* ABP, const FString& MachineName)
{
	for (UEdGraph* Graph : ABP->FunctionGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UAnimGraphNode_StateMachine* SMNode = Cast<UAnimGraphNode_StateMachine>(Node);
			if (!SMNode) continue;

			FString SMTitle = SMNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			int32 NewlineIdx = INDEX_NONE;
			if (SMTitle.FindChar(TEXT('\n'), NewlineIdx))
			{
				SMTitle.LeftInline(NewlineIdx);
			}
			if (SMTitle == MachineName)
			{
				return Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
			}
		}
	}
	return nullptr;
}

/** Find a state node by name within a state machine graph. */
UAnimStateNode* FindStateByName(UAnimationStateMachineGraph* SMGraph, const FString& StateName)
{
	for (UEdGraphNode* Node : SMGraph->Nodes)
	{
		UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node);
		if (StateNode && StateNode->GetStateName() == StateName)
		{
			return StateNode;
		}
	}
	return nullptr;
}

/**
 * Resolve the target graph from graph_name and state_name parameters.
 * - If state_name is provided, searches all state machines for that state and returns its inner graph.
 * - If graph_name is "AnimGraph", returns the top-level AnimGraph.
 * - Otherwise treats graph_name as a state machine name and looks for state_name within it.
 */
UEdGraph* ResolveTargetGraph(UAnimBlueprint* ABP, const FString& GraphName, const FString& StateName, FString& OutError)
{
	// If state_name is specified, find the state and return its inner graph
	if (!StateName.IsEmpty())
	{
		// Search all state machines for this state
		for (UEdGraph* Graph : ABP->FunctionGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UAnimGraphNode_StateMachine* SMNode = Cast<UAnimGraphNode_StateMachine>(Node);
				if (!SMNode) continue;

				UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
				if (!SMGraph) continue;

				UAnimStateNode* StateNode = FindStateByName(SMGraph, StateName);
				if (StateNode)
				{
					UAnimationStateGraph* StateGraph = Cast<UAnimationStateGraph>(StateNode->BoundGraph);
					if (!StateGraph)
					{
						OutError = FString::Printf(TEXT("State '%s' has no inner animation graph (BoundGraph is null)"), *StateName);
						return nullptr;
					}
					return StateGraph;
				}
			}
		}
		OutError = FString::Printf(TEXT("State '%s' not found in any state machine"), *StateName);
		return nullptr;
	}

	// No state_name — use graph_name
	if (GraphName.Equals(TEXT("AnimGraph"), ESearchCase::IgnoreCase) || GraphName.IsEmpty())
	{
		// Find the main AnimGraph (first UAnimationGraph in FunctionGraphs)
		for (UEdGraph* Graph : ABP->FunctionGraphs)
		{
			if (UAnimationGraph* AG = Cast<UAnimationGraph>(Graph))
			{
				return AG;
			}
		}
		OutError = TEXT("No AnimGraph found in this Animation Blueprint");
		return nullptr;
	}

	// Treat graph_name as a named function graph
	for (UEdGraph* Graph : ABP->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == GraphName)
		{
			return Graph;
		}
	}
	OutError = FString::Printf(TEXT("Graph '%s' not found. Use 'AnimGraph' for the main graph, or provide state_name to target a state's inner graph."), *GraphName);
	return nullptr;
}

/** Find a node by UObject name across all graphs in an ABP, or within a specific graph. */
UEdGraphNode* FindNodeByName(UAnimBlueprint* ABP, const FString& NodeName, UEdGraph* InGraph = nullptr)
{
	auto SearchGraph = [&](UEdGraph* Graph) -> UEdGraphNode*
	{
		if (!Graph) return nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->GetName() == NodeName)
			{
				return Node;
			}
		}
		return nullptr;
	};

	if (InGraph)
	{
		return SearchGraph(InGraph);
	}

	// Search all function graphs and their subgraphs
	for (UEdGraph* Graph : ABP->FunctionGraphs)
	{
		if (UEdGraphNode* Found = SearchGraph(Graph))
			return Found;

		// Search inside state machine graphs
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UAnimGraphNode_StateMachine* SMNode = Cast<UAnimGraphNode_StateMachine>(Node);
			if (!SMNode) continue;

			UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
			if (!SMGraph) continue;

			if (UEdGraphNode* Found = SearchGraph(SMGraph))
				return Found;

			// Search inside each state's inner graph
			for (UEdGraphNode* SMChild : SMGraph->Nodes)
			{
				UAnimStateNode* StateNode = Cast<UAnimStateNode>(SMChild);
				if (!StateNode || !StateNode->BoundGraph) continue;

				if (UEdGraphNode* Found = SearchGraph(StateNode->BoundGraph))
					return Found;
			}
		}
	}
	return nullptr;
}

/** Build a JSON array describing a node's pins. */
TArray<TSharedPtr<FJsonValue>> BuildPinList(UEdGraphNode* Node)
{
	TArray<TSharedPtr<FJsonValue>> PinsArr;
	PinsArr.Reserve(Node->Pins.Num());
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin) continue;

		TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
		PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
		PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
		PinObj->SetBoolField(TEXT("is_pose"), UAnimationGraphSchema::IsPosePin(Pin->PinType));
		PinObj->SetBoolField(TEXT("is_connected"), Pin->LinkedTo.Num() > 0);
		PinObj->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());

		PinsArr.Add(MakeShared<FJsonValueObject>(PinObj));
	}
	return PinsArr;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Action: add_anim_graph_node
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAbpWriteActions::HandleAddAnimGraphNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)) return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	FString NodeType;
	Params->TryGetStringField(TEXT("node_type"), NodeType);
	FString NodeClassSpecifier;
	Params->TryGetStringField(TEXT("node_class"), NodeClassSpecifier);
	FString GraphName = TEXT("AnimGraph");
	Params->TryGetStringField(TEXT("graph_name"), GraphName);
	FString StateName = TEXT("");
	Params->TryGetStringField(TEXT("state_name"), StateName);
	FString AnimAsset = TEXT("");
	Params->TryGetStringField(TEXT("anim_asset"), AnimAsset);

	double TempVal;
	float PosX = 200.f;
	float PosY = 0.f;
	if (Params->TryGetNumberField(TEXT("position_x"), TempVal)) PosX = static_cast<float>(TempVal);
	if (Params->TryGetNumberField(TEXT("position_y"), TempVal)) PosY = static_cast<float>(TempVal);

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	// Resolve the node class
	FString ClassError;
	UClass* NodeClass = ResolveAnimGraphNodeClass(NodeType, NodeClassSpecifier, ClassError);
	if (!NodeClass)
	{
		return FMonolithActionResult::Error(ClassError);
	}

	// Resolve the target graph
	FString GraphError;
	UEdGraph* TargetGraph = ResolveTargetGraph(ABP, GraphName, StateName, GraphError);
	if (!TargetGraph) return FMonolithActionResult::Error(GraphError);

	// ---- BlendStack-derived nodes (MotionMatching, MotionMatchingInteraction) ----
	// These own a UPROPERTY BoundGraph and CreateGraph() does check(BoundGraph == nullptr)
	// inside PostPlacedNewNode (AnimGraphNode_BlendStack.cpp:261). The default template
	// path (NewObject template -> FEdGraphSchemaAction_K2NewNode::PerformAction) duplicates
	// the template via DuplicateObject, which copies the BoundGraph UPROPERTY, so the
	// duplicated node already carries a non-null BoundGraph and PostPlacedNewNode asserts.
	// Spawn these via FGraphNodeCreator instead: it builds a PRISTINE node (no template
	// duplication, BoundGraph stays null) and runs PostPlacedNewNode exactly once in
	// Finalize(). All other node classes keep the existing template/PerformAction path.
	if (NodeClass->IsChildOf(UAnimGraphNode_BlendStack_Base::StaticClass()))
	{
		GEditor->BeginTransaction(FText::FromString(TEXT("Add Anim Graph Node")));
		TargetGraph->Modify();

		FGraphNodeCreator<UAnimGraphNode_Base> Creator(*TargetGraph);
		UAnimGraphNode_Base* NewNode = Creator.CreateNode(/*bSelectNewNode=*/false, NodeClass);
		if (!NewNode)
		{
			GEditor->EndTransaction();
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("FGraphNodeCreator failed to create node for class '%s'"), *NodeClass->GetPathName()));
		}
		NewNode->NodePosX = static_cast<int32>(PosX);
		NewNode->NodePosY = static_cast<int32>(PosY);
		Creator.Finalize(); // runs PostPlacedNewNode (CreateGraph) once on the pristine node

		GEditor->EndTransaction();

		// Do NOT ReconstructNode() here — the node is already fully formed by Finalize().
		ABP->MarkPackageDirty();

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("node_name"), NewNode->GetName());
		Root->SetStringField(TEXT("node_class"), NewNode->GetClass()->GetName());
		Root->SetStringField(TEXT("node_class_path"), NewNode->GetClass()->GetPathName());
		Root->SetStringField(TEXT("node_guid"), NewNode->NodeGuid.ToString());
		Root->SetNumberField(TEXT("position_x"), NewNode->NodePosX);
		Root->SetNumberField(TEXT("position_y"), NewNode->NodePosY);
		Root->SetArrayField(TEXT("pins"), BuildPinList(NewNode));
		return FMonolithActionResult::Success(Root);
	}

	// Create the template node on the transient package (will be duplicated by PerformAction)
	UAnimGraphNode_Base* Template = Cast<UAnimGraphNode_Base>(NewObject<UObject>(GetTransientPackage(), NodeClass));
	if (!Template)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create node template for class '%s'"), *NodeClass->GetPathName()));
	}

	const UEdGraphSchema* TargetSchema = TargetGraph->GetSchema();
	if (!TargetSchema || !Template->CanCreateUnderSpecifiedSchema(TargetSchema))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Class '%s' cannot be created under target graph '%s' with schema '%s'. Use an AnimGraph or animation state graph."),
			*NodeClass->GetPathName(),
			*TargetGraph->GetName(),
			TargetSchema ? *TargetSchema->GetClass()->GetName() : TEXT("<null>")));
	}

	// Set animation asset before spawning (gets duplicated with the node)
	if (!AnimAsset.IsEmpty())
	{
		UAnimGraphNode_AssetPlayerBase* AssetPlayer = Cast<UAnimGraphNode_AssetPlayerBase>(Template);
		if (AssetPlayer)
		{
			UAnimationAsset* Asset = FMonolithAssetUtils::LoadAssetByPath<UAnimationAsset>(AnimAsset);
			if (!Asset)
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("Animation asset not found: %s"), *AnimAsset));
			}
			AssetPlayer->SetAnimationAsset(Asset);
		}
		else
		{
			// Non-asset-player node doesn't support anim_asset — just warn via log, don't fail
			UE_LOG(LogTemp, Warning, TEXT("Monolith: Node type '%s' does not support anim_asset parameter — ignored"), *NodeType);
		}
	}

	// Skeletal control node configuration (set on template before spawn)
	if (UAnimGraphNode_TwoBoneIK* IKTemplate = Cast<UAnimGraphNode_TwoBoneIK>(Template))
	{
		FString IKBone;
		if (Params->TryGetStringField(TEXT("ik_bone"), IKBone) && !IKBone.IsEmpty())
		{
			IKTemplate->Node.IKBone.BoneName = FName(*IKBone);
		}
		FString EffectorSpace;
		if (Params->TryGetStringField(TEXT("effector_space"), EffectorSpace))
		{
			IKTemplate->Node.EffectorLocationSpace = ParseBoneControlSpace(EffectorSpace);
		}
		FString JointTargetSpace;
		if (Params->TryGetStringField(TEXT("joint_target_space"), JointTargetSpace))
		{
			IKTemplate->Node.JointTargetLocationSpace = ParseBoneControlSpace(JointTargetSpace);
		}
	}
	else if (UAnimGraphNode_ModifyBone* ModifyTemplate = Cast<UAnimGraphNode_ModifyBone>(Template))
	{
		FString ModifyBoneName;
		if (Params->TryGetStringField(TEXT("bone_to_modify"), ModifyBoneName) && !ModifyBoneName.IsEmpty())
		{
			ModifyTemplate->Node.BoneToModify.BoneName = FName(*ModifyBoneName);
		}
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Add Anim Graph Node")));
	TargetGraph->Modify();

	// Spawn via FEdGraphSchemaAction_K2NewNode — same path as the editor
	FEdGraphSchemaAction_K2NewNode Action;
	Action.NodeTemplate = Template;
	UEdGraphNode* SpawnedNode = Action.PerformAction(TargetGraph, /*FromPin=*/nullptr, FVector2f(PosX, PosY), /*bSelectNewNode=*/false);

	GEditor->EndTransaction();

	if (!SpawnedNode)
	{
		return FMonolithActionResult::Error(TEXT("PerformAction failed — node was not spawned. Check that the target graph supports this node type."));
	}

	// Expose optional-pin properties (e.g. EffectorLocation, JointTargetLocation, Alpha on TwoBoneIK)
	{
		UAnimGraphNode_Base* SpawnedAnim = Cast<UAnimGraphNode_Base>(SpawnedNode);
		if (SpawnedAnim)
		{
			TArray<FName> PinsToExpose;

			const TArray<TSharedPtr<FJsonValue>>* ExposePinsArr = nullptr;
			if (Params->TryGetArrayField(TEXT("expose_pins"), ExposePinsArr) && ExposePinsArr)
			{
				for (const TSharedPtr<FJsonValue>& V : *ExposePinsArr)
				{
					if (V.IsValid()) PinsToExpose.AddUnique(FName(*V->AsString()));
				}
			}

			// TwoBoneIK defaults: auto-expose common input pins
			if (Cast<UAnimGraphNode_TwoBoneIK>(SpawnedAnim) && PinsToExpose.Num() == 0)
			{
				PinsToExpose.Add(TEXT("EffectorLocation"));
				PinsToExpose.Add(TEXT("JointTargetLocation"));
				PinsToExpose.Add(TEXT("Alpha"));
			}

			bool bAnyExposed = false;
			for (FOptionalPinFromProperty& OptPin : SpawnedAnim->ShowPinForProperties)
			{
				if (PinsToExpose.Contains(OptPin.PropertyName) && !OptPin.bShowPin)
				{
					OptPin.bShowPin = true;
					bAnyExposed = true;
				}
			}
			if (bAnyExposed)
			{
				SpawnedAnim->ReconstructNode();
			}
		}
	}

	// Do NOT compile here — caller should batch node adds then wire, then compile once.
	// Just mark dirty.
	ABP->MarkPackageDirty();

	// Build response
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("node_name"), SpawnedNode->GetName());
	Root->SetStringField(TEXT("node_class"), SpawnedNode->GetClass()->GetName());
	Root->SetStringField(TEXT("node_class_path"), SpawnedNode->GetClass()->GetPathName());
	Root->SetStringField(TEXT("node_guid"), SpawnedNode->NodeGuid.ToString());
	Root->SetNumberField(TEXT("position_x"), SpawnedNode->NodePosX);
	Root->SetNumberField(TEXT("position_y"), SpawnedNode->NodePosY);
	Root->SetArrayField(TEXT("pins"), BuildPinList(SpawnedNode));
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Action: connect_anim_graph_pins
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAbpWriteActions::HandleConnectAnimGraphPins(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)) return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	FString SourceNode;
	if (!Params->TryGetStringField(TEXT("source_node"), SourceNode)) return FMonolithActionResult::Error(TEXT("Missing required parameter: source_node"));
	FString SourcePin;
	if (!Params->TryGetStringField(TEXT("source_pin"), SourcePin)) return FMonolithActionResult::Error(TEXT("Missing required parameter: source_pin"));
	FString TargetNode;
	if (!Params->TryGetStringField(TEXT("target_node"), TargetNode)) return FMonolithActionResult::Error(TEXT("Missing required parameter: target_node"));
	FString TargetPin;
	if (!Params->TryGetStringField(TEXT("target_pin"), TargetPin)) return FMonolithActionResult::Error(TEXT("Missing required parameter: target_pin"));
	FString GraphName = TEXT("");
	Params->TryGetStringField(TEXT("graph_name"), GraphName);
	FString StateName = TEXT("");
	Params->TryGetStringField(TEXT("state_name"), StateName);

	bool bCompile = true;
	if (Params->HasField(TEXT("compile")))
	{
		if (!Params->TryGetBoolField(TEXT("compile"), bCompile))
		{
			return FMonolithActionResult::Error(TEXT("Parameter 'compile' must be a boolean"));
		}
	}

	if (SourceNode.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: source_node"));
	if (SourcePin.IsEmpty())  return FMonolithActionResult::Error(TEXT("Missing required parameter: source_pin"));
	if (TargetNode.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: target_node"));
	if (TargetPin.IsEmpty())  return FMonolithActionResult::Error(TEXT("Missing required parameter: target_pin"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	// Optionally resolve to a specific graph for scoping the search
	UEdGraph* ScopeGraph = nullptr;
	if (!StateName.IsEmpty() || (!GraphName.IsEmpty() && !GraphName.Equals(TEXT("AnimGraph"), ESearchCase::IgnoreCase)))
	{
		FString GraphError;
		ScopeGraph = ResolveTargetGraph(ABP, GraphName, StateName, GraphError);
		// If scope resolution fails, we still search globally as fallback
	}

	// Find source and target nodes
	UEdGraphNode* SrcNode = FindNodeByName(ABP, SourceNode, ScopeGraph);
	if (!SrcNode)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Source node '%s' not found in ABP"), *SourceNode));
	}

	UEdGraphNode* DstNode = FindNodeByName(ABP, TargetNode, ScopeGraph);
	if (!DstNode)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Target node '%s' not found in ABP"), *TargetNode));
	}

	// Find output pin on source
	UEdGraphPin* OutPin = SrcNode->FindPin(FName(*SourcePin), EGPD_Output);
	if (!OutPin)
	{
		// List available output pins for debugging
		FString AvailPins;
		for (UEdGraphPin* P : SrcNode->Pins)
		{
			if (P && P->Direction == EGPD_Output)
			{
				if (!AvailPins.IsEmpty()) AvailPins += TEXT(", ");
				AvailPins += P->PinName.ToString();
			}
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Output pin '%s' not found on node '%s'. Available output pins: [%s]"),
			*SourcePin, *SourceNode, *AvailPins));
	}

	// Find input pin on target
	UEdGraphPin* InPin = DstNode->FindPin(FName(*TargetPin), EGPD_Input);
	if (!InPin)
	{
		FString AvailPins;
		for (UEdGraphPin* P : DstNode->Pins)
		{
			if (P && P->Direction == EGPD_Input)
			{
				if (!AvailPins.IsEmpty()) AvailPins += TEXT(", ");
				AvailPins += P->PinName.ToString();
			}
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Input pin '%s' not found on node '%s'. Available input pins: [%s]"),
			*TargetPin, *TargetNode, *AvailPins));
	}

	// Verify both nodes are in the same graph
	if (SrcNode->GetGraph() != DstNode->GetGraph())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Source node '%s' and target node '%s' are in different graphs — connections must be within the same graph"),
			*SourceNode, *TargetNode));
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Connect Anim Graph Pins")));
	SrcNode->GetGraph()->Modify();

	// Use the graph's own schema for the connection (UAnimationGraphSchema or UAnimationStateGraphSchema)
	const UEdGraphSchema* Schema = SrcNode->GetGraph()->GetSchema();
	const bool bConnected = Schema->TryCreateConnection(OutPin, InPin);

	GEditor->EndTransaction();

	if (!bConnected)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("TryCreateConnection failed: '%s.%s' -> '%s.%s'. Pin types may be incompatible."),
			*SourceNode, *SourcePin, *TargetNode, *TargetPin));
	}

	if (bCompile)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ABP);
		FKismetEditorUtilities::CompileBlueprint(ABP);
	}

	ABP->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("source_node"), SourceNode);
	Root->SetStringField(TEXT("source_pin"), SourcePin);
	Root->SetStringField(TEXT("target_node"), TargetNode);
	Root->SetStringField(TEXT("target_pin"), TargetPin);
	Root->SetBoolField(TEXT("compiled"), bCompile);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Action: set_state_animation
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAbpWriteActions::HandleSetStateAnimation(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)) return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	FString MachineName;
	if (!Params->TryGetStringField(TEXT("machine_name"), MachineName)) return FMonolithActionResult::Error(TEXT("Missing required parameter: machine_name"));
	FString StateName;
	if (!Params->TryGetStringField(TEXT("state_name"), StateName)) return FMonolithActionResult::Error(TEXT("Missing required parameter: state_name"));
	FString AnimAssetPath;
	if (!Params->TryGetStringField(TEXT("anim_asset_path"), AnimAssetPath)) return FMonolithActionResult::Error(TEXT("Missing required parameter: anim_asset_path"));

	bool bLoop = false;
	if (Params->HasField(TEXT("loop")))
	{
		if (!Params->TryGetBoolField(TEXT("loop"), bLoop))
		{
			return FMonolithActionResult::Error(TEXT("Parameter 'loop' must be a boolean"));
		}
	}

	bool bClearExisting = true;
	if (Params->HasField(TEXT("clear_existing")))
	{
		if (!Params->TryGetBoolField(TEXT("clear_existing"), bClearExisting))
		{
			return FMonolithActionResult::Error(TEXT("Parameter 'clear_existing' must be a boolean"));
		}
	}

	if (MachineName.IsEmpty())  return FMonolithActionResult::Error(TEXT("Missing required parameter: machine_name"));
	if (StateName.IsEmpty())    return FMonolithActionResult::Error(TEXT("Missing required parameter: state_name"));
	if (AnimAssetPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: anim_asset_path"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	// Find the state machine and state
	UAnimationStateMachineGraph* SMGraph = FindSMGraphByName(ABP, MachineName);
	if (!SMGraph) return FMonolithActionResult::Error(FString::Printf(TEXT("State machine '%s' not found in ABP"), *MachineName));

	UAnimStateNode* StateNode = FindStateByName(SMGraph, StateName);
	if (!StateNode) return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' not found in machine '%s'"), *StateName, *MachineName));

	UAnimationStateGraph* StateGraph = Cast<UAnimationStateGraph>(StateNode->BoundGraph);
	if (!StateGraph) return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' has no inner animation graph"), *StateName));

	UAnimGraphNode_StateResult* ResultNode = StateGraph->MyResultNode;
	if (!ResultNode) return FMonolithActionResult::Error(FString::Printf(TEXT("State '%s' has no result node — state graph may be corrupt"), *StateName));

	// Load the animation asset
	UAnimationAsset* AnimAsset = FMonolithAssetUtils::LoadAssetByPath<UAnimationAsset>(AnimAssetPath);
	if (!AnimAsset) return FMonolithActionResult::Error(FString::Printf(TEXT("Animation asset not found: %s"), *AnimAssetPath));

	// Determine node class using the engine's own mapping
	UClass* NodeClass = GetNodeClassForAsset(AnimAsset->GetClass());
	if (!NodeClass)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No animation player node type for asset class '%s'. Supported: AnimSequence, BlendSpace."),
			*AnimAsset->GetClass()->GetName()));
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Set State Animation")));
	StateGraph->Modify();

	// Find the Result input pin
	UEdGraphPin* ResultInputPin = ResultNode->FindPin(TEXT("Result"), EGPD_Input);
	if (!ResultInputPin)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(TEXT("Could not find 'Result' input pin on state result node"));
	}

	// Optionally clear existing nodes wired to the result
	if (bClearExisting)
	{
		// Collect nodes currently connected to the result pin
		TArray<UEdGraphNode*> NodesToRemove;
		for (UEdGraphPin* LinkedPin : ResultInputPin->LinkedTo)
		{
			if (LinkedPin && LinkedPin->GetOwningNode())
			{
				NodesToRemove.Add(LinkedPin->GetOwningNode());
			}
		}

		// Break all connections to the result pin
		ResultInputPin->BreakAllPinLinks();

		// Remove the previously-wired nodes (but not the result node itself)
		for (UEdGraphNode* OldNode : NodesToRemove)
		{
			if (OldNode && OldNode != ResultNode)
			{
				OldNode->BreakAllNodeLinks();
				StateGraph->RemoveNode(OldNode);
			}
		}
	}

	// Create template node on transient package
	UAnimGraphNode_AssetPlayerBase* Template = Cast<UAnimGraphNode_AssetPlayerBase>(
		NewObject<UObject>(GetTransientPackage(), NodeClass));
	if (!Template)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(TEXT("Failed to create animation player node template"));
	}

	Template->SetAnimationAsset(AnimAsset);
	Template->CopySettingsFromAnimationAsset(AnimAsset);

	// Set loop flag if requested — need to access the FAnimNode struct via reflection
	if (bLoop)
	{
		// Try to set bLoopAnimation via the runtime node struct
		FStructProperty* NodeProp = Template->GetFNodeProperty();
		if (NodeProp)
		{
			void* NodePtr = NodeProp->ContainerPtrToValuePtr<void>(Template);
			FProperty* LoopProp = NodeProp->Struct->FindPropertyByName(FName(TEXT("bLoopAnimation")));
			if (LoopProp)
			{
				FBoolProperty* BoolProp = CastField<FBoolProperty>(LoopProp);
				if (BoolProp)
				{
					BoolProp->SetPropertyValue(BoolProp->ContainerPtrToValuePtr<void>(NodePtr), true);
				}
			}
		}
	}

	// Spawn into the state graph, positioned to the left of the result node
	float SpawnX = static_cast<float>(ResultNode->NodePosX - 300);
	float SpawnY = static_cast<float>(ResultNode->NodePosY);

	FEdGraphSchemaAction_K2NewNode Action;
	Action.NodeTemplate = Template;
	UEdGraphNode* SpawnedNode = Action.PerformAction(StateGraph, /*FromPin=*/nullptr, FVector2f(SpawnX, SpawnY), /*bSelectNewNode=*/false);

	if (!SpawnedNode)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(TEXT("PerformAction failed — animation player node was not spawned"));
	}

	// Wire the pose output to the result input
	UEdGraphPin* PoseOutput = SpawnedNode->FindPin(TEXT("Pose"), EGPD_Output);
	if (!PoseOutput)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(TEXT("Spawned node has no 'Pose' output pin — cannot wire to state result"));
	}

	const UEdGraphSchema* Schema = StateGraph->GetSchema();
	const bool bWired = Schema->TryCreateConnection(PoseOutput, ResultInputPin);

	GEditor->EndTransaction();

	if (!bWired)
	{
		return FMonolithActionResult::Error(TEXT("TryCreateConnection failed wiring Pose -> Result. The node was spawned but not connected."));
	}

	// Compile
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ABP);
	FKismetEditorUtilities::CompileBlueprint(ABP);
	ABP->MarkPackageDirty();

	// Build response
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("machine_name"), MachineName);
	Root->SetStringField(TEXT("state_name"), StateName);
	Root->SetStringField(TEXT("anim_asset_path"), AnimAssetPath);
	Root->SetStringField(TEXT("node_name"), SpawnedNode->GetName());
	Root->SetStringField(TEXT("node_class"), SpawnedNode->GetClass()->GetName());
	Root->SetBoolField(TEXT("loop"), bLoop);
	Root->SetBoolField(TEXT("cleared_existing"), bClearExisting);
	Root->SetArrayField(TEXT("pins"), BuildPinList(SpawnedNode));
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Action: add_variable_get
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAbpWriteActions::HandleAddVariableGet(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)) return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	FString VarName;
	if (!Params->TryGetStringField(TEXT("variable_name"), VarName)) return FMonolithActionResult::Error(TEXT("Missing required parameter: variable_name"));
	FString GraphName = TEXT("AnimGraph");
	Params->TryGetStringField(TEXT("graph_name"), GraphName);
	FString StateName = TEXT("");
	Params->TryGetStringField(TEXT("state_name"), StateName);

	double TempVal;
	float PosX = 0.f;
	float PosY = 0.f;
	if (Params->TryGetNumberField(TEXT("position_x"), TempVal)) PosX = static_cast<float>(TempVal);
	if (Params->TryGetNumberField(TEXT("position_y"), TempVal)) PosY = static_cast<float>(TempVal);

	if (VarName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: variable_name"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	FString GraphError;
	UEdGraph* TargetGraph = ResolveTargetGraph(ABP, GraphName, StateName, GraphError);
	if (!TargetGraph) return FMonolithActionResult::Error(GraphError);

	// Validate variable exists on skeleton class (BP-declared or C++ UPROPERTY)
	const FName VarFName(*VarName);
	UClass* SkeletonClass = ABP->SkeletonGeneratedClass ? ABP->SkeletonGeneratedClass : ABP->GeneratedClass;
	if (SkeletonClass && !SkeletonClass->FindPropertyByName(VarFName))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Variable '%s' not found on %s — check spelling and BlueprintReadOnly/ReadWrite on the UPROPERTY."),
			*VarName, *SkeletonClass->GetName()));
	}

	UK2Node_VariableGet* Template = NewObject<UK2Node_VariableGet>(GetTransientPackage());
	Template->VariableReference.SetSelfMember(VarFName);

	GEditor->BeginTransaction(FText::FromString(TEXT("Add Variable Get")));
	TargetGraph->Modify();

	FEdGraphSchemaAction_K2NewNode Action;
	Action.NodeTemplate = Template;
	UEdGraphNode* SpawnedNode = Action.PerformAction(TargetGraph, /*FromPin=*/nullptr, FVector2f(PosX, PosY), /*bSelectNewNode=*/false);

	GEditor->EndTransaction();

	if (!SpawnedNode)
	{
		return FMonolithActionResult::Error(TEXT("PerformAction failed for K2Node_VariableGet."));
	}

	ABP->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("node_name"), SpawnedNode->GetName());
	Root->SetStringField(TEXT("node_class"), SpawnedNode->GetClass()->GetName());
	Root->SetStringField(TEXT("node_guid"), SpawnedNode->NodeGuid.ToString());
	Root->SetStringField(TEXT("variable_name"), VarName);
	Root->SetNumberField(TEXT("position_x"), SpawnedNode->NodePosX);
	Root->SetNumberField(TEXT("position_y"), SpawnedNode->NodePosY);
	Root->SetArrayField(TEXT("pins"), BuildPinList(SpawnedNode));
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Action: set_anim_graph_node_property
// ---------------------------------------------------------------------------

namespace
{
/**
 * Resolve a dotted property path inside a container (struct value).
 * On success, `OutProperty` is the final property, `OutContainer` is the
 * address of the immediately-enclosing struct/object where that property lives.
 *
 *   path="BoneToModify.BoneName"  →  Prop=FNameProperty, Container=&Node.BoneToModify
 *   path="RotationMode"            →  Prop=FEnumProperty,  Container=&Node
 */
bool ResolvePropertyPath(UStruct* StructType, void* StructAddr, const FString& Path,
                         FProperty*& OutProperty, void*& OutContainer, FString& OutError)
{
	TArray<FString> Tokens;
	Path.ParseIntoArray(Tokens, TEXT("."));
	if (Tokens.Num() == 0)
	{
		OutError = TEXT("property_path is empty");
		return false;
	}

	UStruct* Cursor = StructType;
	void*    Addr   = StructAddr;

	for (int32 i = 0; i < Tokens.Num(); ++i)
	{
		const FString& Tok = Tokens[i];
		FProperty* Prop = Cursor ? Cursor->FindPropertyByName(FName(*Tok)) : nullptr;
		if (!Prop)
		{
			OutError = FString::Printf(TEXT("Property '%s' not found on %s"),
				*Tok, Cursor ? *Cursor->GetName() : TEXT("<null>"));
			return false;
		}

		if (i == Tokens.Num() - 1)
		{
			OutProperty  = Prop;
			OutContainer = Addr;
			return true;
		}

		// Descend into nested structs.
		FStructProperty* StructProp = CastField<FStructProperty>(Prop);
		if (!StructProp)
		{
			OutError = FString::Printf(TEXT("Cannot descend into '%s' — not a struct property"), *Tok);
			return false;
		}
		Cursor = StructProp->Struct;
		Addr   = StructProp->ContainerPtrToValuePtr<void>(Addr);
	}

	OutError = TEXT("unreachable");
	return false;
}

// ---------------------------------------------------------------------------
// Shared FAnimNode reflection helpers (Sprint 4 — reused by the MM graph
// configure/build handlers; mirror the resolution path in
// HandleSetAnimGraphNodeProperty).
// ---------------------------------------------------------------------------

/**
 * Resolve the inner FAnimNode struct (its UStruct + value address) on an
 * editor UAnimGraphNode_Base. Every UAnimGraphNode_X holds a UPROPERTY-tagged
 * FAnimNode_X member; we scan for the first FStructProperty derived from
 * FAnimNode_Base (same heuristic as HandleSetAnimGraphNodeProperty).
 */
bool ResolveInnerAnimNode(UAnimGraphNode_Base* AnimNode, UScriptStruct*& OutStruct,
                          void*& OutAddr, FString& OutError)
{
	OutStruct = nullptr;
	OutAddr = nullptr;
	for (TFieldIterator<FStructProperty> It(AnimNode->GetClass()); It; ++It)
	{
		FStructProperty* P = *It;
		if (!P || !P->Struct) continue;
		if (P->Struct->IsChildOf(FAnimNode_Base::StaticStruct()))
		{
			OutStruct = P->Struct;
			OutAddr   = P->ContainerPtrToValuePtr<void>(AnimNode);
			return true;
		}
	}
	OutError = FString::Printf(
		TEXT("Could not locate FAnimNode struct on '%s' — does the class inherit from FAnimNode_Base?"),
		*AnimNode->GetClass()->GetName());
	return false;
}

/** Write a named property on a struct via ImportText (the Details-panel parser). Searches superstructs. */
bool ImportTextOntoStruct(UScriptStruct* Struct, void* StructAddr, const FName& PropName,
                          const FString& Value, UObject* Owner, FString& OutError)
{
	FProperty* Prop = Struct ? Struct->FindPropertyByName(PropName) : nullptr;
	if (!Prop)
	{
		OutError = FString::Printf(TEXT("Property '%s' not found on %s"),
			*PropName.ToString(), Struct ? *Struct->GetName() : TEXT("<null>"));
		return false;
	}
	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(StructAddr);
	const TCHAR* Result = Prop->ImportText_Direct(*Value, ValuePtr, Owner, PPF_None);
	if (!Result)
	{
		OutError = FString::Printf(TEXT("ImportText failed for '%s' with value '%s'"),
			*PropName.ToString(), *Value);
		return false;
	}
	return true;
}

/**
 * Write a TArray<FBoneReference> property from an array of bone-name strings.
 * Builds the array element-by-element via reflection so we never depend on
 * the typed FBoneReference header layout beyond its `BoneName` FName field.
 */
bool WriteBoneReferenceArray(UScriptStruct* Struct, void* StructAddr, const FName& PropName,
                             const TArray<FString>& BoneNames, FString& OutError)
{
	FProperty* Prop = Struct ? Struct->FindPropertyByName(PropName) : nullptr;
	FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
	if (!ArrayProp)
	{
		OutError = FString::Printf(TEXT("Property '%s' is not an array property"), *PropName.ToString());
		return false;
	}
	FStructProperty* ElemStructProp = CastField<FStructProperty>(ArrayProp->Inner);
	if (!ElemStructProp || !ElemStructProp->Struct)
	{
		OutError = FString::Printf(TEXT("Array '%s' inner is not a struct"), *PropName.ToString());
		return false;
	}
	FProperty* BoneNameProp = ElemStructProp->Struct->FindPropertyByName(TEXT("BoneName"));
	FNameProperty* NameProp = CastField<FNameProperty>(BoneNameProp);
	if (!NameProp)
	{
		OutError = FString::Printf(TEXT("Element struct of '%s' has no FName 'BoneName' field"), *PropName.ToString());
		return false;
	}

	void* ArrayValuePtr = ArrayProp->ContainerPtrToValuePtr<void>(StructAddr);
	FScriptArrayHelper Helper(ArrayProp, ArrayValuePtr);
	Helper.EmptyValues();
	for (const FString& BoneName : BoneNames)
	{
		const int32 Index = Helper.AddValue();
		void* ElemPtr = Helper.GetRawPtr(Index);
		void* NamePtr = NameProp->ContainerPtrToValuePtr<void>(ElemPtr);
		NameProp->SetPropertyValue(NamePtr, FName(*BoneName));
	}
	return true;
}

} // anonymous namespace

FMonolithActionResult FMonolithAbpWriteActions::HandleSetAnimGraphNodeProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath)) return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	FString NodeId;
	if (!Params->TryGetStringField(TEXT("node_id"), NodeId)) return FMonolithActionResult::Error(TEXT("Missing required parameter: node_id"));
	FString PropertyPath;
	if (!Params->TryGetStringField(TEXT("property_path"), PropertyPath)) return FMonolithActionResult::Error(TEXT("Missing required parameter: property_path"));
	FString Value;
	if (!Params->TryGetStringField(TEXT("value"), Value)) return FMonolithActionResult::Error(TEXT("Missing required parameter: value"));
	FString GraphName = TEXT("");
	Params->TryGetStringField(TEXT("graph_name"), GraphName);
	FString StateName = TEXT("");
	Params->TryGetStringField(TEXT("state_name"), StateName);

	if (NodeId.IsEmpty())       return FMonolithActionResult::Error(TEXT("Missing required parameter: node_id"));
	if (PropertyPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: property_path"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AssetPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath));

	// Optional graph scope — same resolution as connect_anim_graph_pins.
	UEdGraph* ScopeGraph = nullptr;
	if (!StateName.IsEmpty() || (!GraphName.IsEmpty() && !GraphName.Equals(TEXT("AnimGraph"), ESearchCase::IgnoreCase)))
	{
		FString GraphError;
		ScopeGraph = ResolveTargetGraph(ABP, GraphName, StateName, GraphError);
	}

	UEdGraphNode* FoundNode = FindNodeByName(ABP, NodeId, ScopeGraph);
	if (!FoundNode)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Node '%s' not found"), *NodeId));
	}

	UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(FoundNode);
	if (!AnimNode)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Node '%s' is not a UAnimGraphNode_Base (class: %s) — this action only mutates anim graph nodes"),
			*NodeId, *FoundNode->GetClass()->GetName()));
	}

	// Find the inner `Node` FStructProperty on the UAnimGraphNode subclass. Every
	// UAnimGraphNode_X has a UPROPERTY-tagged FAnimNode_X field — conventionally
	// named "Node", but some subclasses rename it, so we scan for any FStructProperty
	// whose struct inherits from FAnimNode_Base.
	FStructProperty* NodeStructProp = nullptr;
	for (TFieldIterator<FStructProperty> It(AnimNode->GetClass()); It; ++It)
	{
		FStructProperty* P = *It;
		if (!P || !P->Struct) continue;
		// Heuristic: accept any struct derived from FAnimNode_Base.
		if (P->Struct->IsChildOf(FAnimNode_Base::StaticStruct()))
		{
			NodeStructProp = P;
			break;
		}
	}
	if (!NodeStructProp)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Could not locate FAnimNode struct on '%s' — does the class inherit from FAnimNode_Base?"),
			*AnimNode->GetClass()->GetName()));
	}

	void* NodeStructAddr = NodeStructProp->ContainerPtrToValuePtr<void>(AnimNode);

	FProperty* TargetProp   = nullptr;
	void*      TargetContainer = nullptr;
	FString    ResolveError;
	if (!ResolvePropertyPath(NodeStructProp->Struct, NodeStructAddr, PropertyPath, TargetProp, TargetContainer, ResolveError))
	{
		return FMonolithActionResult::Error(ResolveError);
	}

	// Capture old value for diff.
	FString OldValueText;
	TargetProp->ExportText_InContainer(0, OldValueText, TargetContainer, TargetContainer, nullptr, PPF_None);

	// Write via ImportText — same parser the Details panel uses.
	AnimNode->Modify();
	const TCHAR* Buffer = *Value;
	void* TargetValue = TargetProp->ContainerPtrToValuePtr<void>(TargetContainer);
	const TCHAR* ImportResult = TargetProp->ImportText_Direct(Buffer, TargetValue, AnimNode, PPF_None);
	if (!ImportResult)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("ImportText failed for property '%s' with value '%s'"),
			*PropertyPath, *Value));
	}

	FString NewValueText;
	TargetProp->ExportText_InContainer(0, NewValueText, TargetContainer, TargetContainer, nullptr, PPF_None);

	// Refresh the node's UI and notify the BP so the change isn't lost on next save.
	AnimNode->ReconstructNode();
	ABP->MarkPackageDirty();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ABP);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("node_id"), AnimNode->GetName());
	Root->SetStringField(TEXT("property_path"), PropertyPath);
	Root->SetStringField(TEXT("old_value"), OldValueText);
	Root->SetStringField(TEXT("new_value"), NewValueText);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Action: configure_pose_history_node (Sprint 4.2)
// Writes FAnimNode_PoseSearchHistoryCollector_Base properties by reflection.
// Trajectory comes from bGenerateTrajectory — there is NO CharacterTrajectoryComponent
// in UE 5.7 (gotcha #3).
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAbpWriteActions::HandleConfigurePoseHistoryNode(const TSharedPtr<FJsonObject>& Params)
{
	const FString AbpPath = Params->GetStringField(TEXT("abp_path"));
	const FString NodeId  = Params->GetStringField(TEXT("node_id"));
	if (NodeId.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: node_id"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AbpPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AbpPath));

	UEdGraphNode* FoundNode = FindNodeByName(ABP, NodeId, nullptr);
	if (!FoundNode) return FMonolithActionResult::Error(FString::Printf(TEXT("Node '%s' not found"), *NodeId));

	UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(FoundNode);
	if (!AnimNode) return FMonolithActionResult::Error(FString::Printf(
		TEXT("Node '%s' is not a UAnimGraphNode_Base (class: %s)"), *NodeId, *FoundNode->GetClass()->GetName()));

	UScriptStruct* NodeStruct = nullptr;
	void* NodeAddr = nullptr;
	FString ResolveError;
	if (!ResolveInnerAnimNode(AnimNode, NodeStruct, NodeAddr, ResolveError))
		return FMonolithActionResult::Error(ResolveError);

	GEditor->BeginTransaction(FText::FromString(TEXT("Configure Pose History Node")));
	AnimNode->Modify();

	TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
	FString WriteError;
	bool bAnyWrite = false;

	bool bGenTraj;
	if (Params->TryGetBoolField(TEXT("generate_trajectory"), bGenTraj))
	{
		if (!ImportTextOntoStruct(NodeStruct, NodeAddr, TEXT("bGenerateTrajectory"), bGenTraj ? TEXT("true") : TEXT("false"), AnimNode, WriteError))
		{ GEditor->EndTransaction(); return FMonolithActionResult::Error(WriteError); }
		Applied->SetBoolField(TEXT("bGenerateTrajectory"), bGenTraj); bAnyWrite = true;
	}

	double NumVal = 0.0;
	if (Params->TryGetNumberField(TEXT("pose_count"), NumVal))
	{
		if (!ImportTextOntoStruct(NodeStruct, NodeAddr, TEXT("PoseCount"), FString::FromInt(static_cast<int32>(NumVal)), AnimNode, WriteError))
		{ GEditor->EndTransaction(); return FMonolithActionResult::Error(WriteError); }
		Applied->SetNumberField(TEXT("PoseCount"), static_cast<int32>(NumVal)); bAnyWrite = true;
	}
	if (Params->TryGetNumberField(TEXT("sampling_interval"), NumVal))
	{
		if (!ImportTextOntoStruct(NodeStruct, NodeAddr, TEXT("SamplingInterval"), FString::SanitizeFloat(NumVal), AnimNode, WriteError))
		{ GEditor->EndTransaction(); return FMonolithActionResult::Error(WriteError); }
		Applied->SetNumberField(TEXT("SamplingInterval"), NumVal); bAnyWrite = true;
	}
	if (Params->TryGetNumberField(TEXT("trajectory_history_count"), NumVal))
	{
		if (!ImportTextOntoStruct(NodeStruct, NodeAddr, TEXT("TrajectoryHistoryCount"), FString::FromInt(static_cast<int32>(NumVal)), AnimNode, WriteError))
		{ GEditor->EndTransaction(); return FMonolithActionResult::Error(WriteError); }
		Applied->SetNumberField(TEXT("TrajectoryHistoryCount"), static_cast<int32>(NumVal)); bAnyWrite = true;
	}
	if (Params->TryGetNumberField(TEXT("trajectory_prediction_count"), NumVal))
	{
		if (!ImportTextOntoStruct(NodeStruct, NodeAddr, TEXT("TrajectoryPredictionCount"), FString::FromInt(static_cast<int32>(NumVal)), AnimNode, WriteError))
		{ GEditor->EndTransaction(); return FMonolithActionResult::Error(WriteError); }
		Applied->SetNumberField(TEXT("TrajectoryPredictionCount"), static_cast<int32>(NumVal)); bAnyWrite = true;
	}

	const TArray<TSharedPtr<FJsonValue>>* BonesArr = nullptr;
	if (Params->TryGetArrayField(TEXT("collected_bones"), BonesArr) && BonesArr)
	{
		TArray<FString> BoneNames;
		for (const TSharedPtr<FJsonValue>& V : *BonesArr)
		{
			if (V.IsValid()) BoneNames.Add(V->AsString());
		}
		if (!WriteBoneReferenceArray(NodeStruct, NodeAddr, TEXT("CollectedBones"), BoneNames, WriteError))
		{ GEditor->EndTransaction(); return FMonolithActionResult::Error(WriteError); }
		Applied->SetNumberField(TEXT("CollectedBones"), BoneNames.Num()); bAnyWrite = true;
	}

	GEditor->EndTransaction();

	if (bAnyWrite)
	{
		AnimNode->ReconstructNode();
		ABP->MarkPackageDirty();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ABP);
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("abp_path"), AbpPath);
	Root->SetStringField(TEXT("node_id"), AnimNode->GetName());
	Root->SetStringField(TEXT("node_struct"), NodeStruct->GetName());
	Root->SetObjectField(TEXT("applied"), Applied);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Action: configure_motion_matching_node (Sprint 4.3)
// Writes FAnimNode_MotionMatching props + base FAnimNode_BlendStack_Standalone
// props by reflection. MaxActiveBlends / bShouldFilterNotifies / NotifyRecencyTimeOut
// live on the BASE struct (gotcha #4); FindPropertyByName walks superstructs so the
// reflection write resolves them regardless. PoseJumpThresholdTime is FFloatInterval
// (gotcha #5) — written as (Min=..,Max=..).
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAbpWriteActions::HandleConfigureMotionMatchingNode(const TSharedPtr<FJsonObject>& Params)
{
	const FString AbpPath = Params->GetStringField(TEXT("abp_path"));
	const FString NodeId  = Params->GetStringField(TEXT("node_id"));
	if (NodeId.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: node_id"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AbpPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AbpPath));

	UEdGraphNode* FoundNode = FindNodeByName(ABP, NodeId, nullptr);
	if (!FoundNode) return FMonolithActionResult::Error(FString::Printf(TEXT("Node '%s' not found"), *NodeId));

	UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(FoundNode);
	if (!AnimNode) return FMonolithActionResult::Error(FString::Printf(
		TEXT("Node '%s' is not a UAnimGraphNode_Base (class: %s)"), *NodeId, *FoundNode->GetClass()->GetName()));

	UScriptStruct* NodeStruct = nullptr;
	void* NodeAddr = nullptr;
	FString ResolveError;
	if (!ResolveInnerAnimNode(AnimNode, NodeStruct, NodeAddr, ResolveError))
		return FMonolithActionResult::Error(ResolveError);

	GEditor->BeginTransaction(FText::FromString(TEXT("Configure Motion Matching Node")));
	AnimNode->Modify();

	TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
	FString WriteError;
	bool bAnyWrite = false;
	double NumVal = 0.0;
	bool BoolVal = false;

	auto WriteNum = [&](const TCHAR* JsonKey, const FName& Prop, bool bAsInt) -> bool
	{
		double V;
		if (!Params->TryGetNumberField(JsonKey, V)) return true; // not provided — skip
		const FString Text = bAsInt ? FString::FromInt(static_cast<int32>(V)) : FString::SanitizeFloat(V);
		if (!ImportTextOntoStruct(NodeStruct, NodeAddr, Prop, Text, AnimNode, WriteError)) return false;
		if (bAsInt) Applied->SetNumberField(Prop.ToString(), static_cast<int32>(V));
		else        Applied->SetNumberField(Prop.ToString(), V);
		bAnyWrite = true;
		return true;
	};

	if (!WriteNum(TEXT("blend_time"), TEXT("BlendTime"), false))
	{ GEditor->EndTransaction(); return FMonolithActionResult::Error(WriteError); }
	if (!WriteNum(TEXT("search_throttle"), TEXT("SearchThrottleTime"), false))
	{ GEditor->EndTransaction(); return FMonolithActionResult::Error(WriteError); }
	if (!WriteNum(TEXT("notify_recency_timeout"), TEXT("NotifyRecencyTimeOut"), false))
	{ GEditor->EndTransaction(); return FMonolithActionResult::Error(WriteError); }
	if (!WriteNum(TEXT("max_active_blends"), TEXT("MaxActiveBlends"), true))
	{ GEditor->EndTransaction(); return FMonolithActionResult::Error(WriteError); }

	if (Params->TryGetBoolField(TEXT("use_inertial_blend"), BoolVal))
	{
		if (!ImportTextOntoStruct(NodeStruct, NodeAddr, TEXT("bUseInertialBlend"), BoolVal ? TEXT("true") : TEXT("false"), AnimNode, WriteError))
		{ GEditor->EndTransaction(); return FMonolithActionResult::Error(WriteError); }
		Applied->SetBoolField(TEXT("bUseInertialBlend"), BoolVal); bAnyWrite = true;
	}
	if (Params->TryGetBoolField(TEXT("should_filter_notifies"), BoolVal))
	{
		if (!ImportTextOntoStruct(NodeStruct, NodeAddr, TEXT("bShouldFilterNotifies"), BoolVal ? TEXT("true") : TEXT("false"), AnimNode, WriteError))
		{ GEditor->EndTransaction(); return FMonolithActionResult::Error(WriteError); }
		Applied->SetBoolField(TEXT("bShouldFilterNotifies"), BoolVal); bAnyWrite = true;
	}

	// PoseJumpThresholdTime is an FFloatInterval — resolve current Min/Max, override
	// whichever was supplied, then write the whole struct via ImportText (Min=..,Max=..).
	const bool bHasMin = Params->TryGetNumberField(TEXT("pose_jump_threshold_min"), NumVal);
	double JumpMin = NumVal;
	const bool bHasMax = Params->TryGetNumberField(TEXT("pose_jump_threshold_max"), NumVal);
	double JumpMax = NumVal;
	if (bHasMin || bHasMax)
	{
		FProperty* IntervalProp = NodeStruct->FindPropertyByName(TEXT("PoseJumpThresholdTime"));
		FStructProperty* IntervalStructProp = CastField<FStructProperty>(IntervalProp);
		if (!IntervalStructProp)
		{ GEditor->EndTransaction(); return FMonolithActionResult::Error(TEXT("PoseJumpThresholdTime is not a struct property")); }

		// Read existing values to preserve the un-supplied side.
		void* IntervalAddr = IntervalStructProp->ContainerPtrToValuePtr<void>(NodeAddr);
		double CurMin = 0.0, CurMax = 0.0;
		if (FFloatProperty* MinP = CastField<FFloatProperty>(IntervalStructProp->Struct->FindPropertyByName(TEXT("Min"))))
			CurMin = MinP->GetPropertyValue_InContainer(IntervalAddr);
		if (FFloatProperty* MaxP = CastField<FFloatProperty>(IntervalStructProp->Struct->FindPropertyByName(TEXT("Max"))))
			CurMax = MaxP->GetPropertyValue_InContainer(IntervalAddr);

		const double FinalMin = bHasMin ? JumpMin : CurMin;
		const double FinalMax = bHasMax ? JumpMax : CurMax;
		const FString IntervalText = FString::Printf(TEXT("(Min=%s,Max=%s)"),
			*FString::SanitizeFloat(FinalMin), *FString::SanitizeFloat(FinalMax));
		if (!ImportTextOntoStruct(NodeStruct, NodeAddr, TEXT("PoseJumpThresholdTime"), IntervalText, AnimNode, WriteError))
		{ GEditor->EndTransaction(); return FMonolithActionResult::Error(WriteError); }
		Applied->SetStringField(TEXT("PoseJumpThresholdTime"), IntervalText); bAnyWrite = true;
	}

	GEditor->EndTransaction();

	if (bAnyWrite)
	{
		AnimNode->ReconstructNode();
		ABP->MarkPackageDirty();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ABP);
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("abp_path"), AbpPath);
	Root->SetStringField(TEXT("node_id"), AnimNode->GetName());
	Root->SetStringField(TEXT("node_struct"), NodeStruct->GetName());
	Root->SetObjectField(TEXT("applied"), Applied);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Action: build_motion_matching_node (Sprint 4.4 — COMPOSITE)
// Spawns a Pose History + Motion Matching node via the existing add_anim_graph_node
// internals (using the 4.1 "pose_history" alias), wires History pose-out -> MM
// pose-in via the connect internals, then assigns the MM Database pointer directly
// (a UObject pointer write, NOT AddAnimationAsset). Applies 4.2/4.3 defaults + compiles.
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAbpWriteActions::HandleBuildMotionMatchingNode(const TSharedPtr<FJsonObject>& Params)
{
	const FString AbpPath      = Params->GetStringField(TEXT("abp_path"));
	const FString DatabasePath = Params->GetStringField(TEXT("database_path"));
	FString ChooserPath;
	Params->TryGetStringField(TEXT("chooser_path"), ChooserPath);

	if (DatabasePath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: database_path"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AbpPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AbpPath));

	UPoseSearchDatabase* Database = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchDatabase>(DatabasePath);
	if (!Database) return FMonolithActionResult::Error(FString::Printf(TEXT("UPoseSearchDatabase not found: %s"), *DatabasePath));

	// --- Spawn the Pose History node via existing add_anim_graph_node internals (alias from 4.1) ---
	TSharedPtr<FJsonObject> HistParams = MakeShared<FJsonObject>();
	HistParams->SetStringField(TEXT("asset_path"), AbpPath);
	HistParams->SetStringField(TEXT("node_type"), TEXT("pose_history"));
	HistParams->SetNumberField(TEXT("position_x"), 0.0);
	HistParams->SetNumberField(TEXT("position_y"), 0.0);
	FMonolithActionResult HistResult = HandleAddAnimGraphNode(HistParams);
	if (!HistResult.bSuccess) return HistResult;
	const FString HistNodeName = HistResult.Result.IsValid() ? HistResult.Result->GetStringField(TEXT("node_name")) : FString();

	// --- Spawn the Motion Matching node ---
	TSharedPtr<FJsonObject> MMParams = MakeShared<FJsonObject>();
	MMParams->SetStringField(TEXT("asset_path"), AbpPath);
	MMParams->SetStringField(TEXT("node_type"), TEXT("MotionMatching"));
	MMParams->SetNumberField(TEXT("position_x"), -400.0);
	MMParams->SetNumberField(TEXT("position_y"), 0.0);
	FMonolithActionResult MMResult = HandleAddAnimGraphNode(MMParams);
	if (!MMResult.bSuccess) return MMResult;
	const FString MMNodeName = MMResult.Result.IsValid() ? MMResult.Result->GetStringField(TEXT("node_name")) : FString();

	// --- Wire MM pose-out -> History pose-in via existing connect internals ---
	// Topology (matches Epic GASP): the Motion Matching node is an asset player whose
	// 'Pose' output feeds the Pose History collector's 'Source' input pose link; the
	// History node's own 'Pose' output then flows downstream to the graph result.
	// So the History node WRAPS the MM node's output, collecting its pose over time.
	TSharedPtr<FJsonObject> ConnParams = MakeShared<FJsonObject>();
	ConnParams->SetStringField(TEXT("asset_path"), AbpPath);
	ConnParams->SetStringField(TEXT("source_node"), MMNodeName);
	ConnParams->SetStringField(TEXT("source_pin"), TEXT("Pose"));
	ConnParams->SetStringField(TEXT("target_node"), HistNodeName);
	ConnParams->SetStringField(TEXT("target_pin"), TEXT("Source"));
	ConnParams->SetBoolField(TEXT("compile"), false);
	FMonolithActionResult ConnResult = HandleConnectAnimGraphPins(ConnParams);
	const bool bWired = ConnResult.bSuccess;
	FString WireNote = bWired ? TEXT("connected") : (ConnResult.ErrorMessage.IsEmpty() ? TEXT("not connected") : ConnResult.ErrorMessage);

	// --- Assign the MM node's Database pointer directly (UObject pointer write) ---
	UAnimBlueprint* ABP2 = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AbpPath);
	UEdGraphNode* MMNode = FindNodeByName(ABP2, MMNodeName, nullptr);
	UAnimGraphNode_Base* MMAnim = Cast<UAnimGraphNode_Base>(MMNode);
	if (!MMAnim) return FMonolithActionResult::Error(TEXT("Spawned MotionMatching node could not be re-resolved"));

	UScriptStruct* MMStruct = nullptr;
	void* MMAddr = nullptr;
	FString ResolveError;
	if (!ResolveInnerAnimNode(MMAnim, MMStruct, MMAddr, ResolveError))
		return FMonolithActionResult::Error(ResolveError);

	GEditor->BeginTransaction(FText::FromString(TEXT("Assign Motion Matching Database")));
	MMAnim->Modify();

	// Database is TObjectPtr<const UPoseSearchDatabase> — the FObjectProperty carries no
	// C++ const, so a direct reflection pointer set is the correct write (not AddAnimationAsset).
	FProperty* DbProp = MMStruct->FindPropertyByName(TEXT("Database"));
	FObjectPropertyBase* DbObjProp = CastField<FObjectPropertyBase>(DbProp);
	if (!DbObjProp)
	{ GEditor->EndTransaction(); return FMonolithActionResult::Error(TEXT("MM node has no FObjectProperty 'Database'")); }
	DbObjProp->SetObjectPropertyValue_InContainer(MMAddr, Database);

	GEditor->EndTransaction();

	// --- Chooser-driven DB selection (best-effort note; Database is always set as fallback) ---
	bool bChooserNoted = false;
	if (!ChooserPath.IsEmpty())
	{
		// Chooser-driven selection is wired at runtime via an Anim Node Function calling
		// SetDatabaseToSearch/SetDatabasesToSearch from a chooser result; that function
		// graph wiring is out of scope for this composite. Database is set above as the
		// always-valid fallback; record the chooser path for the caller to wire the function.
		bChooserNoted = true;
	}

	// --- Apply 4.2 / 4.3 sensible defaults ---
	{
		TSharedPtr<FJsonObject> HistCfg = MakeShared<FJsonObject>();
		HistCfg->SetStringField(TEXT("abp_path"), AbpPath);
		HistCfg->SetStringField(TEXT("node_id"), HistNodeName);
		HistCfg->SetBoolField(TEXT("generate_trajectory"), true);
		HandleConfigurePoseHistoryNode(HistCfg);

		TSharedPtr<FJsonObject> MMCfg = MakeShared<FJsonObject>();
		MMCfg->SetStringField(TEXT("abp_path"), AbpPath);
		MMCfg->SetStringField(TEXT("node_id"), MMNodeName);
		MMCfg->SetNumberField(TEXT("blend_time"), 0.2);
		MMCfg->SetNumberField(TEXT("max_active_blends"), 4);
		HandleConfigureMotionMatchingNode(MMCfg);
	}

	// --- Wire History pose-out -> Output Pose (UAnimGraphNode_Root 'Result' input) ---
	// Without this the graph has no final pose driving the output, so the AnimBP plays
	// nothing. Resolve the main AnimGraph, find its Root node, and connect the History
	// node's 'Pose' output to the Root's 'Result' input via the same connect internals.
	bool bOutputPoseWired = false;
	FString OutputWireNote;
	{
		FString RootGraphError;
		UEdGraph* RootGraph = ResolveTargetGraph(ABP2, TEXT("AnimGraph"), TEXT(""), RootGraphError);
		UAnimGraphNode_Root* RootNode = nullptr;
		if (RootGraph)
		{
			TArray<UAnimGraphNode_Root*> Roots;
			RootGraph->GetNodesOfClass<UAnimGraphNode_Root>(Roots);
			if (Roots.Num() > 0)
			{
				RootNode = Roots[0];
			}
		}

		if (!RootNode)
		{
			OutputWireNote = RootGraph
				? TEXT("Output Pose (UAnimGraphNode_Root) not found in AnimGraph")
				: RootGraphError;
		}
		else
		{
			TSharedPtr<FJsonObject> RootConn = MakeShared<FJsonObject>();
			RootConn->SetStringField(TEXT("asset_path"), AbpPath);
			RootConn->SetStringField(TEXT("source_node"), HistNodeName);
			RootConn->SetStringField(TEXT("source_pin"), TEXT("Pose"));
			RootConn->SetStringField(TEXT("target_node"), RootNode->GetName());
			RootConn->SetStringField(TEXT("target_pin"), TEXT("Result"));
			RootConn->SetBoolField(TEXT("compile"), false);
			FMonolithActionResult RootConnResult = HandleConnectAnimGraphPins(RootConn);
			bOutputPoseWired = RootConnResult.bSuccess;
			OutputWireNote = bOutputPoseWired
				? TEXT("connected")
				: (RootConnResult.ErrorMessage.IsEmpty() ? TEXT("not connected") : RootConnResult.ErrorMessage);
		}
	}

	MMAnim->ReconstructNode();
	ABP2->MarkPackageDirty();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ABP2);
	FKismetEditorUtilities::CompileBlueprint(ABP2);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("abp_path"), AbpPath);
	Root->SetStringField(TEXT("pose_history_node"), HistNodeName);
	Root->SetStringField(TEXT("motion_matching_node"), MMNodeName);
	Root->SetStringField(TEXT("database"), DatabasePath);
	Root->SetBoolField(TEXT("wired"), bWired);
	Root->SetStringField(TEXT("wire_note"), WireNote);
	Root->SetBoolField(TEXT("output_pose_wired"), bOutputPoseWired);
	Root->SetStringField(TEXT("output_pose_wire_note"), OutputWireNote);
	if (bChooserNoted) Root->SetStringField(TEXT("chooser_path"), ChooserPath);
	Root->SetBoolField(TEXT("compiled"), true);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Action: get_anim_graph_output_connection (READ-ONLY)
// Find the UAnimGraphNode_Root and report whether its 'Result' input pose pin is
// linked, and by which node/pin. Confirms the graph drives the final output pose.
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAbpWriteActions::HandleGetAnimGraphOutputConnection(const TSharedPtr<FJsonObject>& Params)
{
	const FString AbpPath = Params->GetStringField(TEXT("abp_path"));
	FString GraphName = Params->HasField(TEXT("graph_name")) ? Params->GetStringField(TEXT("graph_name")) : TEXT("AnimGraph");
	if (GraphName.IsEmpty()) GraphName = TEXT("AnimGraph");

	if (AbpPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: abp_path"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AbpPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AbpPath));

	FString GraphError;
	UEdGraph* Graph = ResolveTargetGraph(ABP, GraphName, TEXT(""), GraphError);
	if (!Graph) return FMonolithActionResult::Error(GraphError);

	TArray<UAnimGraphNode_Root*> Roots;
	Graph->GetNodesOfClass<UAnimGraphNode_Root>(Roots);
	if (Roots.Num() == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No Output Pose (UAnimGraphNode_Root) found in graph '%s'"), *GraphName));
	}

	UAnimGraphNode_Root* RootNode = Roots[0];
	UEdGraphPin* ResultPin = RootNode->FindPin(FName(TEXT("Result")), EGPD_Input);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("abp_path"), AbpPath);
	Root->SetStringField(TEXT("graph_name"), GraphName);
	Root->SetStringField(TEXT("root_node"), RootNode->GetName());

	if (!ResultPin)
	{
		Root->SetBoolField(TEXT("output_connected"), false);
		Root->SetStringField(TEXT("note"), TEXT("Output Pose node has no 'Result' input pin"));
		return FMonolithActionResult::Success(Root);
	}

	const bool bConnected = ResultPin->LinkedTo.Num() > 0;
	Root->SetBoolField(TEXT("output_connected"), bConnected);
	if (bConnected)
	{
		UEdGraphPin* SrcPin = ResultPin->LinkedTo[0];
		UEdGraphNode* SrcNode = SrcPin ? SrcPin->GetOwningNodeUnchecked() : nullptr;
		Root->SetStringField(TEXT("source_node"), SrcNode ? SrcNode->GetName() : FString(TEXT("<unknown>")));
		Root->SetStringField(TEXT("source_pin"), SrcPin ? SrcPin->PinName.ToString() : FString(TEXT("<unknown>")));
	}
	return FMonolithActionResult::Success(Root);
}


// ---------------------------------------------------------------------------
// Pack C — build_foot_ik_pass (COMPOSITE)
//
// Inserts a lightweight foot-grounding pass into the post-MM pose chain. Foot IK
// (TwoBoneIK) + pelvis offset (ModifyBone) are SkeletalControl nodes that operate
// in COMPONENT space (their pose link is FComponentSpacePoseLink 'ComponentPose'),
// so the pass is bracketed by a LocalToComponentSpace converter at entry and a
// ComponentToLocalSpace converter at exit. The whole bracket is spliced between
// whatever currently drives the Output Pose (e.g. Inertialization) and the Root.
//
// Chain (local -> ... -> local):
//   [src] -> L2C(LocalPose) ==CS==> ModifyBone(pelvis) -> TwoBoneIK(L) -> TwoBoneIK(R) -> C2L -> [Root.Result]
//
// Foot IK alphas are driven by the contact_l/contact_r curves (AlphaInputType=Curve,
// AlphaCurveName=<curve>) so the planted foot is locked and swing-foot IK relaxes.
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAbpWriteActions::HandleBuildFootIkPass(const TSharedPtr<FJsonObject>& Params)
{
	const FString AbpPath       = Params->GetStringField(TEXT("abp_path"));
	const FString LeftFootBone  = Params->GetStringField(TEXT("left_foot_bone"));
	const FString RightFootBone = Params->GetStringField(TEXT("right_foot_bone"));
	const FString PelvisBone    = Params->GetStringField(TEXT("pelvis_bone"));
	FString GraphName = Params->HasField(TEXT("graph_name")) ? Params->GetStringField(TEXT("graph_name")) : TEXT("AnimGraph");
	if (GraphName.IsEmpty()) GraphName = TEXT("AnimGraph");
	FString LeftCurve  = Params->HasField(TEXT("left_contact_curve"))  ? Params->GetStringField(TEXT("left_contact_curve"))  : TEXT("contact_l");
	FString RightCurve = Params->HasField(TEXT("right_contact_curve")) ? Params->GetStringField(TEXT("right_contact_curve")) : TEXT("contact_r");

	if (AbpPath.IsEmpty())       return FMonolithActionResult::Error(TEXT("Missing required parameter: abp_path"));
	if (LeftFootBone.IsEmpty() || RightFootBone.IsEmpty() || PelvisBone.IsEmpty())
		return FMonolithActionResult::Error(TEXT("left_foot_bone, right_foot_bone and pelvis_bone are all required"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AbpPath);
	if (!ABP) return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint not found: %s"), *AbpPath));

	FString GraphError;
	UEdGraph* Graph = ResolveTargetGraph(ABP, GraphName, TEXT(""), GraphError);
	if (!Graph) return FMonolithActionResult::Error(GraphError);

	// --- Capture the current Output Pose source (the node feeding Root 'Result') ---
	TArray<UAnimGraphNode_Root*> Roots;
	Graph->GetNodesOfClass<UAnimGraphNode_Root>(Roots);
	if (Roots.Num() == 0)
		return FMonolithActionResult::Error(FString::Printf(TEXT("No Output Pose (UAnimGraphNode_Root) in graph '%s'"), *GraphName));
	UAnimGraphNode_Root* RootNode = Roots[0];
	UEdGraphPin* RootResultPin = RootNode->FindPin(FName(TEXT("Result")), EGPD_Input);
	if (!RootResultPin)
		return FMonolithActionResult::Error(TEXT("Output Pose node has no 'Result' input pin"));

	FString SrcNodeName, SrcPinName;
	if (RootResultPin->LinkedTo.Num() > 0 && RootResultPin->LinkedTo[0])
	{
		UEdGraphPin* SrcPin = RootResultPin->LinkedTo[0];
		UEdGraphNode* SrcNode = SrcPin->GetOwningNodeUnchecked();
		SrcNodeName = SrcNode ? SrcNode->GetName() : FString();
		SrcPinName  = SrcPin->PinName.ToString();
	}
	const bool bHadSource = !SrcNodeName.IsEmpty();

	// --- Local helper: spawn a node via add_anim_graph_node internals, return its node_name ---
	auto SpawnNode = [&](const TSharedPtr<FJsonObject>& P) -> FString
	{
		P->SetStringField(TEXT("asset_path"), AbpPath);
		P->SetStringField(TEXT("graph_name"), GraphName);
		FMonolithActionResult R = HandleAddAnimGraphNode(P);
		return (R.bSuccess && R.Result.IsValid()) ? R.Result->GetStringField(TEXT("node_name")) : FString();
	};

	// --- Spawn the bracket + IK nodes ---
	TSharedPtr<FJsonObject> L2CParams = MakeShared<FJsonObject>();
	L2CParams->SetStringField(TEXT("node_type"), TEXT("LocalToComponentSpace"));
	L2CParams->SetNumberField(TEXT("position_x"), 300.0);
	const FString L2CNode = SpawnNode(L2CParams);

	TSharedPtr<FJsonObject> PelvisParams = MakeShared<FJsonObject>();
	PelvisParams->SetStringField(TEXT("node_type"), TEXT("ModifyBone"));
	PelvisParams->SetStringField(TEXT("bone_to_modify"), PelvisBone);
	PelvisParams->SetNumberField(TEXT("position_x"), 500.0);
	const FString PelvisNode = SpawnNode(PelvisParams);

	TSharedPtr<FJsonObject> IkLParams = MakeShared<FJsonObject>();
	IkLParams->SetStringField(TEXT("node_type"), TEXT("TwoBoneIK"));
	IkLParams->SetStringField(TEXT("ik_bone"), LeftFootBone);
	IkLParams->SetNumberField(TEXT("position_x"), 700.0);
	const FString IkLNode = SpawnNode(IkLParams);

	TSharedPtr<FJsonObject> IkRParams = MakeShared<FJsonObject>();
	IkRParams->SetStringField(TEXT("node_type"), TEXT("TwoBoneIK"));
	IkRParams->SetStringField(TEXT("ik_bone"), RightFootBone);
	IkRParams->SetNumberField(TEXT("position_x"), 900.0);
	const FString IkRNode = SpawnNode(IkRParams);

	TSharedPtr<FJsonObject> C2LParams = MakeShared<FJsonObject>();
	C2LParams->SetStringField(TEXT("node_type"), TEXT("ComponentToLocalSpace"));
	C2LParams->SetNumberField(TEXT("position_x"), 1100.0);
	const FString C2LNode = SpawnNode(C2LParams);

	if (L2CNode.IsEmpty() || PelvisNode.IsEmpty() || IkLNode.IsEmpty() || IkRNode.IsEmpty() || C2LNode.IsEmpty())
		return FMonolithActionResult::Error(TEXT("One or more foot-IK pass nodes failed to spawn"));

	// --- Gate foot IK alphas by the contact curves (skeletal-control alpha-as-curve) ---
	auto SetProp = [&](const FString& NodeId, const FString& PropPath, const FString& Value)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), AbpPath);
		P->SetStringField(TEXT("graph_name"), GraphName);
		P->SetStringField(TEXT("node_id"), NodeId);
		P->SetStringField(TEXT("property_path"), PropPath);
		P->SetStringField(TEXT("value"), Value);
		HandleSetAnimGraphNodeProperty(P);
	};
	if (!LeftCurve.IsEmpty())
	{
		SetProp(IkLNode, TEXT("AlphaInputType"), TEXT("Curve"));
		SetProp(IkLNode, TEXT("AlphaCurveName"), LeftCurve);
	}
	if (!RightCurve.IsEmpty())
	{
		SetProp(IkRNode, TEXT("AlphaInputType"), TEXT("Curve"));
		SetProp(IkRNode, TEXT("AlphaCurveName"), RightCurve);
	}

	// --- Make the skeletal-control nodes NON-INERT ---
	// Without these, the pelvis ModifyBone leaves TranslationMode = BMM_Ignore (does nothing)
	// and the TwoBoneIK nodes solve toward EffectorLocation (0,0,0) in component space — which
	// yanks the feet to the component origin. Set real modes / reference frames so the pass is
	// functional. EffectorLocation itself is exposed as an input pin (auto-exposed by
	// add_anim_graph_node for TwoBoneIK) so a downstream ground-trace solve can drive it; until
	// then BoneSpace + EffectorTarget=foot bone makes EffectorLocation(0,0,0) an identity solve
	// (foot stays at its animated location) rather than a destructive component-origin pull.
	//
	// EBoneModificationMode: BMM_Ignore / BMM_Replace / BMM_Additive (AnimNode_ModifyBone.h:14-25).
	// EBoneControlSpace: BCS_WorldSpace / BCS_ComponentSpace / BCS_ParentBoneSpace / BCS_BoneSpace.
	// Enum names are imported by ImportText_Direct (the same parser the Details panel uses).

	// Pelvis offset: additive vertical adjust in component space (the Translation pin drives the amount).
	SetProp(PelvisNode, TEXT("TranslationMode"),  TEXT("BMM_Additive"));
	SetProp(PelvisNode, TEXT("TranslationSpace"), TEXT("BCS_ComponentSpace"));

	// Foot IK effectors: solve relative to the foot bone's own space, with the effector target
	// bound to the foot bone, so EffectorLocation is a meaningful (non-component-origin) offset.
	SetProp(IkLNode, TEXT("EffectorLocationSpace"), TEXT("BCS_BoneSpace"));
	SetProp(IkLNode, TEXT("EffectorTarget.BoneReference.BoneName"), LeftFootBone);
	SetProp(IkRNode, TEXT("EffectorLocationSpace"), TEXT("BCS_BoneSpace"));
	SetProp(IkRNode, TEXT("EffectorTarget.BoneReference.BoneName"), RightFootBone);

	// --- Wire the chain (verified pin names: LocalPose / ComponentPose / Pose / Result) ---
	auto Connect = [&](const FString& SN, const FString& SP, const FString& TN, const FString& TP) -> bool
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), AbpPath);
		P->SetStringField(TEXT("graph_name"), GraphName);
		P->SetStringField(TEXT("source_node"), SN);
		P->SetStringField(TEXT("source_pin"), SP);
		P->SetStringField(TEXT("target_node"), TN);
		P->SetStringField(TEXT("target_pin"), TP);
		P->SetBoolField(TEXT("compile"), false);
		return HandleConnectAnimGraphPins(P).bSuccess;
	};

	TSharedPtr<FJsonObject> Wires = MakeShared<FJsonObject>();
	// entry: previous source (local) -> L2C 'LocalPose'
	if (bHadSource)
		Wires->SetBoolField(TEXT("src_to_entry"), Connect(SrcNodeName, SrcPinName, L2CNode, TEXT("LocalPose")));
	// component-space chain: L2C 'ComponentPose' -> ModifyBone 'ComponentPose' -> IK_L 'ComponentPose' -> IK_R 'ComponentPose'
	Wires->SetBoolField(TEXT("l2c_to_pelvis"), Connect(L2CNode, TEXT("ComponentPose"), PelvisNode, TEXT("ComponentPose")));
	Wires->SetBoolField(TEXT("pelvis_to_ikl"), Connect(PelvisNode, TEXT("Pose"), IkLNode, TEXT("ComponentPose")));
	Wires->SetBoolField(TEXT("ikl_to_ikr"),    Connect(IkLNode, TEXT("Pose"), IkRNode, TEXT("ComponentPose")));
	// exit: IK_R 'Pose' (component) -> C2L 'ComponentPose' -> Root 'Result' (local)
	Wires->SetBoolField(TEXT("ikr_to_c2l"),    Connect(IkRNode, TEXT("Pose"), C2LNode, TEXT("ComponentPose")));
	Wires->SetBoolField(TEXT("exit_to_root"),  Connect(C2LNode, TEXT("Pose"), RootNode->GetName(), TEXT("Result")));

	ABP->MarkPackageDirty();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ABP);
	FKismetEditorUtilities::CompileBlueprint(ABP);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("abp_path"), AbpPath);
	Root->SetStringField(TEXT("graph_name"), GraphName);
	Root->SetStringField(TEXT("previous_output_source"), bHadSource ? SrcNodeName : FString(TEXT("<none>")));
	// entry/exit so the caller can re-splice: entry consumes the upstream pose, exit drives Output Pose.
	Root->SetStringField(TEXT("entry_node"), L2CNode);
	Root->SetStringField(TEXT("entry_pin"), TEXT("LocalPose"));
	Root->SetStringField(TEXT("exit_node"), C2LNode);
	Root->SetStringField(TEXT("exit_pin"), TEXT("Pose"));
	Root->SetStringField(TEXT("pelvis_modify_node"), PelvisNode);
	Root->SetStringField(TEXT("foot_ik_left_node"), IkLNode);
	Root->SetStringField(TEXT("foot_ik_right_node"), IkRNode);
	Root->SetStringField(TEXT("left_contact_curve"), LeftCurve);
	Root->SetStringField(TEXT("right_contact_curve"), RightCurve);
	Root->SetObjectField(TEXT("wires"), Wires);
	Root->SetBoolField(TEXT("compiled"), true);
	Root->SetBoolField(TEXT("saved"), false);
	return FMonolithActionResult::Success(Root);
}


// ---------------------------------------------------------------------------
// Pack C — assign_post_process_anim_rig
//
// Sets USkeletalMesh::PostProcessAnimBlueprint (the post-process anim instance
// class), which runs after the main anim instance and before physics. Verified
// accessor: USkeletalMesh::SetPostProcessAnimBlueprint(TSubclassOf<UAnimInstance>)
// (SkeletalMesh.h:2235). Direct member access is deprecated; the setter is public.
// An empty post_process_abp_path clears the assignment.
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithAbpWriteActions::HandleAssignPostProcessAnimRig(const TSharedPtr<FJsonObject>& Params)
{
	const FString MeshPath = Params->GetStringField(TEXT("mesh_path"));
	FString PostAbpPath;
	Params->TryGetStringField(TEXT("post_process_abp_path"), PostAbpPath);

	if (MeshPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: mesh_path"));

	USkeletalMesh* Mesh = FMonolithAssetUtils::LoadAssetByPath<USkeletalMesh>(MeshPath);
	if (!Mesh) return FMonolithActionResult::Error(FString::Printf(TEXT("SkeletalMesh not found: %s"), *MeshPath));

	TSubclassOf<UAnimInstance> PostClass = nullptr;
	const bool bClearing = PostAbpPath.IsEmpty() || PostAbpPath.Equals(TEXT("None"), ESearchCase::IgnoreCase);
	if (!bClearing)
	{
		// Accept either an AnimBlueprint asset (resolve its generated class) or a direct class path.
		UAnimBlueprint* PostAbp = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(PostAbpPath);
		if (PostAbp)
		{
			UClass* GenClass = PostAbp->GeneratedClass;
			if (!GenClass || !GenClass->IsChildOf(UAnimInstance::StaticClass()))
				return FMonolithActionResult::Error(FString::Printf(TEXT("AnimBlueprint '%s' has no UAnimInstance generated class (recompile it first)"), *PostAbpPath));
			PostClass = GenClass;
		}
		else
		{
			// Fall back to resolving as a class path (e.g. /Game/.../ABP_X.ABP_X_C).
			UClass* AsClass = LoadClass<UAnimInstance>(nullptr, *PostAbpPath);
			if (!AsClass)
				return FMonolithActionResult::Error(FString::Printf(TEXT("Could not resolve post-process AnimBlueprint or AnimInstance class: %s"), *PostAbpPath));
			PostClass = AsClass;
		}
	}

	Mesh->Modify();
	Mesh->SetPostProcessAnimBlueprint(PostClass);
	Mesh->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("mesh_path"), MeshPath);
	Root->SetBoolField(TEXT("cleared"), bClearing);
	Root->SetStringField(TEXT("post_process_class"), PostClass ? PostClass->GetPathName() : FString(TEXT("<none>")));
	Root->SetBoolField(TEXT("saved"), false);
	return FMonolithActionResult::Success(Root);
}
