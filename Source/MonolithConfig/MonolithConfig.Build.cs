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
			"Projects",
			"SourceControl",
			// `DeveloperSettings` is its OWN module (NOT part of Engine) — required
			// by the dev-gated `set_developer_setting` action which mutates
			// UDeveloperSettings CDOs at runtime. The header lives at
			// Engine/DeveloperSettings.h but resolves to this module.
			"DeveloperSettings"
		});

		// Installed engine distributions ship the Localization editor DLL but
		// may omit its static import library. Consume only the public runtime
		// module interface and reflected model types so MonolithConfig remains
		// linkable from a foreign project/plugin checkout.
		PrivateIncludePathModuleNames.Add("Localization");
		DynamicallyLoadedModuleNames.Add("Localization");
	}
}
