using UnrealBuildTool;
using System.IO;
using EpicGames.Core;

public class MonolithUI : ModuleRules
{
	// Returns true iff `PluginName` is ENABLED for this target (not merely present on disk).
	// Mirrors UnrealBuildTool Plugins.IsPluginEnabledForTarget (UE 5.7
	// Engine/Source/Programs/UnrealBuildTool/System/Plugins.cs:693). Fixes issue #71:
	// engine plugins ship-but-default-off (EnabledByDefault:false), so disk presence != enablement.
	// Design + API citations: Docs/plans/2026-06-15-issue71-plugin-enablement-gating.md.
	// Keep BYTE-IDENTICAL with the copies in MonolithMesh/MonolithIndex/MonolithAudio/MonolithAnimation.
	private static bool IsPluginEnabled(ReadOnlyTargetRules Target, string PluginName)
	{
		// 1. Target-level overrides win outright (uncommon but correct: -EnablePlugin=/-DisablePlugin=).
		if (Target.DisablePlugins != null && System.Linq.Enumerable.Contains(Target.DisablePlugins, PluginName, System.StringComparer.OrdinalIgnoreCase)) { return false; }
		if (Target.EnablePlugins  != null && System.Linq.Enumerable.Contains(Target.EnablePlugins,  PluginName, System.StringComparer.OrdinalIgnoreCase)) { return true;  }

		if (Target.ProjectFile == null)
		{
			return false;   // engine/program target with no .uproject: every gated engine plugin is EnabledByDefault:false -> treat as OFF
		}

		// 2. The .uproject's explicit Plugins[] entry is the deciding signal.
		try
		{
			ProjectDescriptor Project = ProjectDescriptor.FromFile(Target.ProjectFile);
			if (Project.Plugins != null)
			{
				foreach (PluginReferenceDescriptor Ref in Project.Plugins)
				{
					if (string.Equals(Ref.Name, PluginName, System.StringComparison.OrdinalIgnoreCase))
					{
						return Ref.bEnabled
							&& Ref.IsEnabledForPlatform(Target.Platform)
							&& Ref.IsEnabledForTargetConfiguration(Target.Configuration)
							&& Ref.IsEnabledForTarget(Target.Type);
					}
				}
			}
		}
		catch (System.Exception)
		{
			return false;   // unreadable .uproject -> fail safe to OFF (never hard-link)
		}

		// 3. No .uproject entry -> falls to the .uplugin EnabledByDefault. ALL gated plugins here are
		//    engine plugins with EnabledByDefault:false, so absence == disabled. Return false.
		//    (If a future gated plugin were EnabledByDefault:true, this branch would need to read its
		//    .uplugin; documented as a known limitation in the plan.)
		return false;
	}

    public MonolithUI(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core", "CoreUObject", "Engine",
            // EditorSubsystem is Public because UMonolithUIRegistrySubsystem
            // (a UEditorSubsystem) is declared in Public/Registry/. Downstream
            // modules that include the header need the parent class visible.
            "EditorSubsystem",
            // Phase G: UMonolithUISettings derives from UDeveloperSettings,
            // which lives in the DeveloperSettings module (NOT Engine).
            // Verified at C:\Program Files (x86)\UE_5.7\Engine\Source\Runtime\
            // DeveloperSettings\Public\Engine\DeveloperSettings.h:23
            // (UCLASS(Abstract, MinimalAPI) in module DeveloperSettings).
            // Missing this dep = LNK2019 on the constructor symbol.
            "DeveloperSettings"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "MonolithCore", "UnrealEd", "UMG", "UMGEditor",
            "Slate", "SlateCore", "Json", "JsonUtilities", "XmlParser",
            "ImageWrapper",
            "GameplayTags",
            "KismetCompiler", "MovieScene", "MovieSceneTracks",
            // Hoisted action requirements (Phase D — design effects and gradient MID factory):
            //   AssetTools                      -- CreateUniqueAssetName for new assets on disk
            //   Kismet                          -- FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified
            //   MaterialEditor                  -- UMaterialEditingLibrary::UpdateMaterialInstance
            "AssetTools",
            "Kismet",
            "MaterialEditor",
            // Automation tests and editor helpers query assets generically
            // without hardcoding optional provider mount names.
            "AssetRegistry",
            // Phase 2 (2026-05-16 UI gap audit) — Item #10 apply_token_binding
            // probes IPluginManager::Get().FindPlugin("TokenforgeRuntime") to
            // emit -32011 when the optional Tokenforge provider is absent.
            // IPluginManager lives in the Projects module.
            "Projects",
            // Phase 2 (2026-05-16 UI gap audit) — Item #8 add_widget_variable
            // references UEdGraphSchema_K2::PC_* FName constants (Boolean,
            // Byte, Class, Double, Float, Int, Int64, Real, Name, Object,
            // SoftObject, String, Text, Struct, Wildcard, Enum) when parsing
            // var_type tokens into FEdGraphPinType. The constants are
            // dllimport'd from the BlueprintGraph module.
            "BlueprintGraph"
        });

        // CommonUI optional integration — detected across 3 install vectors so the
        // public Monolith free release can ship without hard-requiring CommonUI:
        //   1. Project-local Plugins/ folder (user copied CommonUI into their project)
        //   2. Engine Plugins/Marketplace/ (Fab/marketplace install)
        //   3. Engine Plugins/Runtime/ (stock UE 5.7 — first-party Epic plugin)
        // Set MONOLITH_RELEASE_BUILD=1 to force detection off (validates WITH_COMMONUI=0 path).
        //
        // IMPORTANT: Even if CommonUI exists in the engine, we must also verify that
        // Monolith's own CommonUI source files are present. The public release zip
        // gitignores these files — without this gate, end users get WITH_COMMONUI=1
        // but missing headers (C1083). See GitHub issue #36.
        bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";

        // Self-check: do our own CommonUI source files exist? Release zips strip them.
        string OurCommonUIDir = Path.Combine(ModuleDirectory, "Public", "CommonUI");
        bool bHasOurCommonUISources = Directory.Exists(OurCommonUIDir)
            && File.Exists(Path.Combine(OurCommonUIDir, "MonolithCommonUIActions.h"));

        bool bHasCommonUI = !bReleaseBuild && bHasOurCommonUISources && IsPluginEnabled(Target, "CommonUI");

        if (bHasCommonUI)
        {
            PrivateDependencyModuleNames.AddRange(new[] { "CommonUI", "CommonInput" });
            PublicDefinitions.Add("WITH_COMMONUI=1");
        }
        else
        {
            PublicDefinitions.Add("WITH_COMMONUI=0");
        }
    }
}
