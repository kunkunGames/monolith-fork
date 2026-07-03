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
		Paths.Add(MakeShared<FJsonValueString>(TEXT("Project.uproject")));
		Params->SetArrayField(TEXT("paths"), Paths);
	}

	bool ExpectActionSuccess(FAutomationTestBase& Test, const TCHAR* Action, const TSharedPtr<FJsonObject>& Params)
	{
		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("source_control"), Action, Params);
		return Test.TestTrue(
			*FString::Printf(TEXT("source_control.%s accepts tolerant params"), Action),
			Result.bSuccess);
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
			{ TEXT("revert_unchanged"), true, TEXT("source_control.revert_unchanged registers") },
			{ TEXT("list_opened"), true, TEXT("source_control.list_opened registers") },
			{ TEXT("map_depot_paths"), true, TEXT("source_control.map_depot_paths registers") }
		});

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("source_control"));

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
					Params->SetNumberField(TEXT("paths"), 1.0);
				},
				TEXT("paths"),
				TEXT("source_control.get_status rejects non-string/non-array paths")
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
					Params->SetStringField(TEXT("dry_run"), TEXT("later"));
				},
				TEXT("dry_run"),
				TEXT("source_control.checkout_or_add rejects malformed dry_run string")
			},
			{
				TEXT("delete"),
				[](TSharedRef<FJsonObject> Params)
				{
					AddValidPathArray(Params);
					Params->SetStringField(TEXT("confirm"), TEXT("sure"));
				},
				TEXT("confirm"),
				TEXT("source_control.delete rejects malformed confirm string")
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
					Params->SetStringField(TEXT("confirm"), TEXT("sure"));
				},
				TEXT("confirm"),
				TEXT("source_control.revert rejects malformed confirm string")
			},
			{
				TEXT("revert_unchanged"),
				[](TSharedRef<FJsonObject> Params)
				{
					AddValidPathArray(Params);
					Params->SetStringField(TEXT("dry_run"), TEXT("later"));
				},
				TEXT("dry_run"),
				TEXT("source_control.revert_unchanged rejects malformed dry_run string")
			},
			{
				TEXT("list_opened"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetStringField(TEXT("resolve_packages"), TEXT("later"));
				},
				TEXT("resolve_packages"),
				TEXT("source_control.list_opened rejects malformed resolve_packages string")
			},
			{
				TEXT("map_depot_paths"),
				[](TSharedRef<FJsonObject> Params)
				{
					Params->SetNumberField(TEXT("paths"), 1.0);
				},
				TEXT("paths"),
				TEXT("source_control.map_depot_paths rejects non-string/non-array paths")
			}
		});

	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithSourceControlInputToleranceTest, "Monolith.ParamValidation.MonolithSourceControl.InputTolerance", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSourceControlInputToleranceTest::RunTest(const FString& Parameters)
{
	FMonolithScopedTestNamespace ScopedNamespace(TEXT("source_control"));
	FMonolithSourceControlActions::RegisterActions();

	bool bOk = true;

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("files"), TEXT("Project.uproject"));
		bOk &= ExpectActionSuccess(*this, TEXT("get_status"), Params);
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("files"), TEXT("Project.uproject"));
		Params->SetStringField(TEXT("dry_run"), TEXT("true"));
		bOk &= ExpectActionSuccess(*this, TEXT("checkout"), Params);
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("paths"), TEXT("Project.uproject"));
		Params->SetStringField(TEXT("dry_run"), TEXT("yes"));
		bOk &= ExpectActionSuccess(*this, TEXT("delete"), Params);
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("paths"), TEXT("Project.uproject"));
		Params->SetStringField(TEXT("dry_run"), TEXT("1"));
		bOk &= ExpectActionSuccess(*this, TEXT("checkout_or_add"), Params);
	}

	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("files"), TEXT("Project.uproject"));
		bOk &= ExpectActionSuccess(*this, TEXT("map_depot_paths"), Params);
	}

	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
