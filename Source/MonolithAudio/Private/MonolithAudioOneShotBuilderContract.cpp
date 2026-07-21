#include "MonolithAudioOneShotBuilderContract.h"

#if WITH_METASOUND

#include "MetasoundBuilderSubsystem.h"
#include "Sound/SoundWave.h"

namespace MonolithAudio::OneShotBuilderContract
{
	namespace
	{
		void SetBuilderStepError(const TCHAR* Step, EMetaSoundBuilderResult Result, FString& OutError)
		{
			OutError = FString::Printf(
				TEXT("create_oneshot_sfx graph build failed at %s (builder result=%s)."),
				Step,
				Result == EMetaSoundBuilderResult::Succeeded ? TEXT("Succeeded") : TEXT("Failed"));
		}
	}

	bool BuildMonoWavePlayerGraph(
		UMetaSoundSourceBuilder& Builder,
		USoundWave& SoundWave,
		const FMetaSoundBuilderNodeOutputHandle& OnPlayOutput,
		const FMetaSoundBuilderNodeInputHandle& OnFinishedInput,
		const TArray<FMetaSoundBuilderNodeInputHandle>& AudioOutInputs,
		FMonoWavePlayerGraph& OutGraph,
		FString& OutError)
	{
		OutGraph = FMonoWavePlayerGraph();
		OutError.Reset();

		if (AudioOutInputs.Num() != 1)
		{
			OutError = FString::Printf(
				TEXT("create_oneshot_sfx graph contract requires exactly one Mono AudioOut input; received %d."),
				AudioOutInputs.Num());
			return false;
		}

		if (!Builder.ContainsNodeOutput(OnPlayOutput))
		{
			OutError = TEXT("create_oneshot_sfx graph contract received an invalid OnPlay output handle.");
			return false;
		}

		if (!Builder.ContainsNodeInput(OnFinishedInput))
		{
			OutError = TEXT("create_oneshot_sfx graph contract received an invalid OnFinished input handle.");
			return false;
		}

		if (!Builder.ContainsNodeInput(AudioOutInputs[0]))
		{
			OutError = TEXT("create_oneshot_sfx graph contract received an invalid Mono AudioOut input handle.");
			return false;
		}

		EMetaSoundBuilderResult StepResult = EMetaSoundBuilderResult::Failed;
		const FMetaSoundNodeHandle WavePlayer = Builder.AddNodeByClassName(
			FMetasoundFrontendClassName(FName(TEXT("UE")), FName(TEXT("Wave Player")), FName(TEXT("Mono"))),
			StepResult,
			1);
		if (StepResult != EMetaSoundBuilderResult::Succeeded || !Builder.ContainsNode(WavePlayer))
		{
			SetBuilderStepError(TEXT("AddNodeByClassName(UE.Wave Player.Mono)"), StepResult, OutError);
			return false;
		}

		const FMetaSoundBuilderNodeInputHandle WaveInput = Builder.FindNodeInputByName(
			WavePlayer,
			FName(TEXT("Wave Asset")),
			StepResult);
		if (StepResult != EMetaSoundBuilderResult::Succeeded || !Builder.ContainsNodeInput(WaveInput))
		{
			SetBuilderStepError(TEXT("FindNodeInputByName(Wave Asset)"), StepResult, OutError);
			return false;
		}

		const FMetasoundFrontendLiteral WaveLiteral = UMetaSoundBuilderSubsystem::GetChecked().CreateObjectMetaSoundLiteral(&SoundWave);
		Builder.SetNodeInputDefault(WaveInput, WaveLiteral, StepResult);
		if (StepResult != EMetaSoundBuilderResult::Succeeded)
		{
			SetBuilderStepError(TEXT("SetNodeInputDefault(Wave Asset)"), StepResult, OutError);
			return false;
		}

		const FMetaSoundBuilderNodeInputHandle PlayInput = Builder.FindNodeInputByName(
			WavePlayer,
			FName(TEXT("Play")),
			StepResult);
		if (StepResult != EMetaSoundBuilderResult::Succeeded || !Builder.ContainsNodeInput(PlayInput))
		{
			SetBuilderStepError(TEXT("FindNodeInputByName(Play)"), StepResult, OutError);
			return false;
		}

		Builder.ConnectNodes(OnPlayOutput, PlayInput, StepResult);
		if (StepResult != EMetaSoundBuilderResult::Succeeded)
		{
			SetBuilderStepError(TEXT("ConnectNodes(OnPlay -> Play)"), StepResult, OutError);
			return false;
		}

		const TArray<FMetaSoundBuilderNodeOutputHandle> TypedAudioOutputs = Builder.FindNodeOutputsByDataType(
			WavePlayer,
			StepResult,
			FName(TEXT("Audio")));
		if (StepResult != EMetaSoundBuilderResult::Succeeded)
		{
			SetBuilderStepError(TEXT("FindNodeOutputsByDataType(Audio)"), StepResult, OutError);
			return false;
		}

		if (TypedAudioOutputs.Num() != 1)
		{
			OutError = FString::Printf(
				TEXT("create_oneshot_sfx graph contract requires exactly one Mono Wave Player Audio output; discovered %d by data type."),
				TypedAudioOutputs.Num());
			return false;
		}

		if (!Builder.ContainsNodeOutput(TypedAudioOutputs[0]))
		{
			OutError = TEXT("create_oneshot_sfx graph contract discovered an invalid Mono Wave Player Audio output handle.");
			return false;
		}

		Builder.ConnectNodes(TypedAudioOutputs[0], AudioOutInputs[0], StepResult);
		if (StepResult != EMetaSoundBuilderResult::Succeeded)
		{
			SetBuilderStepError(TEXT("ConnectNodes(Mono Audio -> AudioOut)"), StepResult, OutError);
			return false;
		}

		const FMetaSoundBuilderNodeOutputHandle PlayerFinished = Builder.FindNodeOutputByName(
			WavePlayer,
			FName(TEXT("On Finished")),
			StepResult);
		if (StepResult != EMetaSoundBuilderResult::Succeeded || !Builder.ContainsNodeOutput(PlayerFinished))
		{
			SetBuilderStepError(TEXT("FindNodeOutputByName(On Finished)"), StepResult, OutError);
			return false;
		}

		Builder.ConnectNodes(PlayerFinished, OnFinishedInput, StepResult);
		if (StepResult != EMetaSoundBuilderResult::Succeeded)
		{
			SetBuilderStepError(TEXT("ConnectNodes(On Finished -> graph OnFinished)"), StepResult, OutError);
			return false;
		}

		OutGraph.WavePlayerNode = WavePlayer;
		OutGraph.AudioOutput = TypedAudioOutputs[0];
		return true;
	}
}

#endif // WITH_METASOUND
