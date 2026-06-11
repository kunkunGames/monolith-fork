using UnrealBuildTool;
using System.IO;

public class MonolithComboGraph : ModuleRules
{
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

		string[] Dirs = Directory.GetDirectories(SearchDir, "ComboGraph_*", SearchOption.TopDirectoryOnly);
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

		if (!bReleaseBuild)
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
