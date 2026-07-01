#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "MonolithNiagaraActions.h"

BEGIN_DEFINE_SPEC(FNiagaraNPCParamGuardSpec, "Monolith.Niagara.ParamGuard.NPC", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
	FMonolithToolRegistry* Registry = nullptr;
END_DEFINE_SPEC(FNiagaraNPCParamGuardSpec)

void FNiagaraNPCParamGuardSpec::Define()
{
	BeforeEach([this]()
	{
		Registry = &FMonolithToolRegistry::Get();
		if (!Registry->HasAction(TEXT("niagara"), TEXT("add_npc_parameter")))
		{
			FMonolithNiagaraActions::RegisterActions(*Registry);
		}
	});

	Describe("add_npc_parameter", [this]()
	{
		It("should reject missing or non-string name", [this]()
		{
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Test"));
			Params->SetStringField(TEXT("type"), TEXT("float"));

			// Missing
			FMonolithActionResult Result = Registry->ExecuteAction(TEXT("niagara"), TEXT("add_npc_parameter"), Params);
			TestFalse(TEXT("Missing name should fail"), Result.bSuccess);
			TestTrue(TEXT("Missing name should return expected error"), Result.ErrorMessage.Contains(TEXT("Missing required field: name")));

			// Wrong type
			Params->SetNumberField(TEXT("name"), 123);
			Result = Registry->ExecuteAction(TEXT("niagara"), TEXT("add_npc_parameter"), Params);
			TestFalse(TEXT("Numeric name should fail"), Result.bSuccess);
			TestTrue(TEXT("Numeric name should return expected error"), Result.ErrorMessage.Contains(TEXT("Missing required field: name")));
		});

		It("should reject missing or non-string type", [this]()
		{
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Test"));
			Params->SetStringField(TEXT("name"), TEXT("MyParam"));

			// Missing
			FMonolithActionResult Result = Registry->ExecuteAction(TEXT("niagara"), TEXT("add_npc_parameter"), Params);
			TestFalse(TEXT("Missing type should fail"), Result.bSuccess);
			TestTrue(TEXT("Missing type should return expected error"), Result.ErrorMessage.Contains(TEXT("Missing required field: type")));

			// Wrong type
			Params->SetBoolField(TEXT("type"), true);
			Result = Registry->ExecuteAction(TEXT("niagara"), TEXT("add_npc_parameter"), Params);
			TestFalse(TEXT("Bool type should fail"), Result.bSuccess);
			TestTrue(TEXT("Bool type should return expected error"), Result.ErrorMessage.Contains(TEXT("Missing required field: type")));
		});
	});

	Describe("remove_npc_parameter", [this]()
	{
		It("should reject missing or non-string name", [this]()
		{
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Test"));

			// Missing
			FMonolithActionResult Result = Registry->ExecuteAction(TEXT("niagara"), TEXT("remove_npc_parameter"), Params);
			TestFalse(TEXT("Missing name should fail"), Result.bSuccess);
			TestTrue(TEXT("Missing name should return expected error"), Result.ErrorMessage.Contains(TEXT("Missing required field: name")));

			// Wrong type
			Params->SetNumberField(TEXT("name"), 123);
			Result = Registry->ExecuteAction(TEXT("niagara"), TEXT("remove_npc_parameter"), Params);
			TestFalse(TEXT("Numeric name should fail"), Result.bSuccess);
			TestTrue(TEXT("Numeric name should return expected error"), Result.ErrorMessage.Contains(TEXT("Missing required field: name")));
		});
	});

	Describe("set_npc_default", [this]()
	{
		It("should reject missing or non-string name", [this]()
		{
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Test"));
			Params->SetNumberField(TEXT("value"), 42.0);

			// Missing
			FMonolithActionResult Result = Registry->ExecuteAction(TEXT("niagara"), TEXT("set_npc_default"), Params);
			TestFalse(TEXT("Missing name should fail"), Result.bSuccess);
			TestTrue(TEXT("Missing name should return expected error"), Result.ErrorMessage.Contains(TEXT("Missing required field: name")));

			// Wrong type
			Params->SetNumberField(TEXT("name"), 123);
			Result = Registry->ExecuteAction(TEXT("niagara"), TEXT("set_npc_default"), Params);
			TestFalse(TEXT("Numeric name should fail"), Result.bSuccess);
			TestTrue(TEXT("Numeric name should return expected error"), Result.ErrorMessage.Contains(TEXT("Missing required field: name")));
		});
	});
}
