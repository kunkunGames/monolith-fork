#include "MonolithResourceRegistry.h"

#include "MonolithActionExecutionGuard.h"
#include "MonolithJsonUtils.h"
#include "MonolithProgressRegistry.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"

namespace
{
	struct FDefaultDocResource
	{
		const TCHAR* Uri;
		const TCHAR* RelativePath;
		const TCHAR* Name;
		const TCHAR* Description;
	};

	const TArray<FDefaultDocResource>& GetDefaultDocResources()
	{
		static const TArray<FDefaultDocResource> Resources = {
			{
				TEXT("monolith://docs/specs/core"),
				TEXT("Docs/specs/SPEC_MonolithCore.md"),
				TEXT("MonolithCore spec"),
				TEXT("Top-level MonolithCore module behavior and contracts")
			},
			{
				TEXT("monolith://docs/specs/toolcall-ledger"),
				TEXT("Docs/specs/SPEC_MonolithToolCallLedger.md"),
				TEXT("ToolCall ledger spec"),
				TEXT("Redacted local ToolCall record and analysis contract")
			},
			{
				TEXT("monolith://docs/specs/mcp-resources"),
				TEXT("Docs/specs/SPEC_MonolithMcpResources.md"),
				TEXT("MCP resources spec"),
				TEXT("Read-only MCP resources/list and resources/read contract")
			},
			{
				TEXT("monolith://docs/api-reference"),
				TEXT("Docs/API_REFERENCE.md"),
				TEXT("Monolith API reference"),
				TEXT("Public C++ and Blueprint-facing Monolith API reference")
			},
			{
				TEXT("monolith://docs/todo"),
				TEXT("Docs/TODO.md"),
				TEXT("Monolith docs TODO"),
				TEXT("Accepted documentation and verification backlog")
			}
		};
		return Resources;
	}

	FString GetMonolithPluginBaseDir()
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
		if (Plugin.IsValid())
		{
			return Plugin->GetBaseDir();
		}
		return FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("Monolith"));
	}

	bool TryReadPluginRelativeTextFile(const FString& RelativePath, FString& OutText)
	{
		OutText.Empty();
		const FString AbsolutePath = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(GetMonolithPluginBaseDir(), RelativePath));
		if (!FPaths::FileExists(AbsolutePath))
		{
			return false;
		}
		return FFileHelper::LoadFileToString(OutText, *AbsolutePath);
	}
}

FMonolithResourceRegistry& FMonolithResourceRegistry::Get()
{
	static FMonolithResourceRegistry Instance;
	return Instance;
}

void FMonolithResourceRegistry::RegisterTextResource(
	const FMonolithResourceDescriptor& Descriptor,
	const FTextResourceProvider& Provider,
	int32 MaxChars)
{
	if (Descriptor.Uri.IsEmpty() || !Provider.IsBound())
	{
		return;
	}

	FRegisteredResource Resource;
	Resource.Descriptor = Descriptor;
	Resource.Provider = Provider;
	Resource.MaxChars = FMath::Clamp(MaxChars, 1, 1024 * 1024);

	FScopeLock Lock(&ResourceLock);
	Resources.Add(Descriptor.Uri, MoveTemp(Resource));
}

void FMonolithResourceRegistry::RegisterDefaultResources()
{
	FScopeLock Lock(&ResourceLock);
	if (bDefaultResourcesRegistered)
	{
		return;
	}
	bDefaultResourcesRegistered = true;
	Lock.Unlock();

	for (const FDefaultDocResource& Doc : GetDefaultDocResources())
	{
		FString DocText;
		if (!TryReadPluginRelativeTextFile(Doc.RelativePath, DocText) || DocText.IsEmpty())
		{
			continue;
		}

		FMonolithResourceDescriptor Descriptor;
		Descriptor.Uri = Doc.Uri;
		Descriptor.Name = Doc.Name;
		Descriptor.Description = Doc.Description;
		Descriptor.MimeType = TEXT("text/markdown");

		RegisterTextResource(
			Descriptor,
			FTextResourceProvider::CreateLambda([DocText]()
			{
				return DocText;
			}),
			65536);
	}

	// Live (read-time) resources backed by Monolith services. Unlike the doc
	// resources above, these evaluate their provider on each read so the payload
	// reflects current state. Both return redacted, bounded JSON (no raw payloads).
	{
		FMonolithResourceDescriptor Descriptor;
		Descriptor.Uri = TEXT("monolith://tool-calls/recent");
		Descriptor.Name = TEXT("Recent ToolCall records");
		Descriptor.Description = TEXT("Most recent redacted ToolCall audit records (no raw request/response payloads)");
		Descriptor.MimeType = TEXT("application/json");
		RegisterTextResource(
			Descriptor,
			FTextResourceProvider::CreateLambda([]()
			{
				const TSharedPtr<FJsonObject> Records =
					FMonolithActionExecutionGuard::Get().GetToolCallRecordsJson(50, FString(), FString());
				return Records.IsValid() ? FMonolithJsonUtils::Serialize(Records) : FString(TEXT("{}"));
			}),
			131072);
	}
	{
		FMonolithResourceDescriptor Descriptor;
		Descriptor.Uri = TEXT("monolith://audit/recent");
		Descriptor.Name = TEXT("Recent action audit");
		Descriptor.Description = TEXT("Most recent central action-execution audit rows (redacted, bounded)");
		Descriptor.MimeType = TEXT("application/json");
		RegisterTextResource(
			Descriptor,
			FTextResourceProvider::CreateLambda([]()
			{
				const TSharedPtr<FJsonObject> Audit =
					FMonolithActionExecutionGuard::Get().GetRecentAuditJson(50);
				return Audit.IsValid() ? FMonolithJsonUtils::Serialize(Audit) : FString(TEXT("{}"));
			}),
			131072);
	}
	{
		FMonolithResourceDescriptor Descriptor;
		Descriptor.Uri = TEXT("monolith://progress/active");
		Descriptor.Name = TEXT("Active MCP progress");
		Descriptor.Description = TEXT("In-flight per-progressToken progress reported by long-running actions (poll-delivered; real-time SSE push is transport-limited)");
		Descriptor.MimeType = TEXT("application/json");
		RegisterTextResource(
			Descriptor,
			FTextResourceProvider::CreateLambda([]()
			{
				const TSharedPtr<FJsonObject> Progress =
					FMonolithProgressRegistry::Get().GetActiveJson();
				return Progress.IsValid() ? FMonolithJsonUtils::Serialize(Progress) : FString(TEXT("{}"));
			}),
			131072);
	}
}

bool FMonolithResourceRegistry::HasDefaultResourcesRegistered() const
{
	FScopeLock Lock(&ResourceLock);
	return bDefaultResourcesRegistered;
}

int32 FMonolithResourceRegistry::GetResourceCount() const
{
	FScopeLock Lock(&ResourceLock);
	return Resources.Num();
}

TSharedPtr<FJsonObject> FMonolithResourceRegistry::ListResourcesJson(int32 Limit, const FString& Cursor) const
{
	const int32 ClampedLimit = FMath::Clamp(Limit, 1, 100);
	int32 StartIndex = 0;
	if (!Cursor.IsEmpty())
	{
		LexTryParseString(StartIndex, *Cursor);
		StartIndex = FMath::Max(0, StartIndex);
	}

	TArray<FString> Uris;
	{
		FScopeLock Lock(&ResourceLock);
		Resources.GetKeys(Uris);
	}
	Uris.Sort();

	TArray<TSharedPtr<FJsonValue>> ResourceArray;
	const int32 EndIndex = FMath::Min(Uris.Num(), StartIndex + ClampedLimit);
	ResourceArray.Reserve(FMath::Max(0, EndIndex - StartIndex));
	for (int32 Index = StartIndex; Index < EndIndex; ++Index)
	{
		FRegisteredResource Resource;
		{
			FScopeLock Lock(&ResourceLock);
			if (const FRegisteredResource* Found = Resources.Find(Uris[Index]))
			{
				Resource = *Found;
			}
		}

		if (!Resource.Descriptor.Uri.IsEmpty())
		{
			ResourceArray.Add(MakeShared<FJsonValueObject>(DescriptorToJson(Resource.Descriptor)));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("resources"), ResourceArray);
	if (EndIndex < Uris.Num())
	{
		Result->SetStringField(TEXT("nextCursor"), FString::FromInt(EndIndex));
	}
	return Result;
}

FMonolithResourceReadResult FMonolithResourceRegistry::ReadResource(const FString& Uri) const
{
	FRegisteredResource Resource;
	{
		FScopeLock Lock(&ResourceLock);
		if (const FRegisteredResource* Found = Resources.Find(Uri))
		{
			Resource = *Found;
		}
	}

	FMonolithResourceReadResult Result;
	Result.Uri = Uri;
	if (Resource.Descriptor.Uri.IsEmpty())
	{
		Result.Error = FString::Printf(TEXT("Resource not found: %s"), *Uri);
		return Result;
	}

	Result.MimeType = Resource.Descriptor.MimeType.IsEmpty() ? TEXT("text/plain") : Resource.Descriptor.MimeType;
	Result.Text = Resource.Provider.Execute();
	Result.bFound = true;
	if (Result.Text.Len() > Resource.MaxChars)
	{
		Result.Text = Result.Text.Left(Resource.MaxChars);
		Result.bTruncated = true;
	}
	return Result;
}

TSharedPtr<FJsonObject> FMonolithResourceRegistry::ReadResourceJson(const FString& Uri) const
{
	FMonolithResourceReadResult Read = ReadResource(Uri);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> Contents;
	if (Read.bFound)
	{
		TSharedPtr<FJsonObject> Content = MakeShared<FJsonObject>();
		Content->SetStringField(TEXT("uri"), Read.Uri);
		Content->SetStringField(TEXT("mimeType"), Read.MimeType);
		Content->SetStringField(TEXT("text"), Read.Text);
		Content->SetBoolField(TEXT("truncated"), Read.bTruncated);
		Contents.Add(MakeShared<FJsonValueObject>(Content));
	}
	Result->SetArrayField(TEXT("contents"), Contents);
	return Result;
}

TSharedPtr<FJsonObject> FMonolithResourceRegistry::DescriptorToJson(const FMonolithResourceDescriptor& Descriptor)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("uri"), Descriptor.Uri);
	Obj->SetStringField(TEXT("name"), Descriptor.Name);
	Obj->SetStringField(TEXT("description"), Descriptor.Description);
	Obj->SetStringField(TEXT("mimeType"), Descriptor.MimeType.IsEmpty() ? TEXT("text/plain") : Descriptor.MimeType);
	return Obj;
}

#if WITH_DEV_AUTOMATION_TESTS
void FMonolithResourceRegistry::ResetForTests()
{
	FScopeLock Lock(&ResourceLock);
	Resources.Empty();
	bDefaultResourcesRegistered = false;
}
#endif
