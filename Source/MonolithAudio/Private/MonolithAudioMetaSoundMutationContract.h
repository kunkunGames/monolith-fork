#pragma once

#include "CoreMinimal.h"

#if WITH_METASOUND

#include "MetasoundBuilderBase.h"

class UMetaSoundBuilderBase;

namespace MonolithAudio::MetaSoundMutationContract
{
	struct FNamedConnection
	{
		FMetaSoundBuilderNodeOutputHandle Output;
		FMetaSoundBuilderNodeInputHandle Input;
		bool bAlreadyConnected = false;
	};

	struct FPersistedAsset
	{
		FString AssetPath;
		FString PackageName;
		FString Filename;
	};

	/**
	 * Resolves the exact named pins, connects them, and verifies the resulting edge.
	 * A successful return guarantees NodesAreConnected(Output, Input) is true.
	 */
	bool ConnectNamedPinsAndVerify(
		UMetaSoundBuilderBase& Builder,
		const FMetaSoundNodeHandle& SourceNode,
		FName SourceOutput,
		const FMetaSoundNodeHandle& DestinationNode,
		FName DestinationInput,
		FNamedConnection& OutConnection,
		FString& OutError);

	/** Synchronizes an asset-backed builder with the MetaSound frontend. */
	bool SynchronizeAttachedAsset(
		UMetaSoundBuilderBase& Builder,
		FString& OutError);

	/**
	 * Synchronizes an asset-backed builder with the MetaSound frontend and saves
	 * its package. Transient builders fail closed and are never reported as saved.
	 */
	bool PersistAttachedAsset(
		UMetaSoundBuilderBase& Builder,
		FPersistedAsset& OutPersistedAsset,
		FString& OutError);
}

#endif // WITH_METASOUND
