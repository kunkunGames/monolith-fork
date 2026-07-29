#if WITH_DEV_AUTOMATION_TESTS

#include "MonolithGameplayMessageActions.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "MonolithToolRegistry.h"

namespace
{
	FMonolithToolRegistry& GameplayMessageRegistry()
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("gameplay_message"), TEXT("get_status")))
		{
			FMonolithGameplayMessageActions::RegisterActions(Registry);
		}
		return Registry;
	}

	void ExpectInvalidParams(
		FAutomationTestBase& Test,
		FMonolithToolRegistry& Registry,
		const FString& Action,
		const TSharedPtr<FJsonObject>& Params,
		const FString& Label)
	{
		const FMonolithActionResult Result = Registry.ExecuteAction(
			TEXT("gameplay_message"),
			Action,
			Params);
		Test.TestFalse(*FString::Printf(TEXT("%s fails"), *Label), Result.bSuccess);
		Test.TestEqual(
			*FString::Printf(TEXT("%s uses invalid-param code"), *Label),
			Result.ErrorCode,
			-32602);
		Test.TestTrue(
			*FString::Printf(TEXT("%s reports an error"), *Label),
			!Result.ErrorMessage.IsEmpty());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGameplayMessageParamGuardTest,
	"Monolith.GameplayMessage.ParamGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGameplayMessageParamGuardTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = GameplayMessageRegistry();

	TSharedPtr<FJsonObject> WhitespacePath = MakeShared<FJsonObject>();
	WhitespacePath->SetStringField(
		TEXT("message_struct"),
		TEXT(" /Script/GameplayTags.GameplayTagContainer"));
	ExpectInvalidParams(
		*this,
		Registry,
		TEXT("validate_message_struct"),
		WhitespacePath,
		TEXT("leading-whitespace object path"));

	TSharedPtr<FJsonObject> BackslashPath = MakeShared<FJsonObject>();
	BackslashPath->SetStringField(
		TEXT("message_struct"),
		TEXT("\\Script\\GameplayTags.GameplayTagContainer"));
	ExpectInvalidParams(
		*this,
		Registry,
		TEXT("validate_message_struct"),
		BackslashPath,
		TEXT("backslash object path"));

	TSharedPtr<FJsonObject> WrongBoolType = MakeShared<FJsonObject>();
	WrongBoolType->SetStringField(
		TEXT("message_struct"),
		TEXT("/Script/GameplayTags.GameplayTagContainer"));
	WrongBoolType->SetStringField(TEXT("require_blueprint_type"), TEXT("false"));
	ExpectInvalidParams(
		*this,
		Registry,
		TEXT("validate_message_struct"),
		WrongBoolType,
		TEXT("string-encoded boolean"));

	TSharedPtr<FJsonObject> InvalidTag = MakeShared<FJsonObject>();
	InvalidTag->SetStringField(TEXT("channel_tag"), TEXT("Monolith Bad Tag"));
	ExpectInvalidParams(
		*this,
		Registry,
		TEXT("validate_channel_contract"),
		InvalidTag,
		TEXT("invalid gameplay tag syntax"));

	TSharedPtr<FJsonObject> WrongMatchCase = MakeShared<FJsonObject>();
	WrongMatchCase->SetStringField(
		TEXT("channel_tag"),
		TEXT("Monolith.GameplayMessage.Preflight"));
	WrongMatchCase->SetBoolField(TEXT("require_registered_tag"), false);
	WrongMatchCase->SetStringField(TEXT("match_type"), TEXT("exactmatch"));
	ExpectInvalidParams(
		*this,
		Registry,
		TEXT("validate_channel_contract"),
		WrongMatchCase,
		TEXT("noncanonical match type"));

	TSharedPtr<FJsonObject> EmptyTagSegment = MakeShared<FJsonObject>();
	EmptyTagSegment->SetStringField(TEXT("channel_tag"), TEXT("Monolith..GameplayMessage"));
	ExpectInvalidParams(
		*this,
		Registry,
		TEXT("validate_channel_contract"),
		EmptyTagSegment,
		TEXT("empty gameplay tag segment"));

	TSharedPtr<FJsonObject> StringEncodedLimit = MakeShared<FJsonObject>();
	StringEncodedLimit->SetStringField(TEXT("max_results"), TEXT("10"));
	ExpectInvalidParams(
		*this,
		Registry,
		TEXT("trace_channel_usage"),
		StringEncodedLimit,
		TEXT("string-encoded trace result limit"));

	TSharedPtr<FJsonObject> InvalidTraceTag = MakeShared<FJsonObject>();
	InvalidTraceTag->SetStringField(TEXT("channel_tag"), TEXT("Monolith Bad.Trace"));
	ExpectInvalidParams(
		*this,
		Registry,
		TEXT("trace_channel_usage"),
		InvalidTraceTag,
		TEXT("invalid trace channel filter"));

	TSharedPtr<FJsonObject> EmptyTraceTagSegment = MakeShared<FJsonObject>();
	EmptyTraceTagSegment->SetStringField(TEXT("channel_tag"), TEXT("Monolith..Trace"));
	ExpectInvalidParams(
		*this,
		Registry,
		TEXT("trace_channel_usage"),
		EmptyTraceTagSegment,
		TEXT("empty trace channel segment"));

	TSharedPtr<FJsonObject> FractionalLimit = MakeShared<FJsonObject>();
	FractionalLimit->SetNumberField(TEXT("max_results"), 1.5);
	ExpectInvalidParams(
		*this,
		Registry,
		TEXT("trace_channel_usage"),
		FractionalLimit,
		TEXT("fractional trace result limit"));

	TSharedPtr<FJsonObject> ZeroLimit = MakeShared<FJsonObject>();
	ZeroLimit->SetNumberField(TEXT("max_files"), 0);
	ExpectInvalidParams(
		*this,
		Registry,
		TEXT("trace_channel_usage"),
		ZeroLimit,
		TEXT("zero trace file limit"));

	FString EngineDirectory = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());
	FPaths::NormalizeDirectoryName(EngineDirectory);
	TSharedPtr<FJsonObject> OutsideProject = MakeShared<FJsonObject>();
	OutsideProject->SetStringField(TEXT("source_root"), EngineDirectory);
	ExpectInvalidParams(
		*this,
		Registry,
		TEXT("trace_channel_usage"),
		OutsideProject,
		TEXT("source root outside project"));

	FString MissingProjectDirectory = FPaths::Combine(
		FPaths::ProjectDir(),
		TEXT("Source/MonolithDefinitelyMissingSourceRoot"));
	FPaths::NormalizeDirectoryName(MissingProjectDirectory);
	TSharedPtr<FJsonObject> MissingRoot = MakeShared<FJsonObject>();
	MissingRoot->SetStringField(TEXT("source_root"), MissingProjectDirectory);
	ExpectInvalidParams(
		*this,
		Registry,
		TEXT("trace_channel_usage"),
		MissingRoot,
		TEXT("missing project source root"));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
