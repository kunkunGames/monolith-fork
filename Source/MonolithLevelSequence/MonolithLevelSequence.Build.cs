using UnrealBuildTool;
using System.IO;

public class MonolithLevelSequence : ModuleRules
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

	public MonolithLevelSequence(ReadOnlyTargetRules Target) : base(Target)

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
			"MonolithIndex",
			"SQLiteCore",
			"UnrealEd",
			"AssetRegistry",
			"Projects",
			"MovieScene",
			"MovieSceneTracks",
			"LevelSequence",
			"BlueprintGraph",
			"Kismet",
			"EditorSubsystem",
			"Json",
			"JsonUtilities"
		});

		bool bHasMovieRenderPipeline = false;
		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";

		if (!bReleaseBuild)
		{
			// 1. Check project Plugins/ folder
			if (Target.ProjectFile != null)
			{
				string ProjectPluginsDir = Path.Combine(Target.ProjectFile.Directory.FullName, "Plugins");
				bHasMovieRenderPipeline = HasPluginDir(ProjectPluginsDir, "MovieRenderPipeline");
			}

			// 2. Check Engine Plugins/ folder
			if (!bHasMovieRenderPipeline)
			{
				string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
				string EnginePluginsDir = Path.Combine(EngineDir, "Plugins");

				bHasMovieRenderPipeline =
					HasPluginDir(Path.Combine(EnginePluginsDir, "Runtime"), "MovieRenderPipeline")
					|| HasPluginDir(Path.Combine(EnginePluginsDir, "Marketplace"), "MovieRenderPipeline")
					|| HasPluginDir(Path.Combine(EnginePluginsDir, "MovieScene"), "MovieRenderPipeline")
					|| HasPluginDir(EnginePluginsDir, "MovieRenderPipeline");
			}
		}

		PublicDefinitions.Add("WITH_MONOLITH_MRQ=" + (bHasMovieRenderPipeline ? "1" : "0"));

		if (bHasMovieRenderPipeline)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"MovieRenderPipelineCore",
				"MovieRenderPipelineEditor"
			});
		}
	}
}
