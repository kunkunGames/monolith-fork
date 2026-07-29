using UnrealBuildTool;

public class MonolithGameplayMessage : ModuleRules
{
	public MonolithGameplayMessage(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"GameplayTags",
			"MonolithCore"
		});

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Json",
			"Projects"
		});
	}
}
