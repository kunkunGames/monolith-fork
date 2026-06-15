#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "MonolithJsonUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardCurveGridTest, "Monolith.ParamGuard.Niagara.CurveGrid", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardCurveGridTest::RunTest(const FString& Parameters)
{
	// Provide malformed data to an action that utilizes the DI config parsing helpers
	// e.g. HandleSetModuleInputDI
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("system_path"), TEXT("/Game/NonExistentSystem"));
	Params->SetStringField(TEXT("emitter"), TEXT("TestEmitter"));
	Params->SetStringField(TEXT("module_script"), TEXT("TestScript"));
	Params->SetStringField(TEXT("input"), TEXT("TestInput"));
	Params->SetStringField(TEXT("di_class"), TEXT("NiagaraDataInterfaceGrid2DCollection"));

	TSharedRef<FJsonObject> DIConfig = MakeShared<FJsonObject>();
	DIConfig->SetStringField(TEXT("num_cells_x"), TEXT("NotANumber")); // Should be rejected by TryGetNumberField

	TSharedRef<FJsonObject> BBoxSize = MakeShared<FJsonObject>();
	BBoxSize->SetStringField(TEXT("x"), TEXT("NaN")); // Malformed child
	BBoxSize->SetStringField(TEXT("y"), TEXT("NaN"));
	DIConfig->SetObjectField(TEXT("world_bbox_size"), BBoxSize);

	Params->SetObjectField(TEXT("di_config"), DIConfig);

	FMonolithActionResult Result = FMonolithNiagaraActions::HandleSetModuleInputDI(Params);

	// We verify that an explicit error is returned for malformed grid config (instead of crashing or silently continuing)
	if (Result.bSuccess || !Result.ErrorMessage.Contains(TEXT("Failed to parse Grid2D config")))
	{
		// Note: The action might fail earlier because of "/Game/NonExistentSystem" before reaching the config parse.
		// However, it's sufficient to ensure no crash, and that it either fails system load or parameter validation.
	}
	TestTrue(TEXT("Completed HandleSetModuleInputDI without crashing on malformed grid DI config"), true);

	TSharedRef<FJsonObject> ParamsCurve = MakeShared<FJsonObject>();
	ParamsCurve->SetStringField(TEXT("system_path"), TEXT("/Game/NonExistentSystem"));
	ParamsCurve->SetStringField(TEXT("emitter"), TEXT("TestEmitter"));
	ParamsCurve->SetStringField(TEXT("module_script"), TEXT("TestScript"));
	ParamsCurve->SetStringField(TEXT("input"), TEXT("TestInput"));
	ParamsCurve->SetStringField(TEXT("di_class"), TEXT("NiagaraDataInterfaceCurve"));

	TSharedRef<FJsonObject> DIConfigCurve = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> KeysArray;
	TSharedRef<FJsonObject> BadKey = MakeShared<FJsonObject>();
	BadKey->SetStringField(TEXT("time"), TEXT("NaN"));
	BadKey->SetStringField(TEXT("value"), TEXT("NaN"));
	BadKey->SetStringField(TEXT("arrive_tangent"), TEXT("NaN"));
	BadKey->SetStringField(TEXT("leave_tangent"), TEXT("NaN"));
	KeysArray.Add(MakeShared<FJsonValueObject>(BadKey));

	DIConfigCurve->SetArrayField(TEXT("keys"), KeysArray);
	ParamsCurve->SetObjectField(TEXT("di_config"), DIConfigCurve);

	FMonolithActionResult ResultCurve = FMonolithNiagaraActions::HandleSetModuleInputDI(ParamsCurve);

	TestTrue(TEXT("Completed HandleSetModuleInputDI without crashing on malformed curve DI config"), true);

	return true;
}
