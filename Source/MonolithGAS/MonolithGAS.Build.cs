using UnrealBuildTool;
using System.IO;
using EpicGames.Core;

public class MonolithGAS : ModuleRules
{
	// Returns true iff `PluginName` is ENABLED for this target (not merely present on disk).
	// Mirrors UnrealBuildTool Plugins.IsPluginEnabledForTarget (UE 5.7
	// Engine/Source/Programs/UnrealBuildTool/System/Plugins.cs:693). Fixes issue #71:
	// engine plugins ship-but-default-off (EnabledByDefault:false), so disk presence != enablement.
	// Design + API citations: Docs/plans/2026-06-15-issue71-plugin-enablement-gating.md.
	// Keep BYTE-IDENTICAL with the copies in MonolithMesh/MonolithIndex/MonolithAudio/MonolithAnimation.
	private static bool IsPluginEnabled(ReadOnlyTargetRules Target, string PluginName)
	{
		if (Target.ProjectFile == null)
		{
			return false;   // engine/program target with no .uproject: every gated engine plugin is EnabledByDefault:false -> treat as OFF
		}

		// 1. Target-level overrides win outright (uncommon but correct: -EnablePlugin=/-DisablePlugin=).
		if (Target.DisablePlugins != null && System.Linq.Enumerable.Contains(Target.DisablePlugins, PluginName)) { return false; }
		if (Target.EnablePlugins  != null && System.Linq.Enumerable.Contains(Target.EnablePlugins,  PluginName)) { return true;  }

		// 2. The .uproject's explicit Plugins[] entry (non-optional) is the deciding signal.
		try
		{
			ProjectDescriptor Project = ProjectDescriptor.FromFile(Target.ProjectFile);
			if (Project.Plugins != null)
			{
				foreach (PluginReferenceDescriptor Ref in Project.Plugins)
				{
					if (string.Equals(Ref.Name, PluginName, System.StringComparison.OrdinalIgnoreCase) && !Ref.bOptional)
					{
						return Ref.bEnabled
							&& Ref.IsEnabledForPlatform(Target.Platform)
							&& Ref.IsEnabledForTargetConfiguration(Target.Configuration)
							&& Ref.IsEnabledForTarget(Target.Type);
					}
				}
			}
		}
		catch (System.Exception)
		{
			return false;   // unreadable .uproject -> fail safe to OFF (never hard-link)
		}

		// 3. No .uproject entry -> falls to the .uplugin EnabledByDefault. ALL gated plugins here are
		//    engine plugins with EnabledByDefault:false, so absence == disabled. Return false.
		//    (If a future gated plugin were EnabledByDefault:true, this branch would need to read its
		//    .uplugin; documented as a known limitation in the plan.)
		return false;
	}

	public MonolithGAS(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Always-available engine GAS modules
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine",
			"GameplayAbilities", "GameplayTags", "GameplayTasks",
			// UMG: needed publicly because MonolithGASAttributeBindingClassExtension subclasses
			// UWidgetBlueprintGeneratedClassExtension (UMG) and exposes USTRUCTs referenced by other modules.
			"UMG", "Slate", "SlateCore"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"MonolithCore", "MonolithBlueprint",
			"UnrealEd", "BlueprintGraph",
			"GameplayAbilitiesEditor", "GameplayTasksEditor",
			"GameplayTagsEditor",
			"EnhancedInput",
			"InputCore",
			"EditorScriptingUtilities",
			"Json", "JsonUtilities",
			// UMGEditor: editor-side UWidgetBlueprintExtension + FWidgetBlueprintCompilerContext
			// (used only by Phase H1 attribute-binding action handlers; module already gated as Type:"Editor").
			"UMGEditor",
			"AssetRegistry"
		});

		// --- Conditional: GBA (Blueprint Attributes) ---
		// The actual UE module is "BlueprintAttributes", not "GBAPlugin".
		//
		// Release builds: set MONOLITH_RELEASE_BUILD=1 to force all optional deps off.
		// This ensures binary releases don't link against plugins the user may not have.
		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";
		bool bHasGBA = !bReleaseBuild && IsPluginEnabled(Target, "BlueprintAttributes");

		if (bHasGBA)
		{
			PrivateDependencyModuleNames.Add("BlueprintAttributes");
			PublicDefinitions.Add("WITH_GBA=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_GBA=0");
		}
	}
}
