using UnrealBuildTool;
using System.IO;

public class MonolithWorldConditions : ModuleRules
{
	private static bool HasPluginDir(string BaseDir, string PluginName)
	{
		if (!Directory.Exists(BaseDir))
		{
			return false;
		}

		if (Directory.Exists(Path.Combine(BaseDir, PluginName)) && File.Exists(Path.Combine(BaseDir, PluginName, PluginName + ".uplugin")))
		{
			return true;
		}

		string[] Dirs = Directory.GetDirectories(BaseDir, PluginName + "_*", SearchOption.TopDirectoryOnly);
		foreach (string Dir in Dirs)
		{
			if (File.Exists(Path.Combine(Dir, PluginName + ".uplugin")))
			{
				return true;
			}
		}

		return false;
	}

	public MonolithWorldConditions(ReadOnlyTargetRules Target) : base(Target)
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
			"AssetRegistry",
			"Json",
			"JsonUtilities"
		});

		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";
		bool bHasWorldConditions = false;
		bool bHasSmartObjects = false;

		if (!bReleaseBuild)
		{
			string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
			string EnginePluginsDir = Path.Combine(EngineDir, "Plugins");

			bHasWorldConditions =
				HasPluginDir(Path.Combine(EnginePluginsDir, "Runtime"), "WorldConditions")
				|| HasPluginDir(Path.Combine(EnginePluginsDir, "Experimental"), "WorldConditions")
				|| HasPluginDir(EnginePluginsDir, "WorldConditions");

			bHasSmartObjects =
				HasPluginDir(Path.Combine(EnginePluginsDir, "Runtime"), "SmartObjects")
				|| HasPluginDir(Path.Combine(EnginePluginsDir, "AI"), "SmartObjects")
				|| HasPluginDir(EnginePluginsDir, "SmartObjects");

			if (Target.ProjectFile != null)
			{
				string ProjectPluginsDir = Path.Combine(Target.ProjectFile.Directory.FullName, "Plugins");
				if (Directory.Exists(ProjectPluginsDir))
				{
					bHasWorldConditions = bHasWorldConditions
						|| HasPluginDir(ProjectPluginsDir, "WorldConditions");
					bHasSmartObjects = bHasSmartObjects
						|| HasPluginDir(ProjectPluginsDir, "SmartObjects");
				}
			}
		}

		PublicDefinitions.Add("WITH_MONOLITH_WORLDCONDITIONS=" + (bHasWorldConditions ? "1" : "0"));
		PublicDefinitions.Add("WITH_MONOLITH_WORLDCONDITIONS_SMARTOBJECTS=" + (bHasWorldConditions && bHasSmartObjects ? "1" : "0"));

		if (bHasWorldConditions)
		{
			PrivateDependencyModuleNames.Add("WorldConditions");
		}

		if (bHasWorldConditions && bHasSmartObjects)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"SmartObjectsModule",
				"GameplayTags"
			});
		}
	}
}
