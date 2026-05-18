using UnrealBuildTool;

public class MonolithConfig : ModuleRules
{
	public MonolithConfig(ReadOnlyTargetRules Target) : base(Target)
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
			"UnrealEd",
			"AssetTools",
			"Json",
			"JsonUtilities",
			"Projects"
		});
	}
}
