#include "MonolithResourceRegistry.h"

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
				TEXT("monolith://docs/specs/unrealmcp/readme"),
				TEXT("PRD/UnrealMCP/Spec/README.md"),
				TEXT("UnrealMCP spec index"),
				TEXT("Index for Monolith UnrealMCP implementation specs")
			},
			{
				TEXT("monolith://docs/specs/unrealmcp/00"),
				TEXT("PRD/UnrealMCP/Spec/00_implementation_order_and_flags.md"),
				TEXT("UnrealMCP implementation order"),
				TEXT("Implementation order, feature flags, and shared contracts")
			},
			{
				TEXT("monolith://docs/specs/unrealmcp/01"),
				TEXT("PRD/UnrealMCP/Spec/01_deferred_monolith_domain_catalog.md"),
				TEXT("Deferred Monolith domain catalog"),
				TEXT("Deferred domain catalog contract and metadata-only loading behavior")
			},
			{
				TEXT("monolith://docs/specs/unrealmcp/03"),
				TEXT("PRD/UnrealMCP/Spec/03_mcp_resources_and_typed_results.md"),
				TEXT("MCP resources and typed results"),
				TEXT("Resource endpoint and typed MCP result contract")
			},
			{
				TEXT("monolith://docs/specs/unrealmcp/06"),
				TEXT("PRD/UnrealMCP/Spec/06_local_toolcall_record_analysis.md"),
				TEXT("Local ToolCall record and analysis"),
				TEXT("Local redacted ToolCall record and analysis contract")
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

	FString ReadPluginRelativeTextFile(const FString& RelativePath)
	{
		FString Text;
		const FString AbsolutePath = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(GetMonolithPluginBaseDir(), RelativePath));
		if (!FPaths::FileExists(AbsolutePath))
		{
			return FString();
		}
		FFileHelper::LoadFileToString(Text, *AbsolutePath);
		return Text;
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
		FMonolithResourceDescriptor Descriptor;
		Descriptor.Uri = Doc.Uri;
		Descriptor.Name = Doc.Name;
		Descriptor.Description = Doc.Description;
		Descriptor.MimeType = TEXT("text/markdown");

		const FString RelativePath = Doc.RelativePath;
		RegisterTextResource(
			Descriptor,
			FTextResourceProvider::CreateLambda([RelativePath]()
			{
				return ReadPluginRelativeTextFile(RelativePath);
			}),
			65536);
	}
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
	else
	{
		Result->SetField(TEXT("nextCursor"), MakeShared<FJsonValueNull>());
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

	Result.bFound = true;
	Result.MimeType = Resource.Descriptor.MimeType.IsEmpty() ? TEXT("text/plain") : Resource.Descriptor.MimeType;
	Result.Text = Resource.Provider.Execute();
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
