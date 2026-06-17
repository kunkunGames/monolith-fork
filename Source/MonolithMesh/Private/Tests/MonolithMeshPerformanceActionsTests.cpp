#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithMeshPerformanceActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	enum class EPerformancePayloadState
	{
		Available,
		NoEditorWorld,
		Failed
	};

	static TArray<TSharedPtr<FJsonValue>> MakeVector(double X, double Y, double Z)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(3);
		Out.Add(MakeShared<FJsonValueNumber>(X));
		Out.Add(MakeShared<FJsonValueNumber>(Y));
		Out.Add(MakeShared<FJsonValueNumber>(Z));
		return Out;
	}

	static TSharedPtr<FJsonObject> MakeFarRegionParams()
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("region_min"), MakeVector(100000000.0, 100000000.0, 100000000.0));
		Params->SetArrayField(TEXT("region_max"), MakeVector(100000100.0, 100000100.0, 100000100.0));
		return Params;
	}

	static TSharedPtr<FJsonObject> MakeFarCameraParams()
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("viewpoint"), MakeVector(100000000.0, 0.0, 0.0));
		Params->SetArrayField(TEXT("view_direction"), MakeVector(1.0, 0.0, 0.0));
		Params->SetNumberField(TEXT("fov"), 90.0);
		return Params;
	}

	static const TCHAR* const PerformanceActionNames[] =
	{
		TEXT("get_region_performance"),
		TEXT("estimate_placement_cost"),
		TEXT("find_overdraw_hotspots"),
		TEXT("analyze_shadow_cost"),
		TEXT("get_triangle_budget")
	};

	static bool ArePerformanceActionsRegistered()
	{
		for (const TCHAR* ActionName : PerformanceActionNames)
		{
			if (!FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), ActionName))
			{
				return false;
			}
		}

		return true;
	}

	static void EnsurePerformanceActionsRegistered()
	{
		if (!ArePerformanceActionsRegistered())
		{
			FMonolithMeshPerformanceActions::RegisterActions(FMonolithToolRegistry::Get());
		}
	}

	static EPerformancePayloadState RequireWorldActionPayload(
		FAutomationTestBase& Test,
		const TCHAR* ActionName,
		const FMonolithActionResult& Result,
		TSharedPtr<FJsonObject>& OutPayload)
	{
		if (!Result.bSuccess && Result.ErrorMessage.Contains(TEXT("No editor world available")))
		{
			Test.AddInfo(FString::Printf(
				TEXT("%s reached handler validation and reported no editor world; payload assertions are skipped in non-world automation contexts."),
				ActionName));
			return EPerformancePayloadState::NoEditorWorld;
		}

		if (!Test.TestTrue(FString::Printf(TEXT("%s succeeds"), ActionName), Result.bSuccess))
		{
			Test.AddError(FString::Printf(TEXT("%s error: %s"), ActionName, *Result.ErrorMessage));
			return EPerformancePayloadState::Failed;
		}

		if (!Test.TestNotNull(FString::Printf(TEXT("%s returns a payload"), ActionName), Result.Result.Get()))
		{
			return EPerformancePayloadState::Failed;
		}

		OutPayload = Result.Result;
		return EPerformancePayloadState::Available;
	}

	static bool TestNumberFieldEquals(
		FAutomationTestBase& Test,
		const TSharedPtr<FJsonObject>& Payload,
		const TCHAR* FieldName,
		double Expected)
	{
		double Actual = 0.0;
		const bool bHasField = Payload.IsValid() && Payload->TryGetNumberField(FieldName, Actual);
		Test.TestTrue(FString::Printf(TEXT("payload has numeric field %s"), FieldName), bHasField);
		if (bHasField)
		{
			Test.TestEqual(FString::Printf(TEXT("%s value"), FieldName), Actual, Expected);
		}
		return bHasField && FMath::IsNearlyEqual(Actual, Expected);
	}

	static bool TestBoolFieldEquals(
		FAutomationTestBase& Test,
		const TSharedPtr<FJsonObject>& Payload,
		const TCHAR* FieldName,
		bool bExpected)
	{
		bool bActual = false;
		const bool bHasField = Payload.IsValid() && Payload->TryGetBoolField(FieldName, bActual);
		Test.TestTrue(FString::Printf(TEXT("payload has bool field %s"), FieldName), bHasField);
		if (bHasField)
		{
			Test.TestEqual(FString::Printf(TEXT("%s value"), FieldName), bActual, bExpected);
		}
		return bHasField && bActual == bExpected;
	}

	static bool TestArrayFieldExists(
		FAutomationTestBase& Test,
		const TSharedPtr<FJsonObject>& Payload,
		const TCHAR* FieldName)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		const bool bHasField = Payload.IsValid() && Payload->TryGetArrayField(FieldName, Values) && Values != nullptr;
		Test.TestTrue(FString::Printf(TEXT("payload has array field %s"), FieldName), bHasField);
		return bHasField;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshPerformanceActionsRegisterTest,
	"Monolith.Performance.MonolithMesh.RegistersPerformanceActions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshPerformanceActionsRegisterTest::RunTest(const FString& /*Parameters*/)
{
	EnsurePerformanceActionsRegistered();

	for (const TCHAR* ActionName : PerformanceActionNames)
	{
		TestTrue(
			FString::Printf(TEXT("mesh.%s is registered"), ActionName),
			FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), ActionName));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshGetRegionPerformanceEmptyRegionTest,
	"Monolith.Performance.MonolithMesh.GetRegionPerformance.EmptyRegion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshGetRegionPerformanceEmptyRegionTest::RunTest(const FString& /*Parameters*/)
{
	EnsurePerformanceActionsRegistered();

	TSharedPtr<FJsonObject> Payload;
	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("mesh"),
		TEXT("get_region_performance"),
		MakeFarRegionParams());

	const EPerformancePayloadState State = RequireWorldActionPayload(*this, TEXT("get_region_performance"), Result, Payload);
	if (State == EPerformancePayloadState::NoEditorWorld)
	{
		return true;
	}
	if (State == EPerformancePayloadState::Failed)
	{
		return false;
	}

	bool bOk = true;
	bOk &= TestNumberFieldEquals(*this, Payload, TEXT("actor_count"), 0.0);
	bOk &= TestNumberFieldEquals(*this, Payload, TEXT("total_triangles"), 0.0);
	bOk &= TestNumberFieldEquals(*this, Payload, TEXT("draw_call_estimate"), 0.0);
	bOk &= TestNumberFieldEquals(*this, Payload, TEXT("light_count"), 0.0);
	bOk &= TestNumberFieldEquals(*this, Payload, TEXT("shadow_caster_count"), 0.0);
	bOk &= TestNumberFieldEquals(*this, Payload, TEXT("static_mesh_components"), 0.0);
	bOk &= TestNumberFieldEquals(*this, Payload, TEXT("skeletal_mesh_components"), 0.0);
	bOk &= TestArrayFieldExists(*this, Payload, TEXT("region_min"));
	bOk &= TestArrayFieldExists(*this, Payload, TEXT("region_max"));
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshEstimatePlacementCostValidationTest,
	"Monolith.Performance.MonolithMesh.EstimatePlacementCost.Validation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshEstimatePlacementCostValidationTest::RunTest(const FString& /*Parameters*/)
{
	EnsurePerformanceActionsRegistered();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("assets"), TArray<TSharedPtr<FJsonValue>>());

	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("mesh"),
		TEXT("estimate_placement_cost"),
		Params);

	bool bOk = true;
	bOk &= TestFalse(TEXT("estimate_placement_cost rejects empty asset arrays"), Result.bSuccess);
	bOk &= TestTrue(
		TEXT("estimate_placement_cost reports empty asset validation"),
		Result.ErrorMessage.Contains(TEXT("Missing or empty required param: assets")));
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshFindOverdrawHotspotsEmptyViewTest,
	"Monolith.Performance.MonolithMesh.FindOverdrawHotspots.EmptyView",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshFindOverdrawHotspotsEmptyViewTest::RunTest(const FString& /*Parameters*/)
{
	EnsurePerformanceActionsRegistered();

	TSharedPtr<FJsonObject> Payload;
	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("mesh"),
		TEXT("find_overdraw_hotspots"),
		MakeFarCameraParams());

	const EPerformancePayloadState State = RequireWorldActionPayload(*this, TEXT("find_overdraw_hotspots"), Result, Payload);
	if (State == EPerformancePayloadState::NoEditorWorld)
	{
		return true;
	}
	if (State == EPerformancePayloadState::Failed)
	{
		return false;
	}

	bool bOk = true;
	bOk &= TestNumberFieldEquals(*this, Payload, TEXT("translucent_actor_count"), 0.0);
	bOk &= TestNumberFieldEquals(*this, Payload, TEXT("max_tile_overlap"), 0.0);
	bOk &= TestNumberFieldEquals(*this, Payload, TEXT("hotspot_tile_count"), 0.0);
	bOk &= TestArrayFieldExists(*this, Payload, TEXT("hotspot_tiles"));
	bOk &= TestArrayFieldExists(*this, Payload, TEXT("translucent_actors"));
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshAnalyzeShadowCostEmptyRegionTest,
	"Monolith.Performance.MonolithMesh.AnalyzeShadowCost.EmptyRegion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshAnalyzeShadowCostEmptyRegionTest::RunTest(const FString& /*Parameters*/)
{
	EnsurePerformanceActionsRegistered();

	TSharedPtr<FJsonObject> Payload;
	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("mesh"),
		TEXT("analyze_shadow_cost"),
		MakeFarRegionParams());

	const EPerformancePayloadState State = RequireWorldActionPayload(*this, TEXT("analyze_shadow_cost"), Result, Payload);
	if (State == EPerformancePayloadState::NoEditorWorld)
	{
		return true;
	}
	if (State == EPerformancePayloadState::Failed)
	{
		return false;
	}

	bool bOk = true;
	bOk &= TestNumberFieldEquals(*this, Payload, TEXT("total_shadow_casters"), 0.0);
	bOk &= TestNumberFieldEquals(*this, Payload, TEXT("shadow_casting_lights"), 0.0);
	bOk &= TestNumberFieldEquals(*this, Payload, TEXT("small_prop_shadow_casters"), 0.0);
	bOk &= TestNumberFieldEquals(*this, Payload, TEXT("issue_count"), 0.0);
	bOk &= TestArrayFieldExists(*this, Payload, TEXT("issues"));
	bOk &= TestArrayFieldExists(*this, Payload, TEXT("shadow_casting_lights_detail"));
	bOk &= TestArrayFieldExists(*this, Payload, TEXT("region_min"));
	bOk &= TestArrayFieldExists(*this, Payload, TEXT("region_max"));
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshGetTriangleBudgetEmptyViewTest,
	"Monolith.Performance.MonolithMesh.GetTriangleBudget.EmptyView",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshGetTriangleBudgetEmptyViewTest::RunTest(const FString& /*Parameters*/)
{
	EnsurePerformanceActionsRegistered();

	TSharedPtr<FJsonObject> Params = MakeFarCameraParams();
	Params->SetNumberField(TEXT("budget"), 1000.0);

	TSharedPtr<FJsonObject> Payload;
	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("mesh"),
		TEXT("get_triangle_budget"),
		Params);

	const EPerformancePayloadState State = RequireWorldActionPayload(*this, TEXT("get_triangle_budget"), Result, Payload);
	if (State == EPerformancePayloadState::NoEditorWorld)
	{
		return true;
	}
	if (State == EPerformancePayloadState::Failed)
	{
		return false;
	}

	bool bOk = true;
	bOk &= TestNumberFieldEquals(*this, Payload, TEXT("budget"), 1000.0);
	bOk &= TestNumberFieldEquals(*this, Payload, TEXT("visible_triangles"), 0.0);
	bOk &= TestNumberFieldEquals(*this, Payload, TEXT("visible_actors"), 0.0);
	bOk &= TestNumberFieldEquals(*this, Payload, TEXT("visible_draw_calls"), 0.0);
	bOk &= TestNumberFieldEquals(*this, Payload, TEXT("budget_usage_pct"), 0.0);
	bOk &= TestBoolFieldEquals(*this, Payload, TEXT("over_budget"), false);
	bOk &= TestArrayFieldExists(*this, Payload, TEXT("viewpoint"));
	bOk &= TestArrayFieldExists(*this, Payload, TEXT("top_contributors"));
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
