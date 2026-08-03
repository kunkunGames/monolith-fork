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
		if (AssetPath.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("'asset_path' parameter cannot be empty"), -32602);
		}
	}
	else if (Params->HasField(TEXT("package_path")))
	{
		if (!Params->TryGetStringField(TEXT("package_path"), AssetPath))
		{
			return FMonolithActionResult::Error(TEXT("'package_path' parameter must be a string"), -32602);
		}
		if (AssetPath.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("'package_path' parameter cannot be empty"), -32602);
		}
	}

	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'asset_path' (or 'package_path') parameter is required"), -32602);
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

	int32 MaxDepth = 2;
	if (Params->HasField(TEXT("max_depth")))
	{
		double DepthValue = 0.0;
		FString StringValue;
		if (Params->TryGetNumberField(TEXT("max_depth"), DepthValue))
		{
			MaxDepth = FMath::Clamp(static_cast<int32>(DepthValue) <= 0 ? 2 : static_cast<int32>(DepthValue), 1, 6);
		}
		else if (Params->TryGetStringField(TEXT("max_depth"), StringValue) && StringValue.IsNumeric())
		{
			MaxDepth = FMath::Clamp(FCString::Atoi(*StringValue) <= 0 ? 2 : FCString::Atoi(*StringValue), 1, 6);
		}
		else
		{
			return FMonolithActionResult::Error(TEXT("'max_depth' parameter must be a number or numeric string"), -32602);
		}
	}

	int32 MaxResults = 200;
	if (Params->HasField(TEXT("max_results")))
	{
		double ResultsValue = 0.0;
		FString StringValue;
		if (Params->TryGetNumberField(TEXT("max_results"), ResultsValue))
		{
			MaxResults = FMath::Clamp(static_cast<int32>(ResultsValue) <= 0 ? 200 : static_cast<int32>(ResultsValue), 1, 2000);
		}
		else if (Params->TryGetStringField(TEXT("max_results"), StringValue) && StringValue.IsNumeric())
		{
			MaxResults = FMath::Clamp(FCString::Atoi(*StringValue) <= 0 ? 200 : FCString::Atoi(*StringValue), 1, 2000);
		}
		else
		{
			return FMonolithActionResult::Error(TEXT("'max_results' parameter must be a number or numeric string"), -32602);
		}
	}

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
		.Required(TEXT("asset_path"), TEXT("string"), TEXT("Package path of the seed asset"), { TEXT("package_path") })
		.Optional(TEXT("direction"), TEXT("string"), TEXT("in|out|both (in=referencers, out=dependencies)"), TEXT("both"))
		.Optional(TEXT("max_depth"), TEXT("integer"), TEXT("Max traversal hops"), TEXT("2"))
		.Optional(TEXT("max_results"), TEXT("integer"), TEXT("Max impacted assets"), TEXT("200"))
		.Optional(TEXT("dependency_type"), TEXT("string"), TEXT("Filter edges by type (Hard/Soft/...)"))
		.Build();
}
