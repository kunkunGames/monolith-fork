using UnrealBuildTool;
using System.IO;

public class MonolithLogicDriver : ModuleRules
{
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

		string[] Dirs = Directory.GetDirectories(BaseDir, DirName + "_*", SearchOption.TopDirectoryOnly);
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

		if (!bReleaseBuild)
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
