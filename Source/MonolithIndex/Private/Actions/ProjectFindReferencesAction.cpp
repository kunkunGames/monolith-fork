#include "Actions/ProjectFindReferencesAction.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithParamSchema.h"
#include "Editor.h"

FMonolithActionResult FProjectFindReferencesAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	FString PackagePath;
	// Try asset_path first if present, but don't reject an empty value when the
	// legacy package_path alias can still provide a value.
	if (Params->HasField(TEXT("asset_path")))
	{
		if (!Params->HasTypedField<EJson::String>(TEXT("asset_path")) ||
			!Params->TryGetStringField(TEXT("asset_path"), PackagePath))
		{
			return FMonolithActionResult::Error(TEXT("'asset_path' parameter must be a string"), -32602);
		}
	}
	if (PackagePath.IsEmpty() && Params->HasField(TEXT("package_path")))
	{
		if (!Params->HasTypedField<EJson::String>(TEXT("package_path")) ||
			!Params->TryGetStringField(TEXT("package_path"), PackagePath))
		{
			return FMonolithActionResult::Error(TEXT("'package_path' parameter must be a string"), -32602);
		}
		if (PackagePath.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("'package_path' parameter cannot be empty"), -32602);
		}
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

	TSharedPtr<FJsonObject> Refs = Subsystem->FindReferences(PackagePath);
	if (!Refs.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Asset not found in index"));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("asset_path"), PackagePath);
	Result->SetObjectField(TEXT("references"), Refs);
	return FMonolithActionResult::Success(Result);
}

TSharedPtr<FJsonObject> FProjectFindReferencesAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Package path of the asset (e.g. /Game/Characters/BP_Hero)"), { TEXT("package_path") })
		.Build();
}
