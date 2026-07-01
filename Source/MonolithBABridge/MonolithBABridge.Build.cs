using UnrealBuildTool;
using System.IO;

public class MonolithBABridge : ModuleRules
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


	private static bool HasPluginDir(string BaseDir, string PluginName)
	{
		if (!Directory.Exists(BaseDir))
		{
			return false;
		}

		if (File.Exists(Path.Combine(BaseDir, PluginName, PluginName + ".uplugin")))
		{
			return true;
		}

		string[] Dirs = Directory.Exists(BaseDir) ? Directory.GetDirectories(BaseDir, PluginName + "*", SearchOption.TopDirectoryOnly) : new string[0];
		foreach (string Dir in Dirs)
		{
			if (File.Exists(Path.Combine(Dir, PluginName + ".uplugin")))
			{
				return true;
			}
		}

		return false;
	}

	public MonolithBABridge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Probe for Blueprint Assist in multiple locations.
		// BA installs to Engine/Plugins/Marketplace/ with an obfuscated folder name,
		// or can be manually placed in the project's Plugins/ folder.
		//
		// Release builds: set MONOLITH_RELEASE_BUILD=1 to force all optional deps off.
		// This ensures binary releases don't link against plugins the user may not have.
		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";

		bool bHasBlueprintAssist = false;
		if (!bReleaseBuild && IsPluginEnabled(Target, "BlueprintAssist"))
		{
			if (Target.ProjectFile != null)
			{
				string ProjectPluginsDir = Path.Combine(Target.ProjectFile.Directory.FullName, "Plugins");
				bHasBlueprintAssist = HasPluginDir(ProjectPluginsDir, "BlueprintAssist");
			}

			if (!bHasBlueprintAssist)
			{
				string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
				string MarketplaceDir = Path.Combine(EngineDir, "Plugins", "Marketplace");
				bHasBlueprintAssist = HasPluginDir(MarketplaceDir, "BlueprintAssist");

				if (!bHasBlueprintAssist)
				{
					string EnginePluginsDir = Path.Combine(EngineDir, "Plugins");
					bHasBlueprintAssist = HasPluginDir(EnginePluginsDir, "BlueprintAssist");
				}
			}
		}

		if (bHasBlueprintAssist)
		{
			// Full implementation -- link against BA
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"Core", "CoreUObject", "Engine",
				"Slate", "SlateCore",
				"MonolithCore",
				"BlueprintAssist",
				"UnrealEd", "GraphEditor",
				"Json"
			});
			PublicDefinitions.Add("WITH_BLUEPRINT_ASSIST=1");
		}
		else
		{
			// Empty shell -- compiles clean, does nothing at runtime
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"Core", "CoreUObject", "Engine",
				"MonolithCore"
			});
			PublicDefinitions.Add("WITH_BLUEPRINT_ASSIST=0");
		}
	}
}
