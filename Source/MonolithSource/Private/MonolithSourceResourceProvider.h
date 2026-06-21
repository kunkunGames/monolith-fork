#pragma once

#include "CoreMinimal.h"
#include "IMonolithResourceProvider.h"

/**
 * Source-namespace MCP resource provider (P3b).
 *
 * Exposes the indexed C++ source surface as a read-only resource family under the
 * scheme:
 *
 *     monolith://source/file/{path}
 *
 * where {path} is a checkout/engine-relative or DB-indexed source path (the same path
 * accepted by the source.read_file action). Reads route through the shared
 * FMonolithSourceActions::ResolveAndReadFile so the provider and the action share ONE
 * hardened resolve+read path and the provider can never leak an absolute on-disk path.
 *
 * ListResources advertises exactly ONE template descriptor (the URI family); it never
 * enumerates the indexed file table.
 */
class FMonolithSourceResourceProvider : public IMonolithResourceProvider
{
public:
	/** Scheme prefix for this provider's URI family. */
	static const TCHAR* UriPrefix() { return TEXT("monolith://source/file/"); }

	/** Template descriptor URI advertised by ListResources. */
	static const TCHAR* TemplateUri() { return TEXT("monolith://source/file/{path}"); }

	virtual void ListResources(TArray<FMonolithResourceDescriptor>& OutDescriptors) const override;
	virtual bool ReadResource(const FString& Uri, FMonolithResourceReadResult& Out) const override;
};
