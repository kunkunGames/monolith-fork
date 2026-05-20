#include "MonolithIndexFreshness.h"

#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "MonolithSettings.h"

namespace
{
FString NormalizeDbPath(FString Path)
{
	if (Path.IsEmpty())
	{
		return Path;
	}

	Path = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(Path);
	return Path;
}

FString ResolveMonolithSavedDbPath(const TCHAR* FileName)
{
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
	if (Plugin.IsValid())
	{
		return NormalizeDbPath(Plugin->GetBaseDir() / TEXT("Saved") / FileName);
	}

	return NormalizeDbPath(FPaths::ProjectPluginsDir() / TEXT("Monolith") / TEXT("Saved") / FileName);
}

int32 GetArrayCount(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	return Obj.IsValid() && Obj->TryGetArrayField(FieldName, Values) && Values ? Values->Num() : 0;
}

void CopyObjectField(
	const TSharedPtr<FJsonObject>& From,
	TSharedPtr<FJsonObject>& To,
	const TCHAR* FieldName)
{
	if (!From.IsValid() || !To.IsValid())
	{
		return;
	}

	const TSharedPtr<FJsonObject>* ObjectValue = nullptr;
	if (From->TryGetObjectField(FieldName, ObjectValue) && ObjectValue && ObjectValue->IsValid())
	{
		To->SetObjectField(FieldName, *ObjectValue);
	}
}

void CopyArrayField(
	const TSharedPtr<FJsonObject>& From,
	TSharedPtr<FJsonObject>& To,
	const TCHAR* FieldName)
{
	if (!From.IsValid() || !To.IsValid())
	{
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* ArrayValue = nullptr;
	if (From->TryGetArrayField(FieldName, ArrayValue) && ArrayValue)
	{
		To->SetArrayField(FieldName, *ArrayValue);
	}
}

void CopyArrayFieldLimited(
	const TSharedPtr<FJsonObject>& From,
	TSharedPtr<FJsonObject>& To,
	const TCHAR* FieldName,
	int32 MaxItems)
{
	if (!From.IsValid() || !To.IsValid())
	{
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* ArrayValue = nullptr;
	if (From->TryGetArrayField(FieldName, ArrayValue) && ArrayValue)
	{
		TArray<TSharedPtr<FJsonValue>> Bounded = *ArrayValue;
		if (MaxItems >= 0 && Bounded.Num() > MaxItems)
		{
			Bounded.SetNum(MaxItems);
			To->SetBoolField(FString::Printf(TEXT("%s_truncated"), FieldName), true);
		}
		To->SetArrayField(FieldName, Bounded);
	}
}
}

FString FMonolithIndexFreshnessUtils::ResolveProjectIndexDbPath(const UMonolithSettings* Settings)
{
	if (!Settings)
	{
		Settings = UMonolithSettings::Get();
	}

	if (Settings && !Settings->DatabasePathOverride.Path.IsEmpty())
	{
		return NormalizeDbPath(Settings->DatabasePathOverride.Path / TEXT("ProjectIndex.db"));
	}

	return ResolveMonolithSavedDbPath(TEXT("ProjectIndex.db"));
}

FString FMonolithIndexFreshnessUtils::ResolveSourceIndexDbPath(const UMonolithSettings* Settings)
{
	if (!Settings)
	{
		Settings = UMonolithSettings::Get();
	}

	if (Settings && !Settings->EngineSourceDBPathOverride.Path.IsEmpty())
	{
		return NormalizeDbPath(Settings->EngineSourceDBPathOverride.Path / TEXT("EngineSource.db"));
	}

	return ResolveMonolithSavedDbPath(TEXT("EngineSource.db"));
}

TSharedPtr<FJsonObject> FMonolithIndexFreshnessUtils::MakeDatabaseFreshness(
	const FString& Name,
	const FString& DbPath)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), Name);
	Result->SetStringField(TEXT("path"), DbPath);

	const bool bExists = !DbPath.IsEmpty() && FPaths::FileExists(DbPath);
	Result->SetBoolField(TEXT("exists"), bExists);
	Result->SetStringField(TEXT("status"), bExists ? TEXT("present") : TEXT("missing"));
	if (!bExists)
	{
		return Result;
	}

	const int64 FileSize = IFileManager::Get().FileSize(*DbPath);
	if (FileSize >= 0)
	{
		Result->SetNumberField(TEXT("size_bytes"), static_cast<double>(FileSize));
	}

	const FDateTime LastModifiedUtc = IFileManager::Get().GetTimeStamp(*DbPath);
	if (LastModifiedUtc.GetTicks() > 0)
	{
		Result->SetStringField(TEXT("last_modified_utc"), LastModifiedUtc.ToIso8601());
		const double AgeSeconds = FMath::Max(0.0, (FDateTime::UtcNow() - LastModifiedUtc).GetTotalSeconds());
		Result->SetNumberField(TEXT("age_seconds"), AgeSeconds);
	}

	return Result;
}

TSharedPtr<FJsonObject> FMonolithIndexFreshnessUtils::SummarizeHealthResult(
	const TSharedPtr<FJsonObject>& HealthResult)
{
	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	if (!HealthResult.IsValid())
	{
		Summary->SetBoolField(TEXT("available"), false);
		Summary->SetStringField(TEXT("status"), TEXT("unavailable"));
		return Summary;
	}

	Summary->SetBoolField(TEXT("available"), true);

	FString Status = TEXT("unknown");
	HealthResult->TryGetStringField(TEXT("status"), Status);
	Summary->SetStringField(TEXT("status"), Status);

	FString TextSummary;
	if (HealthResult->TryGetStringField(TEXT("summary"), TextSummary))
	{
		Summary->SetStringField(TEXT("summary"), TextSummary);
	}

	Summary->SetNumberField(TEXT("warning_count"), GetArrayCount(HealthResult, TEXT("warnings")));
	CopyObjectField(HealthResult, Summary, TEXT("schema"));
	CopyObjectField(HealthResult, Summary, TEXT("row_counts"));
	CopyArrayFieldLimited(HealthResult, Summary, TEXT("warnings"), 8);
	CopyArrayField(HealthResult, Summary, TEXT("next_actions"));

	const TArray<TSharedPtr<FJsonValue>>* Checks = nullptr;
	int32 CrgCheckCount = 0;
	int32 CrgWarningCount = 0;
	int32 CrgErrorCount = 0;
	TArray<TSharedPtr<FJsonValue>> CrgWarnings;
	if (HealthResult->TryGetArrayField(TEXT("checks"), Checks) && Checks)
	{
		for (const TSharedPtr<FJsonValue>& CheckValue : *Checks)
		{
			const TSharedPtr<FJsonObject>* CheckObject = nullptr;
			if (!CheckValue.IsValid() || !CheckValue->TryGetObject(CheckObject) || !CheckObject || !CheckObject->IsValid())
			{
				continue;
			}

			FString CheckName;
			(*CheckObject)->TryGetStringField(TEXT("check"), CheckName);
			if (!CheckName.StartsWith(TEXT("crg:")))
			{
				continue;
			}

			++CrgCheckCount;
			FString Result = TEXT("unknown");
			(*CheckObject)->TryGetStringField(TEXT("result"), Result);
			if (Result == TEXT("ok") || Result == TEXT("info"))
			{
				continue;
			}

			if (Result == TEXT("error"))
			{
				++CrgErrorCount;
			}
			else
			{
				++CrgWarningCount;
			}

			if (CrgWarnings.Num() < 8)
			{
				FString Detail;
				(*CheckObject)->TryGetStringField(TEXT("detail"), Detail);
				CrgWarnings.Add(MakeShared<FJsonValueString>(
					Detail.IsEmpty() ? CheckName : FString::Printf(TEXT("%s: %s"), *CheckName, *Detail)));
			}
		}
	}

	TSharedPtr<FJsonObject> CrgSummary = MakeShared<FJsonObject>();
	CrgSummary->SetNumberField(TEXT("check_count"), CrgCheckCount);
	CrgSummary->SetNumberField(TEXT("warning_count"), CrgWarningCount);
	CrgSummary->SetNumberField(TEXT("error_count"), CrgErrorCount);
	CrgSummary->SetStringField(TEXT("status"),
		CrgCheckCount == 0 ? TEXT("unavailable") :
		(CrgErrorCount > 0 ? TEXT("error") : (CrgWarningCount > 0 ? TEXT("warning") : TEXT("ok"))));
	if (CrgWarnings.Num() > 0)
	{
		CrgSummary->SetArrayField(TEXT("warnings"), CrgWarnings);
	}
	Summary->SetObjectField(TEXT("crg"), CrgSummary);

	return Summary;
}
