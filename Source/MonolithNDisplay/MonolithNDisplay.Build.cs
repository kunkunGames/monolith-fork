using UnrealBuildTool;

public class MonolithNDisplay : ModuleRules
{
	public MonolithNDisplay(ReadOnlyTargetRules Target) : base(Target)
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
			"AssetRegistry",
			"Json",
			"JsonUtilities"
		});
	}
}
