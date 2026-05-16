#include "Actions/ProjectImpactRadiusAction.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithIndexReview.h"
#include "MonolithParamSchema.h"
#include "Editor.h"

FMonolithActionResult FProjectImpactRadiusAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = FMonolithIndexReview::PStr(Params, TEXT("asset_path"),
		FMonolithIndexReview::PStr(Params, TEXT("package_path")));
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

	const FString Direction = FMonolithIndexReview::PStr(Params, TEXT("direction"), TEXT("both"));
	const int32 MaxDepth = FMonolithIndexReview::PInt(Params, TEXT("max_depth"), 2);
	const int32 MaxResults = FMonolithIndexReview::PInt(Params, TEXT("max_results"), 200);
	const FString DepType = FMonolithIndexReview::PStr(Params, TEXT("dependency_type"));

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
