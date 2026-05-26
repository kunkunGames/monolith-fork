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
		if (!Registry.HasAction(TEXT("ui"), TEXT("build_menu_from_spec")))
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
	TestTrue(TEXT("deferred aggregation is echoed"), Out->HasField(TEXT("deferred_aggregation")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
