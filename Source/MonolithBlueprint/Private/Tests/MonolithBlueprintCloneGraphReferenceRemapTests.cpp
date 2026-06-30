#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "MonolithBlueprintGraphExportActions.h"
#include "MonolithTestSupport.h"
#include "MonolithToolRegistry.h"

#include <initializer_list>

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	const TCHAR* CloneGraphRemapNamespace = TEXT("blueprint");
	const TCHAR* CloneGraphRemapAction = TEXT("clone_graphs_with_reference_remap");

	void RegisterCloneGraphRemapAction(FMonolithToolRegistry& Registry)
	{
		FMonolithBlueprintGraphExportActions::RegisterActions(Registry);
	}

	bool FindCloneGraphRemapAction(FMonolithActionInfo& OutInfo)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(CloneGraphRemapNamespace, CloneGraphRemapAction))
		{
			RegisterCloneGraphRemapAction(Registry);
		}

		for (const FMonolithActionInfo& Info : Registry.GetActions(CloneGraphRemapNamespace))
		{
			if (Info.Action == CloneGraphRemapAction)
			{
				OutInfo = Info;
				return true;
			}
		}
		return false;
	}

	bool GetSchemaParam(
		FAutomationTestBase& Test,
		const TSharedPtr<FJsonObject>& Schema,
		const TCHAR* ParamName,
		const TSharedPtr<FJsonObject>*& OutParam)
	{
		OutParam = nullptr;
		if (!Schema.IsValid())
		{
			return Test.TestTrue(TEXT("clone_graphs_with_reference_remap schema is valid"), false);
		}

		const bool bHasParam = Schema->TryGetObjectField(ParamName, OutParam) && OutParam && OutParam->IsValid();
		Test.TestTrue(
			*FString::Printf(TEXT("schema contains '%s'"), ParamName),
			bHasParam);
		return bHasParam;
	}

	bool ExpectSchemaParam(
		FAutomationTestBase& Test,
		const TSharedPtr<FJsonObject>& Schema,
		const TCHAR* ParamName,
		const TCHAR* ExpectedType,
		bool bExpectedRequired)
	{
		bool bOk = true;
		const TSharedPtr<FJsonObject>* Param = nullptr;
		if (!GetSchemaParam(Test, Schema, ParamName, Param))
		{
			return false;
		}

		FString ActualType;
		bOk &= Test.TestTrue(
			*FString::Printf(TEXT("schema '%s' declares a type"), ParamName),
			(*Param)->TryGetStringField(TEXT("type"), ActualType));
		bOk &= Test.TestEqual(
			*FString::Printf(TEXT("schema '%s' type"), ParamName),
			ActualType,
			FString(ExpectedType));

		bool bActualRequired = false;
		bOk &= Test.TestTrue(
			*FString::Printf(TEXT("schema '%s' declares required flag"), ParamName),
			(*Param)->TryGetBoolField(TEXT("required"), bActualRequired));
		bOk &= Test.TestEqual(
			*FString::Printf(TEXT("schema '%s' required flag"), ParamName),
			bActualRequired,
			bExpectedRequired);
		return bOk;
	}

	bool ExpectSchemaDefault(
		FAutomationTestBase& Test,
		const TSharedPtr<FJsonObject>& Schema,
		const TCHAR* ParamName,
		const TCHAR* ExpectedDefault)
	{
		bool bOk = true;
		const TSharedPtr<FJsonObject>* Param = nullptr;
		if (!GetSchemaParam(Test, Schema, ParamName, Param))
		{
			return false;
		}

		FString ActualDefault;
		bOk &= Test.TestTrue(
			*FString::Printf(TEXT("schema '%s' declares a default"), ParamName),
			(*Param)->TryGetStringField(TEXT("default"), ActualDefault));
		bOk &= Test.TestEqual(
			*FString::Printf(TEXT("schema '%s' default"), ParamName),
			ActualDefault,
			FString(ExpectedDefault));
		return bOk;
	}

	bool ExpectAssetPathKind(
		FAutomationTestBase& Test,
		const TSharedPtr<FJsonObject>& Schema,
		const TCHAR* ParamName)
	{
		bool bOk = true;
		const TSharedPtr<FJsonObject>* Param = nullptr;
		if (!GetSchemaParam(Test, Schema, ParamName, Param))
		{
			return false;
		}

		FString ActualKind;
		bOk &= Test.TestTrue(
			*FString::Printf(TEXT("schema '%s' declares kind"), ParamName),
			(*Param)->TryGetStringField(TEXT("kind"), ActualKind));
		bOk &= Test.TestEqual(
			*FString::Printf(TEXT("schema '%s' kind"), ParamName),
			ActualKind,
			FString(TEXT("AssetPath")));
		return bOk;
	}

	bool JsonArrayHasString(const TArray<TSharedPtr<FJsonValue>>& Values, const TCHAR* Expected)
	{
		for (const TSharedPtr<FJsonValue>& Value : Values)
		{
			FString Actual;
			if (Value.IsValid() && Value->TryGetString(Actual) && Actual == Expected)
			{
				return true;
			}
		}
		return false;
	}

	bool StringArrayContainsText(const TArray<FString>& Values, const TCHAR* Expected)
	{
		for (const FString& Value : Values)
		{
			if (Value.Contains(Expected, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	bool ExpectEnumValues(
		FAutomationTestBase& Test,
		const TSharedPtr<FJsonObject>& Schema,
		const TCHAR* ParamName,
		std::initializer_list<const TCHAR*> ExpectedValues)
	{
		bool bOk = true;
		const TSharedPtr<FJsonObject>* Param = nullptr;
		if (!GetSchemaParam(Test, Schema, ParamName, Param))
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* EnumValues = nullptr;
		bOk &= Test.TestTrue(
			*FString::Printf(TEXT("schema '%s' declares enum values"), ParamName),
			(*Param)->TryGetArrayField(TEXT("enum"), EnumValues) && EnumValues);
		if (!EnumValues)
		{
			return false;
		}

		for (const TCHAR* ExpectedValue : ExpectedValues)
		{
			bOk &= Test.TestTrue(
				*FString::Printf(TEXT("schema '%s' enum contains '%s'"), ParamName, ExpectedValue),
				JsonArrayHasString(*EnumValues, ExpectedValue));
		}
		return bOk;
	}

	TSharedRef<FJsonObject> MakeMinimalCloneParams()
	{
		TSharedRef<FJsonObject> Params = FMonolithTestSupport::MakeParams();
		Params->SetStringField(TEXT("source_asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_CloneGraphSource_DO_NOT_LOAD"));
		Params->SetStringField(TEXT("destination_asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/BP_CloneGraphDestination_DO_NOT_LOAD"));
		Params->SetStringField(TEXT("graph_name"), TEXT("CloneMe"));
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBlueprintCloneGraphReferenceRemapRegistrySchemaSmokeTest,
	"Monolith.Blueprint.CloneGraphReferenceRemap.RegistrySchemaSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintCloneGraphReferenceRemapRegistrySchemaSmokeTest::RunTest(const FString& Parameters)
{
	bool bOk = true;

	FMonolithActionInfo ActionInfo;
	bOk &= TestTrue(
		TEXT("blueprint.clone_graphs_with_reference_remap is registered"),
		FindCloneGraphRemapAction(ActionInfo));
	if (!ActionInfo.ParamSchema.IsValid())
	{
		return TestTrue(TEXT("blueprint.clone_graphs_with_reference_remap has a param schema"), false);
	}

	const TSharedPtr<FJsonObject>& Schema = ActionInfo.ParamSchema;
	bOk &= ExpectSchemaParam(*this, Schema, TEXT("source_asset_path"), TEXT("string"), true);
	bOk &= ExpectSchemaParam(*this, Schema, TEXT("destination_asset_path"), TEXT("string"), true);
	bOk &= ExpectSchemaParam(*this, Schema, TEXT("graph_name"), TEXT("string"), false);
	bOk &= ExpectSchemaParam(*this, Schema, TEXT("graphs"), TEXT("array"), false);
	bOk &= ExpectSchemaParam(*this, Schema, TEXT("root_remaps"), TEXT("object"), false);
	bOk &= ExpectSchemaParam(*this, Schema, TEXT("source_root"), TEXT("string"), false);
	bOk &= ExpectSchemaParam(*this, Schema, TEXT("dest_root"), TEXT("string"), false);
	bOk &= ExpectSchemaParam(*this, Schema, TEXT("object_remaps"), TEXT("object"), false);
	bOk &= ExpectSchemaParam(*this, Schema, TEXT("class_remaps"), TEXT("object"), false);
	bOk &= ExpectSchemaParam(*this, Schema, TEXT("existing_policy"), TEXT("string"), false);
	bOk &= ExpectSchemaParam(*this, Schema, TEXT("allow_empty_remap"), TEXT("boolean"), false);
	bOk &= ExpectSchemaParam(*this, Schema, TEXT("dry_run"), TEXT("boolean"), false);
	bOk &= ExpectSchemaParam(*this, Schema, TEXT("confirm"), TEXT("boolean"), false);
	bOk &= ExpectSchemaParam(*this, Schema, TEXT("compile"), TEXT("boolean"), false);
	bOk &= ExpectSchemaParam(*this, Schema, TEXT("save"), TEXT("boolean"), false);

	bOk &= ExpectAssetPathKind(*this, Schema, TEXT("source_asset_path"));
	bOk &= ExpectAssetPathKind(*this, Schema, TEXT("destination_asset_path"));
	bOk &= ExpectEnumValues(*this, Schema, TEXT("existing_policy"), { TEXT("fail"), TEXT("replace"), TEXT("skip") });
	bOk &= ExpectSchemaDefault(*this, Schema, TEXT("existing_policy"), TEXT("fail"));
	bOk &= ExpectSchemaDefault(*this, Schema, TEXT("allow_empty_remap"), TEXT("false"));
	bOk &= ExpectSchemaDefault(*this, Schema, TEXT("dry_run"), TEXT("true"));
	bOk &= ExpectSchemaDefault(*this, Schema, TEXT("confirm"), TEXT("false"));
	bOk &= ExpectSchemaDefault(*this, Schema, TEXT("compile"), TEXT("true"));
	bOk &= ExpectSchemaDefault(*this, Schema, TEXT("save"), TEXT("false"));
	bOk &= TestFalse(TEXT("search metadata is declared"), ActionInfo.SearchMetadata.IsEmpty());
	bOk &= TestTrue(
		TEXT("search metadata keywords include remap wording"),
		StringArrayContainsText(ActionInfo.SearchMetadata.Keywords, TEXT("remap")));
	bOk &= TestTrue(
		TEXT("search metadata aliases include clone_graphs"),
		StringArrayContainsText(ActionInfo.SearchMetadata.Aliases, TEXT("clone_graphs")));

	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBlueprintCloneGraphReferenceRemapRequiresConfirmTest,
	"Monolith.Blueprint.CloneGraphReferenceRemap.MutatingCallRequiresConfirm",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintCloneGraphReferenceRemapRequiresConfirmTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> Params = MakeMinimalCloneParams();
	Params->SetBoolField(TEXT("dry_run"), false);
	Params->SetBoolField(TEXT("confirm"), false);

	const FMonolithActionResult Result = FMonolithTestSupport::RegisterAndExecute(
		CloneGraphRemapNamespace,
		CloneGraphRemapAction,
		&RegisterCloneGraphRemapAction,
		Params);

	bool bOk = true;
	bOk &= TestFalse(TEXT("dry_run=false requires confirm=true"), Result.bSuccess);
	bOk &= TestTrue(
		TEXT("confirm guard reports confirm in the error"),
		Result.ErrorMessage.Contains(TEXT("confirm"), ESearchCase::IgnoreCase));
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBlueprintCloneGraphReferenceRemapRejectsMalformedGraphsTest,
	"Monolith.Blueprint.CloneGraphReferenceRemap.RejectsMalformedGraphsParam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintCloneGraphReferenceRemapRejectsMalformedGraphsTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> Params = MakeMinimalCloneParams();
	Params->SetObjectField(TEXT("graphs"), MakeShared<FJsonObject>());

	const FMonolithActionResult Result = FMonolithTestSupport::RegisterAndExecute(
		CloneGraphRemapNamespace,
		CloneGraphRemapAction,
		&RegisterCloneGraphRemapAction,
		Params);

	bool bOk = true;
	bOk &= TestFalse(TEXT("graphs object is rejected before handler execution"), Result.bSuccess);
	bOk &= TestTrue(
		TEXT("graphs type error names the malformed param"),
		Result.ErrorMessage.Contains(TEXT("graphs"), ESearchCase::IgnoreCase));
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
