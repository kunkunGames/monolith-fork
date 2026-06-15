using UnrealBuildTool;
using System.IO;

public class MonolithWorldGen : ModuleRules
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
			"MonolithScene",
			"MonolithLevelDesign",
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
				bHasGeometryScripting = HasPluginDir(ProjectPluginsDir, "GeometryScripting");
			}

			if (!bHasGeometryScripting)
			{
				string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
				string EnginePluginsDir = Path.Combine(EngineDir, "Plugins");
				bHasGeometryScripting =
					HasPluginDir(Path.Combine(EnginePluginsDir, "Runtime"), "GeometryScripting")
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
