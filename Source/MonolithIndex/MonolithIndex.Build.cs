using UnrealBuildTool;
using System.IO;

public class MonolithIndex : ModuleRules
{
	public MonolithIndex(ReadOnlyTargetRules Target) : base(Target)
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
			"JsonUtilities",
			"SQLiteCore",
			"Slate",
			"SlateCore",
			"BlueprintGraph",
			"KismetCompiler",
			"EditorSubsystem",
			"AnimationCore",
			"Niagara",
			"GameplayTags",
			"GameplayAbilities",
			"EnhancedInput",
			"Projects"
		});

		// --- Conditional: MetaSound (engine-shipped Runtime plugin) ---
		// 3-location probe (engine Plugins/Runtime, Plugins/Marketplace, top-level Plugins fallback).
		// Release builds: set MONOLITH_RELEASE_BUILD=1 to force optional deps off (Issue #30 defense).
		bool bHasMetasound = false;
		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";

		if (!bReleaseBuild)
		{
			string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
			string EnginePluginsDir = Path.Combine(EngineDir, "Plugins");
			bHasMetasound =
				Directory.Exists(Path.Combine(EnginePluginsDir, "Runtime", "Metasound"))
				|| Directory.Exists(Path.Combine(EnginePluginsDir, "Marketplace", "Metasound"))
				|| Directory.GetDirectories(EnginePluginsDir, "Metasound", SearchOption.TopDirectoryOnly).Length > 0;
		}

		if (bHasMetasound)
		{
			PrivateDependencyModuleNames.AddRange(new string[] { "MetasoundEngine", "MetasoundFrontend" });
			PublicDefinitions.Add("WITH_METASOUND=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_METASOUND=0");
		}
	}
}
