#pragma once

#include "Internationalization/StringTableCore.h"
#include "Runtime/Launch/Resources/Version.h"

/** Stable StringTable write boundary, including UE 5.8 developer notes. */
namespace MonolithStringTableCompat
{
	inline void SetSourceString(
		const FStringTableRef& Table,
		const FTextKey& Key,
		const FString& SourceString,
		const FString* PreservedDevNotes = nullptr)
	{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8 && WITH_EDITORONLY_DATA
		FString DevNotes = PreservedDevNotes ? *PreservedDevNotes : FString();
		if (!PreservedDevNotes)
		{
			if (const FStringTableEntryConstPtr ExistingEntry = Table->FindEntry(Key))
			{
				DevNotes = ExistingEntry->GetDevNotes();
			}
		}
		Table->SetSourceString(Key, SourceString, DevNotes);
#else
		(void)PreservedDevNotes;
		Table->SetSourceString(Key, SourceString);
#endif
	}
}
