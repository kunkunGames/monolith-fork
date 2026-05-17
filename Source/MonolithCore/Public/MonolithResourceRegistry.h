#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Templates/Function.h"

struct FMonolithResourceDescriptor
{
	FString Uri;
	FString Name;
	FString Description;
	FString MimeType;
};

struct FMonolithResourceReadResult
{
	bool bFound = false;
	FString Uri;
	FString MimeType;
	FString Text;
	FString Error;
	bool bTruncated = false;
};

/**
 * Read-only MCP resource registry.
 *
 * Providers are explicit Monolith services, not arbitrary filesystem access.
 * They return bounded text/blob payloads for stable URI families.
 */
class MONOLITHCORE_API FMonolithResourceRegistry
{
public:
	DECLARE_DELEGATE_RetVal(FString, FTextResourceProvider);

	static FMonolithResourceRegistry& Get();

	void RegisterTextResource(
		const FMonolithResourceDescriptor& Descriptor,
		const FTextResourceProvider& Provider,
		int32 MaxChars = 65536);

	void RegisterDefaultResources();
	bool HasDefaultResourcesRegistered() const;
	int32 GetResourceCount() const;

	TSharedPtr<FJsonObject> ListResourcesJson(int32 Limit, const FString& Cursor) const;
	FMonolithResourceReadResult ReadResource(const FString& Uri) const;
	TSharedPtr<FJsonObject> ReadResourceJson(const FString& Uri) const;

#if WITH_DEV_AUTOMATION_TESTS
	void ResetForTests();
#endif

private:
	FMonolithResourceRegistry() = default;

	struct FRegisteredResource
	{
		FMonolithResourceDescriptor Descriptor;
		FTextResourceProvider Provider;
		int32 MaxChars = 65536;
	};

	static TSharedPtr<FJsonObject> DescriptorToJson(const FMonolithResourceDescriptor& Descriptor);

	mutable FCriticalSection ResourceLock;
	TMap<FString, FRegisteredResource> Resources;
	bool bDefaultResourcesRegistered = false;
};
