// SPDX-License-Identifier: MIT
// Contract tests for ui::build_menu_from_spec non-mutating surfaces.

#include "Misc/AutomationTest.h"

#include "Actions/MonolithUISpecActions.h"
#include "MonolithToolRegistry.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	void EnsureMenuSpecActionRegistered()
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("ui"), TEXT("build_menu_from_spec")) ||
			!Registry.HasAction(TEXT("ui"), TEXT("apply_common_menu_transform_spec")))
		{
			MonolithUI::FSpecActions::Register(Registry);
		}
	}

	TSharedPtr<FJsonObject> MakeKindOnlyScreen()
	{
		TSharedPtr<FJsonObject> Screen = MakeShared<FJsonObject>();
		Screen->SetStringField(TEXT("id"), TEXT("main"));
		Screen->SetStringField(TEXT("asset_path"), TEXT("/Game/UI/Test/WBP_MenuContract"));
		Screen->SetStringField(TEXT("kind"), TEXT("main_menu"));
		return Screen;
	}

	TSharedPtr<FJsonObject> ExecuteBuildMenuFromSpec(const TSharedPtr<FJsonObject>& Params)
	{
		EnsureMenuSpecActionRegistered();
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("ui"),
			TEXT("build_menu_from_spec"),
			Params);
		return Result.Result;
	}

	FMonolithActionResult ExecuteApplyCommonMenuTransformSpec(const TSharedPtr<FJsonObject>& Params)
	{
		EnsureMenuSpecActionRegistered();
		return FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("ui"),
			TEXT("apply_common_menu_transform_spec"),
			Params);
	}

	void AddObjectToArrayField(
		const TSharedPtr<FJsonObject>& Owner,
		const FString& FieldName,
		const TSharedPtr<FJsonObject>& Entry)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
		if (Owner->TryGetArrayField(FieldName, Existing) && Existing)
		{
			Values = *Existing;
		}
		Values.Add(MakeShared<FJsonValueObject>(Entry));
		Owner->SetArrayField(FieldName, Values);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUIApplyCommonMenuTransformSpecSchemaTest,
	"Monolith.Registry.UI.ApplyCommonMenuTransformSpecSchema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIApplyCommonMenuTransformSpecSchemaTest::RunTest(const FString& /*Parameters*/)
{
	EnsureMenuSpecActionRegistered();

	bool bFoundAction = false;
	bool bCategoryOk = false;
	bool bHasLayers = false;
	bool bHasWidgetProperties = false;
	bool bDryRunDefault = false;
	bool bConfirmDefault = false;
	bool bCompileDefault = false;
	bool bSaveDefault = false;

	for (const FMonolithActionInfo& ActionInfo : FMonolithToolRegistry::Get().GetActions(TEXT("ui")))
	{
		if (ActionInfo.Action != TEXT("apply_common_menu_transform_spec"))
		{
			continue;
		}

		bFoundAction = true;
		bCategoryOk = ActionInfo.Category == TEXT("Spec Builder");
		if (ActionInfo.ParamSchema.IsValid())
		{
			const TSharedPtr<FJsonObject>* Layers = nullptr;
			const TSharedPtr<FJsonObject>* WidgetProperties = nullptr;
			const TSharedPtr<FJsonObject>* DryRun = nullptr;
			const TSharedPtr<FJsonObject>* Confirm = nullptr;
			const TSharedPtr<FJsonObject>* Compile = nullptr;
			const TSharedPtr<FJsonObject>* Save = nullptr;
			FString DefaultValue;
			bHasLayers = ActionInfo.ParamSchema->TryGetObjectField(TEXT("layers"), Layers) && Layers && Layers->IsValid();
			bHasWidgetProperties = ActionInfo.ParamSchema->TryGetObjectField(TEXT("widget_properties"), WidgetProperties) && WidgetProperties && WidgetProperties->IsValid();
			bDryRunDefault = ActionInfo.ParamSchema->TryGetObjectField(TEXT("dry_run"), DryRun) && DryRun && DryRun->IsValid()
				&& (*DryRun)->TryGetStringField(TEXT("default"), DefaultValue) && DefaultValue == TEXT("true");
			bConfirmDefault = ActionInfo.ParamSchema->TryGetObjectField(TEXT("confirm"), Confirm) && Confirm && Confirm->IsValid()
				&& (*Confirm)->TryGetStringField(TEXT("default"), DefaultValue) && DefaultValue == TEXT("false");
			bCompileDefault = ActionInfo.ParamSchema->TryGetObjectField(TEXT("compile"), Compile) && Compile && Compile->IsValid()
				&& (*Compile)->TryGetStringField(TEXT("default"), DefaultValue) && DefaultValue == TEXT("true");
			bSaveDefault = ActionInfo.ParamSchema->TryGetObjectField(TEXT("save"), Save) && Save && Save->IsValid()
				&& (*Save)->TryGetStringField(TEXT("default"), DefaultValue) && DefaultValue == TEXT("false");
		}
		break;
	}

	TestTrue(TEXT("apply_common_menu_transform_spec registered"), bFoundAction);
	TestTrue(TEXT("apply_common_menu_transform_spec category"), bCategoryOk);
	TestTrue(TEXT("layers schema exists"), bHasLayers);
	TestTrue(TEXT("widget_properties schema exists"), bHasWidgetProperties);
	TestTrue(TEXT("dry_run defaults true"), bDryRunDefault);
	TestTrue(TEXT("confirm defaults false"), bConfirmDefault);
	TestTrue(TEXT("compile defaults true"), bCompileDefault);
	TestTrue(TEXT("save defaults false"), bSaveDefault);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUIApplyCommonMenuTransformSpecRequiresConfirmTest,
	"Monolith.ParamGuard.UI.ApplyCommonMenuTransformSpecRequiresConfirm",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIApplyCommonMenuTransformSpecRequiresConfirmTest::RunTest(const FString& /*Parameters*/)
{
	TSharedPtr<FJsonObject> Property = MakeShared<FJsonObject>();
	Property->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/UI/WBP_Missing"));
	Property->SetStringField(TEXT("widget_name"), TEXT("Title"));
	Property->SetStringField(TEXT("property_name"), TEXT("Text"));
	Property->SetStringField(TEXT("value"), TEXT("Blocked"));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	AddObjectToArrayField(Params, TEXT("widget_properties"), Property);
	Params->SetBoolField(TEXT("dry_run"), false);

	const FMonolithActionResult Result = ExecuteApplyCommonMenuTransformSpec(Params);
	TestFalse(TEXT("mutating common menu transform requires confirm=true"), Result.bSuccess);
	TestTrue(TEXT("confirm guard error is clear"), Result.ErrorMessage.Contains(TEXT("confirm=true")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUIApplyCommonMenuTransformSpecDryRunDeferredAggregationTest,
	"Monolith.UI.ApplyCommonMenuTransformSpec.DryRunDeferredAggregation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIApplyCommonMenuTransformSpecDryRunDeferredAggregationTest::RunTest(const FString& /*Parameters*/)
{
	TSharedPtr<FJsonObject> Screen = MakeShared<FJsonObject>();
	Screen->SetStringField(TEXT("id"), TEXT("host"));
	Screen->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/UI/WBP_HostMenu"));

	TSharedPtr<FJsonObject> Layer = MakeShared<FJsonObject>();
	Layer->SetStringField(TEXT("id"), TEXT("UI.Layer.Menu"));
	Layer->SetStringField(TEXT("widget_name"), TEXT("MenuStack"));

	TSharedPtr<FJsonObject> Focus = MakeShared<FJsonObject>();
	Focus->SetStringField(TEXT("screen"), TEXT("host"));
	Focus->SetStringField(TEXT("target"), TEXT("HostButton"));

	TSharedPtr<FJsonObject> Nav = MakeShared<FJsonObject>();
	Nav->SetStringField(TEXT("screen"), TEXT("host"));
	Nav->SetStringField(TEXT("widget"), TEXT("HostButton"));
	Nav->SetStringField(TEXT("direction"), TEXT("Down"));
	Nav->SetStringField(TEXT("target"), TEXT("BackButton"));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("layout_asset_path"), TEXT("/Game/Tests/Monolith/UI/WBP_PrimaryGameLayout"));
	AddObjectToArrayField(Params, TEXT("screens"), Screen);
	AddObjectToArrayField(Params, TEXT("layers"), Layer);
	AddObjectToArrayField(Params, TEXT("focus_table"), Focus);
	AddObjectToArrayField(Params, TEXT("nav_overrides"), Nav);

	const FMonolithActionResult Result = ExecuteApplyCommonMenuTransformSpec(Params);
	TestTrue(TEXT("dry-run transform returns success envelope"), Result.bSuccess);
	TestTrue(TEXT("dry-run transform returns JSON"), Result.Result.IsValid());
	if (!Result.bSuccess || !Result.Result.IsValid())
	{
		AddError(Result.ErrorMessage);
		return false;
	}

	bool bOk = false;
	bool bChanged = true;
	bool bChangedKnown = false;
	FString Status;
	double StepCount = 0.0;
	double ExecutedStepCount = 1.0;
	double PlannedOnlyStepCount = 0.0;
	TestTrue(TEXT("result exposes ok"), Result.Result->TryGetBoolField(TEXT("ok"), bOk));
	TestTrue(TEXT("dry-run ok=true"), bOk);
	TestTrue(TEXT("status exposed"), Result.Result->TryGetStringField(TEXT("status"), Status));
	TestEqual(TEXT("dry-run status planned"), Status, FString(TEXT("planned")));
	TestTrue(TEXT("step_count exposed"), Result.Result->TryGetNumberField(TEXT("step_count"), StepCount));
	TestEqual(TEXT("three deferred aggregation steps planned"), static_cast<int32>(StepCount), 3);
	TestTrue(TEXT("executed_step_count exposed"), Result.Result->TryGetNumberField(TEXT("executed_step_count"), ExecutedStepCount));
	TestEqual(TEXT("plan-only dry-run does not execute non-dry-run child writers"), static_cast<int32>(ExecutedStepCount), 0);
	TestTrue(TEXT("planned_only_step_count exposed"), Result.Result->TryGetNumberField(TEXT("planned_only_step_count"), PlannedOnlyStepCount));
	TestEqual(TEXT("all three steps are plan-only"), static_cast<int32>(PlannedOnlyStepCount), 3);
	TestTrue(TEXT("changed exposed"), Result.Result->TryGetBoolField(TEXT("changed"), bChanged));
	TestFalse(TEXT("dry-run changed=false"), bChanged);
	TestTrue(TEXT("changed_known exposed"), Result.Result->TryGetBoolField(TEXT("changed_known"), bChangedKnown));
	TestTrue(TEXT("dry-run changed is known"), bChangedKnown);

	const TSharedPtr<FJsonObject>* PlannedCounts = nullptr;
	TestTrue(TEXT("planned_counts exposed"), Result.Result->TryGetObjectField(TEXT("planned_counts"), PlannedCounts));
	if (PlannedCounts && PlannedCounts->IsValid())
	{
		double LayoutCount = 0.0;
		double FocusCount = 0.0;
		double NavCount = 0.0;
		TestTrue(TEXT("layout_layer planned"), (*PlannedCounts)->TryGetNumberField(TEXT("layout_layer"), LayoutCount));
		TestTrue(TEXT("initial_focus planned"), (*PlannedCounts)->TryGetNumberField(TEXT("initial_focus"), FocusCount));
		TestTrue(TEXT("navigation_bulk planned"), (*PlannedCounts)->TryGetNumberField(TEXT("navigation_bulk"), NavCount));
		TestEqual(TEXT("layout layer count"), static_cast<int32>(LayoutCount), 1);
		TestEqual(TEXT("focus count"), static_cast<int32>(FocusCount), 1);
		TestEqual(TEXT("navigation bulk count"), static_cast<int32>(NavCount), 1);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUIMenuSpecKindOnlyNotImplementedTest,
	"Monolith.UI.BuildMenuFromSpec.KindOnlyNotImplemented",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIMenuSpecKindOnlyNotImplementedTest::RunTest(const FString& /*Parameters*/)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Screens;
	Screens.Add(MakeShared<FJsonValueObject>(MakeKindOnlyScreen()));
	Params->SetArrayField(TEXT("screens"), Screens);

	TSharedPtr<FJsonObject> Out = ExecuteBuildMenuFromSpec(Params);
	TestTrue(TEXT("action returns a JSON payload"), Out.IsValid());
	if (!Out.IsValid())
	{
		return false;
	}

	bool bSuccess = true;
	bool bCommittedAllRequestedWork = true;
	FString Status;
	TestTrue(TEXT("payload exposes bSuccess"), Out->TryGetBoolField(TEXT("bSuccess"), bSuccess));
	TestTrue(TEXT("payload exposes bCommittedAllRequestedWork"),
		Out->TryGetBoolField(TEXT("bCommittedAllRequestedWork"), bCommittedAllRequestedWork));
	TestTrue(TEXT("payload exposes status"), Out->TryGetStringField(TEXT("status"), Status));
	TestFalse(TEXT("kind-only build is not a successful menu build"), bSuccess);
	TestFalse(TEXT("kind-only build did not commit all requested work"), bCommittedAllRequestedWork);
	TestEqual(TEXT("top-level status is incomplete_non_mutating"), Status, FString(TEXT("incomplete_non_mutating")));

	const TArray<TSharedPtr<FJsonValue>>* ScreenResults = nullptr;
	TestTrue(TEXT("payload exposes screen results"), Out->TryGetArrayField(TEXT("screens"), ScreenResults));
	TestTrue(TEXT("one screen result"), ScreenResults && ScreenResults->Num() == 1);
	if (ScreenResults && ScreenResults->Num() == 1)
	{
		const TSharedPtr<FJsonObject>* ScreenOut = nullptr;
		TestTrue(TEXT("screen result is object"), (*ScreenResults)[0]->TryGetObject(ScreenOut));
		if (ScreenOut && ScreenOut->IsValid())
		{
			bool bScreenSuccess = true;
			bool bScreenCommitted = true;
			FString ScreenStatus;
			(*ScreenOut)->TryGetBoolField(TEXT("bSuccess"), bScreenSuccess);
			(*ScreenOut)->TryGetBoolField(TEXT("bCommitted"), bScreenCommitted);
			(*ScreenOut)->TryGetStringField(TEXT("status"), ScreenStatus);
			TestFalse(TEXT("screen kind dispatch is not success"), bScreenSuccess);
			TestFalse(TEXT("screen kind dispatch is not committed"), bScreenCommitted);
			TestEqual(TEXT("screen status is not_implemented"), ScreenStatus, FString(TEXT("not_implemented")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUIMenuSpecDeferredAggregationPartialTest,
	"Monolith.UI.BuildMenuFromSpec.DeferredAggregationPartialNonMutating",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIMenuSpecDeferredAggregationPartialTest::RunTest(const FString& /*Parameters*/)
{
	TSharedPtr<FJsonObject> Layer = MakeShared<FJsonObject>();
	Layer->SetStringField(TEXT("id"), TEXT("root"));
	TArray<TSharedPtr<FJsonValue>> LayerScreens;
	LayerScreens.Add(MakeShared<FJsonValueString>(TEXT("main")));
	Layer->SetArrayField(TEXT("screens"), LayerScreens);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Screens;
	Screens.Add(MakeShared<FJsonValueObject>(MakeKindOnlyScreen()));
	Params->SetArrayField(TEXT("screens"), Screens);
	TArray<TSharedPtr<FJsonValue>> Layers;
	Layers.Add(MakeShared<FJsonValueObject>(Layer));
	Params->SetArrayField(TEXT("layers"), Layers);

	TSharedPtr<FJsonObject> Out = ExecuteBuildMenuFromSpec(Params);
	TestTrue(TEXT("action returns a JSON payload"), Out.IsValid());
	if (!Out.IsValid())
	{
		return false;
	}

	bool bSuccess = true;
	bool bCommittedAllRequestedWork = true;
	FString Status;
	Out->TryGetBoolField(TEXT("bSuccess"), bSuccess);
	Out->TryGetBoolField(TEXT("bCommittedAllRequestedWork"), bCommittedAllRequestedWork);
	Out->TryGetStringField(TEXT("status"), Status);
	TestFalse(TEXT("deferred aggregation is not a successful full build"), bSuccess);
	TestFalse(TEXT("deferred aggregation did not commit all requested work"), bCommittedAllRequestedWork);
	TestEqual(TEXT("top-level status is partial_non_mutating"), Status, FString(TEXT("partial_non_mutating")));
	const TSharedPtr<FJsonObject>* DeferredObj = nullptr;
	const bool bHasDeferred = Out->TryGetObjectField(TEXT("deferred_aggregation"), DeferredObj);
	TestTrue(TEXT("deferred aggregation is echoed"), bHasDeferred && DeferredObj && (*DeferredObj)->Values.Num() > 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
