// Copyright Epic Games, Inc. All Rights Reserved.
// Material-instance validation contract regression coverage.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "MonolithMaterialActions.h"
#include "MonolithToolRegistry.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMaterialInstanceValidationTest,
	"Monolith.Material.Validation.MaterialInstanceResolvesOwnedBaseGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialInstanceValidationTest::RunTest(const FString& Parameters)
{
	const FMonolithActionInfo* ValidateActionInfo = nullptr;
	const TArray<FMonolithActionInfo> MaterialActions = FMonolithToolRegistry::Get().GetActions(TEXT("material"));
	for (const FMonolithActionInfo& ActionInfo : MaterialActions)
	{
		if (ActionInfo.Action == TEXT("validate_material"))
		{
			ValidateActionInfo = &ActionInfo;
			break;
		}
	}
	TestNotNull(TEXT("validate_material live action should be registered"), ValidateActionInfo);
	if (ValidateActionInfo)
	{
		TestTrue(
			TEXT("Live action description should advertise material-instance support"),
			ValidateActionInfo->Description.Contains(TEXT("material instance"), ESearchCase::IgnoreCase));

		const TSharedPtr<FJsonObject>* AssetPathSchema = nullptr;
		const TSharedPtr<FJsonObject>* FixIssuesSchema = nullptr;
		TestTrue(
			TEXT("validate_material schema should contain asset_path"),
			ValidateActionInfo->ParamSchema.IsValid()
				&& ValidateActionInfo->ParamSchema->TryGetObjectField(TEXT("asset_path"), AssetPathSchema));
		TestTrue(
			TEXT("validate_material schema should contain fix_issues"),
			ValidateActionInfo->ParamSchema.IsValid()
				&& ValidateActionInfo->ParamSchema->TryGetObjectField(TEXT("fix_issues"), FixIssuesSchema));
		if (AssetPathSchema && AssetPathSchema->IsValid())
		{
			FString Description;
			(*AssetPathSchema)->TryGetStringField(TEXT("description"), Description);
			TestTrue(
				TEXT("asset_path schema should accept material instances explicitly"),
				Description.Contains(TEXT("material instance"), ESearchCase::IgnoreCase));
		}
		if (FixIssuesSchema && FixIssuesSchema->IsValid())
		{
			FString Description;
			(*FixIssuesSchema)->TryGetStringField(TEXT("description"), Description);
			TestTrue(
				TEXT("fix_issues schema should expose the instance ownership guard"),
				Description.Contains(TEXT("rejected for material instances"), ESearchCase::IgnoreCase));
		}
	}

	const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = FString::Printf(
		TEXT("/Game/Tests/Monolith/Transient/M_MaterialInstanceValidation_%s"),
		*UniqueSuffix);
	UPackage* Package = CreatePackage(*PackageName);
	UMaterial* BaseMaterial = Package
		? NewObject<UMaterial>(Package, TEXT("M_ValidationBase"), RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	UMaterialInstanceConstant* ParentInstance = Package
		? NewObject<UMaterialInstanceConstant>(Package, TEXT("MI_ValidationParent"), RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	UMaterialInstanceConstant* ChildInstance = Package
		? NewObject<UMaterialInstanceConstant>(Package, TEXT("MI_ValidationChild"), RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	UMaterialInstanceConstant* MissingParentInstance = Package
		? NewObject<UMaterialInstanceConstant>(Package, TEXT("MI_ValidationMissingParent"), RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	UTexture2D* NonMaterialAsset = Package
		? NewObject<UTexture2D>(Package, TEXT("T_ValidationNotMaterial"), RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;

	ON_SCOPE_EXIT
	{
		if (Package)
		{
			Package->SetDirtyFlag(false);
		}
		for (UObject* Object : { static_cast<UObject*>(NonMaterialAsset), static_cast<UObject*>(ChildInstance), static_cast<UObject*>(ParentInstance), static_cast<UObject*>(MissingParentInstance), static_cast<UObject*>(BaseMaterial) })
		{
			if (Object)
			{
				Object->ClearFlags(RF_Public | RF_Standalone);
				Object->MarkAsGarbage();
			}
		}
		if (Package)
		{
			Package->MarkAsGarbage();
		}
	};

	TestNotNull(TEXT("Validation package should be created"), Package);
	TestNotNull(TEXT("Base material should be created"), BaseMaterial);
	TestNotNull(TEXT("Parent MIC should be created"), ParentInstance);
	TestNotNull(TEXT("Child MIC should be created"), ChildInstance);
	TestNotNull(TEXT("Missing-parent MIC should be created"), MissingParentInstance);
	TestNotNull(TEXT("Non-material type-safety probe should be created"), NonMaterialAsset);
	if (!Package || !BaseMaterial || !ParentInstance || !ChildInstance || !MissingParentInstance || !NonMaterialAsset)
	{
		return false;
	}

	ParentInstance->SetParentEditorOnly(BaseMaterial);
	ChildInstance->SetParentEditorOnly(ParentInstance);

	TSharedPtr<FJsonObject> ValidateParams = MakeShared<FJsonObject>();
	ValidateParams->SetStringField(TEXT("asset_path"), ChildInstance->GetPathName());
	const FMonolithActionResult ValidateResult = FMonolithMaterialActions::ValidateMaterial(ValidateParams);
	TestTrue(TEXT("Nested MIC validation should succeed"), ValidateResult.bSuccess);
	if (ValidateResult.bSuccess && ValidateResult.Result.IsValid())
	{
		FString AssetType;
		FString ValidationScope;
		FString ValidatedGraphPath;
		double ParentChainDepth = -1.0;
		double IssueCount = -1.0;
		const TArray<TSharedPtr<FJsonValue>>* ParentChain = nullptr;

		TestTrue(TEXT("Result should contain asset_type"), ValidateResult.Result->TryGetStringField(TEXT("asset_type"), AssetType));
		TestEqual(TEXT("Result should identify the requested MIC type"), AssetType, FString(TEXT("MaterialInstanceConstant")));
		TestTrue(TEXT("Result should contain validation_scope"), ValidateResult.Result->TryGetStringField(TEXT("validation_scope"), ValidationScope));
		TestEqual(
			TEXT("MIC validation scope should include the resolved base graph"),
			ValidationScope,
			FString(TEXT("material_instance_and_resolved_base_graph")));
		TestTrue(TEXT("Result should contain validated_graph_asset_path"), ValidateResult.Result->TryGetStringField(TEXT("validated_graph_asset_path"), ValidatedGraphPath));
		TestEqual(TEXT("MIC validation should name its owned base graph"), ValidatedGraphPath, BaseMaterial->GetPathName());
		TestTrue(TEXT("Result should contain parent-chain depth"), ValidateResult.Result->TryGetNumberField(TEXT("instance_parent_chain_depth"), ParentChainDepth));
		TestEqual(TEXT("Nested MIC should have a two-link parent chain"), ParentChainDepth, 2.0);
		TestTrue(TEXT("Result should contain issue_count"), ValidateResult.Result->TryGetNumberField(TEXT("issue_count"), IssueCount));
		TestEqual(TEXT("Valid MIC and base graph should have no validation issues"), IssueCount, 0.0);
		TestTrue(TEXT("Result should contain explicit parent chain"), ValidateResult.Result->TryGetArrayField(TEXT("instance_parent_chain"), ParentChain));
		if (ParentChain && ParentChain->Num() == 2)
		{
			TestEqual(TEXT("First parent-chain row should be the immediate parent"), (*ParentChain)[0]->AsString(), ParentInstance->GetPathName());
			TestEqual(TEXT("Parent-chain root should be the base material"), (*ParentChain)[1]->AsString(), BaseMaterial->GetPathName());
		}
		else
		{
			AddError(TEXT("Explicit parent chain should contain exactly two entries"));
		}
	}
	else if (!ValidateResult.bSuccess)
	{
		AddError(ValidateResult.ErrorMessage);
	}

	TSharedPtr<FJsonObject> BaseValidateParams = MakeShared<FJsonObject>();
	BaseValidateParams->SetStringField(TEXT("asset_path"), BaseMaterial->GetPathName());
	const FMonolithActionResult BaseValidateResult = FMonolithMaterialActions::ValidateMaterial(BaseValidateParams);
	TestTrue(TEXT("Existing base-material validation should remain supported"), BaseValidateResult.bSuccess);
	if (BaseValidateResult.bSuccess && BaseValidateResult.Result.IsValid())
	{
		FString BaseScope;
		double BaseParentChainDepth = -1.0;
		BaseValidateResult.Result->TryGetStringField(TEXT("validation_scope"), BaseScope);
		BaseValidateResult.Result->TryGetNumberField(TEXT("instance_parent_chain_depth"), BaseParentChainDepth);
		TestEqual(TEXT("Base material should retain base-graph scope"), BaseScope, FString(TEXT("base_material_graph")));
		TestEqual(TEXT("Base material should not report an instance parent chain"), BaseParentChainDepth, 0.0);
	}

	TSharedPtr<FJsonObject> FixParams = MakeShared<FJsonObject>();
	FixParams->SetStringField(TEXT("asset_path"), ChildInstance->GetPathName());
	FixParams->SetBoolField(TEXT("fix_issues"), true);
	const FMonolithActionResult FixResult = FMonolithMaterialActions::ValidateMaterial(FixParams);
	TestFalse(TEXT("MIC validation must not mutate its parent graph through fix_issues"), FixResult.bSuccess);
	TestTrue(
		TEXT("MIC fix rejection should explain graph ownership"),
		FixResult.ErrorMessage.Contains(TEXT("graph ownership belongs to its resolved base material")));

	TSharedPtr<FJsonObject> MissingParentParams = MakeShared<FJsonObject>();
	MissingParentParams->SetStringField(TEXT("asset_path"), MissingParentInstance->GetPathName());
	const FMonolithActionResult MissingParentResult = FMonolithMaterialActions::ValidateMaterial(MissingParentParams);
	TestFalse(TEXT("MIC with no explicit parent must fail validation"), MissingParentResult.bSuccess);
	TestTrue(
		TEXT("Missing-parent failure should identify the invalid source contract"),
		MissingParentResult.ErrorMessage.Contains(TEXT("missing parent")));

	TSharedPtr<FJsonObject> NonMaterialParams = MakeShared<FJsonObject>();
	NonMaterialParams->SetStringField(TEXT("asset_path"), NonMaterialAsset->GetPathName());
	const FMonolithActionResult NonMaterialResult = FMonolithMaterialActions::ValidateMaterial(NonMaterialParams);
	TestFalse(TEXT("A non-material asset must fail the typed validator load"), NonMaterialResult.bSuccess);
	TestTrue(
		TEXT("Non-material failure should identify the required interface type"),
		NonMaterialResult.ErrorMessage.Contains(TEXT("material interface")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
