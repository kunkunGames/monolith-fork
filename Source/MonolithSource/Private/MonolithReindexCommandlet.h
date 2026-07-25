#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "MonolithReindexCommandlet.generated.h"

/**
 * Builds EngineSource.db WITHOUT a running editor GUI.
 *
 * Reuses the authoritative C++ indexer (FMonolithSourceIndexer) so there is
 * zero parser/schema divergence vs. the in-editor path. Runs synchronously
 * inside UnrealEditor-Cmd.
 *
 * Invoke:
 *   UnrealEditor-Cmd.exe <Project>.uproject -run=MonolithReindex
 *       [-mode=project|full] [-db=<path>] [-enginesource=<path>]
 *       [-projectpath=<path>] [-clean] [-AllowWhenIndexingDisabled]
 *       -unattended -nullrhi
 *
 * Modes:
 *   project (default) — incremental project-only C++ reindex; keeps existing
 *                        engine symbols. Requires EngineSource.db to exist.
 *   full              — engine + shaders + project, clean rebuild.
 *
 * Durable indexing activation must be enabled. The explicit
 * -AllowWhenIndexingDisabled switch permits one maintenance run without
 * changing the persisted state.
 *
 * Exit code: 0 on success, 1 on failure (errors, or DB not produced).
 */
UCLASS()
class UMonolithReindexCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UMonolithReindexCommandlet();

	//~ UCommandlet
	virtual int32 Main(const FString& Params) override;
};
