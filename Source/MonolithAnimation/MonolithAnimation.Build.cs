using UnrealBuildTool;
using System.IO;

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
			"AnimGraph",
			"AnimGraphRuntime",
			"BlueprintGraph",
			"AnimationBlueprintLibrary",
			"Json",
			"JsonUtilities",
			"PoseSearch",
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

		// --- Conditional: Chooser (UChooserTable authoring) ---
		// The Chooser plugin ships with the engine but can be disabled per-project.
		// Gate the dependency so a project without it still links MonolithAnimation
		// (the chooser handlers fall back to a clean "not available" error).
		//
		// Release builds: MONOLITH_RELEASE_BUILD=1 forces this OFF so binary releases
		// never hard-link against a plugin the end user may have disabled.
		bool bHasChooser = false;
		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";

		if (!bReleaseBuild)
		{
			// 1. Project Plugins/ folder (manual install or symlink)
			string ProjectPluginsDir = Path.Combine(
				Target.ProjectFile.Directory.FullName, "Plugins");
			if (Directory.Exists(ProjectPluginsDir))
			{
				bHasChooser = Directory.Exists(
					Path.Combine(ProjectPluginsDir, "Chooser"))
					|| HasChooserVariantDir(ProjectPluginsDir);
			}

			// 2. Engine Plugins/Marketplace/ folder (Fab install)
			if (!bHasChooser)
			{
				string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
				string MarketplaceDir = Path.Combine(
					EngineDir, "Plugins", "Marketplace");
				if (Directory.Exists(MarketplaceDir))
				{
					bHasChooser = Directory.Exists(
						Path.Combine(MarketplaceDir, "Chooser"))
						|| HasChooserVariantDir(MarketplaceDir);
				}

				// 3. Engine Plugins/ root (default UE install location)
				if (!bHasChooser)
				{
					string EnginePluginsDir = Path.Combine(EngineDir, "Plugins");
					bHasChooser = Directory.Exists(
						Path.Combine(EnginePluginsDir, "Chooser"))
						|| Directory.Exists(
							Path.Combine(EnginePluginsDir, "Animation", "Chooser"))
						|| Directory.Exists(
							Path.Combine(EnginePluginsDir, "Experimental", "Chooser"))
						|| HasChooserVariantDir(EnginePluginsDir);
				}
			}
		}

		if (bHasChooser)
		{
			PrivateDependencyModuleNames.Add("Chooser");
			// FGameplayTagColumn cells use FGameplayTagContainer + UGameplayTagsManager.
			// Chooser keeps GameplayTags as a PRIVATE dep (not re-exported), so the
			// chooser-authoring GameplayTag cell-write path needs it directly. Only
			// linked when WITH_CHOOSER, matching the gated #if WITH_CHOOSER bodies.
			PrivateDependencyModuleNames.Add("GameplayTags");
			// PUBLIC so every TU in the module (including a sibling graph-surgery
			// file added later) sees WITH_CHOOSER consistently.
			PublicDefinitions.Add("WITH_CHOOSER=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_CHOOSER=0");
		}
	}

	// A "Chooser_*" sibling (e.g. a suffixed Fab/marketplace install) only counts
	// when the directory actually contains the Chooser plugin descriptor. Scratch
	// folders like Plugins/Chooser_Backup must not force a Chooser module
	// dependency that UBT cannot resolve.
	private static bool HasChooserVariantDir(string ParentDir)
	{
		if (!Directory.Exists(ParentDir))
		{
			return false;
		}
		foreach (string Dir in Directory.GetDirectories(ParentDir, "Chooser_*", SearchOption.TopDirectoryOnly))
		{
			if (File.Exists(Path.Combine(Dir, "Chooser.uplugin")))
			{
				return true;
			}
		}
		return false;
	}
}
