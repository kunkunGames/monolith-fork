#if WITH_DEV_AUTOMATION_TESTS

#include "MonolithEditorActions.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Modules/ModuleManager.h"

namespace
{
	void RegisterContentValidationDependencies()
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("source_control"), TEXT("map_depot_paths")))
		{
			FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("MonolithSourceControl"));
		}
		if (!Registry.HasAction(TEXT("editor"), TEXT("plan_content_validation_changeset"))
			|| !Registry.HasAction(TEXT("editor"), TEXT("validate_changeset_assets")))
		{
			FMonolithEditorActions::RegisterActions(nullptr);
		}
	}

	TSharedPtr<FJsonObject> MakePathPayload(const FString& Path)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetBoolField(TEXT("include_opened"), false);
		TArray<TSharedPtr<FJsonValue>> Paths;
		Paths.Add(MakeShared<FJsonValueString>(Path));
		Params->SetArrayField(TEXT("paths"), Paths);
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithEditorContentValidationChangesetTest,
	"Monolith.Editor.ContentValidationChangesetPlanner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorContentValidationChangesetTest::RunTest(const FString& Parameters)
{
	RegisterContentValidationDependencies();
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	for (const FString& Action : { TEXT("plan_content_validation_changeset"), TEXT("validate_changeset_assets") })
	{
		TestTrue(FString::Printf(TEXT("editor.%s action is registered"), *Action),
			Registry.HasAction(TEXT("editor"), Action));
		TestEqual(FString::Printf(TEXT("editor.%s is read-only"), *Action),
			Registry.GetActionExecutionPolicy(TEXT("editor"), Action).PolicyId,
			FString(TEXT("read_only")));
	}

	FMonolithActionResult Plan = Registry.ExecuteAction(
		TEXT("editor"),
		TEXT("plan_content_validation_changeset"),
		MakePathPayload(TEXT("/Game/MonolithTests/Validation/DA_Test")));
	TestTrue(TEXT("plan_content_validation_changeset succeeds for explicit package path"), Plan.bSuccess);
	TestTrue(TEXT("planner returns json"), Plan.Result.IsValid());
	if (Plan.Result.IsValid())
	{
		double PackageCount = 0.0;
		Plan.Result->TryGetNumberField(TEXT("package_count"), PackageCount);
		TestEqual(TEXT("planner reports one validation package"), static_cast<int32>(PackageCount), 1);

		const TSharedPtr<FJsonObject>* ValidationParams = nullptr;
		TestTrue(TEXT("planner returns validation_params"),
			Plan.Result->TryGetObjectField(TEXT("validation_params"), ValidationParams) && ValidationParams && ValidationParams->IsValid());
		if (ValidationParams && ValidationParams->IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Packages = nullptr;
			TestTrue(TEXT("validation_params contains packages"),
				(*ValidationParams)->TryGetArrayField(TEXT("packages"), Packages) && Packages && Packages->Num() == 1);
			FString Usecase;
			(*ValidationParams)->TryGetStringField(TEXT("validation_usecase"), Usecase);
			TestEqual(TEXT("planner defaults validation usecase to pre_submit"), Usecase, FString(TEXT("pre_submit")));
		}
	}

	FMonolithActionResult CodeOnly = Registry.ExecuteAction(
		TEXT("editor"),
		TEXT("validate_changeset_assets"),
		MakePathPayload(TEXT("Source/Speed/Private/MonolithPlannerSmoke.cpp")));
	TestTrue(TEXT("validate_changeset_assets succeeds for source-only input"), CodeOnly.bSuccess);
	TestTrue(TEXT("source-only validation returns json"), CodeOnly.Result.IsValid());
	if (CodeOnly.Result.IsValid())
	{
		bool bSkipped = false;
		CodeOnly.Result->TryGetBoolField(TEXT("validation_skipped"), bSkipped);
		TestTrue(TEXT("source-only changeset skips asset validation"), bSkipped);
	}

	TSharedPtr<FJsonObject> BadParams = MakeShared<FJsonObject>();
	BadParams->SetStringField(TEXT("paths"), TEXT("/Game/Invalid"));
	FMonolithActionResult Bad = Registry.ExecuteAction(TEXT("editor"), TEXT("plan_content_validation_changeset"), BadParams);
	TestFalse(TEXT("plan_content_validation_changeset rejects non-array paths"), Bad.bSuccess);
	TestTrue(TEXT("bad paths error names paths"), Bad.ErrorMessage.Contains(TEXT("paths")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
