using UnrealBuildTool;
using EpicGames.Core;
using System.IO;

public class MonolithComboGraph : ModuleRules
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

	private bool FindComboGraphPlugin(string SearchDir)
	{
		if (!Directory.Exists(SearchDir))
		{
			return false;
		}

		if (File.Exists(Path.Combine(SearchDir, "ComboGraph", "ComboGraph.uplugin")))
		{
			return true;
		}

		string[] Dirs = Directory.Exists(SearchDir) ? Directory.GetDirectories(SearchDir, "ComboGraph_*", SearchOption.TopDirectoryOnly) : new string[0];
		foreach (string Dir in Dirs)
		{
			if (File.Exists(Path.Combine(Dir, "ComboGraph.uplugin")))
			{
				return true;
			}
		}

		return false;
	}

	public MonolithComboGraph(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Release builds: set MONOLITH_RELEASE_BUILD=1 to force all optional deps off.
		bool bHasComboGraph = false;
		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";

		if (!bReleaseBuild && IsPluginEnabled(Target, "ComboGraph"))
		{
			// 1. Check project Plugins/ folder
			if (Target.ProjectFile != null)
			{
				string ProjectPluginsDir = Path.Combine(
					Target.ProjectFile.Directory.FullName, "Plugins");
				bHasComboGraph = FindComboGraphPlugin(ProjectPluginsDir);
			}

			// 2. Check Engine Plugins/Marketplace/ folder (Fab install)
			if (!bHasComboGraph)
			{
				string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
				string MarketplaceDir = Path.Combine(EngineDir, "Plugins", "Marketplace");
				bHasComboGraph = FindComboGraphPlugin(MarketplaceDir);

				// 3. Check Engine Plugins/ root
				if (!bHasComboGraph)
				{
					string EnginePluginsDir = Path.Combine(EngineDir, "Plugins");
					bHasComboGraph = FindComboGraphPlugin(EnginePluginsDir);
				}
			}
		}

		if (bHasComboGraph)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"Core", "CoreUObject", "Engine",
				"MonolithCore",
				"UnrealEd",
				"GameplayAbilities", "GameplayTags",
				"GameplayAbilitiesEditor", "GameplayTasksEditor",
				"BlueprintGraph",
				"EditorScriptingUtilities",
				"ComboGraph", "ComboGraphEditor",
				"Json", "JsonUtilities",
				"AssetRegistry"
			});
			PublicDefinitions.Add("WITH_COMBOGRAPH=1");
		}
		else
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"Core", "CoreUObject", "Engine",
				"MonolithCore"
			});
			PublicDefinitions.Add("WITH_COMBOGRAPH=0");
		}
	}
}
