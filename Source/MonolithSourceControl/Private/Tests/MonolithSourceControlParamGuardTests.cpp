#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "MonolithSourceControlActions.h"
#include "MonolithTestSupport.h"
#include "MonolithToolRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	void AddValidPathArray(TSharedRef<FJsonObject> Params)
	{
		TArray<TSharedPtr<FJsonValue>> Paths;
		Paths.Add(MakeShared<FJsonValueString>(TEXT("GO.uproject")));
		Params->SetArrayField(TEXT("paths"), Paths);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithSourceControlTypedParamsTest, "Monolith.ParamValidation.MonolithSourceControl.TypedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSourceControlTypedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithScopedTestNamespace ScopedNamespace(TEXT("source_control"));

	bool bOk = FMonolithTestSupport::RunRegistryContractCases(
		*this,
		TEXT("source_control"),
		[](FMonolithToolRegistry& /*Registry*/)
		{
			FMonolithSourceControlActions::RegisterActions();
		},
		{
			{ TEXT("get_capabilities"), true, TEXT("source_control.get_capabilities registers") },
			{ TEXT("get_status"), true, TEXT("source_control.get_status registers") },
			{ TEXT("checkout"), true, TEXT("source_control.checkout registers") },
			{ TEXT("add"), true, TEXT("source_control.add registers") },
			{ TEXT("checkout_or_add"), true, TEXT("source_control.checkout_or_add registers") },
			{ TEXT("delete"), true, TEXT("source_control.delete registers") },
			{ TEXT("mark_for_delete"), true, TEXT("source_control.mark_for_delete registers") },
			{ TEXT("revert"), true, TEXT("source_control.revert registers") },
			{ TEXT("revert_unchanged"), true, TEXT("source_control.revert_unchanged registers") }
		});

	bOk &= FMonolithTestSupport::RunParamGuardCases(
		*this,
		TEXT("source_control"),
		[](FMonolithToolRegistry& /*Registry*/)
		{
			FMonolithSourceControlActions::RegisterActions();
		},
		{
			{
				TEXT("get_status"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("paths"), TEXT("GO.uproject"));
				},
				TEXT("paths"),
				TEXT("source_control.get_status rejects non-array paths")
			},
			{
				TEXT("checkout"),
				[](TSharedRef<FJsonObject> Params)
				{
					AddValidPathArray(Params);
					Params->SetStringField(TEXT("dry_run"), TEXT("true"));
				},
				TEXT("dry_run"),
				TEXT("source_control.checkout rejects non-bool dry_run")
			},
			{
				TEXT("add"),
				[](TSharedRef<FJsonObject> Params)
				{
					AddValidPathArray(Params);
					Params->SetNumberField(TEXT("dry_run"), 1.0);
				},
				TEXT("dry_run"),
				TEXT("source_control.add rejects numeric dry_run")
			},
			{
				TEXT("checkout_or_add"),
				[](TSharedRef<FJsonObject> Params)
				{
					AddValidPathArray(Params);
					Params->SetStringField(TEXT("dry_run"), TEXT("false"));
				},
				TEXT("dry_run"),
				TEXT("source_control.checkout_or_add rejects non-bool dry_run")
			},
			{
				TEXT("delete"),
				[](TSharedRef<FJsonObject> Params)
				{
					AddValidPathArray(Params);
					Params->SetStringField(TEXT("confirm"), TEXT("yes"));
				},
				TEXT("confirm"),
				TEXT("source_control.delete rejects non-bool confirm")
			},
			{
				TEXT("mark_for_delete"),
				[](TSharedRef<FJsonObject> Params)
				{
					AddValidPathArray(Params);
					Params->SetNumberField(TEXT("dry_run"), 1.0);
				},
				TEXT("dry_run"),
				TEXT("source_control.mark_for_delete rejects numeric dry_run")
			},
			{
				TEXT("revert"),
				[](TSharedRef<FJsonObject> Params)
				{
					AddValidPathArray(Params);
					Params->SetStringField(TEXT("confirm"), TEXT("yes"));
				},
				TEXT("confirm"),
				TEXT("source_control.revert rejects non-bool confirm")
			},
			{
				TEXT("revert_unchanged"),
				[](TSharedRef<FJsonObject> Params)
				{
					AddValidPathArray(Params);
					Params->SetStringField(TEXT("dry_run"), TEXT("true"));
				},
				TEXT("dry_run"),
				TEXT("source_control.revert_unchanged rejects non-bool dry_run")
			}
		});

	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
