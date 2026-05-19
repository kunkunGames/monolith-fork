using UnrealBuildTool;
using System.IO;

public class MonolithWorldConditions : ModuleRules
{
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
				Directory.Exists(Path.Combine(EnginePluginsDir, "Runtime", "WorldConditions"))
				|| Directory.Exists(Path.Combine(EnginePluginsDir, "Experimental", "WorldConditions"))
				|| Directory.Exists(Path.Combine(EnginePluginsDir, "WorldConditions"));

			bHasSmartObjects =
				Directory.Exists(Path.Combine(EnginePluginsDir, "Runtime", "SmartObjects"))
				|| Directory.Exists(Path.Combine(EnginePluginsDir, "AI", "SmartObjects"))
				|| Directory.Exists(Path.Combine(EnginePluginsDir, "SmartObjects"));

			if (Target.ProjectFile != null)
			{
				string ProjectPluginsDir = Path.Combine(Target.ProjectFile.Directory.FullName, "Plugins");
				if (Directory.Exists(ProjectPluginsDir))
				{
					bHasWorldConditions = bHasWorldConditions
						|| Directory.Exists(Path.Combine(ProjectPluginsDir, "WorldConditions"));
					bHasSmartObjects = bHasSmartObjects
						|| Directory.Exists(Path.Combine(ProjectPluginsDir, "SmartObjects"));
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
