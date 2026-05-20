using UnrealBuildTool;
using System.IO;

public class MonolithWorldGen : ModuleRules
{
	public MonolithWorldGen(ReadOnlyTargetRules Target) : base(Target)
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
			"MonolithMesh",
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
			"LevelInstanceEditor",
			"ImageCore"
		});

		// Optional GeometryScripting -- mirror MonolithMesh without a shared
		// rules helper so this module remains self-contained in UBT rule builds.
		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";
		bool bHasGeometryScripting = false;
		if (!bReleaseBuild)
		{
			if (Target.ProjectFile != null)
			{
				string ProjectPluginsDir = Path.Combine(Target.ProjectFile.Directory.FullName, "Plugins");
				if (Directory.Exists(ProjectPluginsDir))
				{
					bHasGeometryScripting =
						Directory.Exists(Path.Combine(ProjectPluginsDir, "GeometryScripting"))
						|| Directory.GetDirectories(
							ProjectPluginsDir, "GeometryScripting_*",
							SearchOption.TopDirectoryOnly).Length > 0;
				}
			}

			if (!bHasGeometryScripting)
			{
				string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
				string EnginePluginsDir = Path.Combine(EngineDir, "Plugins");
				bHasGeometryScripting =
					Directory.Exists(Path.Combine(EnginePluginsDir, "Runtime", "GeometryScripting"))
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
