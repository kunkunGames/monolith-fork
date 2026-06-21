#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

class IMonolithResourceProvider;

struct FMonolithResourceDescriptor
{
	FString Uri;
	FString Name;
	FString Description;
	FString MimeType;
};

/** Whether a registered resource serves bounded UTF-8 text or a bounded binary blob. */
enum class EMonolithResourceKind : uint8
{
	Text,
	Blob,
};

struct FMonolithResourceReadResult
{
	bool bFound = false;
	FString Uri;
	FString MimeType;
	FString Text;
	FString Error;
	bool bTruncated = false;

	// Binary/blob slice (P3a). When bBinary is true the payload is BlobBytes and Text is unused;
	// wire serialization base64-encodes BlobBytes into the "blob" field. Text resources keep
	// bBinary=false and BlobBytes empty so their JSON shape is byte-identical to the text-only slice.
	bool bBinary = false;
	TArray<uint8> BlobBytes;
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

	/**
	 * Register a bounded binary/blob resource (P3a). Bytes are stored eagerly and served
	 * verbatim; reads emit a base64-encoded "blob" field instead of "text". MaxBytes is a
	 * separate cap from text MaxChars and is clamped to a safe upper bound.
	 */
	void RegisterBlobResource(
		const FMonolithResourceDescriptor& Descriptor,
		const TArray<uint8>& BlobBytes,
		int32 MaxBytes = 1024 * 1024);

	void RegisterDefaultResources();
	bool HasDefaultResourcesRegistered() const;
	int32 GetResourceCount() const;

	// --- Per-namespace resource provider seam (P3b) ---
	//
	// A provider contributes a bounded URI family (e.g. monolith://source/file/{path})
	// that cannot be expressed as a fixed static descriptor map. Providers are consulted
	// AFTER the static-map and eager-blob branches miss in ReadResource, and their template
	// descriptors are appended AFTER the static descriptors in ListResourcesJson. Registration
	// is by code only; there is no caller-driven provider registration path.

	/** Register a resource provider. Idempotent: the same provider is registered at most once. */
	void RegisterProvider(const TSharedRef<IMonolithResourceProvider>& Provider);

	/** Unregister a previously registered provider. No-op if not registered. */
	void UnregisterProvider(const TSharedRef<IMonolithResourceProvider>& Provider);

	/** Number of currently registered providers. */
	int32 GetProviderCount() const;

	TSharedPtr<FJsonObject> ListResourcesJson(int32 Limit, const FString& Cursor) const;
	FMonolithResourceReadResult ReadResource(const FString& Uri) const;
	TSharedPtr<FJsonObject> ReadResourceJson(const FString& Uri) const;

	/** Serialize an already-resolved read result into the MCP resources/read "contents" payload.
	 *  Lets callers that already hold a FMonolithResourceReadResult (e.g. the HTTP handler, which
	 *  first checks bFound for the JSON-RPC not-found error) build the success body without a
	 *  second ReadResource() — avoiding a duplicate provider/file read per request. */
	static TSharedPtr<FJsonObject> ResultToContentsJson(const FMonolithResourceReadResult& Read);

#if WITH_DEV_AUTOMATION_TESTS
	void ResetForTests();
#endif

private:
	FMonolithResourceRegistry() = default;

	struct FRegisteredResource
	{
		FMonolithResourceDescriptor Descriptor;
		EMonolithResourceKind Kind = EMonolithResourceKind::Text;

		// Text-kind payload.
		FTextResourceProvider Provider;
		int32 MaxChars = 65536;

		// Blob-kind payload (P3a). Bytes are stored eagerly; MaxBytes is a separate cap from MaxChars.
		TArray<uint8> BlobBytes;
		int32 MaxBytes = 1024 * 1024;
	};

	static TSharedPtr<FJsonObject> DescriptorToJson(const FMonolithResourceDescriptor& Descriptor);

	mutable FCriticalSection ResourceLock;
	TMap<FString, FRegisteredResource> Resources;
	bool bDefaultResourcesRegistered = false;

	// Per-namespace providers (P3b). Guarded by ResourceLock for registration/copy; the
	// copied array is invoked OUTSIDE the lock so a provider read cannot deadlock or stall
	// concurrent registry access.
	TArray<TSharedRef<IMonolithResourceProvider>> Providers;
};
