#include "Actions/ProjectGetAssetDetailsAction.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithParamSchema.h"
#include "Editor.h"

FMonolithActionResult FProjectGetAssetDetailsAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	FString PackagePath;
	if (Params->HasField(TEXT("asset_path")) && (!Params->TryGetStringField(TEXT("asset_path"), PackagePath) || PackagePath.IsEmpty()))
	{
		return FMonolithActionResult::Error(TEXT("'asset_path' parameter must be a non-empty string"), -32602);
	}
	if (PackagePath.IsEmpty() && Params->HasField(TEXT("package_path")) && (!Params->TryGetStringField(TEXT("package_path"), PackagePath) || PackagePath.IsEmpty()))
	{
		return FMonolithActionResult::Error(TEXT("'package_path' parameter must be a non-empty string"), -32602);
	}
	if (PackagePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'asset_path' (or 'package_path') parameter is required"), -32602);
	}

	UMonolithIndexSubsystem* Subsystem = GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>();
	if (!Subsystem)
	{
		return FMonolithActionResult::Error(TEXT("Index subsystem not available"));
	}

	TSharedPtr<FJsonObject> Details = Subsystem->GetAssetDetails(PackagePath);
	if (!Details.IsValid() || !Details->HasField(TEXT("asset_name")))
	{
		return FMonolithActionResult::Error(TEXT("Asset not found in index"));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetObjectField(TEXT("asset"), Details);
	return FMonolithActionResult::Success(Result);
}

TSharedPtr<FJsonObject> FProjectGetAssetDetailsAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Package path of the asset (e.g. /Game/Characters/BP_Hero)"))
		.Build();
}
