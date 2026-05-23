using UnrealBuildTool;

public class MonolithSprite : ModuleRules
{
	public MonolithSprite(ReadOnlyTargetRules Target) : base(Target)
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
			"Projects",
			"ImageWrapper",
			"Json",
			"JsonUtilities"
		});
	}
}
