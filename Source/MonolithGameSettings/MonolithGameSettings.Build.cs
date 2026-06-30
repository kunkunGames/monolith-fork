using UnrealBuildTool;

public class MonolithGameSettings : ModuleRules
{
	public MonolithGameSettings(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"EnhancedInput",
				"InputCore",
				"Json",
				"JsonUtilities",
				"MonolithCore",
				"Projects"
			});
	}
}
