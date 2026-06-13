using UnrealBuildTool;
using System.IO;

public class MonolithAudio : ModuleRules
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

		return false;
	}

	public MonolithAudio(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"MonolithCore",            // Registry, FMonolithActionResult
			"MonolithAudioRuntime",    // UMonolithSoundPerceptionUserData (Phase I3)
			"AIModule",                // UAISense_Hearing for sense-class resolution (Phase I3)
			"AudioMixer",              // SoundSubmix, submix effects
			"AudioEditor",             // All UFactory classes, graph schemas
			"AssetRegistry",           // IAssetRegistry / FAssetData
			"AssetTools",              // FAssetToolsModule for rename operations
			"Json", "JsonUtilities",
			"Slate", "SlateCore",      // Editor module transitive deps
			"UnrealEd"                 // GEditor for preview, asset tools
		});

		// --- Conditional: MetaSound support ---
		// Release builds: set MONOLITH_RELEASE_BUILD=1 to force all optional deps off.
		bool bHasMetaSound = false;
		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";

		if (!bReleaseBuild)
		{
			// 1. Check project Plugins/ folder, including optional subfolders used by some vendor layouts.
			if (Target.ProjectFile != null)
			{
				string ProjectPluginsDir = Path.Combine(Target.ProjectFile.Directory.FullName, "Plugins");
				if (Directory.Exists(ProjectPluginsDir))
				{
					bHasMetaSound =
						HasPluginDir(ProjectPluginsDir, "Metasound")
						|| HasPluginDir(Path.Combine(ProjectPluginsDir, "Runtime"), "Metasound")
						|| HasPluginDir(Path.Combine(ProjectPluginsDir, "Developer"), "Metasound")
						|| HasPluginDir(Path.Combine(ProjectPluginsDir, "Experimental"), "Metasound")
						|| HasPluginDir(Path.Combine(ProjectPluginsDir, "Marketplace"), "Metasound");
				}
			}

			// 2. Check Engine Plugins/ folder
			if (!bHasMetaSound)
			{
				string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
				string EnginePluginsDir = Path.Combine(EngineDir, "Plugins");

				// 3-location probe (engine Plugins/Runtime, Plugins/Marketplace, top-level Plugins fallback)
				// Note: MetaSound is a built-in engine plugin (ships with UE 5.7) usually found in Runtime.
				bHasMetaSound =
					HasPluginDir(Path.Combine(EnginePluginsDir, "Runtime"), "Metasound")
					|| HasPluginDir(Path.Combine(EnginePluginsDir, "Developer"), "Metasound")
					|| HasPluginDir(Path.Combine(EnginePluginsDir, "Experimental"), "Metasound")
					|| HasPluginDir(Path.Combine(EnginePluginsDir, "Marketplace"), "Metasound")
					|| HasPluginDir(EnginePluginsDir, "Metasound");
			}
		}

		PublicDefinitions.Add("WITH_METASOUND=" + (bHasMetaSound ? "1" : "0"));

		if (bHasMetaSound)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"MetasoundEngine",    // UMetaSoundBuilderSubsystem, Builder API
				"MetasoundFrontend",  // FMetasoundFrontendDocument, structs
				"MetasoundEditor"     // UMetaSoundEditorSubsystem, BuildToAsset
			});
		}
	}
}
