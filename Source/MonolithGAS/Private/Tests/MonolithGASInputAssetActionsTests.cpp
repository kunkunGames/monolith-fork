// SPDX-License-Identifier: MIT

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace MonolithGASInputAssetActionsTestDetail
{
	static const TArray<FString> RequiredActions = {
		TEXT("list_input_actions"),
		TEXT("get_input_action"),
		TEXT("create_input_action"),
		TEXT("set_input_action_properties"),
		TEXT("list_input_mapping_contexts"),
		TEXT("get_input_mapping_context"),
		TEXT("create_input_mapping_context"),
		TEXT("add_input_mapping"),
		TEXT("remove_input_mapping"),
		TEXT("validate_input_mappings")
	};

	static const TArray<FString> MutatingActions = {
		TEXT("create_input_action"),
		TEXT("set_input_action_properties"),
		TEXT("create_input_mapping_context"),
		TEXT("add_input_mapping"),
		TEXT("remove_input_mapping")
	};

	static FString MakeObjectPath(const FString& PackagePath)
	{
		return PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath);
	}

	static FMonolithActionResult Execute(
		const FString& Action,
		const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("input"), Action, Params);
	}

	static TSharedPtr<FJsonObject> MakeCreateParams(
		const FString& AssetPath,
		bool bDryRun,
		bool bConfirm)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		if (bDryRun)
		{
			Params->SetBoolField(TEXT("dry_run"), true);
		}
		if (bConfirm)
		{
			Params->SetBoolField(TEXT("confirm"), true);
		}
		Params->SetBoolField(TEXT("save"), false);
		return Params;
	}

	static TSharedPtr<FJsonObject> MakeMappingParams(
		const FString& ContextPath,
		const FString& ActionPath,
		const FString& Key,
		bool bDryRun = false,
		bool bConfirm = true)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("context_path"), ContextPath);
		Params->SetStringField(TEXT("action_path"), ActionPath);
		Params->SetStringField(TEXT("key"), Key);
		if (bDryRun)
		{
			Params->SetBoolField(TEXT("dry_run"), true);
		}
		if (bConfirm)
		{
			Params->SetBoolField(TEXT("confirm"), true);
		}
		Params->SetBoolField(TEXT("save"), false);
		return Params;
	}

	static TArray<TSharedPtr<FJsonValue>> MakeStringArray(std::initializer_list<FString> Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(static_cast<int32>(Values.size()));
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	static bool GetBool(const FMonolithActionResult& ActionResult, const TCHAR* Field, bool Default = false)
	{
		bool Value = Default;
		if (ActionResult.Result.IsValid())
		{
			ActionResult.Result->TryGetBoolField(Field, Value);
		}
		return Value;
	}

	static int32 GetInt(const FMonolithActionResult& ActionResult, const TCHAR* Field, int32 Default = 0)
	{
		double Value = static_cast<double>(Default);
		if (ActionResult.Result.IsValid())
		{
			ActionResult.Result->TryGetNumberField(Field, Value);
		}
		return static_cast<int32>(Value);
	}

	static UObject* FindTestAsset(const FString& PackagePath)
	{
		return StaticFindObject(UObject::StaticClass(), nullptr, *MakeObjectPath(PackagePath));
	}

	static void CleanupTestAsset(const FString& PackagePath)
	{
		UObject* Asset = FindTestAsset(PackagePath);
		if (!Asset)
		{
			if (UPackage* Package = FindPackage(nullptr, *PackagePath))
			{
				Package->SetDirtyFlag(false);
				Package->MarkAsGarbage();
			}
			return;
		}

		if (UInputMappingContext* Context = Cast<UInputMappingContext>(Asset))
		{
			Context->UnmapAll();
		}

		UPackage* Package = Asset->GetOutermost();
		FAssetRegistryModule::AssetDeleted(Asset);
		Asset->ClearFlags(RF_Public | RF_Standalone);
		Asset->MarkAsGarbage();
		if (Package)
		{
			Package->SetDirtyFlag(false);
			Package->MarkAsGarbage();
		}
	}

	struct FScopedInputAssets
	{
		FString ActionA;
		FString ActionB;
		FString Context;

		FScopedInputAssets()
		{
			const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			const FString Root = FString::Printf(TEXT("/Game/Tests/Monolith/Input/%s"), *Suffix);
			ActionA = Root + TEXT("/IA_Primary");
			ActionB = Root + TEXT("/IA_Secondary");
			Context = Root + TEXT("/IMC_Test");
		}

		~FScopedInputAssets()
		{
			CleanupTestAsset(Context);
			CleanupTestAsset(ActionB);
			CleanupTestAsset(ActionA);
		}

		bool Create(bool bCreateSecondary = false) const
		{
			const FMonolithActionResult PrimaryResult =
				Execute(TEXT("create_input_action"), MakeCreateParams(ActionA, false, true));
			if (!PrimaryResult.bSuccess)
			{
				return false;
			}

			if (bCreateSecondary)
			{
				const FMonolithActionResult SecondaryResult =
					Execute(TEXT("create_input_action"), MakeCreateParams(ActionB, false, true));
				if (!SecondaryResult.bSuccess)
				{
					return false;
				}
			}

			const FMonolithActionResult ContextResult =
				Execute(TEXT("create_input_mapping_context"), MakeCreateParams(Context, false, true));
			return ContextResult.bSuccess;
		}

		UInputMappingContext* GetContext() const
		{
			return Cast<UInputMappingContext>(FindTestAsset(Context));
		}

		bool HasSavedFiles() const
		{
			for (const FString& PackagePath : { ActionA, ActionB, Context })
			{
				const FString Filename = FPackageName::LongPackageNameToFilename(
					PackagePath,
					FPackageName::GetAssetPackageExtension());
				if (IFileManager::Get().FileExists(*Filename))
				{
					return true;
				}
			}
			return false;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGASInputAssetRegistrationTest,
	"Monolith.ParamGuard.GAS.InputAssets.Registration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGASInputAssetRegistrationTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithGASInputAssetActionsTestDetail;

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	bool bPassed = true;
	for (const FString& Action : RequiredActions)
	{
		bPassed &= TestTrue(
			*FString::Printf(TEXT("input.%s is registered by MonolithGAS startup"), *Action),
			Registry.HasAction(TEXT("input"), Action));
	}
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGASInputAssetWriteGateTest,
	"Monolith.ParamGuard.GAS.InputAssets.WriteGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGASInputAssetWriteGateTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithGASInputAssetActionsTestDetail;

	bool bPassed = true;
	for (const FString& Action : MutatingActions)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		if (Action == TEXT("create_input_action")
			|| Action == TEXT("set_input_action_properties")
			|| Action == TEXT("create_input_mapping_context"))
		{
			Params->SetStringField(
				TEXT("asset_path"),
				TEXT("/Game/Tests/Monolith/Input/WriteGate/Asset"));
		}
		else
		{
			Params->SetStringField(
				TEXT("context_path"),
				TEXT("/Game/Tests/Monolith/Input/WriteGate/IMC_Missing"));
			Params->SetStringField(
				TEXT("action_path"),
				TEXT("/Game/Tests/Monolith/Input/WriteGate/IA_Missing"));
			Params->SetStringField(TEXT("key"), TEXT("SpaceBar"));
		}

		const FMonolithActionResult Result = Execute(Action, Params);
		bPassed &= TestFalse(
			*FString::Printf(TEXT("input.%s rejects an unconfirmed write"), *Action),
			Result.bSuccess);
		bPassed &= TestEqual(
			*FString::Printf(TEXT("input.%s reports invalid params"), *Action),
			Result.ErrorCode,
			FMonolithJsonUtils::ErrInvalidParams);
		bPassed &= TestTrue(
			*FString::Printf(TEXT("input.%s explains the mutation gate"), *Action),
			Result.ErrorMessage.Contains(TEXT("dry_run=true"))
				&& Result.ErrorMessage.Contains(TEXT("confirm=true")));
	}
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGASInputAssetDryRunTest,
	"Monolith.ParamGuard.GAS.InputAssets.DryRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGASInputAssetDryRunTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithGASInputAssetActionsTestDetail;

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ActionPath =
		FString::Printf(TEXT("/Game/Tests/Monolith/Input/%s/IA_DryRun"), *Suffix);
	const FString ContextPath =
		FString::Printf(TEXT("/Game/Tests/Monolith/Input/%s/IMC_DryRun"), *Suffix);

	const FMonolithActionResult ActionResult =
		Execute(TEXT("create_input_action"), MakeCreateParams(ActionPath, true, false));
	const FMonolithActionResult ContextResult =
		Execute(TEXT("create_input_mapping_context"), MakeCreateParams(ContextPath, true, false));

	bool bPassed = true;
	bPassed &= TestTrue(TEXT("InputAction dry-run succeeds"), ActionResult.bSuccess);
	bPassed &= TestTrue(TEXT("InputAction dry-run predicts creation"), GetBool(ActionResult, TEXT("would_create")));
	bPassed &= TestFalse(TEXT("InputAction dry-run does not report creation"), GetBool(ActionResult, TEXT("created")));
	bPassed &= TestTrue(TEXT("InputAction dry-run is marked dry_run"), GetBool(ActionResult, TEXT("dry_run")));
	bPassed &= TestNull(TEXT("InputAction dry-run creates no object"), FindTestAsset(ActionPath));
	bPassed &= TestNull(TEXT("InputAction dry-run creates no package"), FindPackage(nullptr, *ActionPath));

	bPassed &= TestTrue(TEXT("InputMappingContext dry-run succeeds"), ContextResult.bSuccess);
	bPassed &= TestTrue(TEXT("InputMappingContext dry-run predicts creation"), GetBool(ContextResult, TEXT("would_create")));
	bPassed &= TestFalse(TEXT("InputMappingContext dry-run does not report creation"), GetBool(ContextResult, TEXT("created")));
	bPassed &= TestTrue(TEXT("InputMappingContext dry-run is marked dry_run"), GetBool(ContextResult, TEXT("dry_run")));
	bPassed &= TestNull(TEXT("InputMappingContext dry-run creates no object"), FindTestAsset(ContextPath));
	bPassed &= TestNull(TEXT("InputMappingContext dry-run creates no package"), FindPackage(nullptr, *ContextPath));
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGASInputAssetStrictParamsTest,
	"Monolith.ParamGuard.GAS.InputAssets.StrictParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGASInputAssetStrictParamsTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithGASInputAssetActionsTestDetail;

	struct FInvalidCase
	{
		FString Label;
		FString Action;
		FString ExpectedField;
		TSharedPtr<FJsonObject> Params;
	};

	TArray<FInvalidCase> Cases;

	TSharedPtr<FJsonObject> StringSave = MakeCreateParams(
		TEXT("/Game/Tests/Monolith/Input/Strict/IA_StringSave"),
		true,
		false);
	StringSave->SetStringField(TEXT("save"), TEXT("false"));
	Cases.Add({ TEXT("string save"), TEXT("create_input_action"), TEXT("save"), StringSave });

	TSharedPtr<FJsonObject> StringConfirm = MakeShared<FJsonObject>();
	StringConfirm->SetStringField(
		TEXT("asset_path"),
		TEXT("/Game/Tests/Monolith/Input/Strict/IMC_StringConfirm"));
	StringConfirm->SetStringField(TEXT("confirm"), TEXT("true"));
	Cases.Add({
		TEXT("string confirm"),
		TEXT("create_input_mapping_context"),
		TEXT("confirm"),
		StringConfirm
	});

	TSharedPtr<FJsonObject> BadContextPaths = MakeShared<FJsonObject>();
	BadContextPaths->SetArrayField(
		TEXT("context_paths"),
		{ MakeShared<FJsonValueNumber>(42.0) });
	Cases.Add({
		TEXT("numeric context path item"),
		TEXT("validate_input_mappings"),
		TEXT("context_paths[0]"),
		BadContextPaths
	});

	TSharedPtr<FJsonObject> PartialSource = MakeMappingParams(
		TEXT("/Game/Tests/Monolith/Input/Strict/IMC_Missing"),
		TEXT("/Game/Tests/Monolith/Input/Strict/IA_Missing"),
		TEXT("SpaceBar"),
		true,
		false);
	PartialSource->SetStringField(
		TEXT("source_context_path"),
		TEXT("/Game/Tests/Monolith/Input/Strict/IMC_Source"));
	Cases.Add({
		TEXT("partial source mapping"),
		TEXT("add_input_mapping"),
		TEXT("source_action_path"),
		PartialSource
	});

	TSharedPtr<FJsonObject> NullModifierArray = MakeMappingParams(
		TEXT("/Game/Tests/Monolith/Input/Strict/IMC_Missing"),
		TEXT("/Game/Tests/Monolith/Input/Strict/IA_Missing"),
		TEXT("SpaceBar"),
		true,
		false);
	NullModifierArray->SetField(TEXT("modifier_classes"), MakeShared<FJsonValueNull>());
	Cases.Add({
		TEXT("null modifier array"),
		TEXT("add_input_mapping"),
		TEXT("modifier_classes"),
		NullModifierArray
	});

	TSharedPtr<FJsonObject> OutsideGame = MakeCreateParams(
		TEXT("/Engine/Tests/IA_OutsideGame"),
		true,
		false);
	Cases.Add({
		TEXT("outside Game path"),
		TEXT("create_input_action"),
		TEXT("/Game"),
		OutsideGame
	});

	bool bPassed = true;
	for (const FInvalidCase& TestCase : Cases)
	{
		const FMonolithActionResult Result = Execute(TestCase.Action, TestCase.Params);
		bPassed &= TestFalse(*TestCase.Label, Result.bSuccess);
		bPassed &= TestEqual(
			*FString::Printf(TEXT("%s uses invalid-params"), *TestCase.Label),
			Result.ErrorCode,
			FMonolithJsonUtils::ErrInvalidParams);
		bPassed &= TestTrue(
			*FString::Printf(TEXT("%s identifies the bad contract"), *TestCase.Label),
			Result.ErrorMessage.Contains(TestCase.ExpectedField));
	}
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGASInputAssetLifecycleAndCloneTest,
	"Monolith.ParamGuard.GAS.InputAssets.LifecycleAndClone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGASInputAssetLifecycleAndCloneTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithGASInputAssetActionsTestDetail;

	FScopedInputAssets Assets;
	if (!TestTrue(TEXT("disposable input fixture is created"), Assets.Create(true)))
	{
		return false;
	}

	TSharedPtr<FJsonObject> AddPrimary = MakeMappingParams(
		Assets.Context,
		Assets.ActionA,
		TEXT("SpaceBar"));
	AddPrimary->SetArrayField(
		TEXT("modifier_classes"),
		MakeStringArray({ UInputModifierNegate::StaticClass()->GetPathName() }));
	AddPrimary->SetArrayField(
		TEXT("trigger_classes"),
		MakeStringArray({ UInputTriggerHold::StaticClass()->GetPathName() }));

	const FMonolithActionResult PrimaryResult =
		Execute(TEXT("add_input_mapping"), AddPrimary);
	bool bPassed = true;
	bPassed &= TestTrue(TEXT("explicit modifier/trigger mapping succeeds"), PrimaryResult.bSuccess);
	bPassed &= TestTrue(TEXT("primary mapping is created"), GetBool(PrimaryResult, TEXT("created")));
	bPassed &= TestEqual(TEXT("primary modifier count"), GetInt(PrimaryResult, TEXT("modifier_count")), 1);
	bPassed &= TestEqual(TEXT("primary trigger count"), GetInt(PrimaryResult, TEXT("trigger_count")), 1);
	bPassed &= TestFalse(TEXT("save=false does not write primary mapping"), GetBool(PrimaryResult, TEXT("saved")));

	TSharedPtr<FJsonObject> AddClone = MakeMappingParams(
		Assets.Context,
		Assets.ActionB,
		TEXT("Enter"));
	AddClone->SetStringField(TEXT("source_context_path"), Assets.Context);
	AddClone->SetStringField(TEXT("source_action_path"), Assets.ActionA);
	AddClone->SetStringField(TEXT("source_key"), TEXT("SpaceBar"));
	const FMonolithActionResult CloneResult =
		Execute(TEXT("add_input_mapping"), AddClone);
	bPassed &= TestTrue(TEXT("source mapping clone succeeds"), CloneResult.bSuccess);
	bPassed &= TestTrue(TEXT("clone reports source provenance"), GetBool(CloneResult, TEXT("cloned_from_source")));
	bPassed &= TestEqual(TEXT("cloned modifier count"), GetInt(CloneResult, TEXT("modifier_count")), 1);
	bPassed &= TestEqual(TEXT("cloned trigger count"), GetInt(CloneResult, TEXT("trigger_count")), 1);

	UInputMappingContext* Context = Assets.GetContext();
	if (TestNotNull(TEXT("created mapping context is loadable"), Context))
	{
		bPassed &= TestEqual(TEXT("context contains two mappings"), Context->GetMappings().Num(), 2);
		if (Context->GetMappings().Num() == 2)
		{
			bPassed &= TestEqual(TEXT("source modifier is instantiated"), Context->GetMappings()[0].Modifiers.Num(), 1);
			bPassed &= TestEqual(TEXT("source trigger is instantiated"), Context->GetMappings()[0].Triggers.Num(), 1);
			bPassed &= TestEqual(TEXT("clone modifier is instantiated"), Context->GetMappings()[1].Modifiers.Num(), 1);
			bPassed &= TestEqual(TEXT("clone trigger is instantiated"), Context->GetMappings()[1].Triggers.Num(), 1);
			if (Context->GetMappings()[1].Modifiers.Num() == 1)
			{
				bPassed &= TestTrue(
					TEXT("clone preserves modifier class"),
					Context->GetMappings()[1].Modifiers[0]->IsA(UInputModifierNegate::StaticClass()));
			}
			if (Context->GetMappings()[1].Triggers.Num() == 1)
			{
				bPassed &= TestTrue(
					TEXT("clone preserves trigger class"),
					Context->GetMappings()[1].Triggers[0]->IsA(UInputTriggerHold::StaticClass()));
			}
		}
	}

	TSharedPtr<FJsonObject> ValidateParams = MakeShared<FJsonObject>();
	ValidateParams->SetArrayField(
		TEXT("context_paths"),
		MakeStringArray({ Assets.Context }));
	const FMonolithActionResult ValidateResult =
		Execute(TEXT("validate_input_mappings"), ValidateParams);
	bPassed &= TestTrue(TEXT("mapping validation action succeeds"), ValidateResult.bSuccess);
	bPassed &= TestTrue(TEXT("different keys validate cleanly"), GetBool(ValidateResult, TEXT("valid")));
	bPassed &= TestEqual(TEXT("one context is validated"), GetInt(ValidateResult, TEXT("contexts_checked")), 1);

	const FMonolithActionResult RemoveResult = Execute(
		TEXT("remove_input_mapping"),
		MakeMappingParams(Assets.Context, Assets.ActionB, TEXT("Enter")));
	bPassed &= TestTrue(TEXT("mapping removal succeeds"), RemoveResult.bSuccess);
	bPassed &= TestEqual(TEXT("one mapping is removed"), GetInt(RemoveResult, TEXT("removed_count")), 1);
	bPassed &= TestTrue(TEXT("mapping removal reports change"), GetBool(RemoveResult, TEXT("changed")));
	bPassed &= TestEqual(
		TEXT("context retains the primary mapping"),
		Context ? Context->GetMappings().Num() : -1,
		1);
	bPassed &= TestFalse(TEXT("save=false creates no uasset files"), Assets.HasSavedFiles());
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGASInputAssetIdempotencyConflictNoOpTest,
	"Monolith.ParamGuard.GAS.InputAssets.IdempotencyConflictNoOp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGASInputAssetIdempotencyConflictNoOpTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithGASInputAssetActionsTestDetail;

	FScopedInputAssets Assets;
	if (!TestTrue(TEXT("disposable input fixture is created"), Assets.Create(true)))
	{
		return false;
	}

	UInputMappingContext* Context = Assets.GetContext();
	if (!TestNotNull(TEXT("created mapping context is loadable"), Context))
	{
		return false;
	}

	const FMonolithActionResult InitialAdd = Execute(
		TEXT("add_input_mapping"),
		MakeMappingParams(Assets.Context, Assets.ActionA, TEXT("SpaceBar")));
	bool bPassed = TestTrue(TEXT("initial mapping succeeds"), InitialAdd.bSuccess);
	bPassed &= TestEqual(TEXT("initial mapping count"), Context->GetMappings().Num(), 1);

	Context->GetOutermost()->SetDirtyFlag(false);
	const FMonolithActionResult RepeatedAdd = Execute(
		TEXT("add_input_mapping"),
		MakeMappingParams(Assets.Context, Assets.ActionA, TEXT("SpaceBar")));
	bPassed &= TestTrue(TEXT("repeated mapping call succeeds"), RepeatedAdd.bSuccess);
	bPassed &= TestFalse(TEXT("repeated mapping is a no-op"), GetBool(RepeatedAdd, TEXT("changed"), true));
	bPassed &= TestFalse(TEXT("repeated mapping predicts no change"), GetBool(RepeatedAdd, TEXT("would_change"), true));
	bPassed &= TestEqual(TEXT("repeated mapping does not duplicate"), Context->GetMappings().Num(), 1);
	bPassed &= TestFalse(
		TEXT("repeated mapping does not dirty the package"),
		Context->GetOutermost()->IsDirty());

	const FMonolithActionResult ConflictingAdd = Execute(
		TEXT("add_input_mapping"),
		MakeMappingParams(Assets.Context, Assets.ActionB, TEXT("SpaceBar")));
	bPassed &= TestTrue(TEXT("second action on the same key is authored"), ConflictingAdd.bSuccess);
	bPassed &= TestEqual(TEXT("conflicting mapping count"), Context->GetMappings().Num(), 2);

	TSharedPtr<FJsonObject> ValidateParams = MakeShared<FJsonObject>();
	ValidateParams->SetArrayField(
		TEXT("context_paths"),
		MakeStringArray({ Assets.Context }));
	const FMonolithActionResult ValidateResult =
		Execute(TEXT("validate_input_mappings"), ValidateParams);
	bPassed &= TestTrue(TEXT("conflict validation action succeeds"), ValidateResult.bSuccess);
	bPassed &= TestFalse(TEXT("same key on different actions is invalid"), GetBool(ValidateResult, TEXT("valid"), true));
	bPassed &= TestEqual(TEXT("one duplicate-key conflict is reported"), GetInt(ValidateResult, TEXT("conflicts")), 1);

	Context->GetOutermost()->SetDirtyFlag(false);
	const FMonolithActionResult MissingRemove = Execute(
		TEXT("remove_input_mapping"),
		MakeMappingParams(Assets.Context, Assets.ActionA, TEXT("Enter")));
	bPassed &= TestTrue(TEXT("removing an absent mapping succeeds as a no-op"), MissingRemove.bSuccess);
	bPassed &= TestFalse(TEXT("absent mapping removal reports no change"), GetBool(MissingRemove, TEXT("changed"), true));
	bPassed &= TestEqual(TEXT("absent mapping removal removes nothing"), GetInt(MissingRemove, TEXT("removed_count")), 0);
	bPassed &= TestEqual(TEXT("absent mapping removal predicts nothing"), GetInt(MissingRemove, TEXT("would_remove_count")), 0);
	bPassed &= TestEqual(TEXT("absent mapping removal preserves mappings"), Context->GetMappings().Num(), 2);
	bPassed &= TestFalse(
		TEXT("absent mapping removal does not dirty the package"),
		Context->GetOutermost()->IsDirty());
	bPassed &= TestFalse(TEXT("save=false creates no uasset files"), Assets.HasSavedFiles());
	return bPassed;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
