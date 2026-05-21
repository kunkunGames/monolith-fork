using UnrealBuildTool;

public class MonolithImageGen : ModuleRules
{
	public MonolithImageGen(ReadOnlyTargetRules Target) : base(Target)
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
			"MonolithAsset",
			"UnrealEd",
			"HTTP",
			"Json",
			"JsonUtilities"
		});
	}
}
