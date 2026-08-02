using UnrealBuildTool;

public class MonolithAnimation : ModuleRules
{
	public MonolithAnimation(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"MonolithCore",
			"UnrealEd",
			"Slate",        // STableRow/SCompoundWidget instantiation (FRetargetChainElement rows).
			"SlateCore",    // FSlateAttributeDescriptor / SWidget::PrivateRegisterAttributes — was
			                // transitively satisfied on 5.7, not re-exported on 5.8; link explicitly on both.
			"AnimGraph",
			"AnimGraphRuntime",
			"BlueprintGraph",
			"AnimationBlueprintLibrary",
			"Json",
			"JsonUtilities",
			"PoseSearch",
			// PoseSearch declares Chooser as an enabled, non-optional plugin dependency
			// in both supported engines (UE 5.7/5.8). MonolithAnimation consumes Chooser
			// APIs directly, so its module link must follow that required transitive edge.
			"Chooser",
			// Chooser keeps GameplayTags private; the GameplayTag chooser-cell authoring
			// path therefore needs an explicit module dependency.
			"GameplayTags",
			"EditorScriptingUtilities",
			"AnimationModifiers",
			"IKRig",
			"IKRigEditor",
			"ControlRig",
			"ControlRigDeveloper",
			"RigVM",
			"RigVMDeveloper",
			"PoseSearchEditor",    // UAnimGraphNode_MotionMatching (Wave 7 ABP graph wiring)
			"AssetRegistry",
			"BlendStackEditor",    // UAnimGraphNode_BlendStack_Base (Sprint 4 BoundGraph-node spawn fix)
		});

		// Keep the existing source guards deterministic across every target. Chooser
		// cannot be disabled independently while required PoseSearch remains enabled.
		PublicDefinitions.Add("WITH_CHOOSER=1");
	}

}
