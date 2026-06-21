#include "MonolithSourceResourceProvider.h"

#include "MonolithSourceActions.h"
#include "MonolithSourceDatabase.h"
#include "MonolithSourceSubsystem.h"
#include "Editor.h"

namespace
{
	// Resolve the engine source DB the same way the source actions do (via the editor
	// subsystem), so the provider does not need access to the actions' private GetDB().
	FMonolithSourceDatabase* GetSourceDatabase()
	{
		if (!GEditor)
		{
			return nullptr;
		}
		UMonolithSourceSubsystem* Subsystem = Cast<UMonolithSourceSubsystem>(
			GEditor->GetEditorSubsystemBase(UMonolithSourceSubsystem::StaticClass()));
		if (!Subsystem)
		{
			return nullptr;
		}
		return Subsystem->GetDatabase();
	}
}

void FMonolithSourceResourceProvider::ListResources(TArray<FMonolithResourceDescriptor>& OutDescriptors) const
{
	// ONE template descriptor only — never enumerate the indexed file table (which can hold
	// hundreds of thousands of rows). The descriptor advertises the URI family; concrete
	// reads are resolved on demand in ReadResource.
	FMonolithResourceDescriptor Descriptor;
	Descriptor.Uri = TemplateUri();
	Descriptor.Name = TEXT("Indexed source file");
	Descriptor.Description = TEXT("Read a bounded, line-numbered slice of an indexed C++ source file by checkout/engine-relative path.");
	Descriptor.MimeType = TEXT("text/plain");
	OutDescriptors.Add(MoveTemp(Descriptor));
}

bool FMonolithSourceResourceProvider::ReadResource(const FString& Uri, FMonolithResourceReadResult& Out) const
{
	// Only own URIs under this provider's scheme; anything else lets the registry try the
	// next provider.
	const FString Prefix = UriPrefix();
	if (!Uri.StartsWith(Prefix))
	{
		return false;
	}

	const FString RequestedPath = Uri.Mid(Prefix.Len());
	if (RequestedPath.IsEmpty())
	{
		// The bare scheme / unfilled template is not a concrete resource. Owned-but-unresolved.
		Out.bFound = false;
		Out.Uri = Uri;
		Out.Error = TEXT("Source resource URI is missing a {path} segment.");
		return true;
	}

	FMonolithSourceDatabase* DB = GetSourceDatabase();
	const FMonolithSourceActions::FResolveReadResult Resolved =
		FMonolithSourceActions::ResolveAndReadFile(DB, RequestedPath, /*StartLine*/ 1, /*EndLine*/ 0, /*DefaultWindow*/ 200);

	Out.Uri = Uri;
	Out.MimeType = TEXT("text/plain");

	if (!Resolved.bResolved)
	{
		// Owned URI, but the file did not resolve. Report not-found WITHOUT echoing any
		// absolute path (ResolveAndReadFile never produces one). Returning true tells the
		// registry this provider owns the scheme and no other provider should be tried.
		Out.bFound = false;
		Out.Error = FString::Printf(TEXT("Source resource not found: %s"), *Uri);
		return true;
	}

	// ShortPath() and the line-numbered slice are both absolute-path-free. Prepend the same
	// header the source.read_file action emits so the resource read is self-describing.
	const FString Header = FString::Printf(
		TEXT("--- %s (lines %d-%d) ---"), *Resolved.ShortPath, Resolved.StartLine, Resolved.EndLine);

	Out.bFound = true;
	Out.bBinary = false;
	Out.Text = Header + TEXT("\n") + Resolved.Text;
	return true;
}
