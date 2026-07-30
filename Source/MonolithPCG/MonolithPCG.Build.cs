using UnrealBuildTool;

public class MonolithPCG : ModuleRules
{
	public MonolithPCG(ReadOnlyTargetRules Target) : base(Target)
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
			"PCG",
			"UnrealEd",
			"AssetRegistry",
			"Json",
			"JsonUtilities"
		});
	}
}
