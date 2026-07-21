// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "MonolithMaterialActions.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMaterialNamedRerouteRoundTripTest,
	"Monolith.Material.GraphRoundTrip.NamedRerouteReferencesStayInDestinationPackage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialNamedRerouteRoundTripTest::RunTest(const FString& Parameters)
{
	const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString SourcePackageName = FString::Printf(TEXT("/Game/Tests/Monolith/Transient/M_NamedRerouteSource_%s"), *UniqueSuffix);
	const FString TargetPackageName = FString::Printf(TEXT("/Game/Tests/Monolith/Transient/M_NamedRerouteTarget_%s"), *UniqueSuffix);
	const FString SourceAssetName = FPackageName::GetLongPackageAssetName(SourcePackageName);
	const FString TargetAssetName = FPackageName::GetLongPackageAssetName(TargetPackageName);
	const FString SaveDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/Monolith"));
	const FString SaveFilename = FPaths::Combine(
		SaveDirectory,
		FString::Printf(TEXT("M_NamedRerouteTarget_%s.uasset"), *UniqueSuffix));

	UPackage* SourcePackage = CreatePackage(*SourcePackageName);
	UMaterial* SourceMaterial = SourcePackage
		? NewObject<UMaterial>(SourcePackage, *SourceAssetName, RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	UMaterialExpressionNamedRerouteDeclaration* SourceDeclaration = SourceMaterial
		? NewObject<UMaterialExpressionNamedRerouteDeclaration>(
			SourceMaterial,
			TEXT("MaterialExpressionNamedRerouteDeclaration_0"),
			RF_Transactional)
		: nullptr;

	UPackage* TargetPackage = CreatePackage(*TargetPackageName);
	UMaterial* TargetMaterial = TargetPackage
		? NewObject<UMaterial>(TargetPackage, *TargetAssetName, RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	const FString TargetAssetPath = TargetMaterial ? TargetMaterial->GetPathName() : FString();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*SaveFilename, false, true);
		if (SourcePackage)
		{
			SourcePackage->SetDirtyFlag(false);
		}
		if (TargetPackage)
		{
			TargetPackage->SetDirtyFlag(false);
		}
		if (SourceMaterial)
		{
			SourceMaterial->ClearFlags(RF_Public | RF_Standalone);
			SourceMaterial->MarkAsGarbage();
		}
		if (TargetMaterial)
		{
			TargetMaterial->ClearFlags(RF_Public | RF_Standalone);
			TargetMaterial->MarkAsGarbage();
		}
		if (SourcePackage)
		{
			SourcePackage->MarkAsGarbage();
		}
		if (TargetPackage)
		{
			TargetPackage->MarkAsGarbage();
		}
	};

	TestNotNull(TEXT("Source material should be created"), SourceMaterial);
	TestNotNull(TEXT("Source declaration should be created"), SourceDeclaration);
	TestNotNull(TEXT("Target material should be created"), TargetMaterial);
	if (!SourceMaterial || !SourceDeclaration || !TargetMaterial)
	{
		return false;
	}

	const FGuid VariableGuid = FGuid::NewGuid();
	SourceDeclaration->VariableGuid = VariableGuid;
	const FString SourceDeclarationReference = FString::Printf(
		TEXT("%s'%s'"),
		*SourceDeclaration->GetClass()->GetName(),
		*SourceDeclaration->GetPathName());

	const auto MakeNode = [](const FString& Id, const FString& ClassName, const TSharedPtr<FJsonObject>& Properties)
	{
		TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("id"), Id);
		Node->SetStringField(TEXT("class"), ClassName);
		Node->SetObjectField(TEXT("props"), Properties.IsValid() ? Properties : MakeShared<FJsonObject>());
		return MakeShared<FJsonValueObject>(Node);
	};

	TSharedPtr<FJsonObject> ConstantProperties = MakeShared<FJsonObject>();
	ConstantProperties->SetNumberField(TEXT("R"), 0.5);
	TSharedPtr<FJsonObject> DeclarationProperties = MakeShared<FJsonObject>();
	DeclarationProperties->SetStringField(TEXT("Name"), TEXT("RoundTripValue"));
	DeclarationProperties->SetStringField(TEXT("VariableGuid"), VariableGuid.ToString(EGuidFormats::DigitsWithHyphensInBraces));
	TSharedPtr<FJsonObject> UsageProperties = MakeShared<FJsonObject>();
	UsageProperties->SetStringField(TEXT("Declaration"), SourceDeclarationReference);
	UsageProperties->SetStringField(TEXT("DeclarationGuid"), VariableGuid.ToString(EGuidFormats::DigitsWithHyphensInBraces));

	TArray<TSharedPtr<FJsonValue>> Nodes;
	Nodes.Add(MakeNode(TEXT("MaterialExpressionConstant_0"), TEXT("Constant"), ConstantProperties));
	Nodes.Add(MakeNode(TEXT("MaterialExpressionNamedRerouteDeclaration_0"), TEXT("NamedRerouteDeclaration"), DeclarationProperties));
	Nodes.Add(MakeNode(TEXT("MaterialExpressionNamedRerouteUsage_0"), TEXT("NamedRerouteUsage"), UsageProperties));

	const auto MakeConnection = [](const FString& From, const FString& To, const FString& ToPin)
	{
		TSharedPtr<FJsonObject> Connection = MakeShared<FJsonObject>();
		Connection->SetStringField(TEXT("from"), From);
		Connection->SetStringField(TEXT("from_pin"), TEXT(""));
		Connection->SetStringField(TEXT("to"), To);
		Connection->SetStringField(TEXT("to_pin"), ToPin);
		return MakeShared<FJsonValueObject>(Connection);
	};

	TArray<TSharedPtr<FJsonValue>> Connections;
	Connections.Add(MakeConnection(
		TEXT("MaterialExpressionConstant_0"),
		TEXT("MaterialExpressionNamedRerouteDeclaration_0"),
		TEXT("Input")));

	TSharedPtr<FJsonObject> EmissiveOutput = MakeShared<FJsonObject>();
	EmissiveOutput->SetStringField(TEXT("from"), TEXT("MaterialExpressionNamedRerouteUsage_0"));
	EmissiveOutput->SetStringField(TEXT("from_pin"), TEXT(""));
	EmissiveOutput->SetStringField(TEXT("to_property"), TEXT("EmissiveColor"));

	TSharedPtr<FJsonObject> Graph = MakeShared<FJsonObject>();
	Graph->SetArrayField(TEXT("nodes"), Nodes);
	Graph->SetArrayField(TEXT("custom_hlsl_nodes"), TArray<TSharedPtr<FJsonValue>>());
	Graph->SetArrayField(TEXT("connections"), Connections);
	TArray<TSharedPtr<FJsonValue>> Outputs;
	Outputs.Add(MakeShared<FJsonValueObject>(EmissiveOutput));
	Graph->SetArrayField(TEXT("outputs"), Outputs);

	FString GraphJson;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&GraphJson);
	TestTrue(TEXT("Graph JSON should serialize"), FJsonSerializer::Serialize(Graph.ToSharedRef(), Writer));

	TSharedPtr<FJsonObject> ImportParams = MakeShared<FJsonObject>();
	ImportParams->SetStringField(TEXT("asset_path"), TargetAssetPath);
	ImportParams->SetStringField(TEXT("graph_json"), GraphJson);
	ImportParams->SetStringField(TEXT("mode"), TEXT("overwrite"));
	const FMonolithActionResult ImportResult = FMonolithMaterialActions::ImportMaterialGraph(ImportParams);
	TestTrue(TEXT("Graph import should succeed"), ImportResult.bSuccess);
	if (!ImportResult.bSuccess)
	{
		AddError(ImportResult.ErrorMessage);
		return false;
	}

	bool bHasImportErrors = false;
	if (ImportResult.Result.IsValid())
	{
		ImportResult.Result->TryGetBoolField(TEXT("has_errors"), bHasImportErrors);
	}
	TestFalse(TEXT("Graph import should not report connection or remap errors"), bHasImportErrors);

	UMaterialExpressionNamedRerouteDeclaration* TargetDeclaration = nullptr;
	UMaterialExpressionNamedRerouteUsage* TargetUsage = nullptr;
	for (const TObjectPtr<UMaterialExpression>& Expression : TargetMaterial->GetExpressions())
	{
		if (!TargetDeclaration)
		{
			TargetDeclaration = Cast<UMaterialExpressionNamedRerouteDeclaration>(Expression);
		}
		if (!TargetUsage)
		{
			TargetUsage = Cast<UMaterialExpressionNamedRerouteUsage>(Expression);
		}
	}

	TestNotNull(TEXT("Target declaration should exist"), TargetDeclaration);
	TestNotNull(TEXT("Target usage should exist"), TargetUsage);
	if (TargetDeclaration && TargetUsage)
	{
		UMaterialExpressionNamedRerouteDeclaration* RemappedDeclaration = TargetUsage->Declaration.Get();
		TestNotNull(TEXT("Usage should hold a declaration reference"), RemappedDeclaration);
		TestTrue(TEXT("Usage declaration should remap to the destination expression"), RemappedDeclaration == TargetDeclaration);
		if (RemappedDeclaration)
		{
			TestTrue(TEXT("Usage declaration should belong to the destination package"), RemappedDeclaration->GetOutermost() == TargetPackage);
			TestFalse(TEXT("Usage declaration must not retain the source private object"), RemappedDeclaration == SourceDeclaration);
		}
	}

	TSharedPtr<FJsonObject> ValidateParams = MakeShared<FJsonObject>();
	ValidateParams->SetStringField(TEXT("asset_path"), TargetAssetPath);
	const FMonolithActionResult ValidateResult = FMonolithMaterialActions::ValidateMaterial(ValidateParams);
	TestTrue(TEXT("Validation should succeed"), ValidateResult.bSuccess);
	if (ValidateResult.bSuccess && ValidateResult.Result.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr;
		if (ValidateResult.Result->TryGetArrayField(TEXT("issues"), Issues) && Issues)
		{
			for (const TSharedPtr<FJsonValue>& IssueValue : *Issues)
			{
				const TSharedPtr<FJsonObject>* Issue = nullptr;
				if (IssueValue.IsValid() && IssueValue->TryGetObject(Issue) && Issue && Issue->IsValid())
				{
					FString Type;
					(*Issue)->TryGetStringField(TEXT("type"), Type);
					TestTrue(TEXT("Reachable named-reroute nodes must not be reported as islands"), Type != TEXT("island"));
				}
			}
		}
	}

	IFileManager::Get().MakeDirectory(*SaveDirectory, true);
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	const bool bSaved = UPackage::SavePackage(TargetPackage, TargetMaterial, *SaveFilename, SaveArgs);
	TestTrue(TEXT("Destination package should save without an external-private-object fatal"), bSaved);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
