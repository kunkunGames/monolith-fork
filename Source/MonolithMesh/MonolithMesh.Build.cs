using UnrealBuildTool;
using System.IO;

public class MonolithMesh : ModuleRules
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

	public MonolithMesh(ReadOnlyTargetRules Target) : base(Target)
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
			"EditorSubsystem",
			"MeshDescription",
			"StaticMeshDescription",
			"MeshConversion",
			"PhysicsCore",
			"NavigationSystem",
			"RenderCore",
			"RHI",
			"EditorScriptingUtilities",
			"Json",
			"JsonUtilities",
			"Slate",
			"SlateCore",
			"AssetRegistry",
			"AssetTools",
			"MeshReductionInterface",
			"MeshMergeUtilities",
			"LevelInstanceEditor"
		});

		// Optional: GeometryScripting (Tier 5 mesh operations only)
		//
		// Release builds: set MONOLITH_RELEASE_BUILD=1 to force this dep off so
		// the released DLL doesn't carry a hard import on UnrealEditor-GeometryScriptingCore.dll
		// (users who don't have the GeometryScripting plugin enabled in their .uproject
		// would otherwise hit GetLastError=126 at module load — see #26 / #30).
		// Source-tree users with GeometryScripting enabled still get full functionality.
		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";

		bool bHasGeometryScripting = false;
		if (!bReleaseBuild)
		{
			// 1. Check project Plugins/ folder
			if (Target.ProjectFile != null)
			{
				string ProjectPluginsDir = Path.Combine(
					Target.ProjectFile.Directory.FullName, "Plugins");
				bHasGeometryScripting = HasPluginDir(ProjectPluginsDir, "GeometryScripting");
			}

			// 2. Check Engine Plugins/ folder
			if (!bHasGeometryScripting)
			{
				string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
				string EnginePluginsDir = Path.Combine(EngineDir, "Plugins");

				bHasGeometryScripting =
					HasPluginDir(Path.Combine(EnginePluginsDir, "Runtime"), "GeometryScripting")
					|| HasPluginDir(Path.Combine(EnginePluginsDir, "Developer"), "GeometryScripting")
					|| HasPluginDir(Path.Combine(EnginePluginsDir, "Experimental"), "GeometryScripting")
					|| HasPluginDir(Path.Combine(EnginePluginsDir, "Marketplace"), "GeometryScripting")
					|| HasPluginDir(EnginePluginsDir, "GeometryScripting");
			}
		}

		if (bHasGeometryScripting)
		{
			PrivateDependencyModuleNames.Add("GeometryScriptingCore");
			PrivateDependencyModuleNames.Add("GeometryFramework");
			PrivateDependencyModuleNames.Add("GeometryCore");
			PublicDefinitions.Add("WITH_GEOMETRYSCRIPT=1");

			// Delay-bind the GeometryScripting module DLLs so MonolithMesh.dll loads
			// even if these aren't resolvable at module-load time (issue #70 — the
			// import resolves lazily on first Tier-5 call instead of at LoadLibrary,
			// removing the CouldNotBeLoadedByOS / GetLastError=126 first-build/load race).
			// Verified: PublicDelayLoadDLLs at ModuleRules.cs:1308 (UE 5.7). Confirm via
			// dumpbin that these imports move to the Delay Import table.
			if (Target.Platform == UnrealTargetPlatform.Win64)
			{
				PublicDelayLoadDLLs.Add("UnrealEditor-GeometryScriptingCore.dll");
				PublicDelayLoadDLLs.Add("UnrealEditor-GeometryFramework.dll");
				PublicDelayLoadDLLs.Add("UnrealEditor-GeometryCore.dll");
			}
		}
		else
		{
			PublicDefinitions.Add("WITH_GEOMETRYSCRIPT=0");
		}
	}
}
