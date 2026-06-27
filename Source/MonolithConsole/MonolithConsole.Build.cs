using UnrealBuildTool;

public class MonolithConsole : ModuleRules
{
	public MonolithConsole(ReadOnlyTargetRules Target) : base(Target)
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
			"MonolithEditor",
			"MonolithSource",
			"EditorSubsystem",
			"UnrealEd",
			"Slate",
			"SlateCore",
			"Json",
			"JsonUtilities"
		});
	}
}
