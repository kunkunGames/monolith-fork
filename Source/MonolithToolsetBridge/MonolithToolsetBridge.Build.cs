using UnrealBuildTool;
using System.IO;

public class MonolithToolsetBridge : ModuleRules
{
	private static bool HasPluginDir(string BaseDir, string PluginName)
	{
		if (!Directory.Exists(BaseDir))
		{
			return false;
		}

		if (File.Exists(Path.Combine(BaseDir, PluginName, PluginName + ".uplugin")))
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

	public MonolithToolsetBridge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// UE 5.8's ToolsetRegistry is Experimental / NoRedist and is absent from the
		// 5.7 build, so this bridge is OFF by default and compiles as an inert empty
		// shell with no ToolsetRegistry dependency. A source/dev build on an engine
		// that ships ToolsetRegistry can opt in by setting the env var
		// MONOLITH_WITH_TOOLSET_REGISTRY_BRIDGE=1; the dependency is only added when
		// the opt-in is set AND the plugin is actually present. Release builds
		// (MONOLITH_RELEASE_BUILD=1) force it off so binary releases never link it.
		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";
		bool bOptIn = System.Environment.GetEnvironmentVariable("MONOLITH_WITH_TOOLSET_REGISTRY_BRIDGE") == "1";

		bool bHasToolsetRegistry = false;
		if (!bReleaseBuild && bOptIn)
		{
			string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);

			// ToolsetRegistry ships under Engine/Plugins/Experimental in UE 5.8.
			bHasToolsetRegistry = HasPluginDir(Path.Combine(EngineDir, "Plugins", "Experimental"), "ToolsetRegistry");

			if (!bHasToolsetRegistry)
			{
				bHasToolsetRegistry = HasPluginDir(Path.Combine(EngineDir, "Plugins"), "ToolsetRegistry");
			}

			if (!bHasToolsetRegistry && Target.ProjectFile != null)
			{
				string ProjectPluginsDir = Path.Combine(Target.ProjectFile.Directory.FullName, "Plugins");
				bHasToolsetRegistry = HasPluginDir(ProjectPluginsDir, "ToolsetRegistry");
			}
		}

		if (bHasToolsetRegistry)
		{
			// Full bridge -- link against ToolsetRegistry (source/dev build only).
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"Core", "CoreUObject", "Engine",
				"MonolithCore",
				"ToolsetRegistry",
				"UnrealEd", "Json"
			});
			PublicDefinitions.Add("MONOLITH_WITH_TOOLSET_REGISTRY_BRIDGE=1");
		}
		else
		{
			// Empty shell -- compiles clean with no ToolsetRegistry dependency.
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"Core", "CoreUObject", "Engine",
				"MonolithCore"
			});
			PublicDefinitions.Add("MONOLITH_WITH_TOOLSET_REGISTRY_BRIDGE=0");
		}
	}
}
