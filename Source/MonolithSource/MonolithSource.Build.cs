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
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"MonolithCore",
			"MonolithIndex",
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
