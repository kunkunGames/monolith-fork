using UnrealBuildTool;

public class MonolithCore : ModuleRules
{
	public MonolithCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"HTTP",
			"HTTPServer",
			"Json",
			"JsonUtilities",
			"Slate",
			"SlateCore",
			"DeveloperSettings",
			"Projects",
			"AssetRegistry",
			"EditorSubsystem",
			"UnrealEd",
			"Sockets",       // TCP probe for port bind verification
			"Networking",    // Socket address utilities
			"SQLiteCore"     // SQLite database headers
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"SourceControl"
		});

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.Add("bcrypt.lib");
		}
	}
}
