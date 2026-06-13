using UnrealBuildTool;
using System.IO;

public class MonolithDataflow : ModuleRules
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
				bHasDataflowRuntime = HasPluginDir(ProjectPluginsDir, "Dataflow");
			}

			if (!bHasDataflowRuntime)
			{
				string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
				string EnginePluginsDir = Path.Combine(EngineDir, "Plugins");

				bHasDataflowRuntime =
					HasPluginDir(Path.Combine(EnginePluginsDir, "Experimental"), "Dataflow")
					|| HasPluginDir(Path.Combine(EnginePluginsDir, "Runtime"), "Dataflow")
					|| HasPluginDir(EnginePluginsDir, "Dataflow")
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
