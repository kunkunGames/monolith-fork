#include "CoreMinimal.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/AutomationTest.h"
#include "MonolithChooserActions.h"
#include "MonolithChooserAuthoringActions.h"
#include "MonolithChooserReadActions.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

#if WITH_CHOOSER
#include "Chooser.h"
#include "Curves/CurveFloat.h"
#include "ObjectChooser_Asset.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithChooserReadTests
{
	const FMonolithActionInfo* FindAction(
		const FMonolithToolRegistry& Registry,
		const FString& ActionName)
	{
		return Registry.GetActions(TEXT("chooser")).FindByPredicate(
			[&ActionName](const FMonolithActionInfo& Action)
			{
				return Action.Action == ActionName;
			});
	}

	void RegisterChooserActions(FMonolithToolRegistry& Registry)
	{
		if (!Registry.HasAction(TEXT("chooser"), TEXT("list_chooser_tables")))
		{
			FMonolithChooserReadActions::RegisterActions(Registry);
		}
		if (!Registry.HasAction(TEXT("chooser"), TEXT("inspect_chooser")))
		{
			FMonolithChooserActions::RegisterActions(Registry);
		}
		if (!Registry.HasAction(TEXT("chooser"), TEXT("create_chooser_table")))
		{
			FMonolithChooserAuthoringActions::RegisterActions(Registry);
		}
	}

	FString JoinIssueCodes(const TSharedPtr<FJsonObject>& Result)
	{
		const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr;
		if (!Result.IsValid()
			|| !Result->TryGetArrayField(TEXT("issues"), Issues)
			|| !Issues)
		{
			return TEXT("<issues unavailable>");
		}

		TArray<FString> Codes;
		Codes.Reserve(Issues->Num());
		for (const TSharedPtr<FJsonValue>& IssueValue : *Issues)
		{
			const TSharedPtr<FJsonObject>* Issue = nullptr;
			FString Code;
			if (IssueValue.IsValid()
				&& IssueValue->TryGetObject(Issue)
				&& Issue
				&& Issue->IsValid()
				&& (*Issue)->TryGetStringField(TEXT("code"), Code))
			{
				Codes.Add(Code);
			}
		}
		return Codes.IsEmpty() ? TEXT("<none>") : FString::Join(Codes, TEXT(", "));
	}

#if WITH_CHOOSER
	struct FChooserFixture
	{
		UChooserTable* Table = nullptr;
		UPackage* TablePackage = nullptr;
		UCurveFloat* OutputAsset = nullptr;
		UPackage* OutputPackage = nullptr;
		FString TablePackagePath;
		FString TableObjectPath;
		FString OutputObjectPath;
	};

	FChooserFixture CreateFixture()
	{
		FChooserFixture Fixture;
		const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);

		const FString TableName = TEXT("CHT_Read_") + Suffix;
		Fixture.TablePackagePath =
			TEXT("/Game/Developers/MonolithTests/") + TableName;
		Fixture.TablePackage = CreatePackage(*Fixture.TablePackagePath);
		Fixture.Table = NewObject<UChooserTable>(
			Fixture.TablePackage,
			*TableName,
			RF_Public | RF_Standalone | RF_Transactional);
		if (Fixture.Table)
		{
			FAssetRegistryModule::AssetCreated(Fixture.Table);
			Fixture.TablePackage->SetDirtyFlag(false);
			Fixture.TableObjectPath = Fixture.Table->GetPathName();
		}

		const FString OutputName = TEXT("Curve_ChooserOutput_") + Suffix;
		const FString OutputPackagePath =
			TEXT("/Game/Developers/MonolithTests/") + OutputName;
		Fixture.OutputPackage = CreatePackage(*OutputPackagePath);
		Fixture.OutputAsset = NewObject<UCurveFloat>(
			Fixture.OutputPackage,
			*OutputName,
			RF_Public | RF_Standalone | RF_Transactional);
		if (Fixture.OutputAsset)
		{
			FAssetRegistryModule::AssetCreated(Fixture.OutputAsset);
			Fixture.OutputPackage->SetDirtyFlag(false);
			Fixture.OutputObjectPath = Fixture.OutputAsset->GetPathName();
		}
		return Fixture;
	}

	void DiscardAsset(UObject* Asset, UPackage* Package)
	{
		if (Asset)
		{
			FAssetRegistryModule::AssetDeleted(Asset);
			Asset->ClearFlags(RF_Public | RF_Standalone);
			Asset->Rename(
				nullptr,
				GetTransientPackage(),
				REN_DontCreateRedirectors
					| REN_NonTransactional
					| REN_AllowPackageLinkerMismatch);
			Asset->MarkAsGarbage();
		}
		if (Package)
		{
			Package->SetDirtyFlag(false);
		}
	}

	void DiscardFixture(FChooserFixture& Fixture)
	{
		DiscardAsset(Fixture.Table, Fixture.TablePackage);
		DiscardAsset(Fixture.OutputAsset, Fixture.OutputPackage);
	}
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithChooserReadRegistrationTest,
	"Monolith.Chooser.Read.RegistrationAndSchemas",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithChooserReadRegistrationTest::RunTest(const FString& Parameters)
{
	using namespace MonolithChooserReadTests;

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	RegisterChooserActions(Registry);

	const TArray<FString> ExpectedReadActions = {
		TEXT("list_chooser_tables"),
		TEXT("get_chooser_table"),
		TEXT("list_chooser_columns"),
		TEXT("list_chooser_rows"),
		TEXT("list_chooser_references"),
		TEXT("validate_chooser_table")
	};
	for (const FString& Action : ExpectedReadActions)
	{
		TestTrue(
			*FString::Printf(TEXT("chooser.%s is registered"), *Action),
			Registry.HasAction(TEXT("chooser"), Action));
	}

	const TArray<FMonolithActionInfo> Actions = Registry.GetActions(TEXT("chooser"));
	TestEqual(TEXT("Chooser namespace has the closed 16-action lifecycle"), Actions.Num(), 16);

	TSet<FString> UniqueNames;
	for (const FMonolithActionInfo& Action : Actions)
	{
		UniqueNames.Add(Action.Action);
	}
	TestEqual(TEXT("Chooser action names are unique"), UniqueNames.Num(), Actions.Num());

	const FMonolithActionInfo* ListAction =
		FindAction(Registry, TEXT("list_chooser_tables"));
	const FMonolithActionInfo* GetAction =
		FindAction(Registry, TEXT("get_chooser_table"));
	const FMonolithActionInfo* RowsAction =
		FindAction(Registry, TEXT("list_chooser_rows"));
	TestTrue(
		TEXT("list_chooser_tables publishes bounded pagination"),
		ListAction
			&& ListAction->ParamSchema.IsValid()
			&& ListAction->ParamSchema->HasField(TEXT("offset"))
			&& ListAction->ParamSchema->HasField(TEXT("limit")));
	TestTrue(
		TEXT("get_chooser_table requires asset_path"),
		GetAction
			&& GetAction->ParamSchema.IsValid()
			&& GetAction->ParamSchema->HasField(TEXT("asset_path")));
	TestTrue(
		TEXT("list_chooser_rows publishes start_row and limit"),
		RowsAction
			&& RowsAction->ParamSchema.IsValid()
			&& RowsAction->ParamSchema->HasField(TEXT("start_row"))
			&& RowsAction->ParamSchema->HasField(TEXT("limit")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithChooserReadParamGuardTest,
	"Monolith.Chooser.Read.ParamGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithChooserReadParamGuardTest::RunTest(const FString& Parameters)
{
	using namespace MonolithChooserReadTests;

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	RegisterChooserActions(Registry);

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("Game/Choosers/CHT_Invalid"));
		const FMonolithActionResult Result = Registry.ExecuteAction(
			TEXT("chooser"),
			TEXT("get_chooser_table"),
			Params);
		TestFalse(TEXT("Relative asset paths are rejected"), Result.bSuccess);
		TestEqual(
			TEXT("Relative path failure is ErrInvalidParams"),
			Result.ErrorCode,
			FMonolithJsonUtils::ErrInvalidParams);
		TestTrue(
			TEXT("Relative path error explains the canonical path contract"),
			Result.ErrorMessage.Contains(TEXT("canonical Unreal")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(
			TEXT("asset_path"),
			TEXT("\\Game\\Choosers\\CHT_Invalid"));
		const FMonolithActionResult Result = Registry.ExecuteAction(
			TEXT("chooser"),
			TEXT("get_chooser_table"),
			Params);
		TestFalse(TEXT("Backslash asset paths are rejected without normalization"), Result.bSuccess);
		TestEqual(
			TEXT("Backslash path failure is ErrInvalidParams"),
			Result.ErrorCode,
			FMonolithJsonUtils::ErrInvalidParams);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(
			TEXT("asset_path"),
			TEXT("/Game/Choosers/CHT_Invalid.DifferentObject"));
		const FMonolithActionResult Result = Registry.ExecuteAction(
			TEXT("chooser"),
			TEXT("get_chooser_table"),
			Params);
		TestFalse(TEXT("Mismatched top-level object names are rejected"), Result.bSuccess);
		TestEqual(
			TEXT("Object-name mismatch is ErrInvalidParams"),
			Result.ErrorCode,
			FMonolithJsonUtils::ErrInvalidParams);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path_filter"), TEXT("Game/Choosers"));
		const FMonolithActionResult Result = Registry.ExecuteAction(
			TEXT("chooser"),
			TEXT("list_chooser_tables"),
			Params);
		TestFalse(TEXT("Relative list filters are rejected"), Result.bSuccess);
		TestEqual(
			TEXT("Relative list filter failure is ErrInvalidParams"),
			Result.ErrorCode,
			FMonolithJsonUtils::ErrInvalidParams);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 0);
		const FMonolithActionResult Result = Registry.ExecuteAction(
			TEXT("chooser"),
			TEXT("list_chooser_tables"),
			Params);
		TestFalse(TEXT("Zero list limit is rejected"), Result.bSuccess);
		TestEqual(
			TEXT("Limit range failure is ErrInvalidParams"),
			Result.ErrorCode,
			FMonolithJsonUtils::ErrInvalidParams);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Missing/CHT_Missing.CHT_Missing"));
		Params->SetStringField(TEXT("row_limit"), TEXT("50"));
		const FMonolithActionResult Result = Registry.ExecuteAction(
			TEXT("chooser"),
			TEXT("get_chooser_table"),
			Params);
		TestFalse(TEXT("String row_limit is rejected"), Result.bSuccess);
		TestEqual(
			TEXT("Schema type failure is ErrInvalidParams"),
			Result.ErrorCode,
			FMonolithJsonUtils::ErrInvalidParams);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetBoolField(TEXT("asset_path"), true);
		const FMonolithActionResult Result = Registry.ExecuteAction(
			TEXT("chooser"),
			TEXT("get_chooser_table"),
			Params);
		TestFalse(TEXT("Boolean asset_path is rejected"), Result.bSuccess);
		TestEqual(
			TEXT("Boolean asset_path failure is ErrInvalidParams"),
			Result.ErrorCode,
			FMonolithJsonUtils::ErrInvalidParams);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Missing/CHT_Missing.CHT_Missing"));
		Params->SetStringField(TEXT("include_rows"), TEXT("true"));
		const FMonolithActionResult Result = Registry.ExecuteAction(
			TEXT("chooser"),
			TEXT("get_chooser_table"),
			Params);
		TestFalse(TEXT("String include_rows is rejected"), Result.bSuccess);
		TestEqual(
			TEXT("String include_rows failure is ErrInvalidParams"),
			Result.ErrorCode,
			FMonolithJsonUtils::ErrInvalidParams);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithChooserReadEmptyValidationTest,
	"Monolith.Chooser.Read.EmptyTableValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithChooserReadEmptyValidationTest::RunTest(const FString& Parameters)
{
#if !WITH_CHOOSER
	AddInfo(TEXT("Chooser plugin is disabled for this target; exact asset readback is covered by the enabled-host test lanes."));
	return true;
#else
	using namespace MonolithChooserReadTests;

	FChooserFixture Fixture = CreateFixture();
	if (!TestNotNull(TEXT("Creates a ChooserTable fixture"), Fixture.Table))
	{
		DiscardFixture(Fixture);
		return false;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	RegisterChooserActions(Registry);

	TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
	GetParams->SetStringField(TEXT("asset_path"), Fixture.TablePackagePath);
	GetParams->SetBoolField(TEXT("include_rows"), true);
	const FMonolithActionResult GetResult = Registry.ExecuteAction(
		TEXT("chooser"),
		TEXT("get_chooser_table"),
		GetParams);
	TestTrue(TEXT("Package-path readback succeeds"), GetResult.bSuccess);
	if (GetResult.bSuccess)
	{
		TestEqual(
			TEXT("Readback returns canonical object path"),
			GetResult.Result->GetStringField(TEXT("asset_path")),
			Fixture.TableObjectPath);
		TestEqual(
			TEXT("Empty table reports zero rows"),
			static_cast<int32>(GetResult.Result->GetNumberField(TEXT("row_count"))),
			0);
	}

	TSharedPtr<FJsonObject> ValidateParams = MakeShared<FJsonObject>();
	ValidateParams->SetStringField(TEXT("asset_path"), Fixture.TableObjectPath);
	const FMonolithActionResult ValidateResult = Registry.ExecuteAction(
		TEXT("chooser"),
		TEXT("validate_chooser_table"),
		ValidateParams);
	TestTrue(TEXT("Empty-table validation executes"), ValidateResult.bSuccess);
	if (ValidateResult.bSuccess)
	{
		TestTrue(
			TEXT("Warnings alone do not make an empty table structurally invalid"),
			ValidateResult.Result->GetBoolField(TEXT("valid")));
		TestEqual(
			TEXT("Empty table has zero validation errors"),
			static_cast<int32>(ValidateResult.Result->GetNumberField(TEXT("error_count"))),
			0);
		TestTrue(
			TEXT("Empty table reports advisory warnings"),
			ValidateResult.Result->GetNumberField(TEXT("warning_count")) >= 2.0);
	}

	TSharedPtr<FJsonObject> ListParams = MakeShared<FJsonObject>();
	ListParams->SetStringField(
		TEXT("path_filter"),
		TEXT("/Game/Developers/MonolithTests"));
	const FMonolithActionResult ListResult = Registry.ExecuteAction(
		TEXT("chooser"),
		TEXT("list_chooser_tables"),
		ListParams);
	TestTrue(TEXT("Exact package-prefix discovery succeeds"), ListResult.bSuccess);
	if (ListResult.bSuccess)
	{
		TestTrue(
			TEXT("Discovery includes the in-memory ChooserTable fixture"),
			ListResult.Result->GetNumberField(TEXT("total")) >= 1.0);
	}

	TestFalse(
		TEXT("Read and validation actions preserve the package's clean state"),
		Fixture.TablePackage->IsDirty());
	DiscardFixture(Fixture);
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithChooserReadAuthoringRoundTripTest,
	"Monolith.Chooser.Read.AuthoringRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithChooserReadAuthoringRoundTripTest::RunTest(const FString& Parameters)
{
#if !WITH_CHOOSER
	AddInfo(TEXT("Chooser plugin is disabled for this target; authoring round-trip is covered by the enabled-host test lanes."));
	return true;
#else
	using namespace MonolithChooserReadTests;

	FChooserFixture Fixture = CreateFixture();
	if (!TestNotNull(TEXT("Creates a ChooserTable fixture"), Fixture.Table)
		|| !TestNotNull(TEXT("Creates a referenced output fixture"), Fixture.OutputAsset))
	{
		DiscardFixture(Fixture);
		return false;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	RegisterChooserActions(Registry);

	TSharedPtr<FJsonObject> ColumnParams = MakeShared<FJsonObject>();
	ColumnParams->SetStringField(TEXT("asset_path"), Fixture.TableObjectPath);
	ColumnParams->SetStringField(TEXT("column_kind"), TEXT("Bool"));
	const FMonolithActionResult ColumnResult = Registry.ExecuteAction(
		TEXT("chooser"),
		TEXT("add_chooser_column"),
		ColumnParams);
	TestTrue(TEXT("Existing authoring action adds a Bool column"), ColumnResult.bSuccess);

	TArray<TSharedPtr<FJsonValue>> Cells;
	Cells.Add(MakeShared<FJsonValueBoolean>(true));
	TSharedPtr<FJsonObject> RowParams = MakeShared<FJsonObject>();
	RowParams->SetStringField(TEXT("asset_path"), Fixture.TableObjectPath);
	RowParams->SetArrayField(TEXT("cells"), Cells);
	RowParams->SetStringField(TEXT("output_psd"), Fixture.OutputObjectPath);
	bool RowsAuthored = true;
	for (int32 RowIndex = 0; RowIndex < 9; ++RowIndex)
	{
		const FMonolithActionResult RowResult = Registry.ExecuteAction(
			TEXT("chooser"),
			TEXT("add_chooser_row"),
			RowParams);
		TestTrue(
			*FString::Printf(
				TEXT("Existing authoring action adds aligned row %d"),
				RowIndex),
			RowResult.bSuccess);
		RowsAuthored &= RowResult.bSuccess;
	}
	if (!ColumnResult.bSuccess || !RowsAuthored)
	{
		DiscardFixture(Fixture);
		return false;
	}

	// CookedResults is derived data and can temporarily be stale after editor
	// mutations. It must never inflate the authoritative editor row count.
	Fixture.Table->CookedResults = Fixture.Table->ResultsStructs;
	Fixture.Table->CookedResults.AddDefaulted(3);
	Fixture.TablePackage->SetDirtyFlag(false);

	TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
	GetParams->SetStringField(TEXT("asset_path"), Fixture.TableObjectPath);
	GetParams->SetBoolField(TEXT("include_rows"), true);
	const FMonolithActionResult GetResult = Registry.ExecuteAction(
		TEXT("chooser"),
		TEXT("get_chooser_table"),
		GetParams);
	TestTrue(TEXT("Authored table readback succeeds"), GetResult.bSuccess);
	if (GetResult.bSuccess)
	{
		TestEqual(
			TEXT("Readback reports nine rows"),
			static_cast<int32>(GetResult.Result->GetNumberField(TEXT("row_count"))),
			9);
		TestEqual(
			TEXT("Readback exposes twelve stale cooked results without treating them as rows"),
			static_cast<int32>(
				GetResult.Result->GetNumberField(TEXT("cooked_result_count"))),
			12);
		TestEqual(
			TEXT("Readback reports one column"),
			static_cast<int32>(GetResult.Result->GetNumberField(TEXT("column_count"))),
			1);
	}

	TSharedPtr<FJsonObject> ReadParams = MakeShared<FJsonObject>();
	ReadParams->SetStringField(TEXT("asset_path"), Fixture.TableObjectPath);
	const FMonolithActionResult ColumnsResult = Registry.ExecuteAction(
		TEXT("chooser"),
		TEXT("list_chooser_columns"),
		ReadParams);
	const FMonolithActionResult RowsResult = Registry.ExecuteAction(
		TEXT("chooser"),
		TEXT("list_chooser_rows"),
		ReadParams);
	const FMonolithActionResult ReferencesResult = Registry.ExecuteAction(
		TEXT("chooser"),
		TEXT("list_chooser_references"),
		ReadParams);
	const FMonolithActionResult ValidateResult = Registry.ExecuteAction(
		TEXT("chooser"),
		TEXT("validate_chooser_table"),
		ReadParams);

	TestTrue(TEXT("Column readback succeeds"), ColumnsResult.bSuccess);
	TestTrue(TEXT("Row readback succeeds"), RowsResult.bSuccess);
	TestTrue(TEXT("Reference readback succeeds"), ReferencesResult.bSuccess);
	TestTrue(TEXT("Structural validation succeeds"), ValidateResult.bSuccess);
	if (ColumnsResult.bSuccess)
	{
		TestEqual(
			TEXT("Column readback returns one column"),
			static_cast<int32>(ColumnsResult.Result->GetNumberField(TEXT("count"))),
			1);
		const TArray<TSharedPtr<FJsonValue>>& Columns =
			ColumnsResult.Result->GetArrayField(TEXT("columns"));
		const TSharedPtr<FJsonObject>* FirstColumn = nullptr;
		TestTrue(
			TEXT("Column readback returns an object summary"),
			Columns.Num() == 1
				&& Columns[0].IsValid()
				&& Columns[0]->TryGetObject(FirstColumn)
				&& FirstColumn
				&& FirstColumn->IsValid());
		if (FirstColumn && FirstColumn->IsValid())
		{
			TestEqual(
				TEXT("Bool column readback ignores the deprecated RowValues array"),
				static_cast<int32>((*FirstColumn)->GetNumberField(TEXT("row_value_count"))),
				9);

			FString RowValuesProperty;
			const TSharedPtr<FJsonObject>* Fields = nullptr;
			const TSharedPtr<FJsonObject>* SerializedRowValues = nullptr;
			const bool HasBoundedRowValues =
				(*FirstColumn)->TryGetStringField(
					TEXT("row_values_property"),
					RowValuesProperty)
				&& (*FirstColumn)->TryGetObjectField(TEXT("fields"), Fields)
				&& Fields
				&& Fields->IsValid()
				&& (*Fields)->TryGetObjectField(
					RowValuesProperty,
					SerializedRowValues)
				&& SerializedRowValues
				&& SerializedRowValues->IsValid();
			TestTrue(
				TEXT("Compact column fields expose the active bounded row-value container"),
				HasBoundedRowValues);
			if (HasBoundedRowValues)
			{
				TestEqual(
					TEXT("Bounded serializer preserves the full container count"),
					static_cast<int32>(
						(*SerializedRowValues)->GetNumberField(TEXT("count"))),
					9);
				TestEqual(
					TEXT("Compact serializer emits at most eight row values"),
					(*SerializedRowValues)->GetArrayField(TEXT("items")).Num(),
					8);
				TestEqual(
					TEXT("Compact serializer reports its truncation boundary"),
					static_cast<int32>(
						(*SerializedRowValues)->GetNumberField(
							TEXT("truncated_after"))),
					8);
			}
		}
	}
	if (RowsResult.bSuccess)
	{
		TestEqual(
			TEXT("Row readback returns nine rows"),
			static_cast<int32>(RowsResult.Result->GetNumberField(TEXT("count"))),
			9);
	}
	if (ReferencesResult.bSuccess)
	{
		TestTrue(
			TEXT("Reference readback sees the authored output asset"),
			ReferencesResult.Result->GetNumberField(TEXT("total")) >= 1.0);
	}
	if (ValidateResult.bSuccess)
	{
		const FString IssueCodes = JoinIssueCodes(ValidateResult.Result);
		TestTrue(
			*FString::Printf(
				TEXT("Aligned authored table validates cleanly (issues: %s)"),
				*IssueCodes),
			ValidateResult.Result->GetBoolField(TEXT("valid")));
		TestEqual(
			TEXT("Aligned table has no validation errors"),
			static_cast<int32>(ValidateResult.Result->GetNumberField(TEXT("error_count"))),
			0);
	}

	TestFalse(
		TEXT("All readback actions preserve the package's clean state"),
		Fixture.TablePackage->IsDirty());
	DiscardFixture(Fixture);
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithChooserReadRootContextAndResultPayloadTest,
	"Monolith.Chooser.Read.RootContextAndResultPayloadValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithChooserReadRootContextAndResultPayloadTest::RunTest(
	const FString& Parameters)
{
#if !WITH_CHOOSER
	AddInfo(TEXT("Chooser plugin is disabled for this target; root-context and payload validation are covered by the enabled-host test lanes."));
	return true;
#else
	using namespace MonolithChooserReadTests;

	FChooserFixture Fixture = CreateFixture();
	if (!TestNotNull(TEXT("Creates a root ChooserTable fixture"), Fixture.Table))
	{
		DiscardFixture(Fixture);
		return false;
	}

	Fixture.Table->ContextData.AddDefaulted();
	Fixture.Table->ResultsStructs.AddDefaulted();
	Fixture.Table->DisabledRows.Add(false);
	Fixture.Table->ResultsStructs.Add(
		FInstancedStruct::Make<FSoftAssetChooser>());
	Fixture.Table->DisabledRows.Add(false);

	const FString ChildName =
		TEXT("CHT_ReadChild_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ChildPackagePath =
		TEXT("/Game/Developers/MonolithTests/") + ChildName;
	UPackage* ChildPackage = CreatePackage(*ChildPackagePath);
	UChooserTable* Child = NewObject<UChooserTable>(
		ChildPackage,
		*ChildName,
		RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("Creates a child ChooserTable fixture"), Child))
	{
		DiscardFixture(Fixture);
		return false;
	}
	FAssetRegistryModule::AssetCreated(Child);
	Child->RootChooser = Fixture.Table;
	Fixture.TablePackage->SetDirtyFlag(false);
	ChildPackage->SetDirtyFlag(false);

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	RegisterChooserActions(Registry);

	TSharedPtr<FJsonObject> ChildParams = MakeShared<FJsonObject>();
	ChildParams->SetStringField(TEXT("asset_path"), Child->GetPathName());
	const FMonolithActionResult ChildResult = Registry.ExecuteAction(
		TEXT("chooser"),
		TEXT("get_chooser_table"),
		ChildParams);
	TestTrue(TEXT("Child table readback succeeds"), ChildResult.bSuccess);
	if (ChildResult.bSuccess)
	{
		TestEqual(
			TEXT("Child readback uses the root chooser's context view"),
			static_cast<int32>(
				ChildResult.Result->GetNumberField(TEXT("context_entry_count"))),
			1);
	}

	TSharedPtr<FJsonObject> RootParams = MakeShared<FJsonObject>();
	RootParams->SetStringField(TEXT("asset_path"), Fixture.TableObjectPath);
	const FMonolithActionResult ValidateResult = Registry.ExecuteAction(
		TEXT("chooser"),
		TEXT("validate_chooser_table"),
		RootParams);
	TestTrue(TEXT("Result-payload validation executes"), ValidateResult.bSuccess);
	if (ValidateResult.bSuccess)
	{
		const FString IssueCodes = JoinIssueCodes(ValidateResult.Result);
		TestFalse(
			TEXT("Invalid and null result payloads make validation fail"),
			ValidateResult.Result->GetBoolField(TEXT("valid")));
		TestTrue(
			*FString::Printf(
				TEXT("Invalid result struct is reported (issues: %s)"),
				*IssueCodes),
			IssueCodes.Contains(TEXT("invalid_result_struct")));
		TestTrue(
			*FString::Printf(
				TEXT("Known result type with a null target is reported (issues: %s)"),
				*IssueCodes),
			IssueCodes.Contains(TEXT("invalid_result_payload")));
	}

	TestFalse(
		TEXT("Readback preserves the root package's clean state"),
		Fixture.TablePackage->IsDirty());
	TestFalse(
		TEXT("Readback preserves the child package's clean state"),
		ChildPackage->IsDirty());
	DiscardAsset(Child, ChildPackage);
	DiscardFixture(Fixture);
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithChooserReadDeletedAssetPackageShellTest,
	"Monolith.Chooser.Read.DeletedAssetPackageShell",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithChooserReadDeletedAssetPackageShellTest::RunTest(const FString& Parameters)
{
#if !WITH_CHOOSER
	AddInfo(TEXT("Chooser plugin is disabled for this target; exact reference validation is covered by the enabled-host test lanes."));
	return true;
#else
	using namespace MonolithChooserReadTests;

	FChooserFixture Fixture = CreateFixture();
	if (!TestNotNull(TEXT("Creates a ChooserTable fixture"), Fixture.Table)
		|| !TestNotNull(TEXT("Creates a referenced output fixture"), Fixture.OutputAsset)
		|| !TestNotNull(TEXT("Creates an output package fixture"), Fixture.OutputPackage))
	{
		DiscardFixture(Fixture);
		return false;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	RegisterChooserActions(Registry);

	TSharedPtr<FJsonObject> ColumnParams = MakeShared<FJsonObject>();
	ColumnParams->SetStringField(TEXT("asset_path"), Fixture.TableObjectPath);
	ColumnParams->SetStringField(TEXT("column_kind"), TEXT("Bool"));
	const FMonolithActionResult ColumnResult = Registry.ExecuteAction(
		TEXT("chooser"),
		TEXT("add_chooser_column"),
		ColumnParams);

	TArray<TSharedPtr<FJsonValue>> Cells;
	Cells.Add(MakeShared<FJsonValueBoolean>(true));
	TSharedPtr<FJsonObject> RowParams = MakeShared<FJsonObject>();
	RowParams->SetStringField(TEXT("asset_path"), Fixture.TableObjectPath);
	RowParams->SetArrayField(TEXT("cells"), Cells);
	RowParams->SetStringField(TEXT("output_psd"), Fixture.OutputObjectPath);
	const FMonolithActionResult RowResult = Registry.ExecuteAction(
		TEXT("chooser"),
		TEXT("add_chooser_row"),
		RowParams);
	if (!TestTrue(TEXT("Adds the fixture input column"), ColumnResult.bSuccess)
		|| !TestTrue(TEXT("Adds the fixture result row"), RowResult.bSuccess))
	{
		DiscardFixture(Fixture);
		return false;
	}

	const FString MissingAssetName =
		TEXT("Curve_MissingChooserOutput_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString MissingPackageName =
		TEXT("/Game/Developers/MonolithTests/") + MissingAssetName;
	const FString MissingObjectPath =
		MissingPackageName + TEXT(".") + MissingAssetName;
	UPackage* MissingPackage = CreatePackage(*MissingPackageName);
	if (!TestNotNull(
			TEXT("Creates the empty package shell used by the regression"),
			MissingPackage)
		|| !TestTrue(
			TEXT("Fixture row contains a mutable result struct"),
			Fixture.Table->ResultsStructs.IsValidIndex(0)))
	{
		DiscardFixture(Fixture);
		return false;
	}

	FInstancedStruct& ResultStruct = Fixture.Table->ResultsStructs[0];
	ResultStruct.InitializeAs(FSoftAssetChooser::StaticStruct());
	ResultStruct.GetMutable<FSoftAssetChooser>().Asset =
		TSoftObjectPtr<UObject>(FSoftObjectPath(MissingObjectPath));
	Fixture.TablePackage->SetDirtyFlag(false);
	MissingPackage->SetDirtyFlag(false);

	TestNotNull(
		TEXT("The missing asset's empty UPackage shell is loaded"),
		FindPackage(nullptr, *MissingPackageName));
	TestFalse(
		TEXT("The empty package shell has no on-disk package"),
		FPackageName::DoesPackageExist(MissingPackageName));

	TSharedPtr<FJsonObject> ReadParams = MakeShared<FJsonObject>();
	ReadParams->SetStringField(TEXT("asset_path"), Fixture.TableObjectPath);
	const FMonolithActionResult ReferencesResult = Registry.ExecuteAction(
		TEXT("chooser"),
		TEXT("list_chooser_references"),
		ReadParams);
	const FMonolithActionResult ValidateResult = Registry.ExecuteAction(
		TEXT("chooser"),
		TEXT("validate_chooser_table"),
		ReadParams);

	if (!TestTrue(TEXT("Reference readback succeeds for a missing soft target"), ReferencesResult.bSuccess)
		|| !TestTrue(TEXT("Structural validation executes for a missing soft target"), ValidateResult.bSuccess))
	{
		DiscardFixture(Fixture);
		return false;
	}

	bool bFoundMissingReference = false;
	bool bMissingReferenceExists = true;
	const TArray<TSharedPtr<FJsonValue>>& References =
		ReferencesResult.Result->GetArrayField(TEXT("references"));
	for (const TSharedPtr<FJsonValue>& ReferenceValue : References)
	{
		const TSharedPtr<FJsonObject>* Reference = nullptr;
		FString ReferencePath;
		if (ReferenceValue.IsValid()
			&& ReferenceValue->TryGetObject(Reference)
			&& Reference
			&& Reference->IsValid()
			&& (*Reference)->TryGetStringField(TEXT("path"), ReferencePath)
			&& ReferencePath.Equals(MissingObjectPath, ESearchCase::CaseSensitive))
		{
			bFoundMissingReference = true;
			(*Reference)->TryGetBoolField(TEXT("exists"), bMissingReferenceExists);
			break;
		}
	}

	TestTrue(TEXT("Reference readback retains the missing soft path"), bFoundMissingReference);
	TestFalse(
		TEXT("A loaded empty package shell is not accepted as asset existence"),
		bMissingReferenceExists);
	TestFalse(
		TEXT("Missing soft target makes structural validation invalid"),
		ValidateResult.Result->GetBoolField(TEXT("valid")));
	TestTrue(
		TEXT("Missing soft target produces a validation error"),
		ValidateResult.Result->GetNumberField(TEXT("error_count")) >= 1.0);
	TestTrue(
		TEXT("Missing soft target reports unresolved_soft_reference"),
		JoinIssueCodes(ValidateResult.Result).Contains(TEXT("unresolved_soft_reference")));
	TestFalse(
		TEXT("Reference readback and validation preserve the table package's clean state"),
		Fixture.TablePackage->IsDirty());

	MissingPackage->SetDirtyFlag(false);
	DiscardFixture(Fixture);
	return true;
#endif
}

#endif // WITH_DEV_AUTOMATION_TESTS
