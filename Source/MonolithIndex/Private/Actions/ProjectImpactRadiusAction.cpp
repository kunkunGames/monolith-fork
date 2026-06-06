#include "Actions/ProjectImpactRadiusAction.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithIndexReview.h"
#include "MonolithParamSchema.h"
#include "Editor.h"

FMonolithActionResult FProjectImpactRadiusAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (Params->HasField(TEXT("asset_path")))
	{
		if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
		{
			return FMonolithActionResult::Error(TEXT("'asset_path' parameter must be a string"), -32602);
		}
	}
	else if (Params->HasField(TEXT("package_path")))
	{
		if (!Params->TryGetStringField(TEXT("package_path"), AssetPath))
		{
			return FMonolithActionResult::Error(TEXT("'package_path' parameter must be a string"), -32602);
		}
	}

	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'asset_path' parameter is required"), -32602);
	}

	UMonolithIndexSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>() : nullptr;
	FMonolithIndexDatabase* Db = Subsystem ? Subsystem->GetDatabase() : nullptr;
	if (!Db)
	{
		return FMonolithActionResult::Error(TEXT("Index subsystem/database not available"));
	}

	FString Direction = TEXT("both");
	if (Params->HasField(TEXT("direction")) && !Params->TryGetStringField(TEXT("direction"), Direction))
	{
		return FMonolithActionResult::Error(TEXT("'direction' parameter must be a string"), -32602);
	}

	const int32 MaxDepth = FMonolithIndexReview::PInt(Params, TEXT("max_depth"), 2);
	const int32 MaxResults = FMonolithIndexReview::PInt(Params, TEXT("max_results"), 200);

	FString DepType;
	if (Params->HasField(TEXT("dependency_type")) && !Params->TryGetStringField(TEXT("dependency_type"), DepType))
	{
		return FMonolithActionResult::Error(TEXT("'dependency_type' parameter must be a string"), -32602);
	}

	TSharedPtr<FJsonObject> Result =
		FMonolithIndexReview::ImpactRadius(*Db, AssetPath, Direction, MaxDepth, MaxResults, DepType);
	return FMonolithActionResult::Success(Result);
}

TSharedPtr<FJsonObject> FProjectImpactRadiusAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Package path of the seed asset"))
		.Optional(TEXT("direction"), TEXT("string"), TEXT("in|out|both (in=referencers, out=dependencies)"), TEXT("both"))
		.Optional(TEXT("max_depth"), TEXT("integer"), TEXT("Max traversal hops"), TEXT("2"))
		.Optional(TEXT("max_results"), TEXT("integer"), TEXT("Max impacted assets"), TEXT("200"))
		.Optional(TEXT("dependency_type"), TEXT("string"), TEXT("Filter edges by type (Hard/Soft/...)"))
		.Build();
}
