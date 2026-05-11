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
			string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
			string MovieRenderPipelineDir = Path.Combine(EngineDir, "Plugins", "MovieScene", "MovieRenderPipeline");
			bHasMovieRenderPipeline = Directory.Exists(MovieRenderPipelineDir);
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
