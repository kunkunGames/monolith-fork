using UnrealBuildTool;
using System.IO;

public class MonolithLevelSequence : ModuleRules
{
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
				if (Directory.Exists(ProjectPluginsDir))
				{
					bHasMovieRenderPipeline = Directory.Exists(Path.Combine(ProjectPluginsDir, "MovieRenderPipeline"));
				}
			}

			// 2. Check Engine Plugins/ folder
			if (!bHasMovieRenderPipeline)
			{
				string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
				string EnginePluginsDir = Path.Combine(EngineDir, "Plugins");

				bHasMovieRenderPipeline =
					Directory.Exists(Path.Combine(EnginePluginsDir, "Runtime", "MovieRenderPipeline"))
					|| Directory.Exists(Path.Combine(EnginePluginsDir, "Marketplace", "MovieRenderPipeline"))
					|| Directory.Exists(Path.Combine(EnginePluginsDir, "MovieScene", "MovieRenderPipeline"))
					|| Directory.Exists(Path.Combine(EnginePluginsDir, "MovieRenderPipeline"));
			}
		}

		PublicDefinitions.Add("WITH_MONOLITH_MRQ=" + (bHasMovieRenderPipeline ? "1" : "0"));

		if (bHasMovieRenderPipeline)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"AssetRegistry",
				"MovieRenderPipelineCore",
				"MovieRenderPipelineEditor"
			});
		}
	}
}
