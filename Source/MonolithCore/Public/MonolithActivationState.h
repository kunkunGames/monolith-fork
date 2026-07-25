#pragma once

#include "CoreMinimal.h"

/**
 * Durable, per-project operator intent for Monolith background services.
 *
 * The state is deliberately separate from UMonolithSettings:
 * - UMonolithSettings is project policy stored in version-controlled config.
 * - This state is local operator intent stored under the project's Saved folder.
 *
 * Missing values default to true, so a fresh checkout starts the HTTP endpoint
 * and indexing work. Explicit Stop commands persist false independently.
 * Malformed values still fail closed to false.
 */
struct MONOLITHCORE_API FMonolithActivationSnapshot
{
	bool bServerEnabled = true;
	bool bIndexingEnabled = true;
};

class MONOLITHCORE_API FMonolithActivationState
{
public:
	/** Read both durable activation flags. Missing values are true; malformed values are false. */
	static FMonolithActivationSnapshot Load();

	static bool IsServerEnabled();
	static bool IsIndexingEnabled();

	/**
	 * Persist one activation flag without changing the other.
	 * Returns false only when the state file could not be written.
	 */
	static bool SetServerEnabled(bool bEnabled, FString* OutError = nullptr);
	static bool SetIndexingEnabled(bool bEnabled, FString* OutError = nullptr);

	/** Absolute path to Saved/Monolith/Activation.ini for this project. */
	static FString GetStateFilePath();

#if WITH_DEV_AUTOMATION_TESTS
	static FMonolithActivationSnapshot LoadFromFileForTests(const FString& FilePath);
	static bool SetServerEnabledInFileForTests(const FString& FilePath, bool bEnabled, FString* OutError = nullptr);
	static bool SetIndexingEnabledInFileForTests(const FString& FilePath, bool bEnabled, FString* OutError = nullptr);
#endif
};
