// SPDX-License-Identifier: MIT
// Plan: Plugins/Monolith/Docs/plans/2026-05-28-reflection-intelligence.md (Phase 2 — v0.17.0).
//
// ModuleDepRealityTests — Phase 2 §12 test 5 (module-dep reality audit).
//
// The audit action's `HandleAuditModuleDepReality` reads from the project's
// real source tree + the live EngineSource.db. Unit-testing it directly would
// require building a parallel `symbols`/`files`/`modules` mock that mirrors
// MonolithSource's exact schema. Phase 2 §12 test 5 explicitly asks for a
// fixture-driven test, which we implement by:
//
//   1. Asserting the Build.cs string-array parser captures the expected dep
//      identifiers from the sample fixture file. (Direct unit test of the
//      parsing regex — sufficient to gate the bug class without scaffolding
//      a full symbols index.)
//   2. Asserting the audit action's registration is visible in the registry.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Internationalization/Regex.h"
#include "Interfaces/IPluginManager.h"
#include "MonolithToolRegistry.h"
#include "SourceAudit/ModuleDepRealityUtils.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace MonolithModDepTestDetail
{
	static FString GetFixturePath(const FString& Sub)
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
		if (Plugin.IsValid())
		{
			return Plugin->GetBaseDir()
				/ TEXT("Source") / TEXT("MonolithReflectionIntel")
				/ TEXT("Private") / TEXT("Tests") / TEXT("Fixtures") / Sub;
		}
		return FPaths::ProjectPluginsDir()
			/ TEXT("Monolith") / TEXT("Source") / TEXT("MonolithReflectionIntel")
			/ TEXT("Private") / TEXT("Tests") / TEXT("Fixtures") / Sub;
	}
}

// ---------------------------------------------------------------------------
// Test: sample Build.cs string-list extraction — patterns lifted from the
// FModuleDepRealityAdapter.cpp anonymous namespace so the parser stays in
// sync with the audit's regex.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FModuleDepRealitySampleBuildCsParseTest,
	"Monolith.ReflectionIntel.SourceAudit.SampleBuildCsParse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FModuleDepRealitySampleBuildCsParseTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithModDepTestDetail;

	// Fixture path uses `.Build.cs.fixture` extension so UBT's recursive
	// Build.cs glob does not try to compile a phantom module from it.
	const FString Sample = GetFixturePath(FPaths::Combine(TEXT("RiskCorpus"), TEXT("sample.Build.cs.fixture")));
	if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*Sample))
	{
		AddError(FString::Printf(
			TEXT("Sample Build.cs fixture missing at '%s' — Phase 2 fixture invariant violated"),
			*Sample));
		return false;
	}

	FString Text;
	TestTrue(TEXT("LoadFileToString(sample.Build.cs)"),
		FFileHelper::LoadFileToString(Text, *Sample));

	// Mirror FModuleDepRealityAdapter.cpp's site + string patterns.
	const FRegexPattern SitePattern(
		TEXT("(Public|Private)DependencyModuleNames\\s*\\.\\s*(?:Add|AddRange)\\s*\\(([\\s\\S]*?)\\)"));
	const FRegexPattern DepStringPattern(
		TEXT("\"([A-Za-z_][A-Za-z0-9_]*)\""));

	TSet<FString> Captured;
	FRegexMatcher SiteM(SitePattern, Text);
	while (SiteM.FindNext())
	{
		const FString Body = SiteM.GetCaptureGroup(2);
		FRegexMatcher StringM(DepStringPattern, Body);
		while (StringM.FindNext())
		{
			Captured.Add(StringM.GetCaptureGroup(1));
		}
	}

	// Fixture is documented to declare these (see sample.Build.cs):
	TestTrue(TEXT("Captured Core"),         Captured.Contains(TEXT("Core")));
	TestTrue(TEXT("Captured CoreUObject"),  Captured.Contains(TEXT("CoreUObject")));
	TestTrue(TEXT("Captured Engine"),       Captured.Contains(TEXT("Engine")));
	// The sample DOES NOT declare GameplayTags. The audit emits a violation
	// for any UPROPERTY using FGameplayTag in a .h/.cpp within this module —
	// the fixture cpp file uses FGameplayTag. We assert the SET does NOT
	// contain GameplayTags here so the violation gate has a non-trivial assert.
	TestFalse(TEXT("Did NOT capture GameplayTags (bug-class trigger)"),
		Captured.Contains(TEXT("GameplayTags")));

	return true;
}

// ---------------------------------------------------------------------------
// Test: registration smoke — `source_query("audit_module_dep_reality")` is
// reachable from the registry after module init.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FModuleDepRealityRegistrationTest,
	"Monolith.ReflectionIntel.SourceAudit.Registration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FModuleDepRealityRegistrationTest::RunTest(const FString& /*Parameters*/)
{
	const FMonolithToolRegistry& Reg = FMonolithToolRegistry::Get();
	TestTrue(TEXT("source.audit_module_dep_reality registered"),
		Reg.HasAction(TEXT("source"), TEXT("audit_module_dep_reality")));
	return true;
}

// ---------------------------------------------------------------------------
// Test: SuggestBuildCsDepsForward (item 6) — registration smoke + path-first
// declaring-module derivation.
//
// Mirrors the SampleBuildCsParse idiom: the path-derivation logic lives in the
// adapter's anonymous namespace, so we re-assert the same parse here to keep
// the contract pinned (LAST `/Source/` wins so a Plugins/<X>/Source/<Module>/
// path resolves to <Module>, not <X>). Full forward-resolution needs a live
// EngineSource.db; that is covered by the MCP smoke in plan §12.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FModuleDepSuggestBuildCsDepsForwardTest,
	"Monolith.ReflectionIntel.SourceAudit.SuggestBuildCsDepsForward",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FModuleDepSuggestBuildCsDepsForwardTest::RunTest(const FString& /*Parameters*/)
{
	const FMonolithToolRegistry& Reg = FMonolithToolRegistry::Get();
	TestTrue(TEXT("source.suggest_build_cs_deps registered"),
		Reg.HasAction(TEXT("source"), TEXT("suggest_build_cs_deps")));

	auto DeriveModule = [this](const FString& InPath) -> FString
	{
		FString ModuleName;
		FString ModuleDir;
		const bool bDerived =
			MonolithModuleDepReality::DeriveModuleFromSourcePath(
				InPath,
				ModuleName,
				ModuleDir);
		if (!bDerived)
		{
			TestTrue(
				*FString::Printf(TEXT("Expected module path to derive: %s"), *InPath),
				false);
		}
		return ModuleName;
	};
	auto DeriveIndexedModule =
		[this](const FString& InPath, const FString& IndexedModuleName) -> FString
	{
		FString ModuleName;
		FString ModuleDir;
		const bool bDerived =
			MonolithModuleDepReality::DeriveModuleFromIndexedSourcePath(
				InPath,
				IndexedModuleName,
				ModuleName,
				ModuleDir);
		if (!bDerived)
		{
			TestTrue(
				*FString::Printf(TEXT("Expected indexed module path to derive: %s"), *InPath),
				false);
		}
		return ModuleName;
	};

	TestEqual(TEXT("Source/<Module>/ derivation"),
		DeriveModule(TEXT("D:/Proj/Source/MyMod/Public/Thing.h")), FString(TEXT("MyMod")));
	TestEqual(TEXT("Plugins/<X>/Source/<Module>/ derivation (innermost wins)"),
		DeriveModule(TEXT("D:/Proj/Plugins/Foo/Source/FooRuntime/Private/Bar.cpp")),
		FString(TEXT("FooRuntime")));
	TestEqual(TEXT("backslash path normalised"),
		DeriveModule(TEXT("C:\\Proj\\Source\\WinMod\\X.h")), FString(TEXT("WinMod")));
	TestEqual(TEXT("relative Source/<Module>/ derivation"),
		DeriveModule(TEXT("Source/RelativeMod/Private/Thing.cpp")),
		FString(TEXT("RelativeMod")));
	TestEqual(TEXT("project module literally named Runtime is not an engine category"),
		DeriveModule(TEXT("D:/Proj/Source/Runtime/Private/Thing.cpp")),
		FString(TEXT("Runtime")));
	TestEqual(TEXT("relative module literally named Editor is not an engine category"),
		DeriveModule(TEXT("Source/Editor/Public/Thing.h")),
		FString(TEXT("Editor")));
	TestEqual(TEXT("relative indexed engine runtime path uses module segment"),
		DeriveIndexedModule(
			TEXT("Source/Runtime/GameplayTags/Classes/GameplayTagContainer.h"),
			TEXT("GameplayTags")),
		FString(TEXT("GameplayTags")));
	TestEqual(TEXT("Engine-relative indexed runtime path uses module segment"),
		DeriveIndexedModule(
			TEXT("Engine/Source/Runtime/GameplayTags/Classes/GameplayTagContainer.h"),
			TEXT("GameplayTags")),
		FString(TEXT("GameplayTags")));
	TestEqual(TEXT("relative indexed engine editor path uses module segment"),
		DeriveIndexedModule(
			TEXT("Source/Editor/UnrealEd/Public/Editor.h"),
			TEXT("UnrealEd")),
		FString(TEXT("UnrealEd")));
	TestEqual(TEXT("indexed project module named Runtime remains direct"),
		DeriveIndexedModule(
			TEXT("Source/Runtime/Private/Thing.cpp"),
			TEXT("Runtime")),
		FString(TEXT("Runtime")));
	const FString EngineCoreHeader =
		FPaths::ConvertRelativePathToFull(
			FPaths::EngineDir()
			/ TEXT("Source/Runtime/Core/Public/CoreMinimal.h"));
	TestEqual(TEXT("engine source category skips to module"),
		DeriveModule(EngineCoreHeader),
		FString(TEXT("Core")));
	TestEqual(TEXT("engine plugin uses direct module segment"),
		DeriveModule(TEXT("D:/Engine/Engine/Plugins/Foo/Source/FooRuntime/Public/Foo.h")),
		FString(TEXT("FooRuntime")));

	FString NoModuleName;
	FString NoModuleDir;
	TestFalse(
		TEXT("path outside Source is rejected"),
		MonolithModuleDepReality::DeriveModuleFromSourcePath(
			TEXT("D:/Proj/Content/Foo.uasset"),
			NoModuleName,
			NoModuleDir));

	TestTrue(
		TEXT("class is a dependency type kind"),
		MonolithModuleDepReality::IsDependencyTypeSymbolKind(TEXT("class")));
	TestTrue(
		TEXT("struct is a dependency type kind"),
		MonolithModuleDepReality::IsDependencyTypeSymbolKind(TEXT("struct")));
	TestTrue(
		TEXT("enum is a dependency type kind"),
		MonolithModuleDepReality::IsDependencyTypeSymbolKind(TEXT("enum")));
	TestTrue(
		TEXT("union is a dependency type kind"),
		MonolithModuleDepReality::IsDependencyTypeSymbolKind(TEXT("union")));
	TestTrue(
		TEXT("typedef is a dependency type kind"),
		MonolithModuleDepReality::IsDependencyTypeSymbolKind(TEXT("typedef")));
	TestTrue(
		TEXT("type alias is a dependency type kind"),
		MonolithModuleDepReality::IsDependencyTypeSymbolKind(TEXT("type_alias")));
	TestTrue(
		TEXT("dependency type kind comparison is case-insensitive"),
		MonolithModuleDepReality::IsDependencyTypeSymbolKind(TEXT("STRUCT")));
	TestFalse(
		TEXT("function is not a dependency type kind"),
		MonolithModuleDepReality::IsDependencyTypeSymbolKind(TEXT("function")));
	TestFalse(
		TEXT("macro is not a dependency type kind"),
		MonolithModuleDepReality::IsDependencyTypeSymbolKind(TEXT("macro")));
	TestFalse(
		TEXT("UPROPERTY macro is not a dependency candidate"),
		MonolithModuleDepReality::IsDependencyCandidateIdentifier(TEXT("UPROPERTY")));
	TestFalse(
		TEXT("UCLASS macro is not a dependency candidate"),
		MonolithModuleDepReality::IsDependencyCandidateIdentifier(TEXT("UCLASS")));
	TestTrue(
		TEXT("reflected UObject type remains a dependency candidate"),
		MonolithModuleDepReality::IsDependencyCandidateIdentifier(TEXT("UObject")));
	TestTrue(
		TEXT("reflected FAssetData struct remains a dependency candidate"),
		MonolithModuleDepReality::IsDependencyCandidateIdentifier(TEXT("FAssetData")));

	return true;
}

// ---------------------------------------------------------------------------
// Test: risk_query registration — all 5 actions present.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRiskQueryRegistrationTest,
	"Monolith.ReflectionIntel.Risk.QueryRegistration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRiskQueryRegistrationTest::RunTest(const FString& /*Parameters*/)
{
	const FMonolithToolRegistry& Reg = FMonolithToolRegistry::Get();
	TestTrue(TEXT("risk.get_hotspot_score registered"),
		Reg.HasAction(TEXT("risk"), TEXT("get_hotspot_score")));
	TestTrue(TEXT("risk.get_cochange_pairs registered"),
		Reg.HasAction(TEXT("risk"), TEXT("get_cochange_pairs")));
	TestTrue(TEXT("risk.get_file_churn registered"),
		Reg.HasAction(TEXT("risk"), TEXT("get_file_churn")));
	TestTrue(TEXT("risk.get_release_window_hotspots registered"),
		Reg.HasAction(TEXT("risk"), TEXT("get_release_window_hotspots")));
	TestTrue(TEXT("risk.list_conditional_gates registered"),
		Reg.HasAction(TEXT("risk"), TEXT("list_conditional_gates")));
	return true;
}


// ---------------------------------------------------------------------------
// Test: param guard — malformed params reject gracefully.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FModuleDepRealityParamGuardTest,
	"Monolith.ParamGuard.ReflectionIntel.ModuleDepReality",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FModuleDepRealityParamGuardTest::RunTest(const FString& /*Parameters*/)
{
	FMonolithToolRegistry& Reg = FMonolithToolRegistry::Get();

	// Test source.audit_module_dep_reality
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("scan_root"), 123.0); // malformed, should be string
		const FMonolithActionResult Result = Reg.ExecuteAction(TEXT("source"), TEXT("audit_module_dep_reality"), Params);
		TestFalse(TEXT("audit_module_dep_reality rejects malformed scan_root"), Result.bSuccess);
		TestTrue(TEXT("audit_module_dep_reality names scan_root in error"), Result.ErrorMessage.Contains(TEXT("scan_root")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("limit"), TEXT("10")); // malformed, should be number
		const FMonolithActionResult Result = Reg.ExecuteAction(TEXT("source"), TEXT("audit_module_dep_reality"), Params);
		TestFalse(TEXT("audit_module_dep_reality rejects malformed limit"), Result.bSuccess);
		TestTrue(TEXT("audit_module_dep_reality names limit in error"), Result.ErrorMessage.Contains(TEXT("limit")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 10.5); // malformed, schema requires an integer
		const FMonolithActionResult Result = Reg.ExecuteAction(TEXT("source"), TEXT("audit_module_dep_reality"), Params);
		TestFalse(TEXT("audit_module_dep_reality rejects fractional limit"), Result.bSuccess);
		TestTrue(TEXT("fractional-limit error names limit"), Result.ErrorMessage.Contains(TEXT("limit")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("cursor"), 123.0); // malformed, should be string
		const FMonolithActionResult Result = Reg.ExecuteAction(TEXT("source"), TEXT("audit_module_dep_reality"), Params);
		TestFalse(TEXT("audit_module_dep_reality rejects malformed cursor"), Result.bSuccess);
		TestTrue(TEXT("audit_module_dep_reality names cursor in error"), Result.ErrorMessage.Contains(TEXT("cursor")));
	}

	// Test source.suggest_build_cs_deps
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("file_path"), 123.0); // malformed, should be string
		const FMonolithActionResult Result = Reg.ExecuteAction(TEXT("source"), TEXT("suggest_build_cs_deps"), Params);
		TestFalse(TEXT("suggest_build_cs_deps rejects malformed file_path"), Result.bSuccess);
		TestTrue(TEXT("suggest_build_cs_deps names file_path in error"), Result.ErrorMessage.Contains(TEXT("file_path")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Symbols;
		Symbols.Add(MakeShared<FJsonValueString>(TEXT("UObject")));
		Symbols.Add(MakeShared<FJsonValueNumber>(123.0));
		Params->SetArrayField(TEXT("symbols"), Symbols);
		const FMonolithActionResult Result = Reg.ExecuteAction(TEXT("source"), TEXT("suggest_build_cs_deps"), Params);
		TestFalse(TEXT("suggest_build_cs_deps rejects non-string symbol elements"), Result.bSuccess);
		TestTrue(TEXT("symbol-element error identifies symbols[1]"), Result.ErrorMessage.Contains(TEXT("symbols[1]")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Symbols;
		Symbols.Add(MakeShared<FJsonValueString>(TEXT("   ")));
		Params->SetArrayField(TEXT("symbols"), Symbols);
		const FMonolithActionResult Result = Reg.ExecuteAction(TEXT("source"), TEXT("suggest_build_cs_deps"), Params);
		TestFalse(TEXT("suggest_build_cs_deps rejects empty symbol elements"), Result.bSuccess);
		TestTrue(TEXT("empty-symbol error identifies symbols[0]"), Result.ErrorMessage.Contains(TEXT("symbols[0]")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
