#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithSettings.h"
#include "MonolithSlateInspectorActions.h"
#include "MonolithTestSupport.h"
#include "MonolithToolRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithSlateTypedParamsTest, "Monolith.ParamValidation.MonolithSlate.TypedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSlateTypedParamsTest::RunTest(const FString& Parameters)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	const bool bOriginalEnabled = Settings->bEnableSlateInspectorActions;
	Settings->bEnableSlateInspectorActions = true;

	FMonolithScopedTestNamespace ScopedNamespace(TEXT("slate"));

	auto RegisterSlateActions = [](FMonolithToolRegistry& Registry)
	{
		MonolithSlate::FSlateInspectorActions::Register(Registry);
	};

	bool bOk = FMonolithTestSupport::RunRegistryContractCases(
		*this,
		TEXT("slate"),
		RegisterSlateActions,
		{
			{ TEXT("get_inspector_status"), true, TEXT("slate.get_inspector_status is registered") },
			{ TEXT("list_windows"), true, TEXT("slate.list_windows is registered") },
			{ TEXT("snapshot_widgets"), true, TEXT("slate.snapshot_widgets is registered") },
			{ TEXT("describe_widget"), true, TEXT("slate.describe_widget is registered") },
			{ TEXT("capture_widget"), true, TEXT("slate.capture_widget is registered") },
			{ TEXT("wait_for_widget"), true, TEXT("slate.wait_for_widget is registered") }
		});

	bOk &= FMonolithTestSupport::RunParamGuardCases(
		*this,
		TEXT("slate"),
		RegisterSlateActions,
		{
			{
				TEXT("list_windows"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("include_titles"), 42.0);
				},
				TEXT("include_titles"),
				TEXT("slate.list_windows rejects non-bool include_titles")
			},
			{
				TEXT("snapshot_widgets"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("max_widgets"), 0.0);
				},
				TEXT("max_widgets"),
				TEXT("slate.snapshot_widgets rejects max_widgets below range")
			},
			{
				TEXT("snapshot_widgets"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("max_depth"), 33.0);
				},
				TEXT("max_depth"),
				TEXT("slate.snapshot_widgets rejects max_depth above range")
			},
			{
				TEXT("describe_widget"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("ref"), 42.0);
				},
				TEXT("ref"),
				TEXT("slate.describe_widget rejects non-string ref")
			},
			{
				TEXT("capture_widget"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("max_bytes"), 0.0);
				},
				TEXT("max_bytes"),
				TEXT("slate.capture_widget rejects max_bytes below range")
			},
			{
				TEXT("wait_for_widget"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("type"), 42.0);
				},
				TEXT("type"),
				TEXT("slate.wait_for_widget rejects non-string type")
			},
			{
				TEXT("wait_for_widget"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("poll_interval_ms"), 15.0);
				},
				TEXT("poll_interval_ms"),
				TEXT("slate.wait_for_widget rejects poll_interval_ms below range")
			}
		});

	Settings->bEnableSlateInspectorActions = bOriginalEnabled;
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
