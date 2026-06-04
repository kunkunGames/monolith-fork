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
			if (Target.ProjectFile != null)
			{
				string ProjectPluginsDir = Path.Combine(Target.ProjectFile.Directory.FullName, "Plugins");
				if (Directory.Exists(ProjectPluginsDir))
				{
					bHasDataflowRuntime = Directory.Exists(Path.Combine(ProjectPluginsDir, "Dataflow"));
				}
			}

			if (!bHasDataflowRuntime)
			{
				string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
				string EnginePluginsDir = Path.Combine(EngineDir, "Plugins");

				string ExperimentalDir = Path.Combine(EnginePluginsDir, "Experimental", "Dataflow");
				string RuntimeDir = Path.Combine(EnginePluginsDir, "Runtime", "Dataflow");
				string RootDir = Path.Combine(EnginePluginsDir, "Dataflow");

				bHasDataflowRuntime =
					Directory.Exists(ExperimentalDir)
					|| Directory.Exists(RuntimeDir)
					|| Directory.Exists(RootDir)
					|| (Directory.Exists(Path.Combine(EnginePluginsDir, "Experimental")) && Directory.GetDirectories(Path.Combine(EnginePluginsDir, "Experimental"), "Dataflow_*", SearchOption.TopDirectoryOnly).Length > 0)
					|| (Directory.Exists(Path.Combine(EnginePluginsDir, "Runtime")) && Directory.GetDirectories(Path.Combine(EnginePluginsDir, "Runtime"), "Dataflow_*", SearchOption.TopDirectoryOnly).Length > 0)
					|| (Directory.Exists(EnginePluginsDir) && Directory.GetDirectories(EnginePluginsDir, "Dataflow_*", SearchOption.TopDirectoryOnly).Length > 0)
					|| (Directory.Exists(Path.Combine(EngineDir, "Source", "Runtime", "Experimental", "Dataflow", "Core"))
						&& Directory.Exists(Path.Combine(EngineDir, "Source", "Runtime", "Experimental", "Dataflow", "Engine")));
			}
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
