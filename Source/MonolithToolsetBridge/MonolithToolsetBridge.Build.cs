using UnrealBuildTool;
using System.IO;
using EpicGames.Core;

public class MonolithToolsetBridge : ModuleRules
{
	// Returns true iff `PluginName` is ENABLED for this target (not merely present on disk).
	// Mirrors UnrealBuildTool Plugins.IsPluginEnabledForTarget (UE 5.7
	// Engine/Source/Programs/UnrealBuildTool/System/Plugins.cs:693). Fixes issue #71:
	// engine plugins ship-but-default-off (EnabledByDefault:false), so disk presence != enablement.
	// Design + API citations: Docs/plans/2026-06-15-issue71-plugin-enablement-gating.md.
	// Keep BYTE-IDENTICAL with the copies in MonolithMesh/MonolithIndex/MonolithAudio/MonolithAnimation.
	private static bool IsPluginEnabled(ReadOnlyTargetRules Target, string PluginName)
	{
		// 1. Target-level overrides win outright (uncommon but correct: -EnablePlugin=/-DisablePlugin=).
		if (Target.DisablePlugins != null && System.Linq.Enumerable.Contains(Target.DisablePlugins, PluginName)) { return false; }
		if (Target.EnablePlugins  != null && System.Linq.Enumerable.Contains(Target.EnablePlugins,  PluginName)) { return true;  }

		if (Target.ProjectFile == null)
		{
			return false;   // engine/program target with no .uproject: every gated engine plugin is EnabledByDefault:false -> treat as OFF
		}

		// 2. The .uproject's explicit Plugins[] entry is the deciding signal.
		try
		{
			ProjectDescriptor Project = ProjectDescriptor.FromFile(Target.ProjectFile);
			if (Project.Plugins != null)
			{
				foreach (PluginReferenceDescriptor Ref in Project.Plugins)
				{
					if (string.Equals(Ref.Name, PluginName, System.StringComparison.OrdinalIgnoreCase))
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

	public MonolithToolsetBridge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// UE 5.8's ToolsetRegistry is Experimental / NoRedist and is absent from the
		// 5.7 build, so this bridge is OFF by default and compiles as an inert empty
		// shell with no ToolsetRegistry dependency. A source/dev build on an engine
		// that ships ToolsetRegistry can opt in by setting the env var
		// MONOLITH_WITH_TOOLSET_REGISTRY_BRIDGE=1; the dependency is only added when
		// the opt-in is set AND the plugin is actually present. Release builds
		// (MONOLITH_RELEASE_BUILD=1) force it off so binary releases never link it.
		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";
		bool bOptIn = System.Environment.GetEnvironmentVariable("MONOLITH_WITH_TOOLSET_REGISTRY_BRIDGE") == "1";

		bool bHasToolsetRegistry = !bReleaseBuild && bOptIn && IsPluginEnabled(Target, "ToolsetRegistry");

		if (bHasToolsetRegistry)
		{
			// Full bridge -- link against ToolsetRegistry (source/dev build only).
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"Core", "CoreUObject", "Engine",
				"MonolithCore",
				"ToolsetRegistry",
				"UnrealEd", "Json"
			});
			PublicDefinitions.Add("MONOLITH_WITH_TOOLSET_REGISTRY_BRIDGE=1");
		}
		else
		{
			// Empty shell -- compiles clean with no ToolsetRegistry dependency.
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"Core", "CoreUObject", "Engine",
				"MonolithCore"
			});
			PublicDefinitions.Add("MONOLITH_WITH_TOOLSET_REGISTRY_BRIDGE=0");
		}
	}
}
