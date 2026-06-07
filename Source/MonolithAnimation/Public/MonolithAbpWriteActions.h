#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * ABP graph node wiring actions for Monolith — Wave 7.
 * 3 core actions: add_anim_graph_node, connect_anim_graph_pins, set_state_animation.
 * Places animation nodes inside state graphs or the main AnimGraph and wires them.
 */
class MONOLITHANIMATION_API FMonolithAbpWriteActions
{
public:
	/** Register all ABP graph wiring actions with the tool registry */
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult HandleAddAnimGraphNode(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleConnectAnimGraphPins(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetStateAnimation(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddVariableGet(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetAnimGraphNodeProperty(const TSharedPtr<FJsonObject>& Params);

	// Sprint 4 — Motion Matching AnimBP graph authoring.
	static FMonolithActionResult HandleConfigurePoseHistoryNode(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleConfigureMotionMatchingNode(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleBuildMotionMatchingNode(const TSharedPtr<FJsonObject>& Params);

	// READ-ONLY — report whether the AnimGraph's Output Pose (UAnimGraphNode_Root 'Result'
	// input) is driven, and by which node/pin. Verifies the graph actually produces a pose.
	static FMonolithActionResult HandleGetAnimGraphOutputConnection(const TSharedPtr<FJsonObject>& Params);

	// Pack C — foot IK composite + post-process anim rig assignment.
	// build_foot_ik_pass        : insert a lightweight foot IK pass (2x TwoBoneIK + pelvis ModifyBone)
	//                             into the post-MM pose chain, spliced before Output Pose.
	// assign_post_process_anim_rig: set USkeletalMesh::PostProcessAnimBlueprint (post-process anim
	//                             instance class) on a mesh asset.
	static FMonolithActionResult HandleBuildFootIkPass(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAssignPostProcessAnimRig(const TSharedPtr<FJsonObject>& Params);
};
