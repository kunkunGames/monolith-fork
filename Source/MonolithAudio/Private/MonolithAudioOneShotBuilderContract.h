#pragma once

#if WITH_METASOUND

#include "CoreMinimal.h"
#include "MetasoundBuilderBase.h"

class UMetaSoundSourceBuilder;
class USoundWave;

namespace MonolithAudio::OneShotBuilderContract
{
	struct FMonoWavePlayerGraph
	{
		FMetaSoundNodeHandle WavePlayerNode;
		FMetaSoundBuilderNodeOutputHandle AudioOutput;
	};

	/**
	 * Builds the complete transient graph required by create_oneshot_sfx.
	 *
	 * The contract intentionally discovers the mono Wave Player's audio output
	 * by MetaSound data type. Pin display names are engine metadata and are not a
	 * stable API contract (UE 5.8 exposes this pin as "Out Mono").
	 */
	bool BuildMonoWavePlayerGraph(
		UMetaSoundSourceBuilder& Builder,
		USoundWave& SoundWave,
		const FMetaSoundBuilderNodeOutputHandle& OnPlayOutput,
		const FMetaSoundBuilderNodeInputHandle& OnFinishedInput,
		const TArray<FMetaSoundBuilderNodeInputHandle>& AudioOutInputs,
		FMonoWavePlayerGraph& OutGraph,
		FString& OutError);
}

#endif // WITH_METASOUND
