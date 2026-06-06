using UnrealBuildTool;
using System.IO;

public class MonolithMesh : ModuleRules
{
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
				if (Directory.Exists(ProjectPluginsDir))
				{
					bHasGeometryScripting = Directory.Exists(
						Path.Combine(ProjectPluginsDir, "GeometryScripting"))
						|| (Directory.Exists(ProjectPluginsDir) && Directory.GetDirectories(
							ProjectPluginsDir, "GeometryScripting_*",
							SearchOption.TopDirectoryOnly).Length > 0);
				}
			}

			// 2. Check Engine Plugins/ folder
			if (!bHasGeometryScripting)
			{
				string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
				string EnginePluginsDir = Path.Combine(EngineDir, "Plugins");

				bHasGeometryScripting =
					Directory.Exists(Path.Combine(EnginePluginsDir, "Runtime", "GeometryScripting"))
					|| Directory.Exists(Path.Combine(EnginePluginsDir, "Developer", "GeometryScripting"))
					|| Directory.Exists(Path.Combine(EnginePluginsDir, "Experimental", "GeometryScripting"))
					|| Directory.Exists(Path.Combine(EnginePluginsDir, "Marketplace", "GeometryScripting"))
					|| Directory.Exists(Path.Combine(EnginePluginsDir, "GeometryScripting"));
			}
		}

		if (bHasGeometryScripting)
		{
			PrivateDependencyModuleNames.Add("GeometryScriptingCore");
			PrivateDependencyModuleNames.Add("GeometryFramework");
			PrivateDependencyModuleNames.Add("GeometryCore");
			PublicDefinitions.Add("WITH_GEOMETRYSCRIPT=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_GEOMETRYSCRIPT=0");
		}
	}
}
