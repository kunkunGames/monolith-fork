using UnrealBuildTool;
using System.IO;

public class MonolithGameFeatures : ModuleRules
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

		string[] Dirs = Directory.Exists(BaseDir) ? Directory.GetDirectories(BaseDir, PluginName + "_*", SearchOption.TopDirectoryOnly) : new string[0];
		foreach (string Dir in Dirs)
		{
			if (File.Exists(Path.Combine(Dir, PluginName + ".uplugin")))
			{
				return true;
			}
		}

		return false;
	}

	public MonolithGameFeatures(ReadOnlyTargetRules Target) : base(Target)
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
			"GameplayTags",
			"Projects",
			"Json",
			"JsonUtilities"
		});

		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";
		bool bHasGameFeatures = false;

		if (!bReleaseBuild)
		{
			if (Target.ProjectFile != null)
			{
				string ProjectPluginsDir = Path.Combine(Target.ProjectFile.Directory.FullName, "Plugins");
				bHasGameFeatures = HasPluginDir(ProjectPluginsDir, "GameFeatures");
			}

			if (!bHasGameFeatures)
			{
				string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
				string EnginePluginsDir = Path.Combine(EngineDir, "Plugins");
				bHasGameFeatures =
					HasPluginDir(Path.Combine(EnginePluginsDir, "Runtime"), "GameFeatures")
					|| HasPluginDir(Path.Combine(EnginePluginsDir, "Experimental"), "GameFeatures")
					|| HasPluginDir(Path.Combine(EnginePluginsDir, "Editor"), "GameFeatures")
					|| HasPluginDir(EnginePluginsDir, "GameFeatures")
					|| Directory.Exists(Path.Combine(EngineDir, "Source", "Runtime", "GameFeatures"));
			}
		}

		if (bHasGameFeatures)
		{
			PrivateDependencyModuleNames.Add("GameFeatures");
			PublicDefinitions.Add("WITH_MONOLITH_GAMEFEATURES=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_MONOLITH_GAMEFEATURES=0");
		}
	}
}
