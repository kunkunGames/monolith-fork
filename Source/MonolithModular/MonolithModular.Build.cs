using UnrealBuildTool;

public class MonolithModular : ModuleRules
{
	public MonolithModular(ReadOnlyTargetRules Target) : base(Target)
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
			"Json",
			"JsonUtilities",
			"Projects"
		});
	}
}
