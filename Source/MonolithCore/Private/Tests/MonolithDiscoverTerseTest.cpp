// SPDX-License-Identifier: MIT
// Automation tests for terse monolith_discover(namespace).
// Plan: Plugins/Monolith/Docs/plans/2026-06-19-terse-discover.md (Field 10)
//
// Goals:
//   - Per-namespace discover is terse by default: action + description, NO `params`.
//   - `detail=true` (canonical) inlines per-action param schema; `schema_detail=compact` is default for bulk listings.
//   - `verbose=true` is an accepted alias for `detail=true`.
//   - `planning_detail=compact` is default; `planning_detail=full` opts back into heavy planning arrays.
//   - `filter` substring-matches name OR description (case-insensitive).
//   - Default returns a bounded action page with total/cursor metadata.
//   - Pagination supports offset/limit; explicit limit=0 is normalized to the default page; out-of-range clamps.
//   - Unknown namespace still errors.
//
// Lives under Private/Tests/ for the same UBT auto-include reason as the other
// MonolithCore tests in this folder.

#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithDiscoverTerseTestDetail
{
	/** Run monolith.discover with the given params. */
	static FMonolithActionResult Discover(const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("monolith"), TEXT("discover"), Params);
	}

	/** Build a map of action name -> emitted description from a discover result's actions array. */
	static TMap<FString, FString> DescriptionsByAction(const FMonolithActionResult& R)
	{
		TMap<FString, FString> Out;
		if (!R.bSuccess || !R.Result.IsValid())
		{
			return Out;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!R.Result->TryGetArrayField(TEXT("actions"), Arr) || !Arr)
		{
			return Out;
		}
		for (const TSharedPtr<FJsonValue>& Val : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (Val.IsValid() && Val->TryGetObject(Obj) && Obj)
			{
				FString Name, Desc;
				(*Obj)->TryGetStringField(TEXT("action"), Name);
				(*Obj)->TryGetStringField(TEXT("description"), Desc);
				Out.Add(Name, Desc);
			}
		}
		return Out;
	}

	/** Full (unfiltered) action count for the monolith namespace — the discoverability baseline. */
	static int32 FullActionCount()
	{
		return FMonolithToolRegistry::Get().GetActions(TEXT("monolith")).Num();
	}

	/** Registered schema for monolith.discover, or nullptr if the action is unexpectedly absent. */
	static TSharedPtr<FJsonObject> DiscoverSchema()
	{
		for (const FMonolithActionInfo& Info : FMonolithToolRegistry::Get().GetActions(TEXT("monolith")))
		{
			if (Info.Action == TEXT("discover"))
			{
				return Info.ParamSchema;
			}
		}
		return nullptr;
	}

	static FMonolithActionResult ProjectionCapNoop(const TSharedPtr<FJsonObject>& /*Params*/)
	{
		return FMonolithActionResult::Success(MakeShared<FJsonObject>());
	}

	static void RegisterProjectionCapNamespace(const int32 Count)
	{
		FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("projectioncap"));
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FString ActionName = FString::Printf(TEXT("action_%03d"), Index);
			FMonolithToolRegistry::Get().RegisterAction(
				TEXT("projectioncap"),
				ActionName,
				FString::Printf(TEXT("Projection cap test action %03d."), Index),
				FMonolithActionHandler::CreateStatic(&ProjectionCapNoop),
				nullptr);
		}
	}

	/** Pull the `actions` array out of a successful discover result, or null. */
	static const TArray<TSharedPtr<FJsonValue>>* GetActionsArray(const FMonolithActionResult& R)
	{
		if (!R.bSuccess || !R.Result.IsValid())
		{
			return nullptr;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		R.Result->TryGetArrayField(TEXT("actions"), Arr);
		return Arr;
	}
}

// ---------------------------------------------------------------------------
// Test 0: root/default summary is bounded by namespace count, not action count.
// Namespace-scoped discover remains the explicit paginated action-list route.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDiscoverRootSummaryBoundedTest,
	"Monolith.Discover.Terse.RootSummaryBounded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDiscoverRootSummaryBoundedTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDiscoverTerseTestDetail;

	const FMonolithActionResult R = Discover(MakeShared<FJsonObject>());
	TestTrue(TEXT("root discover succeeds"), R.bSuccess);
	TestTrue(TEXT("result object present"), R.Result.IsValid());
	if (!R.bSuccess || !R.Result.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("root mode is summary"), R.Result->GetStringField(TEXT("mode")), FString(TEXT("summary")));
	TestEqual(TEXT("root projection is summary"), R.Result->GetStringField(TEXT("projection")), FString(TEXT("summary")));
	bool bTruncated = true;
	TestTrue(TEXT("truncated field present"), R.Result->TryGetBoolField(TEXT("truncated"), bTruncated));
	TestFalse(TEXT("root summary is not action-list truncated"), bTruncated);
	const TSharedPtr<FJsonObject>* Limits = nullptr;
	TestTrue(TEXT("limits object present"), R.Result->TryGetObjectField(TEXT("limits"), Limits) && Limits);

	const TArray<TSharedPtr<FJsonValue>>* Namespaces = nullptr;
	TestTrue(TEXT("namespaces array present"), R.Result->TryGetArrayField(TEXT("namespaces"), Namespaces) && Namespaces && Namespaces->Num() > 0);
	if (!Namespaces)
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Namespaces)
	{
		TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
		TestTrue(TEXT("namespace row is object"), Obj.IsValid());
		if (!Obj.IsValid())
		{
			continue;
		}
		TestTrue(TEXT("namespace row has namespace"), Obj->HasTypedField<EJson::String>(TEXT("namespace")));
		TestTrue(TEXT("namespace row has action_count"), Obj->HasField(TEXT("action_count")));
		TestTrue(TEXT("namespace row has action listing hint"), Obj->HasTypedField<EJson::String>(TEXT("actions_hint")));
		TestFalse(TEXT("namespace row omits action array in summary"), Obj->HasField(TEXT("actions")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 1: Terse default — each action obj has action+description, NO `params`;
// top-level `schema_hint` present.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDiscoverTerseDefaultTest,
	"Monolith.Discover.Terse.Default",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDiscoverTerseDefaultTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDiscoverTerseTestDetail;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("namespace"), TEXT("monolith"));

	const FMonolithActionResult R = Discover(Params);
	TestTrue(TEXT("terse discover succeeds"), R.bSuccess);

	const TArray<TSharedPtr<FJsonValue>>* Arr = GetActionsArray(R);
	TestNotNull(TEXT("actions array present"), Arr);
	if (Arr)
	{
		TestTrue(TEXT("at least one action returned"), Arr->Num() > 0);
		for (const TSharedPtr<FJsonValue>& Val : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (Val.IsValid() && Val->TryGetObject(Obj) && Obj)
			{
				TestTrue(TEXT("action obj has 'action'"), (*Obj)->HasField(TEXT("action")));
				TestTrue(TEXT("action obj has 'description'"), (*Obj)->HasField(TEXT("description")));
				TestTrue(TEXT("action obj has available_offline metadata"), (*Obj)->HasField(TEXT("available_offline")));
				TestTrue(TEXT("action obj has requires_live_editor metadata"), (*Obj)->HasField(TEXT("requires_live_editor")));
				TestTrue(TEXT("action obj has mutates_assets metadata"), (*Obj)->HasField(TEXT("mutates_assets")));
				TestTrue(TEXT("action obj has writes_logs metadata"), (*Obj)->HasField(TEXT("writes_logs")));
				TestTrue(TEXT("action obj has long_running metadata"), (*Obj)->HasField(TEXT("long_running")));
				TestTrue(TEXT("action obj has supports_progress metadata"), (*Obj)->HasField(TEXT("supports_progress")));
				TestFalse(TEXT("terse action obj has NO 'params'"), (*Obj)->HasField(TEXT("params")));
			}
		}
	}

	if (R.bSuccess && R.Result.IsValid())
	{
		TestTrue(TEXT("terse result has top-level schema_hint"), R.Result->HasField(TEXT("schema_hint")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 2: Detail opt-in — each action obj HAS `params`.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDiscoverDetailOptInTest,
	"Monolith.Discover.Terse.DetailOptIn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDiscoverDetailOptInTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDiscoverTerseTestDetail;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("namespace"), TEXT("monolith"));
	Params->SetBoolField(TEXT("detail"), true);

	const FMonolithActionResult R = Discover(Params);
	TestTrue(TEXT("detail discover succeeds"), R.bSuccess);

	const TArray<TSharedPtr<FJsonValue>>* Arr = GetActionsArray(R);
	TestNotNull(TEXT("actions array present"), Arr);
	if (Arr)
	{
		// In detail mode every monolith action that carries a ParamSchema emits
		// `params`. At least one monolith action (discover/update) has a schema, so
		// assert at least one action obj carries params.
		bool bAnyHasParams = false;
		bool bDiscoverAdvertisesCrossNamespaceFilter = false;
		for (const TSharedPtr<FJsonValue>& Val : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (Val.IsValid() && Val->TryGetObject(Obj) && Obj && (*Obj)->HasField(TEXT("params")))
			{
				bAnyHasParams = true;

				FString ActionName;
				if ((*Obj)->TryGetStringField(TEXT("action"), ActionName)
					&& ActionName == TEXT("discover"))
				{
					const TSharedPtr<FJsonObject>* DiscoverParams = nullptr;
					if ((*Obj)->TryGetObjectField(TEXT("params"), DiscoverParams)
						&& DiscoverParams
						&& DiscoverParams->IsValid())
					{
						// The cross-namespace path reuses the existing filter /
						// offset / limit parameters. Assert the schema still
						// carries exactly those, so the advertised contract
						// cannot drift from the handler.
						bDiscoverAdvertisesCrossNamespaceFilter =
							(*DiscoverParams)->HasField(TEXT("filter"))
							&& (*DiscoverParams)->HasField(TEXT("offset"))
							&& (*DiscoverParams)->HasField(TEXT("limit"));
					}
				}
			}
		}
		TestTrue(TEXT("detail mode inlines params on at least one action"), bAnyHasParams);
		TestTrue(
			TEXT("discover schema advertises the reused filter/offset/limit contract"),
			bDiscoverAdvertisesCrossNamespaceFilter);
	}

	if (R.bSuccess && R.Result.IsValid())
	{
		TestFalse(TEXT("detail result has NO schema_hint"), R.Result->HasField(TEXT("schema_hint")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 2b: Planning detail projection. Bulk action listings keep params when
// detail=true, but omit the heavy planning/schema payload unless requested.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDiscoverPlanningDetailProjectionTest,
	"Monolith.Discover.Terse.PlanningDetailProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDiscoverPlanningDetailProjectionTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDiscoverTerseTestDetail;

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("namespace"), TEXT("monolith"));
		Params->SetBoolField(TEXT("detail"), true);
		Params->SetNumberField(TEXT("limit"), 5);

		const FMonolithActionResult R = Discover(Params);
		TestTrue(TEXT("compact planning detail discover succeeds"), R.bSuccess);
		TestTrue(TEXT("compact result object present"), R.Result.IsValid());
		if (R.Result.IsValid())
		{
			TestEqual(TEXT("top-level planning detail is compact"), R.Result->GetStringField(TEXT("planning_detail")), TEXT("compact"));
			TestEqual(TEXT("top-level schema detail is compact"), R.Result->GetStringField(TEXT("schema_detail")), TEXT("compact"));
			TestTrue(TEXT("compact result includes planning detail hint"), R.Result->HasTypedField<EJson::String>(TEXT("planning_detail_hint")));
			TestTrue(TEXT("compact result includes schema detail hint"), R.Result->HasTypedField<EJson::String>(TEXT("schema_detail_hint")));
		}

		const TArray<TSharedPtr<FJsonValue>>* Arr = GetActionsArray(R);
		TestNotNull(TEXT("compact actions array present"), Arr);
		if (Arr)
		{
			TestTrue(TEXT("compact test returned at least one row"), Arr->Num() > 0);
			bool bAnyHasParams = false;
			for (const TSharedPtr<FJsonValue>& Val : *Arr)
			{
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				if (!Val.IsValid() || !Val->TryGetObject(Obj) || !Obj || !Obj->IsValid())
				{
					continue;
				}
				bAnyHasParams = bAnyHasParams || (*Obj)->HasField(TEXT("params"));
				TestEqual(TEXT("row planning detail is compact"), (*Obj)->GetStringField(TEXT("planning_detail")), TEXT("compact"));
				TestFalse(TEXT("compact row omits search_metadata"), (*Obj)->HasField(TEXT("search_metadata")));
				FString CompactDescription;
				if ((*Obj)->TryGetStringField(TEXT("description"), CompactDescription))
				{
					TestTrue(TEXT("compact row description is terse"), CompactDescription.Len() <= 153);
				}
				TestFalse(TEXT("compact row omits precondition_details"), (*Obj)->HasField(TEXT("precondition_details")));
				TestFalse(TEXT("compact row omits planning_signals"), (*Obj)->HasField(TEXT("planning_signals")));
				TestTrue(TEXT("compact row includes precondition detail count"), (*Obj)->HasField(TEXT("precondition_detail_count")));
				TestTrue(TEXT("compact row includes planning signal count"), (*Obj)->HasField(TEXT("planning_signal_count")));
				if ((*Obj)->HasField(TEXT("params")))
				{
					TestEqual(TEXT("row schema detail is compact"), (*Obj)->GetStringField(TEXT("schema_detail")), TEXT("compact"));
					const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
					TestTrue(TEXT("compact row params object exists"), (*Obj)->TryGetObjectField(TEXT("params"), ParamsObj) && ParamsObj);
					if (ParamsObj && ParamsObj->IsValid())
					{
						for (const auto& ParamPair : FMonolithJsonUtils::GetFields(*ParamsObj))
						{
							const TSharedPtr<FJsonObject>* ParamObj = nullptr;
							if (ParamPair.Value.IsValid() && ParamPair.Value->TryGetObject(ParamObj) && ParamObj && ParamObj->IsValid())
							{
								TestFalse(TEXT("compact schema omits per-param description"), (*ParamObj)->HasField(TEXT("description")));
							}
						}
					}
				}
			}
			TestTrue(TEXT("detail=true still inlines params in compact planning mode"), bAnyHasParams);
		}
	}

	{
		RegisterProjectionCapNamespace(60);

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("namespace"), TEXT("projectioncap"));
		Params->SetBoolField(TEXT("detail"), true);
		Params->SetNumberField(TEXT("limit"), 1000);

		const FMonolithActionResult R = Discover(Params);
		TestTrue(TEXT("detail projection cap discover succeeds"), R.bSuccess);
		const TArray<TSharedPtr<FJsonValue>>* Arr = GetActionsArray(R);
		TestNotNull(TEXT("detail projection cap actions array present"), Arr);
		if (Arr)
		{
			TestEqual(TEXT("detail projection cap returns default page"), Arr->Num(), 50);
		}
		if (R.Result.IsValid())
		{
			bool bTruncated = false;
			TestTrue(TEXT("detail projection cap truncated field present"), R.Result->TryGetBoolField(TEXT("truncated"), bTruncated));
			TestTrue(TEXT("detail projection cap reports truncation"), bTruncated);
			TestTrue(TEXT("detail projection cap emits next_offset"), R.Result->HasField(TEXT("next_offset")));
			const TSharedPtr<FJsonObject>* Limits = nullptr;
			TestTrue(TEXT("detail projection cap limits object present"), R.Result->TryGetObjectField(TEXT("limits"), Limits) && Limits);
			if (Limits && Limits->IsValid())
			{
				int32 LimitValue = -1;
				int32 RequestedLimitValue = -1;
				(*Limits)->TryGetNumberField(TEXT("limit"), LimitValue);
				(*Limits)->TryGetNumberField(TEXT("requested_limit"), RequestedLimitValue);
				TestEqual(TEXT("detail projection cap effective limit"), LimitValue, 50);
				TestEqual(TEXT("detail projection cap preserves requested limit"), RequestedLimitValue, 1000);
				TestTrue(TEXT("detail projection cap explains reason"), (*Limits)->HasTypedField<EJson::String>(TEXT("projection_limit_reason")));
			}
		}

		FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("projectioncap"));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("namespace"), TEXT("monolith"));
		Params->SetBoolField(TEXT("detail"), true);
		Params->SetStringField(TEXT("planning_detail"), TEXT("full"));
		Params->SetStringField(TEXT("schema_detail"), TEXT("full"));
		Params->SetNumberField(TEXT("limit"), 5);

		const FMonolithActionResult R = Discover(Params);
		TestTrue(TEXT("full planning detail discover succeeds"), R.bSuccess);
		if (R.Result.IsValid())
		{
			TestEqual(TEXT("top-level planning detail is full"), R.Result->GetStringField(TEXT("planning_detail")), TEXT("full"));
			TestEqual(TEXT("top-level schema detail is full"), R.Result->GetStringField(TEXT("schema_detail")), TEXT("full"));
			TestFalse(TEXT("full result has no compact planning hint"), R.Result->HasField(TEXT("planning_detail_hint")));
			TestFalse(TEXT("full result has no compact schema hint"), R.Result->HasField(TEXT("schema_detail_hint")));
		}

		const TArray<TSharedPtr<FJsonValue>>* Arr = GetActionsArray(R);
		TestNotNull(TEXT("full actions array present"), Arr);
		if (Arr)
		{
			TestTrue(TEXT("full test returned at least one row"), Arr->Num() > 0);
			bool bAnyFullParamDescription = false;
			for (const TSharedPtr<FJsonValue>& Val : *Arr)
			{
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				if (!Val.IsValid() || !Val->TryGetObject(Obj) || !Obj || !Obj->IsValid())
				{
					continue;
				}
				TestEqual(TEXT("row planning detail is full"), (*Obj)->GetStringField(TEXT("planning_detail")), TEXT("full"));
				TestTrue(TEXT("full row includes precondition_details"), (*Obj)->HasField(TEXT("precondition_details")));
				TestTrue(TEXT("full row includes planning_signals"), (*Obj)->HasField(TEXT("planning_signals")));
				TestFalse(TEXT("full row omits compact precondition count"), (*Obj)->HasField(TEXT("precondition_detail_count")));
				TestFalse(TEXT("full row omits compact planning count"), (*Obj)->HasField(TEXT("planning_signal_count")));
				if ((*Obj)->HasField(TEXT("params")))
				{
					TestEqual(TEXT("row schema detail is full"), (*Obj)->GetStringField(TEXT("schema_detail")), TEXT("full"));
					const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
					if ((*Obj)->TryGetObjectField(TEXT("params"), ParamsObj) && ParamsObj && ParamsObj->IsValid())
					{
						for (const auto& ParamPair : FMonolithJsonUtils::GetFields(*ParamsObj))
						{
							const TSharedPtr<FJsonObject>* ParamObj = nullptr;
							if (ParamPair.Value.IsValid()
								&& ParamPair.Value->TryGetObject(ParamObj)
								&& ParamObj
								&& ParamObj->IsValid()
								&& (*ParamObj)->HasField(TEXT("description")))
							{
								bAnyFullParamDescription = true;
							}
						}
					}
				}
			}
			TestTrue(TEXT("full schema detail includes per-param descriptions"), bAnyFullParamDescription);
		}
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("namespace"), TEXT("monolith"));
		Params->SetStringField(TEXT("planning_detail"), TEXT("verbose"));

		const FMonolithActionResult R = Discover(Params);
		TestFalse(TEXT("invalid planning_detail is rejected"), R.bSuccess);
		if (!R.bSuccess)
		{
			TestTrue(TEXT("invalid planning_detail error names the param"), R.ErrorMessage.Contains(TEXT("planning_detail")));
		}
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("namespace"), TEXT("monolith"));
		Params->SetStringField(TEXT("schema_detail"), TEXT("verbose"));

		const FMonolithActionResult R = Discover(Params);
		TestFalse(TEXT("invalid schema_detail is rejected"), R.bSuccess);
		if (!R.bSuccess)
		{
			TestTrue(TEXT("invalid schema_detail error names the param"), R.ErrorMessage.Contains(TEXT("schema_detail")));
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// Test 3: verbose alias — output is byte-IDENTICAL to detail:true (verbose is a
// registered param, so no spurious unknown-param warning is emitted).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDiscoverVerboseAliasTest,
	"Monolith.Discover.Terse.VerboseAlias",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDiscoverVerboseAliasTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDiscoverTerseTestDetail;

	TSharedPtr<FJsonObject> VerboseParams = MakeShared<FJsonObject>();
	VerboseParams->SetStringField(TEXT("namespace"), TEXT("monolith"));
	VerboseParams->SetBoolField(TEXT("verbose"), true);
	const FMonolithActionResult VerboseR = Discover(VerboseParams);
	TestTrue(TEXT("verbose discover succeeds"), VerboseR.bSuccess);

	TSharedPtr<FJsonObject> DetailParams = MakeShared<FJsonObject>();
	DetailParams->SetStringField(TEXT("namespace"), TEXT("monolith"));
	DetailParams->SetBoolField(TEXT("detail"), true);
	const FMonolithActionResult DetailR = Discover(DetailParams);
	TestTrue(TEXT("detail discover succeeds"), DetailR.bSuccess);

	if (VerboseR.bSuccess && VerboseR.Result.IsValid())
	{
		// verbose must be a recognized param — no spurious unknown-param warning.
		TestFalse(TEXT("verbose result carries no warnings"), VerboseR.Result->HasField(TEXT("warnings")));
	}

	if (VerboseR.Result.IsValid() && DetailR.Result.IsValid())
	{
		const FString VerboseStr = FMonolithJsonUtils::Serialize(VerboseR.Result);
		const FString DetailStr = FMonolithJsonUtils::Serialize(DetailR.Result);
		TestEqual(TEXT("verbose output is byte-identical to detail output"), VerboseStr, DetailStr);
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 4: filter — only actions whose name/description contains "status"
// (case-insensitive); `total` reflects the filtered count.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDiscoverFilterTest,
	"Monolith.Discover.Terse.Filter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDiscoverFilterTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDiscoverTerseTestDetail;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("namespace"), TEXT("monolith"));
	Params->SetStringField(TEXT("filter"), TEXT("status"));

	const FMonolithActionResult R = Discover(Params);
	TestTrue(TEXT("filtered discover succeeds"), R.bSuccess);

	const TArray<TSharedPtr<FJsonValue>>* Arr = GetActionsArray(R);
	TestNotNull(TEXT("actions array present"), Arr);
	if (Arr)
	{
		// Every returned action must match "status" in name OR description (case-insensitive).
		for (const TSharedPtr<FJsonValue>& Val : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (Val.IsValid() && Val->TryGetObject(Obj) && Obj)
			{
				FString Name, Desc;
				(*Obj)->TryGetStringField(TEXT("action"), Name);
				(*Obj)->TryGetStringField(TEXT("description"), Desc);
				const bool bMatches = Name.Contains(TEXT("status"), ESearchCase::IgnoreCase)
					|| Desc.Contains(TEXT("status"), ESearchCase::IgnoreCase);
				TestTrue(TEXT("filtered action matches 'status'"), bMatches);
			}
		}

		// `total` equals the filtered (returned) count when no pagination is applied.
		if (R.Result.IsValid())
		{
			int32 Total = -1;
			R.Result->TryGetNumberField(TEXT("total"), Total);
			TestEqual(TEXT("total equals filtered count"), Total, Arr->Num());
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 5: default returns a bounded page — no limit => array length <= default
// limit, total still reports full count, and cursor fields appear only when more
// data remains.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDiscoverDefaultPageTest,
	"Monolith.Discover.Terse.DefaultPage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDiscoverDefaultPageTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDiscoverTerseTestDetail;

	const int32 DefaultLimit = 50;
	const int32 Full = FullActionCount();
	TestTrue(TEXT("namespace has at least one action"), Full > 0);
	const int32 ExpectedReturned = Full < DefaultLimit ? Full : DefaultLimit;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("namespace"), TEXT("monolith"));

	const FMonolithActionResult R = Discover(Params);
	TestTrue(TEXT("default discover succeeds"), R.bSuccess);

	const TArray<TSharedPtr<FJsonValue>>* Arr = GetActionsArray(R);
	TestNotNull(TEXT("actions array present"), Arr);
	if (Arr)
	{
		TestEqual(TEXT("default array length is bounded by default limit"), Arr->Num(), ExpectedReturned);
	}
	if (R.bSuccess && R.Result.IsValid())
	{
		int32 Total = -1;
		R.Result->TryGetNumberField(TEXT("total"), Total);
		TestEqual(TEXT("total == full action count"), Total, Full);

		bool bTruncated = false;
		TestTrue(TEXT("truncated field present"), R.Result->TryGetBoolField(TEXT("truncated"), bTruncated));
		TestEqual(TEXT("truncated reflects default page remainder"), bTruncated, Full > DefaultLimit);

		const TSharedPtr<FJsonObject>* Limits = nullptr;
		TestTrue(TEXT("limits object present"), R.Result->TryGetObjectField(TEXT("limits"), Limits) && Limits);
		if (Limits)
		{
			int32 LimitValue = -1;
			int32 OffsetValue = -1;
			int32 ReturnedValue = -1;
			(*Limits)->TryGetNumberField(TEXT("limit"), LimitValue);
			(*Limits)->TryGetNumberField(TEXT("offset"), OffsetValue);
			(*Limits)->TryGetNumberField(TEXT("returned"), ReturnedValue);
			TestEqual(TEXT("limits.limit is default page size"), LimitValue, DefaultLimit);
			TestEqual(TEXT("limits.offset defaults to 0"), OffsetValue, 0);
			TestEqual(TEXT("limits.returned matches action array"), ReturnedValue, ExpectedReturned);
		}

		if (Full > DefaultLimit)
		{
			TestTrue(TEXT("next_offset present when default page truncates"), R.Result->HasField(TEXT("next_offset")));
			TestTrue(TEXT("next_cursor present when default page truncates"), R.Result->HasField(TEXT("next_cursor")));
		}
		else
		{
			TestFalse(TEXT("no next_offset when default page reaches end"), R.Result->HasField(TEXT("next_offset")));
			TestFalse(TEXT("no next_cursor when default page reaches end"), R.Result->HasField(TEXT("next_cursor")));
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 6: pagination — offset:1,limit:1 => exactly 1 action, total=full count,
// next cursor present when more remain; explicit limit:0 stays bounded;
// out-of-range offset/limit clamp without error.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDiscoverPaginationTest,
	"Monolith.Discover.Terse.Pagination",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDiscoverPaginationTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDiscoverTerseTestDetail;

	const int32 Full = FullActionCount();
	TestTrue(TEXT("namespace has multiple actions for pagination"), Full >= 2);

	// offset:1, limit:1 => exactly one action returned.
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("namespace"), TEXT("monolith"));
		Params->SetNumberField(TEXT("offset"), 1);
		Params->SetNumberField(TEXT("limit"), 1);

		const FMonolithActionResult R = Discover(Params);
		TestTrue(TEXT("paginated discover succeeds"), R.bSuccess);

		const TArray<TSharedPtr<FJsonValue>>* Arr = GetActionsArray(R);
		TestNotNull(TEXT("actions array present"), Arr);
		if (Arr)
		{
			TestEqual(TEXT("limit:1 returns exactly 1 action"), Arr->Num(), 1);
		}
		if (R.Result.IsValid())
		{
			int32 Total = -1;
			R.Result->TryGetNumberField(TEXT("total"), Total);
			TestEqual(TEXT("total still reports full count under pagination"), Total, Full);

			// More remain after offset 1 + limit 1 when Full > 2 ; equals when Full == 2.
			const bool bMoreRemain = (1 + 1) < Full;
			bool bTruncated = false;
			TestTrue(TEXT("truncated field present under pagination"), R.Result->TryGetBoolField(TEXT("truncated"), bTruncated));
			TestEqual(TEXT("truncated matches pagination remainder"), bTruncated, bMoreRemain);

			const TSharedPtr<FJsonObject>* Limits = nullptr;
			TestTrue(TEXT("limits object present under pagination"), R.Result->TryGetObjectField(TEXT("limits"), Limits) && Limits);
			if (Limits)
			{
				int32 LimitValue = -1;
				int32 OffsetValue = -1;
				int32 ReturnedValue = -1;
				(*Limits)->TryGetNumberField(TEXT("limit"), LimitValue);
				(*Limits)->TryGetNumberField(TEXT("offset"), OffsetValue);
				(*Limits)->TryGetNumberField(TEXT("returned"), ReturnedValue);
				TestEqual(TEXT("limits.limit reflects request"), LimitValue, 1);
				TestEqual(TEXT("limits.offset reflects clamped request"), OffsetValue, 1);
				TestEqual(TEXT("limits.returned reflects page length"), ReturnedValue, 1);
			}
			if (bMoreRemain)
			{
				TestTrue(TEXT("next_offset present when more remain"), R.Result->HasField(TEXT("next_offset")));
				TestTrue(TEXT("next_cursor present when more remain"), R.Result->HasField(TEXT("next_cursor")));
				int32 NextOffset = -1;
				R.Result->TryGetNumberField(TEXT("next_offset"), NextOffset);
				TestEqual(TEXT("next_offset == offset + limit"), NextOffset, 2);
				FString NextCursor;
				R.Result->TryGetStringField(TEXT("next_cursor"), NextCursor);
				TestEqual(TEXT("next_cursor == offset + limit"), NextCursor, TEXT("2"));
			}
			else
			{
				TestFalse(TEXT("no next_offset when slice reaches end"), R.Result->HasField(TEXT("next_offset")));
				TestFalse(TEXT("no next_cursor when slice reaches end"), R.Result->HasField(TEXT("next_cursor")));
			}
		}
	}

	// limit:0 is accepted for older callers, but stays bounded to the default page.
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("namespace"), TEXT("monolith"));
		Params->SetNumberField(TEXT("limit"), 0);

		const FMonolithActionResult R = Discover(Params);
		TestTrue(TEXT("limit:0 discover succeeds"), R.bSuccess);
		const TArray<TSharedPtr<FJsonValue>>* Arr = GetActionsArray(R);
		TestNotNull(TEXT("actions array present"), Arr);
		if (Arr)
		{
			TestEqual(TEXT("limit:0 returns default bounded page"), Arr->Num(), FMath::Min(50, Full));
		}
		if (R.Result.IsValid())
		{
			if (Full > 50)
			{
				TestTrue(TEXT("limit:0 emits next_offset when more remain"), R.Result->HasField(TEXT("next_offset")));
				TestTrue(TEXT("limit:0 emits next_cursor when more remain"), R.Result->HasField(TEXT("next_cursor")));
			}
			else
			{
				TestFalse(TEXT("limit:0 emits no next_offset when default page reaches end"), R.Result->HasField(TEXT("next_offset")));
				TestFalse(TEXT("limit:0 emits no next_cursor when default page reaches end"), R.Result->HasField(TEXT("next_cursor")));
			}
			bool bTruncated = true;
			TestTrue(TEXT("limit:0 truncated field present"), R.Result->TryGetBoolField(TEXT("truncated"), bTruncated));
			TestEqual(TEXT("limit:0 truncated reflects default page remainder"), bTruncated, Full > 50);

			const TSharedPtr<FJsonObject>* Limits = nullptr;
			TestTrue(TEXT("limit:0 limits object present"), R.Result->TryGetObjectField(TEXT("limits"), Limits) && Limits);
			if (Limits)
			{
				int32 LimitValue = -1;
				int32 RequestedLimitValue = -1;
				int32 ReturnedValue = -1;
				(*Limits)->TryGetNumberField(TEXT("limit"), LimitValue);
				(*Limits)->TryGetNumberField(TEXT("requested_limit"), RequestedLimitValue);
				(*Limits)->TryGetNumberField(TEXT("returned"), ReturnedValue);
				TestEqual(TEXT("limits.limit reports normalized default"), LimitValue, 50);
				TestEqual(TEXT("limits.requested_limit preserves caller value"), RequestedLimitValue, 0);
				TestEqual(TEXT("limits.returned reports bounded page length"), ReturnedValue, FMath::Min(50, Full));
				TestTrue(TEXT("limit:0 reports normalization reason"), (*Limits)->HasTypedField<EJson::String>(TEXT("normalized_limit_reason")));
			}
		}
	}

	// Out-of-range offset/limit clamp without error.
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("namespace"), TEXT("monolith"));
		Params->SetNumberField(TEXT("offset"), Full + 100);
		Params->SetNumberField(TEXT("limit"), 50);

		const FMonolithActionResult R = Discover(Params);
		TestTrue(TEXT("out-of-range pagination still succeeds (clamped)"), R.bSuccess);
		const TArray<TSharedPtr<FJsonValue>>* Arr = GetActionsArray(R);
		TestNotNull(TEXT("actions array present"), Arr);
		if (Arr)
		{
			TestEqual(TEXT("offset past end yields empty slice"), Arr->Num(), 0);
		}
		if (R.Result.IsValid())
		{
			int32 Total = -1;
			R.Result->TryGetNumberField(TEXT("total"), Total);
			TestEqual(TEXT("total unaffected by clamped offset"), Total, Full);
			TestFalse(TEXT("no next_offset when offset clamped past end"), R.Result->HasField(TEXT("next_offset")));
			TestFalse(TEXT("no next_cursor when offset clamped past end"), R.Result->HasField(TEXT("next_cursor")));
			bool bTruncated = true;
			TestTrue(TEXT("out-of-range truncated field present"), R.Result->TryGetBoolField(TEXT("truncated"), bTruncated));
			TestFalse(TEXT("out-of-range page is not truncated"), bTruncated);
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 8: terse description trim invariant (blueprint namespace).
// For every action: (a) terse description length <= HardCap + 3 (the "..."),
// and (b) the terse description with any trailing "..." removed is a prefix of
// the full (detail) description. AND at least one action whose full description
// exceeds HardCap must have a terse description ending in "..." (proves trimming
// actually fires). HardCap mirrors MonolithToolText::TerseOneLineDescription
// (150).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDiscoverTerseTrimInvariantTest,
	"Monolith.Discover.Terse.TrimInvariant",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDiscoverTerseTrimInvariantTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDiscoverTerseTestDetail;

	// Mirror the helper's HardCap; terse descriptions never exceed HardCap + len("...").
	const int32 HardCap = 150;
	const FString Ellipsis = TEXT("...");

	// blueprint is registered in-editor and has long, multi-paragraph descriptions.
	TSharedPtr<FJsonObject> TerseParams = MakeShared<FJsonObject>();
	TerseParams->SetStringField(TEXT("namespace"), TEXT("blueprint"));
	const FMonolithActionResult TerseR =
		FMonolithToolRegistry::Get().ExecuteAction(TEXT("monolith"), TEXT("discover"), TerseParams);

	TSharedPtr<FJsonObject> DetailParams = MakeShared<FJsonObject>();
	DetailParams->SetStringField(TEXT("namespace"), TEXT("blueprint"));
	DetailParams->SetBoolField(TEXT("detail"), true);
	const FMonolithActionResult DetailR =
		FMonolithToolRegistry::Get().ExecuteAction(TEXT("monolith"), TEXT("discover"), DetailParams);

	if (!TerseR.bSuccess || !DetailR.bSuccess)
	{
		// blueprint namespace not registered (e.g. headless) — skip rather than fail.
		AddInfo(TEXT("blueprint namespace unavailable; skipping trim-invariant test"));
		return true;
	}

	const TMap<FString, FString> Terse = DescriptionsByAction(TerseR);
	const TMap<FString, FString> Full = DescriptionsByAction(DetailR);
	TestTrue(TEXT("blueprint returned at least one action"), Terse.Num() > 0);

	bool bAnyTrimmed = false;
	for (const TPair<FString, FString>& Pair : Terse)
	{
		const FString& TerseDesc = Pair.Value;
		const FString* FullDescPtr = Full.Find(Pair.Key);
		TestNotNull(TEXT("detail description exists for action"), FullDescPtr);
		if (!FullDescPtr)
		{
			continue;
		}
		const FString& FullDesc = *FullDescPtr;

		// (a) terse length bounded by HardCap + len("...").
		TestTrue(TEXT("terse description length <= HardCap + 3"),
			TerseDesc.Len() <= HardCap + Ellipsis.Len());

		// (b) terse-minus-ellipsis is a prefix of the full description.
		FString Core = TerseDesc;
		const bool bEndsWithEllipsis = Core.EndsWith(Ellipsis, ESearchCase::CaseSensitive);
		if (bEndsWithEllipsis)
		{
			Core.LeftChopInline(Ellipsis.Len());
		}
		// Trimming right-trims trailing whitespace before appending "..."; a prefix
		// match after TrimEnd on the full description tolerates that boundary.
		TestTrue(TEXT("terse core is a prefix of the full description"),
			FullDesc.StartsWith(Core, ESearchCase::CaseSensitive));

		if (bEndsWithEllipsis && FullDesc.Len() > HardCap)
		{
			bAnyTrimmed = true;
		}
	}

	TestTrue(TEXT("at least one long description was trimmed with '...'"), bAnyTrimmed);
	return true;
}

// ---------------------------------------------------------------------------
// Test 7: unknown namespace — error path still fires.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDiscoverUnknownNamespaceTest,
	"Monolith.Discover.Terse.UnknownNamespace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDiscoverUnknownNamespaceTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDiscoverTerseTestDetail;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("namespace"), TEXT("definitely_not_a_real_namespace_xyz"));

	const FMonolithActionResult R = Discover(Params);
	TestFalse(TEXT("unknown namespace must error"), R.bSuccess);
	if (!R.bSuccess)
	{
		TestTrue(TEXT("error mentions the unknown namespace"),
			R.ErrorMessage.Contains(TEXT("Unknown namespace")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 9: registered schema covers every terse-discover param the handler reads.
// This catches strict-param regressions where implementation accepts an option
// but tools/list and STRICT_PARAMS=1 do not know about it.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDiscoverRegisteredSchemaCoversTerseParamsTest,
	"Monolith.Discover.Terse.RegisteredSchemaCoversParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDiscoverRegisteredSchemaCoversTerseParamsTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDiscoverTerseTestDetail;

	const TSharedPtr<FJsonObject> Schema = DiscoverSchema();
	TestTrue(TEXT("discover action has a registered param schema"), Schema.IsValid());
	if (!Schema.IsValid())
	{
		return true;
	}

	const TCHAR* ExpectedParams[] = {
		TEXT("namespace"),
		TEXT("action"),
		TEXT("category"),
		TEXT("mode"),
		TEXT("detail"),
		TEXT("verbose"),
		TEXT("planning_detail"),
		TEXT("schema_detail"),
		TEXT("filter"),
		TEXT("offset"),
		TEXT("limit")
	};
	for (const TCHAR* ParamName : ExpectedParams)
	{
		TestTrue(FString::Printf(TEXT("discover schema declares %s"), ParamName), Schema->HasField(ParamName));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Test 10: schema mode must target one action. Namespace-wide schema inlining is
// detail=true; mode=schema without action used to return terse rows while echoing
// mode=schema, which is ambiguous for clients.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDiscoverSchemaModeRequiresActionTest,
	"Monolith.Discover.Terse.SchemaModeRequiresAction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDiscoverSchemaModeRequiresActionTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDiscoverTerseTestDetail;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("namespace"), TEXT("monolith"));
	Params->SetStringField(TEXT("mode"), TEXT("schema"));

	const FMonolithActionResult R = Discover(Params);
	TestFalse(TEXT("mode=schema without action errors"), R.bSuccess);
	if (!R.bSuccess)
	{
		TestTrue(TEXT("error explains action requirement"), R.ErrorMessage.Contains(TEXT("action")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
