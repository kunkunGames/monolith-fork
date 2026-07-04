using UnrealBuildTool;
using System.IO;
using EpicGames.Core;

public class MonolithLogicDriver : ModuleRules
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

	private static bool HasPluginDescriptor(string PluginDir, params string[] DescriptorNames)
	{
		if (!Directory.Exists(PluginDir))
		{
			return false;
		}

		foreach (string DescriptorName in DescriptorNames)
		{
			if (File.Exists(Path.Combine(PluginDir, DescriptorName + ".uplugin")))
			{
				return true;
			}
		}

		return false;
	}

	private static bool HasPluginDir(string BaseDir, string DirName, params string[] DescriptorNames)
	{
		if (!Directory.Exists(BaseDir))
		{
			return false;
		}

		if (HasPluginDescriptor(Path.Combine(BaseDir, DirName), DescriptorNames))
		{
			return true;
		}

		string[] Dirs = Directory.Exists(BaseDir) ? Directory.GetDirectories(BaseDir, DirName + "_*", SearchOption.TopDirectoryOnly) : new string[0];
		foreach (string Dir in Dirs)
		{
			if (HasPluginDescriptor(Dir, DescriptorNames))
			{
				return true;
			}
		}

		return false;
	}

	public MonolithLogicDriver(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Release builds: set MONOLITH_RELEASE_BUILD=1 to force all optional deps off.
		bool bHasLogicDriver = false;
		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";

		if (!bReleaseBuild && IsPluginEnabled(Target, "SMSystem"))
		{
			// 1. Check project Plugins/ folder
			if (Target.ProjectFile != null)
			{
				string ProjectPluginsDir = Path.Combine(
					Target.ProjectFile.Directory.FullName, "Plugins");
				bHasLogicDriver = HasPluginDir(ProjectPluginsDir, "SMSystem", "SMSystem")
					|| HasPluginDir(ProjectPluginsDir, "LogicDriver", "LogicDriver", "SMSystem");
			}

			// 2. Check Engine Plugins/Marketplace/ folder (Fab install)
			if (!bHasLogicDriver)
			{
				string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
				string MarketplaceDir = Path.Combine(EngineDir, "Plugins", "Marketplace");
				bHasLogicDriver = HasPluginDir(MarketplaceDir, "SMSystem", "SMSystem")
					|| HasPluginDir(MarketplaceDir, "LogicDriver", "LogicDriver", "SMSystem");

				// 3. Check Engine Plugins/ root
				if (!bHasLogicDriver)
				{
					string EnginePluginsDir = Path.Combine(EngineDir, "Plugins");
					bHasLogicDriver = HasPluginDir(EnginePluginsDir, "SMSystem", "SMSystem")
						|| HasPluginDir(EnginePluginsDir, "LogicDriver", "LogicDriver", "SMSystem");
				}
			}
		}

		if (bHasLogicDriver)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"Core", "CoreUObject", "Engine",
				"MonolithCore", "MonolithIndex",
				"UnrealEd",
				"BlueprintGraph",
				"EditorScriptingUtilities",
				"SMSystem", "SMSystemEditor",
				"GameplayTags",
				"Json", "JsonUtilities",
				"AssetRegistry"
			});
			PublicDefinitions.Add("WITH_LOGICDRIVER=1");
		}
		else
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"Core", "CoreUObject", "Engine",
				"MonolithCore"
			});
			PublicDefinitions.Add("WITH_LOGICDRIVER=0");
		}
	}
}
