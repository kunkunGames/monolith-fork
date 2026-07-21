
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithMeshInspectionActions.h"
#include "MonolithMeshTechArtActions.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "MonolithLevelInstanceActions.h"

#if WITH_GEOMETRYSCRIPT
#include "MonolithMeshOperationActions.h"
#include "MonolithMeshProceduralActions.h"
#endif

#if WITH_GEOMETRYSCRIPT
namespace
{
	TSharedPtr<FJsonObject> FindMeshOperationSchema(const FString& ActionName)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("mesh"), ActionName))
		{
			FMonolithMeshOperationActions::RegisterActions(Registry);
		}

		for (const FMonolithActionInfo& Info : Registry.GetActions(TEXT("mesh")))
		{
			if (Info.Action == ActionName)
			{
				return Info.ParamSchema;
			}
		}
		return nullptr;
	}

	TSharedPtr<FJsonObject> FindMeshOperationParam(
		const TSharedPtr<FJsonObject>& Schema,
		const FString& ParamName)
	{
		const TSharedPtr<FJsonObject>* Param = nullptr;
		return Schema.IsValid()
			&& Schema->TryGetObjectField(ParamName, Param)
			&& Param
			? *Param
			: nullptr;
	}
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardMeshInspectionMalformedParamsTest, "Monolith.ParamGuard.MonolithMesh.InspectionRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardMeshInspectionMalformedParamsTest::RunTest(const FString& Parameters)
{
    // Test GetMeshInfo with missing asset_path
    {
        FMonolithMeshInspectionActions::RegisterActions(FMonolithToolRegistry::Get());
        TestTrue(TEXT("get_mesh_info action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), TEXT("get_mesh_info")));
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        // No asset_path
        FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("get_mesh_info"), Params);
        TestFalse(TEXT("GetMeshInfo rejects missing asset_path"), Result.bSuccess);
        // asset_path now flows through the universal registry required-param validator
        // (the asset_path dispatch skip was removed), so a missing asset_path returns the
        // structured missing_required_param contract instead of the handler's ad-hoc message.
        TestTrue(TEXT("GetMeshInfo error names the missing asset_path param"), Result.ErrorMessage.Contains(TEXT("asset_path")));
        TestTrue(TEXT("GetMeshInfo emits structured error data"), Result.ErrorData.IsValid());
        if (Result.ErrorData.IsValid())
        {
            FString FailureCause;
            Result.ErrorData->TryGetStringField(TEXT("failure_cause"), FailureCause);
            TestEqual(TEXT("GetMeshInfo failure_cause is missing_required_param"), FailureCause, FString(TEXT("missing_required_param")));
        }
    }

    return true;
}

#if WITH_GEOMETRYSCRIPT
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardMeshOperationMalformedParamsTest, "Monolith.ParamGuard.MonolithMesh.OperationRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardMeshOperationMalformedParamsTest::RunTest(const FString& Parameters)
{
    FMonolithMeshOperationActions::RegisterActions(FMonolithToolRegistry::Get());
    TestTrue(TEXT("geometry_smooth action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), TEXT("geometry_smooth")));

    // Parameter validation should happen before pool lookups for malformed requests.
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("handle"), TEXT("mesh_123")); // Fails pool lookup if param parsing succeeds, but parsing should fail first
        Params->SetStringField(TEXT("iterations"), TEXT("many")); // Malformed

        FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("geometry_smooth"), Params);
        TestFalse(TEXT("GeometrySmooth rejects malformed iterations parameter"), Result.bSuccess);
        TestTrue(TEXT("GeometrySmooth reports the validation error"), Result.ErrorMessage.Contains(TEXT("Invalid type for parameter 'iterations'. Expected number.")));
    }

    return true;
}
#endif

#if WITH_GEOMETRYSCRIPT
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshOperationRangeContractTest,
	"Monolith.Registry.Mesh.OperationRangeContracts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshOperationRangeContractTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> CollisionSchema = FindMeshOperationSchema(TEXT("generate_collision"));
	const TSharedPtr<FJsonObject> LodsSchema = FindMeshOperationSchema(TEXT("generate_lods"));
	TestNotNull(TEXT("generate_collision schema exists"), CollisionSchema.Get());
	TestNotNull(TEXT("generate_lods schema exists"), LodsSchema.Get());

	const TSharedPtr<FJsonObject> MaxHulls = FindMeshOperationParam(CollisionSchema, TEXT("max_hulls"));
	const TSharedPtr<FJsonObject> LodCount = FindMeshOperationParam(LodsSchema, TEXT("lod_count"));
	const TSharedPtr<FJsonObject> Reduction = FindMeshOperationParam(LodsSchema, TEXT("reduction_per_lod"));
	TestNotNull(TEXT("generate_collision.max_hulls schema exists"), MaxHulls.Get());
	TestNotNull(TEXT("generate_lods.lod_count schema exists"), LodCount.Get());
	TestNotNull(TEXT("generate_lods.reduction_per_lod schema exists"), Reduction.Get());

	if (MaxHulls.IsValid())
	{
		TestEqual(TEXT("max_hulls minimum"), MaxHulls->GetNumberField(TEXT("minimum")), 1.0);
		TestEqual(TEXT("max_hulls maximum"), MaxHulls->GetNumberField(TEXT("maximum")), 256.0);
	}
	if (LodCount.IsValid())
	{
		TestEqual(TEXT("lod_count minimum"), LodCount->GetNumberField(TEXT("minimum")), 1.0);
		TestEqual(TEXT("lod_count maximum"), LodCount->GetNumberField(TEXT("maximum")), 8.0);
	}
	if (Reduction.IsValid())
	{
		TestEqual(TEXT("reduction_per_lod minimum"), Reduction->GetNumberField(TEXT("minimum")), 0.1);
		TestEqual(TEXT("reduction_per_lod maximum"), Reduction->GetNumberField(TEXT("maximum")), 0.9);
	}

	auto ValidateCollisionMaxHulls = [&](double Value)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("handle"), TEXT("range_contract_handle"));
		Params->SetNumberField(TEXT("max_hulls"), Value);
		TArray<FString> Errors;
		return FMonolithParamSchema::ValidateTypedParams(CollisionSchema, Params, Errors);
	};
	TestTrue(TEXT("max_hulls accepts lower boundary"), ValidateCollisionMaxHulls(1.0));
	TestTrue(TEXT("max_hulls accepts upper boundary"), ValidateCollisionMaxHulls(256.0));
	TestFalse(TEXT("max_hulls rejects below lower boundary"), ValidateCollisionMaxHulls(0.0));
	TestFalse(TEXT("max_hulls rejects above upper boundary"), ValidateCollisionMaxHulls(257.0));

	auto ValidateLodParams = [&](double Count, double ReductionValue)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("handle"), TEXT("range_contract_handle"));
		Params->SetNumberField(TEXT("lod_count"), Count);
		Params->SetNumberField(TEXT("reduction_per_lod"), ReductionValue);
		TArray<FString> Errors;
		return FMonolithParamSchema::ValidateTypedParams(LodsSchema, Params, Errors);
	};
	TestTrue(TEXT("generate_lods accepts both lower boundaries"), ValidateLodParams(1.0, 0.1));
	TestTrue(TEXT("generate_lods accepts both upper boundaries"), ValidateLodParams(8.0, 0.9));
	TestFalse(TEXT("lod_count rejects below lower boundary"), ValidateLodParams(0.0, 0.5));
	TestFalse(TEXT("lod_count rejects above upper boundary"), ValidateLodParams(9.0, 0.5));
	TestFalse(TEXT("reduction_per_lod rejects below lower boundary"), ValidateLodParams(1.0, 0.099));
	TestFalse(TEXT("reduction_per_lod rejects above upper boundary"), ValidateLodParams(1.0, 0.901));

	return true;
}
#endif

#if WITH_GEOMETRYSCRIPT
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardMeshSimplifyMalformedParamsTest, "Monolith.ParamGuard.MonolithMesh.SimplifyRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardMeshSimplifyMalformedParamsTest::RunTest(const FString& Parameters)
{
    FMonolithMeshOperationActions::RegisterActions(FMonolithToolRegistry::Get());
    TestTrue(TEXT("mesh_simplify action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), TEXT("mesh_simplify")));

    // Parameter validation should happen before pool lookups for malformed requests.
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("handle"), TEXT("mesh_123")); // Fails pool lookup if param parsing succeeds, but parsing should fail first
        Params->SetStringField(TEXT("target_percentage"), TEXT("half")); // Malformed

        FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("mesh_simplify"), Params);
        TestFalse(TEXT("MeshSimplify rejects malformed target_percentage parameter"), Result.bSuccess);
        TestTrue(TEXT("MeshSimplify reports the validation error"), Result.ErrorMessage.Contains(TEXT("Parameter 'target_percentage' must be a number")));

        Params->RemoveField(TEXT("target_percentage"));
        Params->SetStringField(TEXT("target_triangles"), TEXT("half")); // Malformed
        Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("mesh_simplify"), Params);
        TestFalse(TEXT("MeshSimplify rejects malformed target_triangles parameter"), Result.bSuccess);
        TestTrue(TEXT("MeshSimplify reports the validation error"), Result.ErrorMessage.Contains(TEXT("Parameter 'target_triangles' must be a number")));
    }

    return true;
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardMeshTechArtMalformedParamsTest, "Monolith.ParamGuard.MonolithMesh.TechArtRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardMeshTechArtMalformedParamsTest::RunTest(const FString& Parameters)
{
    FMonolithMeshTechArtActions::RegisterActions(FMonolithToolRegistry::Get());
    TestTrue(TEXT("import_mesh action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), TEXT("import_mesh")));

    const FString TempFbxPath = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithParamGuard"), TEXT(".fbx"));
    TestTrue(TEXT("temporary FBX file is created"), FFileHelper::SaveStringToFile(TEXT("placeholder"), *TempFbxPath));

    TArray<TSharedPtr<FJsonValue>> Files;
    Files.Add(MakeShared<FJsonValueString>(TempFbxPath));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetArrayField(TEXT("files"), Files);
    Params->SetStringField(TEXT("destination"), TEXT("/Game/Temp"));
    Params->SetNumberField(TEXT("material_import"), 1);

    FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("import_mesh"), Params);
    TestFalse(TEXT("ImportMesh rejects malformed material_import parameter"), Result.bSuccess);
    TestTrue(TEXT("ImportMesh reports the validation error"), Result.ErrorMessage.Contains(TEXT("Invalid type for parameter 'material_import'. Expected string.")));

    IFileManager::Get().Delete(*TempFbxPath);
    return true;
}

#if WITH_GEOMETRYSCRIPT
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardMeshProceduralMalformedParamsTest, "Monolith.ParamGuard.MonolithMesh.ProceduralFinalizeRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardMeshProceduralMalformedParamsTest::RunTest(const FString& Parameters)
{
    FMonolithMeshProceduralActions::RegisterActions(FMonolithToolRegistry::Get());
    TestTrue(TEXT("create_parametric_mesh action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), TEXT("create_parametric_mesh")));

    // Test with malformed boolean (use_cache as string instead of bool)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("type"), TEXT("cube"));
        Params->SetStringField(TEXT("use_cache"), TEXT("yes"));

        FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("create_parametric_mesh"), Params);
        TestFalse(TEXT("FinalizeProceduralMesh rejects malformed use_cache parameter"), Result.bSuccess);
        TestTrue(TEXT("FinalizeProceduralMesh reports the validation error"), Result.ErrorMessage.Contains(TEXT("Expected boolean")));
    }

    // Test with malformed number (overwrite as string instead of bool)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("type"), TEXT("cube"));
        Params->SetStringField(TEXT("save_path"), TEXT("/Game/TestCube"));
        Params->SetStringField(TEXT("overwrite"), TEXT("true")); // wrong type

        FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("create_parametric_mesh"), Params);
        TestFalse(TEXT("FinalizeProceduralMesh rejects malformed overwrite parameter"), Result.bSuccess);
        TestTrue(TEXT("FinalizeProceduralMesh reports the validation error"), Result.ErrorMessage.Contains(TEXT("Expected boolean")));
    }

    return true;
}
#endif

#if WITH_GEOMETRYSCRIPT
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardMeshFragmentsMalformedParamsTest, "Monolith.ParamGuard.MonolithMesh.CreateFragmentsRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardMeshFragmentsMalformedParamsTest::RunTest(const FString& Parameters)
{
    FMonolithMeshProceduralActions::RegisterActions(FMonolithToolRegistry::Get());
    TestTrue(TEXT("create_fragments action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), TEXT("create_fragments")));

    // Test with malformed noise (string instead of number)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("source_handle"), TEXT("mesh_123"));
        Params->SetStringField(TEXT("noise"), TEXT("high"));

        FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("create_fragments"), Params);
        TestFalse(TEXT("CreateFragments rejects malformed noise parameter"), Result.bSuccess);
        TestTrue(TEXT("CreateFragments reports the validation error"), Result.ErrorMessage.Contains(TEXT("Invalid type for parameter 'noise'. Expected number.")));
    }

    return true;
}
#endif

#if WITH_GEOMETRYSCRIPT
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardMeshStructureMalformedParamsTest, "Monolith.ParamGuard.MonolithMesh.CreateStructureRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardMeshStructureMalformedParamsTest::RunTest(const FString& Parameters)
{
    FMonolithMeshProceduralActions::RegisterActions(FMonolithToolRegistry::Get());
    TestTrue(TEXT("create_structure action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), TEXT("create_structure")));

    // Test with malformed wall_thickness (string instead of number)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("type"), TEXT("room"));
        Params->SetStringField(TEXT("wall_thickness"), TEXT("thick"));

        FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("create_structure"), Params);
        TestFalse(TEXT("CreateStructure rejects malformed wall_thickness parameter"), Result.bSuccess);
        TestTrue(TEXT("CreateStructure reports the validation error"), Result.ErrorMessage.Contains(TEXT("Invalid type for parameter 'wall_thickness'. Expected number.")));
    }

    return true;
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLevelInstanceMalformedParamsTest, "Monolith.ParamGuard.MonolithMesh.LevelInstanceAliasesRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardLevelInstanceMalformedParamsTest::RunTest(const FString& Parameters)
{
    FMonolithLevelInstanceActions::RegisterActions(FMonolithToolRegistry::Get());
    TestTrue(TEXT("list_child_instances action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("level_instance"), TEXT("list_child_instances")));

    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actor_name"), TEXT("NonExistentTestActor"));

        FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("level_instance"), TEXT("list_child_instances"), Params);
        // It shouldn't fail due to "Unknown parameter: actor_name"
        TestFalse(TEXT("list_child_instances does not reject valid alias 'actor_name' as unknown parameter"), Result.ErrorMessage.Contains(TEXT("Unknown parameter")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardMeshInspectionUvsVertexDataMalformedParamsTest, "Monolith.ParamGuard.MonolithMesh.InspectionUvsVertexDataRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardMeshInspectionUvsVertexDataMalformedParamsTest::RunTest(const FString& Parameters)
{
    FMonolithMeshInspectionActions::RegisterActions(FMonolithToolRegistry::Get());
    TestTrue(TEXT("get_mesh_uvs action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), TEXT("get_mesh_uvs")));
    TestTrue(TEXT("get_vertex_data action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), TEXT("get_vertex_data")));

    // Test get_mesh_uvs with malformed lod_index
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/FakeMesh"));
        Params->SetStringField(TEXT("lod_index"), TEXT("zero"));

        FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("get_mesh_uvs"), Params);
        TestFalse(TEXT("GetMeshUvs rejects malformed lod_index parameter"), Result.bSuccess);
        TestEqual(TEXT("GetMeshUvs returns ErrInvalidParams"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
        TestTrue(TEXT("GetMeshUvs reports the validation error"), Result.ErrorMessage.Contains(TEXT("Expected number")));
    }

    // Test get_mesh_uvs with malformed uv_channel
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/FakeMesh"));
        Params->SetStringField(TEXT("uv_channel"), TEXT("all"));

        FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("get_mesh_uvs"), Params);
        TestFalse(TEXT("GetMeshUvs rejects malformed uv_channel parameter"), Result.bSuccess);
        TestEqual(TEXT("GetMeshUvs returns ErrInvalidParams"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
        TestTrue(TEXT("GetMeshUvs reports the validation error"), Result.ErrorMessage.Contains(TEXT("Expected number")));
    }

    // Test get_vertex_data with malformed limit
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/FakeMesh"));
        Params->SetStringField(TEXT("limit"), TEXT("max"));

        FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("get_vertex_data"), Params);
        TestFalse(TEXT("GetVertexData rejects malformed limit parameter"), Result.bSuccess);
        TestEqual(TEXT("GetVertexData returns ErrInvalidParams"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
        TestTrue(TEXT("GetVertexData reports the validation error"), Result.ErrorMessage.Contains(TEXT("Expected number")));
    }

    // Test get_vertex_data with malformed offset
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/FakeMesh"));
        Params->SetStringField(TEXT("offset"), TEXT("start"));

        FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("get_vertex_data"), Params);
        TestFalse(TEXT("GetVertexData rejects malformed offset parameter"), Result.bSuccess);
        TestEqual(TEXT("GetVertexData returns ErrInvalidParams"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
        TestTrue(TEXT("GetVertexData reports the validation error"), Result.ErrorMessage.Contains(TEXT("Expected number")));
    }

    return true;
}
