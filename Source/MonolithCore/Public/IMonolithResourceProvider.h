#pragma once

#include "CoreMinimal.h"
#include "MonolithResourceRegistry.h"

/**
 * Per-namespace resource provider seam (P3b).
 *
 * A provider is an explicit Monolith service that contributes a bounded family of
 * read-only resources for a stable URI scheme (for example monolith://source/file/{path}).
 * Providers are registered by code on FMonolithResourceRegistry and are consulted only
 * AFTER the registry's static descriptor map and eager blob entries miss, so a provider
 * never shadows an explicitly registered resource.
 *
 * Contract:
 *  - ListResources appends ONE or a few stable TEMPLATE descriptors. A provider MUST NOT
 *    perform an unbounded scan (e.g. enumerate every indexed file) here; the descriptor
 *    advertises the URI family, and concrete reads are resolved on demand by ReadResource.
 *  - ReadResource returns true only when the provider both owns the URI scheme AND resolves
 *    a payload. It must populate Out.bFound/Out.Uri/Out.MimeType and either Out.Text (text)
 *    or Out.BlobBytes + Out.bBinary (blob). A URI the provider does not own returns false so
 *    the registry can try the next provider.
 *  - Provider output is subject to the same safety rules as the registry: no absolute local
 *    paths, secrets, or environment values may leak into client-facing fields (§6 of
 *    SPEC_MonolithMcpResources.md), and content must be bounded.
 *
 * The registry copies the registered provider array under its lock and invokes these methods
 * OUTSIDE the lock, so a provider implementation must be safe to call without holding the
 * registry's ResourceLock and must not call back into the registry's registration mutators.
 */
class IMonolithResourceProvider
{
public:
	virtual ~IMonolithResourceProvider() = default;

	/**
	 * Append this provider's stable template descriptor(s) to OutDescriptors. Bounded and
	 * cheap: one (or a few) template descriptors only, never a per-row enumeration.
	 */
	virtual void ListResources(TArray<FMonolithResourceDescriptor>& OutDescriptors) const = 0;

	/**
	 * Resolve a single read for a URI this provider owns. Returns false (leaving Out
	 * untouched apart from a possible cleared state) when the URI is not part of this
	 * provider's scheme so the registry can try the next provider.
	 */
	virtual bool ReadResource(const FString& Uri, FMonolithResourceReadResult& Out) const = 0;
};
