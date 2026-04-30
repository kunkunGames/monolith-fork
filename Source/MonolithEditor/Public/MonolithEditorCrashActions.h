#pragma once

#include "CoreMinimal.h"

/**
 * Editor namespace actions backing MonolithCrashRecovery_PRD.md:
 *   - editor.get_last_crash_reason
 *   - editor.list_recent_crashes
 *   - editor.get_crash_stats
 *
 * These are pure file-readers — they do NOT run inside the fatal handler
 * and may use full FJsonObject parsing / IFileManager listing.
 */
class FMonolithEditorCrashActions
{
public:
	static void RegisterActions();
};
