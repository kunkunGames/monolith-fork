using UnrealBuildTool;
using System.IO;

public class MonolithDataflow : ModuleRules
{
	public MonolithDataflow(ReadOnlyTargetRules Target) : base(Target)
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
		bool bHasDataflowRuntime = false;

		if (!bReleaseBuild)
		{
			string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
			bHasDataflowRuntime =
				Directory.Exists(Path.Combine(EngineDir, "Source", "Runtime", "Experimental", "Dataflow", "Core"))
				&& Directory.Exists(Path.Combine(EngineDir, "Source", "Runtime", "Experimental", "Dataflow", "Engine"));
		}

		if (bHasDataflowRuntime)
		{
			PrivateDependencyModuleNames.Add("DataflowCore");
			PrivateDependencyModuleNames.Add("DataflowEngine");
			PublicDefinitions.Add("WITH_MONOLITH_DATAFLOW=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_MONOLITH_DATAFLOW=0");
		}
	}
}
