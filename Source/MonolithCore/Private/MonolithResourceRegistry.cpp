#include "MonolithResourceRegistry.h"

#include "MonolithActionExecutionGuard.h"
#include "MonolithJsonUtils.h"
#include "MonolithProgressRegistry.h"
#include "IMonolithResourceProvider.h"
#include "Misc/Base64.h"
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

void FMonolithResourceRegistry::RegisterBlobResource(
	const FMonolithResourceDescriptor& Descriptor,
	const TArray<uint8>& BlobBytes,
	int32 MaxBytes)
{
	if (Descriptor.Uri.IsEmpty())
	{
		return;
	}

	FRegisteredResource Resource;
	Resource.Descriptor = Descriptor;
	Resource.Kind = EMonolithResourceKind::Blob;
	Resource.BlobBytes = BlobBytes;
	Resource.MaxBytes = FMath::Clamp(MaxBytes, 1, 16 * 1024 * 1024);

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

// --- Per-namespace resource provider seam (P3b) ---

void FMonolithResourceRegistry::RegisterProvider(const TSharedRef<IMonolithResourceProvider>& Provider)
{
	FScopeLock Lock(&ResourceLock);
	Providers.AddUnique(Provider);
}

void FMonolithResourceRegistry::UnregisterProvider(const TSharedRef<IMonolithResourceProvider>& Provider)
{
	FScopeLock Lock(&ResourceLock);
	Providers.Remove(Provider);
}

int32 FMonolithResourceRegistry::GetProviderCount() const
{
	FScopeLock Lock(&ResourceLock);
	return Providers.Num();
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
	TArray<TSharedRef<IMonolithResourceProvider>> ProvidersCopy;
	{
		FScopeLock Lock(&ResourceLock);
		Resources.GetKeys(Uris);
		ProvidersCopy = Providers;
	}

	// Per-namespace providers (P3b) contribute bounded TEMPLATE descriptors. Gathered
	// OUTSIDE the lock and merged into the same sorted URI space so pagination stays
	// deterministic. A static-map URI wins over a provider template on collision.
	TMap<FString, FMonolithResourceDescriptor> ProviderDescriptors;
	for (const TSharedRef<IMonolithResourceProvider>& Provider : ProvidersCopy)
	{
		TArray<FMonolithResourceDescriptor> Templates;
		Provider->ListResources(Templates);
		for (const FMonolithResourceDescriptor& Template : Templates)
		{
			if (Template.Uri.IsEmpty() || Uris.Contains(Template.Uri) || ProviderDescriptors.Contains(Template.Uri))
			{
				continue;
			}
			ProviderDescriptors.Add(Template.Uri, Template);
			Uris.Add(Template.Uri);
		}
	}
	Uris.Sort();

	TArray<TSharedPtr<FJsonValue>> ResourceArray;
	const int32 EndIndex = FMath::Min(Uris.Num(), StartIndex + ClampedLimit);
	ResourceArray.Reserve(FMath::Max(0, EndIndex - StartIndex));
	for (int32 Index = StartIndex; Index < EndIndex; ++Index)
	{
		FMonolithResourceDescriptor Descriptor;
		{
			FScopeLock Lock(&ResourceLock);
			if (const FRegisteredResource* Found = Resources.Find(Uris[Index]))
			{
				Descriptor = Found->Descriptor;
			}
		}
		if (Descriptor.Uri.IsEmpty())
		{
			if (const FMonolithResourceDescriptor* Template = ProviderDescriptors.Find(Uris[Index]))
			{
				Descriptor = *Template;
			}
		}

		if (!Descriptor.Uri.IsEmpty())
		{
			ResourceArray.Add(MakeShared<FJsonValueObject>(DescriptorToJson(Descriptor)));
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
		// Static map + eager blob branches missed. Fall back to per-namespace providers
		// (P3b). Copy the provider array under the lock, then invoke each provider OUTSIDE
		// the lock so a provider read (which may touch a DB or the filesystem) cannot stall
		// concurrent registry access. The first provider that owns the URI and resolves a
		// payload wins.
		TArray<TSharedRef<IMonolithResourceProvider>> ProvidersCopy;
		{
			FScopeLock Lock(&ResourceLock);
			ProvidersCopy = Providers;
		}
		for (const TSharedRef<IMonolithResourceProvider>& Provider : ProvidersCopy)
		{
			FMonolithResourceReadResult ProviderResult;
			ProviderResult.Uri = Uri;
			if (Provider->ReadResource(Uri, ProviderResult) && ProviderResult.bFound)
			{
				if (ProviderResult.Uri.IsEmpty())
				{
					ProviderResult.Uri = Uri;
				}
				if (ProviderResult.MimeType.IsEmpty())
				{
					ProviderResult.MimeType = TEXT("text/plain");
				}
				return ProviderResult;
			}
		}

		Result.Error = FString::Printf(TEXT("Resource not found: %s"), *Uri);
		return Result;
	}

	Result.MimeType = Resource.Descriptor.MimeType.IsEmpty() ? TEXT("text/plain") : Resource.Descriptor.MimeType;
	Result.bFound = true;

	if (Resource.Kind == EMonolithResourceKind::Blob)
	{
		Result.bBinary = true;
		Result.BlobBytes = Resource.BlobBytes;
		if (Result.BlobBytes.Num() > Resource.MaxBytes)
		{
			Result.BlobBytes.SetNum(Resource.MaxBytes, EAllowShrinking::No);
			Result.bTruncated = true;
		}
		return Result;
	}

	Result.Text = Resource.Provider.Execute();
	if (Result.Text.Len() > Resource.MaxChars)
	{
		Result.Text = Result.Text.Left(Resource.MaxChars);
		Result.bTruncated = true;
	}
	return Result;
}

TSharedPtr<FJsonObject> FMonolithResourceRegistry::ResultToContentsJson(const FMonolithResourceReadResult& Read)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> Contents;
	if (Read.bFound)
	{
		TSharedPtr<FJsonObject> Content = MakeShared<FJsonObject>();
		Content->SetStringField(TEXT("uri"), Read.Uri);
		Content->SetStringField(TEXT("mimeType"), Read.MimeType);
		if (Read.bBinary)
		{
			Content->SetStringField(TEXT("blob"), FBase64::Encode(Read.BlobBytes));
		}
		else
		{
			Content->SetStringField(TEXT("text"), Read.Text);
		}
		Content->SetBoolField(TEXT("truncated"), Read.bTruncated);
		Contents.Add(MakeShared<FJsonValueObject>(Content));
	}
	Result->SetArrayField(TEXT("contents"), Contents);
	return Result;
}

TSharedPtr<FJsonObject> FMonolithResourceRegistry::ReadResourceJson(const FString& Uri) const
{
	return ResultToContentsJson(ReadResource(Uri));
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
	Providers.Empty();
	bDefaultResourcesRegistered = false;
}
#endif
