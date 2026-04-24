using UnrealBuildTool;

public class MonolithSource : ModuleRules
{
	public MonolithSource(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"MonolithCore"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"SQLiteCore",
			"EditorSubsystem",
			"UnrealEd",
			"Json",
			"JsonUtilities",
			"Slate",
			"SlateCore",
			"Projects"
		});
	}
}
