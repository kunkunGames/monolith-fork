#if WITH_DEV_AUTOMATION_TESTS

#include "MonolithOnlineActions.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithOnlineRegistryTest,
	"Monolith.Online.RegistryAndReadOnlyDiagnostics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithOnlineRegistryTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("online"), TEXT("get_status")))
	{
		FMonolithOnlineActions::RegisterActions(Registry);
	}

	const TArray<FString> Actions =
	{
		TEXT("get_status"),
		TEXT("validate_eos_ossv2_config"),
		TEXT("describe_common_session_flow"),
		TEXT("validate_common_session_schema"),
		TEXT("validate_user_facing_session"),
		TEXT("validate_common_user_initialization_contract"),
		TEXT("validate_common_user_privilege_matrix"),
		TEXT("diagnose_eos_accountportal_logs")
	};

	for (const FString& Action : Actions)
	{
		TestTrue(FString::Printf(TEXT("online.%s action is registered"), *Action), Registry.HasAction(TEXT("online"), Action));
		TestEqual(FString::Printf(TEXT("online.%s is read-only"), *Action),
			Registry.GetActionExecutionPolicy(TEXT("online"), Action).PolicyId,
			FString(TEXT("read_only")));
	}

	FMonolithActionResult Status = FMonolithOnlineActions::GetStatus(MakeShared<FJsonObject>());
	TestTrue(TEXT("online.get_status succeeds"), Status.bSuccess);
	TestTrue(TEXT("online.get_status returns json"), Status.Result.IsValid());

	FMonolithActionResult EOSConfig = FMonolithOnlineActions::ValidateEOSOSSv2Config(MakeShared<FJsonObject>());
	TestTrue(TEXT("online.validate_eos_ossv2_config succeeds"), EOSConfig.bSuccess);
	TestTrue(TEXT("online.validate_eos_ossv2_config returns json"), EOSConfig.Result.IsValid());
	if (EOSConfig.Result.IsValid())
	{
		bool bRedactionApplied = false;
		EOSConfig.Result->TryGetBoolField(TEXT("redaction_applied"), bRedactionApplied);
		TestTrue(TEXT("EOS config validation reports redaction"), bRedactionApplied);
		TestTrue(TEXT("EOS config validation has checks"), EOSConfig.Result->HasField(TEXT("checks")));
		TestTrue(TEXT("EOS config validation has fields"), EOSConfig.Result->HasField(TEXT("fields")));
	}

	FMonolithActionResult SessionSchema = FMonolithOnlineActions::ValidateCommonSessionSchema(MakeShared<FJsonObject>());
	TestTrue(TEXT("online.validate_common_session_schema succeeds"), SessionSchema.bSuccess);
	TestTrue(TEXT("online.validate_common_session_schema returns json"), SessionSchema.Result.IsValid());

	FMonolithActionResult SessionFlow = FMonolithOnlineActions::DescribeCommonSessionFlow(MakeShared<FJsonObject>());
	TestTrue(TEXT("online.describe_common_session_flow succeeds"), SessionFlow.bSuccess);
	TestTrue(TEXT("online.describe_common_session_flow returns json"), SessionFlow.Result.IsValid());
	if (SessionFlow.Result.IsValid())
	{
		TestTrue(TEXT("online.describe_common_session_flow has checks"), SessionFlow.Result->HasField(TEXT("checks")));
		TestTrue(TEXT("online.describe_common_session_flow has flow steps"), SessionFlow.Result->HasField(TEXT("flow_steps")));
		TestTrue(TEXT("online.describe_common_session_flow has branch rules"), SessionFlow.Result->HasField(TEXT("branch_rules")));
		TestTrue(TEXT("online.describe_common_session_flow reports redaction"), SessionFlow.Result->GetBoolField(TEXT("redaction_applied")));
	}

	FMonolithActionResult MissingUserFacingSession = FMonolithOnlineActions::ValidateUserFacingSession(MakeShared<FJsonObject>());
	TestFalse(TEXT("online.validate_user_facing_session rejects missing user_facing_experience_path"), MissingUserFacingSession.bSuccess);
	TestEqual(TEXT("online.validate_user_facing_session invalid param code"), MissingUserFacingSession.ErrorCode, -32602);

	TSharedPtr<FJsonObject> MissingUserFacingParams = MakeShared<FJsonObject>();
	MissingUserFacingParams->SetStringField(TEXT("user_facing_experience_path"), TEXT("/Game/Tests/Monolith/Online/MissingUserFacing.MissingUserFacing"));
	FMonolithActionResult MissingUserFacing = FMonolithOnlineActions::ValidateUserFacingSession(MissingUserFacingParams);
	TestTrue(TEXT("online.validate_user_facing_session returns structured result for missing asset"), MissingUserFacing.bSuccess);
	TestTrue(TEXT("online.validate_user_facing_session returns json"), MissingUserFacing.Result.IsValid());
	if (MissingUserFacing.Result.IsValid())
	{
		TestFalse(TEXT("online.validate_user_facing_session ok=false for missing asset"), MissingUserFacing.Result->GetBoolField(TEXT("ok")));
		TestTrue(TEXT("online.validate_user_facing_session reports redaction"), MissingUserFacing.Result->GetBoolField(TEXT("redaction_applied")));
		TestTrue(TEXT("online.validate_user_facing_session has checks"), MissingUserFacing.Result->HasField(TEXT("checks")));
		TestTrue(TEXT("online.validate_user_facing_session has user_facing_experience report"), MissingUserFacing.Result->HasField(TEXT("user_facing_experience")));
	}

	FMonolithActionResult CommonUser = FMonolithOnlineActions::ValidateCommonUserInitializationContract(MakeShared<FJsonObject>());
	TestTrue(TEXT("online.validate_common_user_initialization_contract succeeds"), CommonUser.bSuccess);
	TestTrue(TEXT("online.validate_common_user_initialization_contract returns json"), CommonUser.Result.IsValid());

	FMonolithActionResult PrivilegeMatrix = FMonolithOnlineActions::ValidateCommonUserPrivilegeMatrix(MakeShared<FJsonObject>());
	TestTrue(TEXT("online.validate_common_user_privilege_matrix succeeds"), PrivilegeMatrix.bSuccess);
	TestTrue(TEXT("online.validate_common_user_privilege_matrix returns json"), PrivilegeMatrix.Result.IsValid());
	if (PrivilegeMatrix.Result.IsValid())
	{
		TestTrue(TEXT("online.validate_common_user_privilege_matrix has checks"), PrivilegeMatrix.Result->HasField(TEXT("checks")));
		TestTrue(TEXT("online.validate_common_user_privilege_matrix has privilege matrix"), PrivilegeMatrix.Result->HasField(TEXT("privilege_matrix")));
		TestTrue(TEXT("online.validate_common_user_privilege_matrix has result buckets"), PrivilegeMatrix.Result->HasField(TEXT("result_buckets")));
		TestTrue(TEXT("online.validate_common_user_privilege_matrix reports redaction"), PrivilegeMatrix.Result->GetBoolField(TEXT("redaction_applied")));
	}

	TSharedPtr<FJsonObject> LogParams = MakeShared<FJsonObject>();
	LogParams->SetNumberField(TEXT("max_results"), 1);
	FMonolithActionResult Logs = FMonolithOnlineActions::DiagnoseEOSAccountPortalLogs(LogParams);
	TestTrue(TEXT("online.diagnose_eos_accountportal_logs succeeds"), Logs.bSuccess);
	TestTrue(TEXT("online.diagnose_eos_accountportal_logs returns json"), Logs.Result.IsValid());
	if (Logs.Result.IsValid())
	{
		bool bRedactionApplied = false;
		Logs.Result->TryGetBoolField(TEXT("redaction_applied"), bRedactionApplied);
		TestTrue(TEXT("log diagnosis reports redaction"), bRedactionApplied);
		TestTrue(TEXT("log diagnosis has matches array"), Logs.Result->HasField(TEXT("matches")));
	}

	TSharedPtr<FJsonObject> BadLogParams = MakeShared<FJsonObject>();
	BadLogParams->SetNumberField(TEXT("max_results"), 0);
	FMonolithActionResult BadLogs = FMonolithOnlineActions::DiagnoseEOSAccountPortalLogs(BadLogParams);
	TestFalse(TEXT("online.diagnose_eos_accountportal_logs rejects max_results=0"), BadLogs.bSuccess);
	TestEqual(TEXT("online.diagnose_eos_accountportal_logs invalid param code"), BadLogs.ErrorCode, -32602);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
