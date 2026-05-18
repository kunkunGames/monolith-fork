#include "Actions/ProjectDetectChangesAction.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithIndexReview.h"
#include "MonolithParamSchema.h"
#include "Dom/JsonValue.h"
#include "Editor.h"

namespace
{
	void AppendPathString(const FString& Raw, TArray<FString>& Out)
	{
		TArray<FString> Parts;
		Raw.ParseIntoArray(Parts, TEXT(","), true);
		if (Parts.Num() == 0 && !Raw.IsEmpty())
		{
			Parts.Add(Raw);
		}
		for (FString Part : Parts)
		{
			Part.TrimStartAndEndInline();
			if (!Part.IsEmpty() && !Out.Contains(Part))
			{
				Out.Add(Part);
			}
		}
	}

	void AppendPathField(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, TArray<FString>& Out)
	{
		if (!Params.IsValid())
		{
			return;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Params->TryGetArrayField(Key, Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Arr)
			{
				if (Value.IsValid())
				{
					AppendPathString(Value->AsString(), Out);
				}
			}
			return;
		}
		FString S;
		if (Params->TryGetStringField(Key, S))
		{
			AppendPathString(S, Out);
		}
	}

	TArray<FString> CollectChangedPaths(const TSharedPtr<FJsonObject>& Params)
	{
		TArray<FString> Paths;
		AppendPathField(Params, TEXT("changed_paths"), Paths);
		AppendPathField(Params, TEXT("paths"), Paths);
		return Paths;
	}
}

FMonolithActionResult FProjectDetectChangesAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	UMonolithIndexSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>() : nullptr;
	FMonolithIndexDatabase* Db = Subsystem ? Subsystem->GetDatabase() : nullptr;
	if (!Db)
	{
		return FMonolithActionResult::Error(TEXT("Index subsystem/database not available"));
	}

	return FMonolithActionResult::Success(FMonolithIndexReview::DetectChanges(*Db,
		CollectChangedPaths(Params),
		FMonolithIndexReview::PInt(Params, TEXT("max_results"), 200),
		FMonolithIndexReview::PStr(Params, TEXT("detail_level"), TEXT("minimal"))));
}

TSharedPtr<FJsonObject> FProjectDetectChangesAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Optional(TEXT("changed_paths"), TEXT("array|string"), TEXT("Changed asset paths; also accepts comma-separated string"))
		.Optional(TEXT("paths"), TEXT("array|string"), TEXT("Alias for changed_paths"))
		.Optional(TEXT("max_results"), TEXT("integer"), TEXT("Max changed entities to return"), TEXT("200"))
		.Optional(TEXT("detail_level"), TEXT("string"), TEXT("minimal|standard"), TEXT("minimal"))
		.Build();
}
