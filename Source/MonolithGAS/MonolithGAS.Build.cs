using UnrealBuildTool;
using System.IO;

public class MonolithGAS : ModuleRules
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

	public MonolithGAS(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Always-available engine GAS modules
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine",
			"GameplayAbilities", "GameplayTags", "GameplayTasks",
			// UMG: needed publicly because MonolithGASAttributeBindingClassExtension subclasses
			// UWidgetBlueprintGeneratedClassExtension (UMG) and exposes USTRUCTs referenced by other modules.
			"UMG", "Slate", "SlateCore"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"MonolithCore", "MonolithBlueprint",
			"UnrealEd", "BlueprintGraph",
			"GameplayAbilitiesEditor", "GameplayTasksEditor",
			"GameplayTagsEditor",
			"EnhancedInput",
			"InputCore",
			"EditorScriptingUtilities",
			"Json", "JsonUtilities",
			// UMGEditor: editor-side UWidgetBlueprintExtension + FWidgetBlueprintCompilerContext
			// (used only by Phase H1 attribute-binding action handlers; module already gated as Type:"Editor").
			"UMGEditor",
			"AssetRegistry"
		});

		// --- Conditional: GBA (Blueprint Attributes) ---
		// The actual UE module is "BlueprintAttributes", not "GBAPlugin".
		//
		// Release builds: set MONOLITH_RELEASE_BUILD=1 to force all optional deps off.
		// This ensures binary releases don't link against plugins the user may not have.
		bool bHasGBA = false;
		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";

		if (!bReleaseBuild)
		{
			// 1. Project Plugins/ folder (manual install or symlink)
			if (Target.ProjectFile != null)
			{
				string ProjectPluginsDir = Path.Combine(
					Target.ProjectFile.Directory.FullName, "Plugins");
				bHasGBA = HasPluginDir(ProjectPluginsDir, "BlueprintAttributes");
			}

			// 2. Engine Plugins/Marketplace/ folder (Fab install)
			if (!bHasGBA)
			{
				string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
				string MarketplaceDir = Path.Combine(
					EngineDir, "Plugins", "Marketplace");
				bHasGBA = HasPluginDir(MarketplaceDir, "BlueprintAttributes");

				// 3. Engine Plugins/ root
				if (!bHasGBA)
				{
					string EnginePluginsDir = Path.Combine(EngineDir, "Plugins");
					bHasGBA = HasPluginDir(EnginePluginsDir, "BlueprintAttributes");
				}
			}
		}

		if (bHasGBA)
		{
			PrivateDependencyModuleNames.Add("BlueprintAttributes");
			PublicDefinitions.Add("WITH_GBA=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_GBA=0");
		}
	}
}
